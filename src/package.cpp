// Fry3DX's own file: the scene's text and its pictures, packed into one small file.
//   "FRY3DX\x1a\n", u32 version, u64 size unpacked, then one zstd frame of chunks:
//   u32 name length, name, u64 size, bytes. "scene": the scene as text. A picture: u32 w, u32 h,
//   then its R, G, B, A planes, each pixel as the difference from what its neighbours guess
//   (left, above, above-left: JPEG-LS's guess), which zstd packs far smaller than raw or PNG.
#include "scene.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

namespace fry {
namespace {

constexpr char kMagic[8] = {'F', 'R', 'Y', '3', 'D', 'X', '\x1a', '\n'};
constexpr uint32_t kVersion = 1;

uint8_t guess(int a, int b, int c) { // a left, b above, c above-left
    if (c >= std::max(a, b)) return uint8_t(std::min(a, b));
    if (c <= std::min(a, b)) return uint8_t(std::max(a, b));
    return uint8_t(a + b - c);
}

void packImage(std::string& out, const Image& im) {
    const uint32_t wh[2] = {uint32_t(im.w), uint32_t(im.h)};
    out.append(reinterpret_cast<const char*>(wh), 8);
    const size_t at = out.size(), n = size_t(im.w) * size_t(im.h);
    out.resize(at + n * 4);
    for (int ch = 0; ch < 4; ++ch) {
        char* plane = out.data() + at + n * size_t(ch);
        auto px = [&](int x, int y) { return x < 0 || y < 0 ? 0 : int(im.px[(size_t(y) * size_t(im.w) + size_t(x)) * 4 + size_t(ch)]); };
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x)
                plane[size_t(y) * size_t(im.w) + size_t(x)] = char(uint8_t(px(x, y) - guess(px(x - 1, y), px(x, y - 1), px(x - 1, y - 1))));
    }
}

bool unpackImage(const std::string& in, Image& im) {
    if (in.size() < 8) return false;
    uint32_t wh[2];
    std::memcpy(wh, in.data(), 8);
    const size_t n = size_t(wh[0]) * size_t(wh[1]);
    if (!wh[0] || !wh[1] || wh[0] > 32768 || wh[1] > 32768 || in.size() != 8 + n * 4) return false;
    im.w = int(wh[0]), im.h = int(wh[1]), im.px.assign(n * 4, 0);
    for (int ch = 0; ch < 4; ++ch) {
        const char* plane = in.data() + 8 + n * size_t(ch);
        auto px = [&](int x, int y) { return x < 0 || y < 0 ? 0 : int(im.px[(size_t(y) * size_t(im.w) + size_t(x)) * 4 + size_t(ch)]); };
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x)
                im.px[(size_t(y) * size_t(im.w) + size_t(x)) * 4 + size_t(ch)] =
                    uint8_t(uint8_t(plane[size_t(y) * size_t(im.w) + size_t(x)]) + guess(px(x - 1, y), px(x, y - 1), px(x - 1, y - 1)));
    }
    return true;
}

void chunk(std::string& out, const std::string& name, const std::string& data) {
    const uint32_t n = uint32_t(name.size());
    const uint64_t size = data.size();
    out.append(reinterpret_cast<const char*>(&n), 4), out += name;
    out.append(reinterpret_cast<const char*>(&size), 8), out += data;
}

} // namespace

bool isPackage(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    char head[8] = {};
    return f.read(head, 8) && std::memcmp(head, kMagic, 8) == 0;
}

