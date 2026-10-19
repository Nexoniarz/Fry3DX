#pragma once
// Pictures to paint: RGBA, 8 bits a channel, as seen (sRGB), rows from the top.
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace fry {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    bool empty() const { return w <= 0 || h <= 0; }
    uint8_t* at(int x, int y) { return &px[(size_t(y) * size_t(w) + size_t(x)) * 4]; }
    const uint8_t* at(int x, int y) const { return &px[(size_t(y) * size_t(w) + size_t(x)) * 4]; }
};

Image makeImage(int w, int h, const glm::vec4& color); // colour 0..1
glm::vec4 pixel(const Image& im, int x, int y);         // 0..1
// Smoothly between pixels (pixel centres at .5), edges held.
glm::vec4 sample(const Image& im, glm::vec2 at);

// A brush stroke: paints over the picture as it was when the stroke began, so going over the same
// place again in one stroke doesn't build up (as in GIMP).
struct Stroke {
    Image base;
    std::vector<float> cover; // how much of the colour each pixel has had, this stroke
    glm::vec4 color{1.0f};    // a: how strong the stroke is
    void begin(const Image& im, const glm::vec4& c);
    // Paints `amount` (0..1) of the colour at a pixel.
    void put(Image& im, int x, int y, float amount);
    // A round dab, `radius` pixels, soft edge (hardness 1: none).
    void dab(Image& im, glm::vec2 centre, float radius, float hardness);
};

// Fills the patch round a pixel whose colour is within `tolerance` (0..1) of it.
void floodFill(Image& im, glm::ivec2 at, const glm::vec4& color, float tolerance);

// The perspective map from the unit square (0,0 top-left .. 1,1) to a four-cornered shape
// (corners top-left, top-right, bottom-right, bottom-left): (u, v, 1) → (x·w, y·w, w).
glm::mat3 squareToQuad(const std::array<glm::vec2, 4>& q);
glm::vec2 mapSquare(const glm::mat3& m, glm::vec2 uv);
// `src` painted into `dst` with its corners at these four pixel points: moved, stretched, turned
// or bent in perspective, as the corners say.
// owner: marks the pixels it shows on with `id` (which picture is on top where).
void stampQuad(Image& dst, const Image& src, const std::array<glm::vec2, 4>& corners, float opacity = 1.0f, std::vector<uint8_t>* owner = nullptr, uint8_t id = 0);

} // namespace fry
