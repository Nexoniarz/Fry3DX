// Combining shapes: one added to, taken from, or kept where it overlaps another. A BSP tree of
// each's faces cuts the other's (as csg.js does), then the pieces are joined up again: points
// shared, points that land on another piece's edge set into it (so it stays closed all round).
#include "mesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace fry {
namespace {

constexpr float kEps = 1e-6f;

struct Poly {
    std::vector<glm::vec3> v;
    glm::vec3 n{0.0f};
    float w = 0.0f;
    void plane() {
        n = glm::normalize(glm::cross(v[1] - v[0], v[2] - v[0]));
        w = glm::dot(n, v[0]);
    }
    void flip() {
        std::reverse(v.begin(), v.end());
        n = -n, w = -w;
    }
};

// Splits `p` by the plane (n, w) into the four lists, as csg.js.
void split(const glm::vec3& n, float w, const Poly& p, std::vector<Poly>& coFront, std::vector<Poly>& coBack, std::vector<Poly>& front, std::vector<Poly>& back) {
    enum { Co = 0, Front = 1, Back = 2, Spanning = 3 };
    int kind = 0;
    std::vector<int> types(p.v.size());
    for (size_t i = 0; i < p.v.size(); ++i) {
        const float t = glm::dot(n, p.v[i]) - w;
        types[i] = t < -kEps ? Back : t > kEps ? Front : Co;
        kind |= types[i];
    }
    switch (kind) {
    case Co: (glm::dot(n, p.n) > 0 ? coFront : coBack).push_back(p); break;
    case Front: front.push_back(p); break;
    case Back: back.push_back(p); break;
    default: {
        Poly f, b;
        for (size_t i = 0; i < p.v.size(); ++i) {
            const size_t j = (i + 1) % p.v.size();
            const int ti = types[i], tj = types[j];
            const glm::vec3 vi = p.v[i], vj = p.v[j];
            if (ti != Back) f.v.push_back(vi);
            if (ti != Front) b.v.push_back(vi);
            if ((ti | tj) == Spanning) {
                const float t = (w - glm::dot(n, vi)) / glm::dot(n, vj - vi);
                const glm::vec3 x = vi + (vj - vi) * t;
                f.v.push_back(x), b.v.push_back(x);
            }
        }
        f.n = b.n = p.n, f.w = b.w = p.w;
        if (f.v.size() >= 3) front.push_back(f);
        if (b.v.size() >= 3) back.push_back(b);
    }
    }
}

struct Node {
    bool hasPlane = false;
    glm::vec3 n{0.0f};
    float w = 0.0f;
    std::unique_ptr<Node> front, back;
    std::vector<Poly> polys;