bool savePackage(const std::string& path, const Scene& scene, int level, std::string* error) {
    Scene s = scene;
    std::string payload;
    for (size_t i = 0; i < s.objects.size(); ++i) {
        Object& o = s.objects[i];
        o.pictureFile.clear();
        if (o.picture) {
            o.pictureFile = std::format("picture {}", i);
            std::string px;
            packImage(px, *o.picture);
            chunk(payload, o.pictureFile, px);
        }
        for (size_t j = 0; j < o.layers.size(); ++j) {
            PictureLayer& l = o.layers[j];
            l.file.clear();
            if (!l.image) continue;
            l.file = std::format("layer {} {}", i, j);
            std::string px;
            packImage(px, *l.image);
            chunk(payload, l.file, px);
        }
    }
    chunk(payload, "scene", sceneText(s));

    ZSTD_CCtx* cx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cx, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(cx, ZSTD_c_checksumFlag, 1);
    if (level >= 15) ZSTD_CCtx_setParameter(cx, ZSTD_c_enableLongDistanceMatching, 1);
    std::string out(20 + ZSTD_compressBound(payload.size()), '\0');
    const uint32_t lo = uint32_t(payload.size()), hi = uint32_t(uint64_t(payload.size()) >> 32); // (the u64 as two halves)
    std::memcpy(out.data(), kMagic, 8), std::memcpy(out.data() + 8, &kVersion, 4), std::memcpy(out.data() + 12, &lo, 4), std::memcpy(out.data() + 16, &hi, 4);
    const size_t packed = ZSTD_compress2(cx, out.data() + 20, out.size() - 20, payload.data(), payload.size());
    ZSTD_freeCCtx(cx);
    if (ZSTD_isError(packed)) {
        if (error) *error = ZSTD_getErrorName(packed);
        return false;
    }
    return writeFileSafely(path, out.data(), 20 + packed, error);
}

std::optional<Scene> loadPackage(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::nullopt;
    };
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("can't open " + path);
    std::string file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 20 || std::memcmp(file.data(), kMagic, 8) != 0) {
        std::istringstream text(file); // (the old plain-text .fry)
        return parseScene(text, path, error);
    }
    uint32_t version = 0, lo = 0, hi = 0;
    std::memcpy(&version, file.data() + 8, 4), std::memcpy(&lo, file.data() + 12, 4), std::memcpy(&hi, file.data() + 16, 4);
    if (version > kVersion) return fail("it was saved by a newer Fry3DX");
    const uint64_t size = uint64_t(lo) | uint64_t(hi) << 32;
    if (size > (uint64_t(4) << 30)) return fail("it's damaged");
    std::string payload(size, '\0');
    ZSTD_DCtx* dx = ZSTD_createDCtx();
    ZSTD_DCtx_setParameter(dx, ZSTD_d_windowLogMax, 31);
    const size_t got = ZSTD_decompressDCtx(dx, payload.data(), payload.size(), file.data() + 20, file.size() - 20);
    ZSTD_freeDCtx(dx);
    if (ZSTD_isError(got) || got != size) return fail("it's damaged (" + std::string(ZSTD_isError(got) ? ZSTD_getErrorName(got) : "too short") + ")");
    std::map<std::string, std::string> chunks;
    for (size_t at = 0; at + 4 <= payload.size();) {
        uint32_t n = 0;
        uint64_t len = 0;
        std::memcpy(&n, payload.data() + at, 4);
        if (at + 4 + n + 8 > payload.size()) return fail("it's damaged");
        std::string name = payload.substr(at + 4, n);
        std::memcpy(&len, payload.data() + at + 4 + n, 8);
        at += 4 + n + 8;
        if (at + len > payload.size()) return fail("it's damaged");
        chunks[name] = payload.substr(at, len);
        at += len;
    }
    std::istringstream text(chunks["scene"]);
    auto s = parseScene(text, path, error);
    if (!s) return s;
    for (Object& o : s->objects) {
        Image im;
        if (!o.pictureFile.empty() && unpackImage(chunks[o.pictureFile], im)) o.picture = std::make_shared<Image>(std::move(im));
        o.pictureFile.clear();
        for (PictureLayer& l : o.layers) {
            Image li;
            if (!l.file.empty() && unpackImage(chunks[l.file], li)) l.image = std::make_shared<Image>(std::move(li));
            l.file.clear();
        }
        std::erase_if(o.layers, [](const PictureLayer& l) { return !l.image; });
    }
    return s;
}

} // namespace fry
