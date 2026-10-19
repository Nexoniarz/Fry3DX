#include "image.h"

#include <algorithm>
#include <cmath>

namespace fry {

Image makeImage(int w, int h, const glm::vec4& c) {
    Image im;
    im.w = w, im.h = h;
    im.px.resize(size_t(w) * size_t(h) * 4);
    const uint8_t b[4] = {uint8_t(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255)), uint8_t(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 255)),
                          uint8_t(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 255)), uint8_t(std::lround(std::clamp(c.a, 0.0f, 1.0f) * 255))};
    for (size_t i = 0; i < im.px.size(); i += 4) std::copy_n(b, 4, &im.px[i]);
    return im;
}

glm::vec4 pixel(const Image& im, int x, int y) {
    const uint8_t* p = im.at(std::clamp(x, 0, im.w - 1), std::clamp(y, 0, im.h - 1));
    return glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
}

glm::vec4 sample(const Image& im, glm::vec2 at) {
    const glm::vec2 p = at - 0.5f;
    const int x = int(std::floor(p.x)), y = int(std::floor(p.y));
    const float fx = p.x - float(x), fy = p.y - float(y);
    return glm::mix(glm::mix(pixel(im, x, y), pixel(im, x + 1, y), fx), glm::mix(pixel(im, x, y + 1), pixel(im, x + 1, y + 1), fx), fy);
}

void Stroke::begin(const Image& im, const glm::vec4& c) {
    base = im, color = c;
    cover.assign(size_t(im.w) * size_t(im.h), 0.0f);
}

void Stroke::put(Image& im, int x, int y, float amount) {
    if (x < 0 || y < 0 || x >= im.w || y >= im.h) return;
    float& c = cover[size_t(y) * size_t(im.w) + size_t(x)];
    if (amount <= c) return;
    c = amount;
    const float a = std::clamp(c * color.a, 0.0f, 1.0f);
    const uint8_t* b = base.at(x, y);
    uint8_t* p = im.at(x, y);
    for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(float(b[k]) + (color[k] * 255.0f - float(b[k])) * a));
    p[3] = uint8_t(std::lround(float(b[3]) + (255.0f - float(b[3])) * a));
}

void Stroke::dab(Image& im, glm::vec2 c, float r, float hardness) {
    r = std::max(r, 0.5f);
    const int x0 = int(std::floor(c.x - r)), x1 = int(std::ceil(c.x + r)), y0 = int(std::floor(c.y - r)), y1 = int(std::ceil(c.y + r));
    const float inner = std::clamp(hardness, 0.0f, 0.999f) * r;
    for (int y = std::max(y0, 0); y <= std::min(y1, im.h - 1); ++y)
        for (int x = std::max(x0, 0); x <= std::min(x1, im.w - 1); ++x) {
            const float d = std::hypot(float(x) + 0.5f - c.x, float(y) + 0.5f - c.y);
            if (d >= r) continue;
            const float t = d <= inner ? 1.0f : 1.0f - (d - inner) / (r - inner);
            put(im, x, y, t * t * (3 - 2 * t));
        }
}

void floodFill(Image& im, glm::ivec2 at, const glm::vec4& color, float tolerance) {
    if (at.x < 0 || at.y < 0 || at.x >= im.w || at.y >= im.h) return;
    const glm::vec4 from = pixel(im, at.x, at.y);
    const Image before = im;
    std::vector<char> done(size_t(im.w) * size_t(im.h), 0);
    std::vector<glm::ivec2> todo{at};
    const uint8_t c[4] = {uint8_t(std::lround(color.r * 255)), uint8_t(std::lround(color.g * 255)), uint8_t(std::lround(color.b * 255)), 255};
    while (!todo.empty()) {
        const glm::ivec2 p = todo.back();
        todo.pop_back();
        if (p.x < 0 || p.y < 0 || p.x >= im.w || p.y >= im.h) continue;
        char& d = done[size_t(p.y) * size_t(im.w) + size_t(p.x)];
        if (d) continue;
        d = 1;
        const glm::vec4 q = pixel(before, p.x, p.y);
        if (std::max({std::abs(q.r - from.r), std::abs(q.g - from.g), std::abs(q.b - from.b), std::abs(q.a - from.a)}) > tolerance) continue;
        std::copy_n(c, 4, im.at(p.x, p.y));
        todo.insert(todo.end(), {{p.x + 1, p.y}, {p.x - 1, p.y}, {p.x, p.y + 1}, {p.x, p.y - 1}});
    }
}

glm::mat3 squareToQuad(const std::array<glm::vec2, 4>& q) {
    // Heckbert's: (0,0)→q0, (1,0)→q1, (1,1)→q2, (0,1)→q3.
    const glm::vec2 d1 = q[1] - q[2], d2 = q[3] - q[2], d3 = q[0] - q[1] + q[2] - q[3];
    float g = 0, h = 0;
    if (std::abs(d3.x) > 1e-9f || std::abs(d3.y) > 1e-9f) {
        const float den = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(den) > 1e-12f) g = (d3.x * d2.y - d2.x * d3.y) / den, h = (d1.x * d3.y - d3.x * d1.y) / den;
    }
    const float a = q[1].x - q[0].x + g * q[1].x, b = q[3].x - q[0].x + h * q[3].x, c = q[0].x;
    const float d = q[1].y - q[0].y + g * q[1].y, e = q[3].y - q[0].y + h * q[3].y, f = q[0].y;
    return glm::mat3(glm::vec3(a, d, g), glm::vec3(b, e, h), glm::vec3(c, f, 1.0f)); // (columns)
}

glm::vec2 mapSquare(const glm::mat3& m, glm::vec2 uv) {
    const glm::vec3 p = m * glm::vec3(uv, 1.0f);
    return glm::vec2(p) / p.z;
}

void stampQuad(Image& dst, const Image& src, const std::array<glm::vec2, 4>& q, float opacity, std::vector<uint8_t>* owner, uint8_t id) {
    if (src.empty()) return;
    const glm::mat3 inv = glm::inverse(squareToQuad(q));
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (const glm::vec2& p : q) lo = glm::min(lo, p), hi = glm::max(hi, p);
    for (int y = std::max(int(lo.y), 0); y <= std::min(int(hi.y) + 1, dst.h - 1); ++y)
        for (int x = std::max(int(lo.x), 0); x <= std::min(int(hi.x) + 1, dst.w - 1); ++x) {
            const glm::vec3 h = inv * glm::vec3(float(x) + 0.5f, float(y) + 0.5f, 1.0f);
            if (std::abs(h.z) < 1e-12f) continue;
            const glm::vec2 uv = glm::vec2(h) / h.z;
            if (uv.x < 0 || uv.y < 0 || uv.x > 1 || uv.y > 1) continue;
            const glm::vec4 s = sample(src, uv * glm::vec2(src.w, src.h));
            const float a = s.a * opacity;
            if (owner && a > 0.05f) (*owner)[size_t(y) * size_t(dst.w) + size_t(x)] = id;
            uint8_t* p = dst.at(x, y);
            for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(float(p[k]) + (s[k] * 255.0f - float(p[k])) * a));
            p[3] = uint8_t(std::lround(float(p[3]) + (255.0f - float(p[3])) * a));
        }
}

} // namespace fry