    void invert() {
        for (Poly& p : polys) p.flip();
        n = -n, w = -w;
        if (front) front->invert();
        if (back) back->invert();
        std::swap(front, back);
    }
    std::vector<Poly> clip(const std::vector<Poly>& in) const {
        if (!hasPlane) return in;
        std::vector<Poly> f, b;
        for (const Poly& p : in) split(n, w, p, f, b, f, b);
        if (front) f = front->clip(f);
        if (back) b = back->clip(b);
        else b.clear();
        f.insert(f.end(), b.begin(), b.end());
        return f;
    }
    void clipTo(const Node& other) {
        polys = other.clip(polys);
        if (front) front->clipTo(other);
        if (back) back->clipTo(other);
    }
    void all(std::vector<Poly>& out) const {
        out.insert(out.end(), polys.begin(), polys.end());
        if (front) front->all(out);
        if (back) back->all(out);
    }
    void build(const std::vector<Poly>& in) {
        if (in.empty()) return;
        if (!hasPlane) hasPlane = true, n = in[0].n, w = in[0].w;
        std::vector<Poly> f, b;
        for (const Poly& p : in) split(n, w, p, polys, polys, f, b);
        if (!f.empty()) {
            if (!front) front = std::make_unique<Node>();
            front->build(f);
        }
        if (!b.empty()) {
            if (!back) back = std::make_unique<Node>();
            back->build(b);
        }
    }
};

std::vector<Poly> polysOf(const Mesh& m) {
    std::vector<Poly> out;
    for (size_t f = 0; f < m.faces.size(); ++f)
        for (const auto& t : triangulate(m, int(f))) {
            Poly p;
            p.v = {m.verts[size_t(t[0])], m.verts[size_t(t[1])], m.verts[size_t(t[2])]};
            if (glm::length(glm::cross(p.v[1] - p.v[0], p.v[2] - p.v[0])) < 1e-12f) continue;
            p.plane();
            out.push_back(p);
        }
    return out;
}

// The pieces as a mesh: points shared, and points that lie along another piece's edge set into it.
Mesh meshOf(const std::vector<Poly>& polys) {
    Mesh m;
    std::map<std::array<int64_t, 3>, int> at;
    const float q = 1e5f; // (0.01 mm)
    auto point = [&](const glm::vec3& p) {
        const std::array<int64_t, 3> k{std::llround(p.x * q), std::llround(p.y * q), std::llround(p.z * q)};
        auto [it, fresh] = at.try_emplace(k, int(m.verts.size()));
        if (fresh) m.verts.push_back(p);
        return it->second;
    };
    for (const Poly& p : polys) {
        std::vector<int> f;
        for (const glm::vec3& v : p.v) {
            const int i = point(v);
            if (f.empty() || f.back() != i) f.push_back(i);
        }
        while (f.size() > 1 && f.front() == f.back()) f.pop_back();
        if (f.size() >= 3) m.faces.push_back(f);
    }
    // T-junctions: a point in the middle of an edge it isn't a corner of.
    std::vector<int> order(m.verts.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return m.verts[size_t(a)].x < m.verts[size_t(b)].x; });
    for (auto& f : m.faces) {
        std::vector<int> out;
        for (size_t i = 0; i < f.size(); ++i) {
            const int a = f[i], b = f[(i + 1) % f.size()];
            out.push_back(a);
            const glm::vec3 pa = m.verts[size_t(a)], pb = m.verts[size_t(b)], d = pb - pa;
            const float len2 = glm::dot(d, d);
            if (len2 < 1e-14f) continue;
            std::vector<std::pair<float, int>> on;
            const float lo = std::min(pa.x, pb.x) - 2e-5f, hi = std::max(pa.x, pb.x) + 2e-5f;
            auto it = std::lower_bound(order.begin(), order.end(), lo, [&](int v, float x) { return m.verts[size_t(v)].x < x; });
            for (; it != order.end() && m.verts[size_t(*it)].x <= hi; ++it) {
                const int v = *it;
                if (v == a || v == b) continue;
                const float t = glm::dot(m.verts[size_t(v)] - pa, d) / len2;
                if (t <= 1e-4f || t >= 1 - 1e-4f) continue;
                if (const glm::vec3 off = pa + d * t - m.verts[size_t(v)]; glm::dot(off, off) < 1e-10f) on.push_back({t, v});
            }
            std::sort(on.begin(), on.end());
            for (auto [t, v] : on) out.push_back(v);
        }
        f = out;
    }
    return m;
}

} // namespace

bool combine(Mesh& a, const Mesh& b, Combine op) {
    Node A, B;
    A.build(polysOf(a));
    B.build(polysOf(b));
    if (!A.hasPlane || !B.hasPlane) return false;
    std::vector<Poly> bp;
    switch (op) {
    case Combine::Add:
        A.clipTo(B), B.clipTo(A), B.invert(), B.clipTo(A), B.invert();
        B.all(bp), A.build(bp);
        break;
    case Combine::Subtract:
        A.invert(), A.clipTo(B), B.clipTo(A), B.invert(), B.clipTo(A), B.invert();
        B.all(bp), A.build(bp), A.invert();
        break;
    case Combine::Overlap:
        A.invert(), B.clipTo(A), B.invert(), A.clipTo(B), B.clipTo(A);
        B.all(bp), A.build(bp), A.invert();
        break;
    }
    std::vector<Poly> out;
    A.all(out);
    a = meshOf(out);
    mergeFlatFaces(a);
    return !a.faces.empty();
}

} // namespace fry
