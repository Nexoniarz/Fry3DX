#pragma once
// Text as a 3D shape: the letters' outlines from a font (TrueType / OpenType), made solid.
#include "mesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fry {

struct TextStyle {
    float height = 0.1f;   // a capital letter's height (m)
    float depth = 0.02f;   // how thick the letters are (m)
    float spacing = 0.0f;  // more (or less) room between letters, times the height
    float lineGap = 0.25f; // room between lines, times the height
    int align = 0;         // 0 left, 1 middle, 2 right
    int detail = 6;        // points along each curve of a letter
};

// The letters lying on the ground (XY), the first line's base along X from 0, going up out of the
// ground to `depth`. Lines split at '\n'. Empty, with `error` set, if the font can't be read.
Mesh makeText(const std::vector<uint8_t>& font, const std::string& text, const TextStyle& style, std::string* error = nullptr);

} // namespace fry
