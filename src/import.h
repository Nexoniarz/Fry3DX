#pragma once
// Models made elsewhere, read in: STL, OBJ, 3MF, glTF / GLB. Each object in the file comes back
// as its own mesh (in metres; points that share a place, one).
#include "mesh.h"

#include <string>
#include <vector>

namespace fry {

struct ImportedMesh {
    std::string name;
    Mesh mesh;
};

// The file's objects, or empty with `error` set. STL and 3MF are in millimetres: made metres.
std::vector<ImportedMesh> importModel(const std::string& path, std::string* error = nullptr);
// The file types it reads, for the file picker (".stl", ...).
const std::vector<std::string>& importFormats();

// A raw DEFLATE stream unpacked (what a zip, so a 3MF, holds); empty if it's broken.
std::vector<uint8_t> inflate(const uint8_t* data, size_t size, size_t expect = 0);

} // namespace fry
