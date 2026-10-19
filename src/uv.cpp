// Laying a shape out flat for a picture: pieces facing about the same way, packed in rows.
#include "mesh.h"
#include "scene.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace fry {

std::vector<std::vector<glm::vec2>> autoUnwrap(const Mesh& m, float margin) {
    const size_t F = m.faces.size();
    std::vector<std::vector<glm::vec2>> uv(F);
    if (!F) return uv;
    std::vector<glm::vec3> normal(F);
    std::vector<float> area(F);
    for (size_t f = 0; f < F; ++f) normal[f] = faceNormal(m, int(f)), area[f] = faceArea(m, int(f));
    std::map<std::pair<int, int>, std::vector<int>> byEdge;
    for (size_t f = 0; f < F; ++f) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) byEdge[{std::min(fv[i], fv[(i + 1) % fv.size()]), std::max(fv[i], fv[(i + 1) % fv.size()])}].push_back(int(f));
    }
    // Pieces: from the biggest faces out, taking in neighbours that face within 60 degrees.
    std::vector<int> order(F), chart(F, -1);
    for (size_t f = 0; f < F; ++f) order[f] = int(f);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return area[size_t(a)] > area[size_t(b)]; });
    struct Chart {
        std::vector<int> faces;
        glm::vec2 lo{1e30f}, hi{-1e30f}, at{0.0f};
    };
    std::vector<Chart> charts;
    const float limit = std::cos(glm::radians(60.0f));
    for (int seed : order) {
        if (chart[size_t(seed)] >= 0) continue;
        const int id = int(charts.size());
        charts.push_back({});
        glm::vec3 sum = normal[size_t(seed)] * area[size_t(seed)];
        std::vector<int> todo{seed};
        chart[size_t(seed)] = id;
        while (!todo.empty()) {
            const int f = todo.back();
            todo.pop_back();
            charts.back().faces.push_back(f);
            const auto& fv = m.faces[size_t(f)];
            for (size_t i = 0; i < fv.size(); ++i)
                for (int g : byEdge[{std::min(fv[i], fv[(i + 1) % fv.size()]), std::max(fv[i], fv[(i + 1) % fv.size()])}]) {
                    const glm::vec3 avg = glm::length(sum) > 0 ? glm::normalize(sum) : normal[size_t(seed)];
                    if (chart[size_t(g)] < 0 && glm::dot(normal[size_t(g)], normal[size_t(seed)]) > limit && glm::dot(normal[size_t(g)], avg) > limit) {
                        chart[size_t(g)] = id, sum += normal[size_t(g)] * area[size_t(g)];
                        todo.push_back(g);
                    }
                }
        }
        // Flat onto the piece's average facing, lined up with its first face's longest edge.
        const glm::vec3 n = glm::normalize(glm::length(sum) > 1e-12f ? sum : glm::vec3(0, 0, 1));
        const auto& sf = m.faces[size_t(seed)];
        glm::vec3 u(0.0f);
        for (size_t i = 0; i < sf.size(); ++i) {
            const glm::vec3 d = m.verts[size_t(sf[(i + 1) % sf.size()])] - m.verts[size_t(sf[i])];
            if (glm::length(d) > glm::length(u)) u = d;
        }
        u -= n * glm::dot(u, n);
        if (glm::length(u) < 1e-9f) u = glm::cross(n, std::abs(n.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
        u = glm::normalize(u);
        const glm::vec3 v = glm::cross(n, u);
        Chart& c = charts.back();
        for (int f : c.faces)
            for (int p : m.faces[size_t(f)]) {
                const glm::vec2 q(glm::dot(m.verts[size_t(p)], u), -glm::dot(m.verts[size_t(p)], v)); // (down the picture: v flipped, so it isn't mirrored)
                uv[size_t(f)].push_back(q);
                c.lo = glm::min(c.lo, q), c.hi = glm::max(c.hi, q);
            }
    }
    // Packed in rows, tallest first, into a square.
    float total = 0;
    for (const Chart& c : charts) total += (c.hi.x - c.lo.x) * (c.hi.y - c.lo.y);
    const float side = std::sqrt(std::max(total, 1e-12f)), pad = margin * side * 1.2f;
    float rowWidth = side * 1.2f;
    for (const Chart& c : charts) rowWidth = std::max(rowWidth, c.hi.x - c.lo.x + 2 * pad);
    std::vector<int> byHeight(charts.size());
    for (size_t i = 0; i < charts.size(); ++i) byHeight[i] = int(i);
    std::sort(byHeight.begin(), byHeight.end(), [&](int a, int b) { return charts[size_t(a)].hi.y - charts[size_t(a)].lo.y > charts[size_t(b)].hi.y - charts[size_t(b)].lo.y; });
    float x = 0, y = 0, rowH = 0, usedW = 0;
    for (int i : byHeight) {
        Chart& c = charts[size_t(i)];
        const float w = c.hi.x - c.lo.x + 2 * pad, h = c.hi.y - c.lo.y + 2 * pad;
        if (x > 0 && x + w > rowWidth) y += rowH, x = 0, rowH = 0;
        c.at = glm::vec2(x + pad, y + pad) - c.lo;
        x += w, rowH = std::max(rowH, h), usedW = std::max(usedW, x);
    }
    const float scale = 1.0f / std::max(usedW, y + rowH);
    for (const Chart& c : charts)
        for (int f : c.faces)
            for (glm::vec2& q : uv[size_t(f)]) q = (q + c.at) * scale;
    return uv;
}

} // namespace fry

