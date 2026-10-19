#include "fs/ymp.h"
#include "core/log.h"

#include <zstd.h>
#include <zstd_seekable.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace keel {
namespace {
constexpr const char* kIndexName = ".ymp-index";
constexpr size_t kBlock = 512;
constexpr size_t kFrameTarget = 64 * 1024; // end a frame at the next file boundary after this
constexpr unsigned kMaxFrame = 1024 * 1024;

uint64_t roundUp(uint64_t n) { return (n + kBlock - 1) / kBlock * kBlock; }

// ustar header; names up to 255 characters via the prefix field.
bool writeHeader(uint8_t* h, const std::string& name, uint64_t size) {
    std::memset(h, 0, kBlock);
    std::string prefix, base = name;
    if (name.size() > 100) {
        size_t cut = name.rfind('/', 155);
        if (cut == std::string::npos || name.size() - cut - 1 > 100) return false;
        prefix = name.substr(0, cut);
        base = name.substr(cut + 1);
    }
    std::memcpy(h, base.data(), base.size());
    std::snprintf((char*)h + 100, 8, "%07o", 0644);
    std::snprintf((char*)h + 108, 8, "%07o", 0);
    std::snprintf((char*)h + 116, 8, "%07o", 0);
    if (size >= (1ull << 33)) return false; // ustar size field limit: 8 GiB per file
    char sizeField[24];
    std::snprintf(sizeField, sizeof sizeField, "%011" PRIo64, size);
    std::memcpy(h + 124, sizeField, 12);
    std::snprintf((char*)h + 136, 12, "%011o", 0);
    h[156] = '0';
    std::memcpy(h + 257, "ustar", 6);
    h[263] = h[264] = '0';
    std::memcpy(h + 345, prefix.data(), prefix.size());
    std::memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (size_t i = 0; i < kBlock; ++i) sum += h[i];
    std::snprintf((char*)h + 148, 8, "%06o", sum);
    h[155] = ' ';
    return true;
}

bool readHeader(const uint8_t* h, std::string& name, uint64_t& size, char& type) {
    if (h[0] == 0) return false; // end of archive
    name.assign((const char*)h, strnlen((const char*)h, 100));
    if (std::memcmp(h + 257, "ustar", 5) == 0 && h[345]) {
        name = std::string((const char*)h + 345, strnlen((const char*)h + 345, 155)) + "/" + name;
    }
    char sizeField[13] = {};
    std::memcpy(sizeField, h + 124, 12);
    size = std::strtoull(sizeField, nullptr, 8);
    type = char(h[156]);
    return true;
}

struct Layout {
    std::string index;
    std::vector<uint64_t> dataOffsets;
};

// Offsets are written with fixed width, so the index size is known before the offsets are.
Layout layout(const std::vector<std::pair<std::string, uint64_t>>& files) {
    size_t indexSize = 0;
    for (auto& f : files) indexSize += 16 + 1 + 16 + 1 + f.first.size() + 1;
    Layout l;
    uint64_t pos = kBlock + roundUp(indexSize);
    char line[48];
    for (auto& f : files) {
        l.dataOffsets.push_back(pos + kBlock);
        std::snprintf(line, sizeof line, "%016" PRIx64 " %016" PRIx64 " ", pos + kBlock, f.second);
        l.index += line + f.first + "\n";
        pos += kBlock + roundUp(f.second);
    }
    return l;
}
} // namespace

std::vector<uint8_t> buildTar(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<std::pair<std::string, uint64_t>> sizes;
    for (auto& f : files) sizes.emplace_back(f.first, f.second.size());
    const Layout l = layout(sizes);
    std::vector<uint8_t> out;
    auto append = [&](const std::string& name, const void* data, size_t n) {
        size_t at = out.size();
        out.resize(at + kBlock + roundUp(n));
        writeHeader(out.data() + at, name, n);
        std::memcpy(out.data() + at + kBlock, data, n);
    };
    append(kIndexName, l.index.data(), l.index.size());
    for (auto& f : files) append(f.first, f.second.data(), f.second.size());
    out.resize(out.size() + 2 * kBlock);
    return out;
}

bool writeYmp(const std::string& outPath, const std::vector<PackInput>& files, int level) {
    std::vector<std::pair<std::string, uint64_t>> sizes;
    for (auto& f : files) {
        std::ifstream in(f.diskPath, std::ios::binary | std::ios::ate);
        if (!in) return log::error("ympack: can't read {}", f.diskPath), false;
        sizes.emplace_back(f.archivePath, uint64_t(in.tellg()));
    }
    const Layout l = layout(sizes);

    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) return log::error("ympack: can't write {}", outPath), false;
    ZSTD_seekable_CStream* zcs = ZSTD_seekable_createCStream();
    ZSTD_seekable_initCStream(zcs, level, 1, kMaxFrame);
    std::vector<uint8_t> outBuf(ZSTD_CStreamOutSize());
    size_t frameBytes = 0;
    bool ok = true;

    auto drain = [&](ZSTD_outBuffer& ob) {
        ok &= std::fwrite(outBuf.data(), 1, ob.pos, out) == ob.pos;
        ob.pos = 0;
    };
    auto feed = [&](const void* data, size_t n) {
        ZSTD_inBuffer in{data, n, 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer ob{outBuf.data(), outBuf.size(), 0};
            if (ZSTD_isError(ZSTD_seekable_compressStream(zcs, &ob, &in))) ok = false;
            drain(ob);
            if (!ok) return;
        }
        frameBytes += n;
    };
    auto endFrameIfBig = [&] {
        if (frameBytes < kFrameTarget) return;
        size_t left;
        do {
            ZSTD_outBuffer ob{outBuf.data(), outBuf.size(), 0};
            left = ZSTD_seekable_endFrame(zcs, &ob);
            drain(ob);
        } while (left > 0 && !ZSTD_isError(left));
        frameBytes = 0;
    };
    auto member = [&](const std::string& name, const std::vector<uint8_t>& data) {
        uint8_t header[kBlock];
        if (!writeHeader(header, name, data.size())) {
            log::error("ympack: path too long or file over 8 GiB: {}", name);
            ok = false;
            return;
        }
        static const uint8_t zeros[kBlock] = {};
        feed(header, kBlock);
        feed(data.data(), data.size());
        feed(zeros, roundUp(data.size()) - data.size());
        endFrameIfBig();
    };

    member(kIndexName, std::vector<uint8_t>(l.index.begin(), l.index.end()));
    for (size_t i = 0; i < files.size() && ok; ++i) {
        std::ifstream in(files[i].diskPath, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
        member(files[i].archivePath, data);
    }
    static const uint8_t endBlocks[2 * kBlock] = {};
    feed(endBlocks, sizeof endBlocks);
    size_t left;
    do {
        ZSTD_outBuffer ob{outBuf.data(), outBuf.size(), 0};
        left = ZSTD_seekable_endStream(zcs, &ob);
        drain(ob);
    } while (left > 0 && !ZSTD_isError(left));
    ZSTD_seekable_freeCStream(zcs);
    ok &= std::fclose(out) == 0;
    return ok;
}

