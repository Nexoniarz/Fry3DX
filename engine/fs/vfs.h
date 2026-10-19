#pragma once
// Virtual filesystem: everything the game loads goes through here. Directories
// and YMP packs are mounted in order; a file in a later mount overrides the
// same path in earlier ones (like Source's search paths), which is how patches
// and mods replace content without touching the base game.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace keel::fs {

bool mountDir(const std::string& dir);
bool mountPack(const std::string& file);
// Mounts dir/*.ymp (in name order: pak0, pak1, ...) and then dir itself, so
// loose files beat packed ones during development.
bool mountGameDir(const std::string& dir);
// Mods: each folder or .ymp in `modsDir` named in `which` ("all" or "a,b,c"),
// mounted after the game so they override it.
void mountMods(const std::string& modsDir, const std::string& which);
// Takes the mods down again (the engine and game stay), e.g. before mounting a new selection.
void unmountMods();
void unmountAll();

// A mod in the mods folder: a folder or a .ymp, described by an optional mod.txt inside it:
//     title "Low gravity"
//     author "Someone"
//     version "1.0"
//     description "Moon gravity and higher jumps."
struct ModInfo {
    std::string name;    // folder / pack name: what fs_mods lists
    std::string path;
    bool pack = false;
    std::string title, author, version, description;
    size_t files = 0;
    bool replacesAssets = false; // has textures or models (already-loaded ones need a restart)
};
std::vector<ModInfo> listMods(const std::string& modsDir);
// Whether `name` is on in an fs_mods value ("all" or "a,b,c").
bool modEnabled(const std::string& which, const std::string& name);

std::optional<std::vector<uint8_t>> read(std::string_view path);
std::optional<std::string> readText(std::string_view path);
bool exists(std::string_view path);
// When the file `path` resolves to was last written (loose files only: a pack's never changes
// under a running game); for hot reload.
std::optional<int64_t> mtime(std::string_view path);
// Files under `dir` (recursive) ending in `ext`, across all mounts, no duplicates.
std::vector<std::string> list(std::string_view dir, std::string_view ext = {});

} // namespace keel::fs
