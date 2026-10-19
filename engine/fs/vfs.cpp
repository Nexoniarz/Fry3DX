#include "fs/vfs.h"
#include "core/cvar.h"
#include "core/log.h"
#include "fs/ymp.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

namespace keel::fs {
namespace {
namespace stdfs = std::filesystem;

struct Mount {
    std::string root; // directory, or pack file
    std::unique_ptr<YmpArchive> pack;
    bool mod = false;
};
bool g_mountingMods = false;
std::vector<Mount> g_mounts; // later = higher priority

std::string normalize(std::string_view p) {
    std::string s(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.starts_with("./")) s.erase(0, 2);
    while (s.starts_with('/')) s.erase(0, 1);
    return s;
}

bool safe(const std::string& p) { return p.find("..") == std::string::npos; }

Cmd cmd_path("path", [](const Args&) {
    for (auto it = g_mounts.rbegin(); it != g_mounts.rend(); ++it)
        log::info("  {}{}", it->root, it->pack ? "  (pack)" : "");
}, "Show mounted directories and packs, highest priority first");
} // namespace

bool mountDir(const std::string& dir) {
    if (!stdfs::is_directory(dir)) return false;
    g_mounts.push_back({stdfs::path(dir).lexically_normal().generic_string(), nullptr, g_mountingMods});
    return true;
}

bool mountPack(const std::string& file) {
    auto pack = std::make_unique<YmpArchive>();
    if (!pack->open(file)) {
        log::warn("Couldn't open pack {}", file);
        return false;
    }
    g_mounts.push_back({file, std::move(pack), g_mountingMods});
    return true;
}

bool mountGameDir(const std::string& dir) {
    if (!stdfs::is_directory(dir)) return false;
    std::vector<std::string> packs;
    for (auto& e : stdfs::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".ymp") packs.push_back(e.path().generic_string());
    std::sort(packs.begin(), packs.end());
    for (auto& p : packs) mountPack(p);
    return mountDir(dir);
}

void mountMods(const std::string& modsDir, const std::string& which) {
    if (which.empty() || !stdfs::is_directory(modsDir)) return;
    std::vector<stdfs::path> entries;
    for (auto& e : stdfs::directory_iterator(modsDir)) entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    g_mountingMods = true;
    for (const auto& p : entries) {
        const std::string name = p.stem().string();
        if (!modEnabled(which, name)) continue;
        const bool ok = stdfs::is_directory(p) ? mountGameDir(p.string()) : p.extension() == ".ymp" && mountPack(p.string());
        if (ok) log::info("Mod: {}", name);
    }
    g_mountingMods = false;
}

void unmountMods() {
    std::erase_if(g_mounts, [](const Mount& m) { return m.mod; });
}

bool modEnabled(const std::string& which, const std::string& name) {
    return which == "all" || ("," + which + ",").find("," + name + ",") != std::string::npos;
}

namespace {
// mod.txt: `key "value"` (or `key value`) lines; // comments.
void parseModTxt(const std::string& text, ModInfo& m) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t k0 = line.find_first_not_of(" \t");
        if (k0 == std::string::npos || line.compare(k0, 2, "//") == 0) continue;
        const size_t k1 = line.find_first_of(" \t", k0);
        if (k1 == std::string::npos) continue;
        const std::string key = line.substr(k0, k1 - k0);
        std::string value = line.substr(line.find_first_not_of(" \t", k1));
        if (value.size() >= 2 && value.front() == '"') value = value.substr(1, value.rfind('"') - 1);
        if (key == "title") m.title = value;
        else if (key == "author") m.author = value;
        else if (key == "version") m.version = value;
        else if (key == "description") m.description = value;
    }
}
bool isAsset(const std::string& f) { return f.starts_with("textures/") || f.starts_with("models/") || f.starts_with("materials/"); }
} // namespace

std::vector<ModInfo> listMods(const std::string& modsDir) {
    std::vector<ModInfo> out;
    std::error_code ec;
    if (!stdfs::is_directory(modsDir, ec)) return out;
    for (auto& e : stdfs::directory_iterator(modsDir, ec)) {
        ModInfo m;
        m.name = e.path().stem().string();
        m.path = e.path().generic_string();
        if (e.is_directory()) {
            for (auto& f : stdfs::recursive_directory_iterator(e.path(), ec)) {
                if (!f.is_regular_file()) continue;
                ++m.files;
                if (isAsset(stdfs::relative(f.path(), e.path()).generic_string())) m.replacesAssets = true;
            }
            if (std::ifstream in(e.path() / "mod.txt"); in) parseModTxt(std::string((std::istreambuf_iterator<char>(in)), {}), m);
        } else if (e.path().extension() == ".ymp") {
            m.pack = true;
            YmpArchive pack;
            if (!pack.open(m.path)) continue;
            for (const std::string& f : pack.files()) {
                ++m.files;
                if (isAsset(f)) m.replacesAssets = true;
            }
            if (std::vector<uint8_t> txt; pack.read("mod.txt", txt)) parseModTxt(std::string(txt.begin(), txt.end()), m);
        } else {
            continue;
        }
        if (m.title.empty()) m.title = m.name;
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const ModInfo& a, const ModInfo& b) { return a.title < b.title; });
    return out;
}

void unmountAll() { g_mounts.clear(); }

std::optional<std::vector<uint8_t>> read(std::string_view path) {
    const std::string p = normalize(path);
    if (!safe(p)) return std::nullopt;
    std::vector<uint8_t> out;
    for (auto it = g_mounts.rbegin(); it != g_mounts.rend(); ++it) {
        if (it->pack) {
            if (it->pack->read(p, out)) return out;
            continue;
        }
        std::ifstream in(it->root + "/" + p, std::ios::binary);
        if (in) return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
    }
    return std::nullopt;
}

std::optional<std::string> readText(std::string_view path) {
    auto data = read(path);
    if (!data) return std::nullopt;
    return std::string(data->begin(), data->end());
}

std::optional<int64_t> mtime(std::string_view path) {
    const std::string p = normalize(path);
    if (!safe(p)) return std::nullopt;
    for (auto it = g_mounts.rbegin(); it != g_mounts.rend(); ++it) {
        if (it->pack) {
            if (it->pack->contains(p)) return std::nullopt;
            continue;
        }
        std::error_code ec;
        const auto t = stdfs::last_write_time(it->root + "/" + p, ec);
        if (!ec) return int64_t(t.time_since_epoch().count());
    }
    return std::nullopt;
}

bool exists(std::string_view path) {
    const std::string p = normalize(path);
    if (!safe(p)) return false;
    for (auto& m : g_mounts)
        if (m.pack ? m.pack->contains(p) : stdfs::is_regular_file(m.root + "/" + p)) return true;
    return false;
}

std::vector<std::string> list(std::string_view dir, std::string_view ext) {
    const std::string d = normalize(dir);
    std::set<std::string> found;
    auto consider = [&](const std::string& f) {
        if (f.starts_with(d) && (ext.empty() || f.ends_with(ext))) found.insert(f);
    };
    for (auto& m : g_mounts) {
        if (m.pack) {
            for (auto& f : m.pack->files()) consider(f);
        } else if (stdfs::is_directory(m.root + "/" + d)) {
            for (auto& e : stdfs::recursive_directory_iterator(m.root + "/" + d))
                if (e.is_regular_file()) consider(stdfs::relative(e.path(), m.root).generic_string());
        }
    }
    return {found.begin(), found.end()};
}

} // namespace keel::fs