YmpArchive::YmpArchive() = default;

YmpArchive::~YmpArchive() {
    if (seekable_) ZSTD_seekable_free(seekable_);
    if (file_) std::fclose(file_);
}

bool YmpArchive::readRange(uint64_t offset, size_t size, void* out) {
    if (!memory_.empty() || !seekable_) {
        if (offset + size > memory_.size()) return false;
        std::memcpy(out, memory_.data() + offset, size);
        return true;
    }
    size_t done = 0;
    while (done < size) {
        size_t r = ZSTD_seekable_decompress(seekable_, (uint8_t*)out + done, size - done, offset + done);
        if (ZSTD_isError(r) || r == 0) return false;
        done += r;
    }
    return true;
}

bool YmpArchive::open(const std::string& path) {
    path_ = path;
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) return false;

    seekable_ = ZSTD_seekable_create();
    if (ZSTD_isError(ZSTD_seekable_initFile(seekable_, file_))) {
        // Not seekable: plain .tar.zst. Decompress it all once.
        ZSTD_seekable_free(seekable_);
        seekable_ = nullptr;
        std::fseek(file_, 0, SEEK_SET);
        std::ifstream raw(path, std::ios::binary);
        std::vector<uint8_t> packed((std::istreambuf_iterator<char>(raw)), {});
        ZSTD_DStream* ds = ZSTD_createDStream();
        ZSTD_inBuffer in{packed.data(), packed.size(), 0};
        std::vector<uint8_t> chunk(ZSTD_DStreamOutSize());
        while (in.pos < in.size) {
            ZSTD_outBuffer ob{chunk.data(), chunk.size(), 0};
            if (ZSTD_isError(ZSTD_decompressStream(ds, &ob, &in))) break;
            memory_.insert(memory_.end(), chunk.data(), chunk.data() + ob.pos);
        }
        ZSTD_freeDStream(ds);
        if (memory_.empty()) return log::warn("{} is not a zstd archive", path), false;
        log::warn("{} is not seekable; loaded whole into memory (repack with ympack)", path);
        return parseTar(memory_.size());
    }

    uint64_t total = 0;
    const unsigned frames = ZSTD_seekable_getNumFrames(seekable_);
    if (frames) total = ZSTD_seekable_getFrameDecompressedOffset(seekable_, frames - 1) + ZSTD_seekable_getFrameDecompressedSize(seekable_, frames - 1);

    // Fast path: the index member.
    uint8_t header[kBlock];
    std::string name;
    uint64_t size = 0;
    char type = 0;
    if (readRange(0, kBlock, header) && readHeader(header, name, size, type) && name == kIndexName) {
        std::string index(size, '\0');
        if (!readRange(kBlock, size, index.data())) return false;
        size_t pos = 0;
        while (pos < index.size()) {
            size_t eol = index.find('\n', pos);
            if (eol == std::string::npos || eol - pos < 35) break;
            Entry e{std::strtoull(index.c_str() + pos, nullptr, 16), std::strtoull(index.c_str() + pos + 17, nullptr, 16)};
            entries_[index.substr(pos + 34, eol - pos - 34)] = e;
            pos = eol + 1;
        }
        return true;
    }
    return parseTar(total);
}

bool YmpArchive::parseTar(uint64_t totalSize) {
    uint64_t pos = 0;
    uint8_t header[kBlock];
    std::string name;
    uint64_t size;
    char type;
    while (pos + kBlock <= totalSize && readRange(pos, kBlock, header) && readHeader(header, name, size, type)) {
        if ((type == '0' || type == 0) && name != kIndexName) entries_[name] = {pos + kBlock, size};
        pos += kBlock + roundUp(size);
    }
    return true;
}

bool YmpArchive::read(std::string_view path, std::vector<uint8_t>& out) {
    auto it = entries_.find(path);
    if (it == entries_.end()) return false;
    out.resize(it->second.size);
    return readRange(it->second.offset, out.size(), out.data());
}

std::vector<std::string> YmpArchive::files() const {
    std::vector<std::string> out;
    for (auto& [name, _] : entries_) out.push_back(name);
    return out;
}

} // namespace keel