namespace fry {

std::vector<std::vector<int>> uvIslands(const Mesh& m, const std::vector<std::vector<glm::vec2>>& uvs) {
    std::vector<std::vector<int>> out;
    if (uvs.size() != m.faces.size()) return out;
    // (an edge's two ends where they sit on the picture, for each face that has it)
    std::map<std::pair<int, int>, std::vector<std::pair<int, std::pair<glm::vec2, glm::vec2>>>> byEdge;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        if (uvs[f].size() != fv.size()) return {};
        for (size_t i = 0; i < fv.size(); ++i) {
            const size_t j = (i + 1) % fv.size();
            const bool up = fv[i] < fv[j];
            byEdge[{std::min(fv[i], fv[j]), std::max(fv[i], fv[j])}].push_back({int(f), up ? std::pair{uvs[f][i], uvs[f][j]} : std::pair{uvs[f][j], uvs[f][i]}});
        }
    }
    std::vector<int> island(m.faces.size(), -1);
    for (size_t start = 0; start < m.faces.size(); ++start) {
        if (island[start] >= 0) continue;
        out.push_back({});
        std::vector<int> todo{int(start)};
        island[start] = int(out.size()) - 1;
        while (!todo.empty()) {
            const int f = todo.back();
            todo.pop_back();
            out.back().push_back(f);
            const auto& fv = m.faces[size_t(f)];
            for (size_t i = 0; i < fv.size(); ++i) {
                const auto& around = byEdge[{std::min(fv[i], fv[(i + 1) % fv.size()]), std::max(fv[i], fv[(i + 1) % fv.size()])}];
                const auto mine = std::find_if(around.begin(), around.end(), [&](const auto& e) { return e.first == f; })->second;
                for (const auto& [g, ends] : around)
                    if (island[size_t(g)] < 0 && glm::distance(ends.first, mine.first) < 1e-5f && glm::distance(ends.second, mine.second) < 1e-5f)
                        island[size_t(g)] = island[start], todo.push_back(g);
            }
        }
    }
    return out;
}

void remapPicture(Image& img, const Mesh& m, const std::vector<std::vector<glm::vec2>>& before, const std::vector<std::vector<glm::vec2>>& after,
                  const std::vector<int>& faces) {
    const Image src = img;
    const glm::vec2 size(float(img.w), float(img.h));
    for (int f : faces) {
        if (f < 0 || f >= int(m.faces.size()) || before[size_t(f)].size() != m.faces[size_t(f)].size() || after[size_t(f)].size() != m.faces[size_t(f)].size()) continue;
        const auto& fv = m.faces[size_t(f)];
        for (const auto& t : triangulate(m, f)) {
            glm::vec2 a[3], b[3];
            for (int k = 0; k < 3; ++k) {
                const size_t c = size_t(std::find(fv.begin(), fv.end(), t[size_t(k)]) - fv.begin());
                a[k] = after[size_t(f)][c] * size, b[k] = before[size_t(f)][c] * size;
            }
            const float twice = (a[1].x - a[0].x) * (a[2].y - a[0].y) - (a[2].x - a[0].x) * (a[1].y - a[0].y);
            if (std::abs(twice) < 1e-9f) continue;
            const glm::vec2 lo = glm::max(glm::min(a[0], glm::min(a[1], a[2])) - 2.0f, glm::vec2(0.0f));
            const glm::vec2 hi = glm::min(glm::max(a[0], glm::max(a[1], a[2])) + 2.0f, size - 1.0f);
            for (int y = int(lo.y); y <= int(hi.y); ++y)
                for (int x = int(lo.x); x <= int(hi.x); ++x) {
                    const glm::vec2 q(float(x) + 0.5f, float(y) + 0.5f);
                    float w[3];
                    bool in = true;
                    for (int k = 0; k < 3; ++k) {
                        const glm::vec2 p = a[(k + 1) % 3], e = a[(k + 2) % 3];
                        w[k] = ((e.x - p.x) * (q.y - p.y) - (q.x - p.x) * (e.y - p.y)) / twice;
                        in &= w[k] * std::abs(twice) / std::max(glm::length(e - p), 1e-6f) >= -1.5f; // (and a little past: no seams)
                    }
                    if (!in) continue;
                    const glm::vec4 c = sample(src, b[0] * w[0] + b[1] * w[1] + b[2] * w[2]);
                    uint8_t* p = img.at(x, y);
                    for (int k = 0; k < 4; ++k) p[k] = uint8_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f));
                }
        }
    }
}

} // namespace fry
