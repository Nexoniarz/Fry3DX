#pragma once
// YMP ("Your Mom's Package") = a tar archive compressed with zstd's seekable
// format. It is a normal .tar.zst (`tar --zstd -xf pak0.ymp` works), and the
// engine can still read any file without decompressing the rest:
//   - frames end at file boundaries, so a small file decompresses on its own
//   - the first tar member, ".ymp-index", lists every file's offset and size
// Plain .tar.zst files (no index, not seekable) also load: they are fully
// decompressed into memory once.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ZSTD_seekable_s;

namespace keel {

struct PackInput {
    std::string archivePath; // path inside the pack, forward slashes
    std::string diskPath;
};

// Writes a pack. Level 19 is slow to build and fast to load; 3 is quick for iteration.
bool writeYmp(const std::string& outPath, const std::vector<PackInput>& files, int level = 19);
// Builds an uncompressed tar (with the YMP index as first member).
std::vector<uint8_t> buildTar(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files);

class YmpArchive {
public:
    YmpArchive();
    ~YmpArchive();
    YmpArchive(const YmpArchive&) = delete;
    YmpArchive& operator=(const YmpArchive&) = delete;

    bool open(const std::string& path);
    bool read(std::string_view path, std::vector<uint8_t>& out);
    bool contains(std::string_view path) const { return entries_.count(std::string(path)) != 0; }
    std::vector<std::string> files() const;

private:
    struct Entry {
        uint64_t offset; // in the uncompressed tar
        uint64_t size;
    };
    bool readRange(uint64_t offset, size_t size, void* out);
    bool parseTar(uint64_t totalSize);

    std::string path_;
    FILE* file_ = nullptr;
    ::ZSTD_seekable_s* seekable_ = nullptr;
    std::vector<uint8_t> memory_; // whole tar, when the archive isn't seekable
    std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace keel
