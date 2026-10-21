#include "bvh.h"

#include <algorithm>
#include <cmath>

namespace fry {

std::optional<float> rayTriangle(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 e1 = b - a, e2 = c - a, p = glm::cross(dir, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12f) return std::nullopt;
    const float inv = 1.0f / det;
    const glm::vec3 s = origin - a;
    const float u = glm::dot(s, p) * inv;
    if (u < 0 || u > 1) return std::nullopt;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(dir, q) * inv;
    if (v < 0 || u + v > 1) return std::nullopt;
    const float t = glm::dot(e2, q) * inv;
    return t > 1e-6f ? std::optional<float>(t) : std::nullopt;
}

namespace {

// Where the ray goes into the box and out, or nothing (in the ray's lengths).
bool rayBox(const glm::vec3& o, const glm::vec3& inv, const glm::vec3& lo, const glm::vec3& hi, float maxT) {
    const glm::vec3 a = (lo - o) * inv, b = (hi - o) * inv;
    const glm::vec3 n = glm::min(a, b), f = glm::max(a, b);
    const float in = std::max(std::max(n.x, n.y), std::max(n.z, 0.0f)), out = std::min(std::min(f.x, f.y), std::min(f.z, maxT));
    return in <= out;
}

} // namespace

TriangleTree::TriangleTree(const Mesh& m, const glm::mat4& matrix) {
    points_.resize(m.verts.size());
    for (size_t i = 0; i < points_.size(); ++i) points_[i] = glm::vec3(matrix * glm::vec4(m.verts[i], 1.0f));
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        if (fv.size() == 3) tris_.push_back({{fv[0], fv[1], fv[2]}, int(f)}); // (the usual ones, without triangulating)
        else if (fv.size() == 4 && glm::dot(glm::cross(m.verts[size_t(fv[1])] - m.verts[size_t(fv[0])], m.verts[size_t(fv[2])] - m.verts[size_t(fv[0])]),
                                            glm::cross(m.verts[size_t(fv[2])] - m.verts[size_t(fv[0])], m.verts[size_t(fv[3])] - m.verts[size_t(fv[0])])) > 0)
            tris_.push_back({{fv[0], fv[1], fv[2]}, int(f)}), tris_.push_back({{fv[0], fv[2], fv[3]}, int(f)}); // (a quad with no dent)
        else
            for (const auto& t : triangulate(m, int(f))) tris_.push_back({t, int(f)});
    }
    if (!tris_.empty()) nodes_.reserve(tris_.size() * 2 / 3 + 1), build(0, int(tris_.size()));
}

int TriangleTree::build(int first, int count) {
    const int at = int(nodes_.size());
    nodes_.push_back({});
    glm::vec3 lo(1e30f), hi(-1e30f), clo(1e30f), chi(-1e30f);
    for (int i = first; i < first + count; ++i) {
        glm::vec3 c(0.0f);
        for (int k : tris_[size_t(i)].v) lo = glm::min(lo, points_[size_t(k)]), hi = glm::max(hi, points_[size_t(k)]), c += points_[size_t(k)];
        clo = glm::min(clo, c / 3.0f), chi = glm::max(chi, c / 3.0f);
    }
    nodes_[size_t(at)].lo = lo, nodes_[size_t(at)].hi = hi;
    if (count <= 4) {
        nodes_[size_t(at)].first = first, nodes_[size_t(at)].count = count;
        return at;
    }
    // Split down the middle of the widest way the triangles' middles spread.
    const glm::vec3 size = chi - clo;
    const int axis = size.x > size.y && size.x > size.z ? 0 : size.y > size.z ? 1 : 2;
    auto mid = [&](const Tri& t) { return points_[size_t(t.v[0])][axis] + points_[size_t(t.v[1])][axis] + points_[size_t(t.v[2])][axis]; };
    const int half = count / 2;
    std::nth_element(tris_.begin() + first, tris_.begin() + first + half, tris_.begin() + first + count, [&](const Tri& a, const Tri& b) { return mid(a) < mid(b); });
    build(first, half);
    const int right = build(first + half, count - half);
    nodes_[size_t(at)].first = right, nodes_[size_t(at)].count = 0;
    return at;
}

std::optional<TriangleTree::Hit> TriangleTree::nearest(const glm::vec3& o, const glm::vec3& d, float maxT) const {
    if (nodes_.empty()) return std::nullopt;
    const glm::vec3 inv(1.0f / d.x, 1.0f / d.y, 1.0f / d.z);
    std::optional<Hit> best;
    int stack[128], n = 0;
    stack[n++] = 0;
    while (n) {
        const Node& node = nodes_[size_t(stack[--n])];
        if (!rayBox(o, inv, node.lo, node.hi, best ? best->t : maxT)) continue;
        if (node.count) {
            for (int i = node.first; i < node.first + node.count; ++i) {
                const Tri& t = tris_[size_t(i)];
                if (auto h = rayTriangle(o, d, points_[size_t(t.v[0])], points_[size_t(t.v[1])], points_[size_t(t.v[2])]); h && *h < (best ? best->t : maxT))
                    best = Hit{*h, t.face, t.v};
            }
        } else if (n + 2 <= 128) {
            stack[n++] = node.first, stack[n++] = int(&node - nodes_.data()) + 1;
        }
    }
    return best;
}

bool TriangleTree::blocked(const glm::vec3& o, const glm::vec3& d, float maxT, int skip) const {
    if (nodes_.empty()) return false;
    const glm::vec3 inv(1.0f / d.x, 1.0f / d.y, 1.0f / d.z);
    int stack[128], n = 0;
    stack[n++] = 0;
    while (n) {
        const Node& node = nodes_[size_t(stack[--n])];
        if (!rayBox(o, inv, node.lo, node.hi, maxT)) continue;
        if (node.count) {
            for (int i = node.first; i < node.first + node.count; ++i) {
                const Tri& t = tris_[size_t(i)];
                if (t.v[0] == skip || t.v[1] == skip || t.v[2] == skip) continue;
                if (auto h = rayTriangle(o, d, points_[size_t(t.v[0])], points_[size_t(t.v[1])], points_[size_t(t.v[2])]); h && *h < maxT) return true;
            }
        } else if (n + 2 <= 128) {
            stack[n++] = node.first, stack[n++] = int(&node - nodes_.data()) + 1;
        }
    }
    return false;
}

std::vector<float> wallThickness(const Mesh& m) {
    const TriangleTree tree(m, glm::mat4(1.0f));
    std::vector<float> out(m.faces.size(), 1e30f);
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        if (fv.size() < 3) continue;
        glm::vec3 c(0.0f);
        for (int v : fv) c += m.verts[size_t(v)];
        c /= float(fv.size());
        const glm::vec3 n = faceNormal(m, int(f));
        // (a hair inside, so the face itself isn't what it meets)
        const glm::vec3 from = c - n * 1e-6f;
        if (auto h = tree.nearest(from, -n)) out[f] = h->t + 1e-6f;
    }
    return out;
}

} // namespace fry
