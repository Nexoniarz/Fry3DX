// Combining shapes: one added to, taken from, or kept where it overlaps another. A BSP tree of
// each's faces cuts the other's (as csg.js does), then the pieces are joined up again: points
// shared, points that land on another piece's edge set into it (so it stays closed all round).
#include "mesh.h"

#include <manifold/manifold.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <set>

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
    void build(const std::vector<Poly>& in, int depth = 0) {
        if (in.empty()) return;
        size_t first = 0;
        if (!hasPlane) { // (the piece the plane is taken from stays here: a thin sliver's own corners can
                         // be off its rounded plane, and split by it again and again, forever)
            hasPlane = true, n = in[0].n, w = in[0].w;
            polys.push_back(in[0]);
            first = 1;
        }
        if (depth > 3000) { // (a tree this deep is gone wrong: the rest kept here, not a stack overflow)
            polys.insert(polys.end(), in.begin() + long(first), in.end());
            return;
        }
        std::vector<Poly> f, b;
        for (size_t i = first; i < in.size(); ++i) split(n, w, in[i], polys, polys, f, b);
        if (!f.empty()) {
            if (!front) front = std::make_unique<Node>();
            front->build(f, depth + 1);
        }
        if (!b.empty()) {
            if (!back) back = std::make_unique<Node>();
            back->build(b, depth + 1);
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
        if (f.size() < 3) continue;
        // A sliver squashed flat by the sharing of points (all its corners on one line): it
        // would be a third face on an edge. Left out; its middle points are set into the
        // neighbours' edges below, so it stays closed all round.
        glm::vec3 area(0.0f);
        for (size_t i = 0; i < f.size(); ++i) area += glm::cross(m.verts[size_t(f[i])], m.verts[size_t(f[(i + 1) % f.size()])]);
        if (glm::length(area) < 2e-11f) continue; // (twice the area: about a square of 3 µm)
        m.faces.push_back(f);
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
                if (const float in = 2e-6f / std::sqrt(len2); t <= in || t >= 1 - in) continue; // (a µm or two from its ends: on a long edge, a part of it)
                if (const glm::vec3 off = pa + d * t - m.verts[size_t(v)]; glm::dot(off, off) < 1e-10f) on.push_back({t, v});
            }
            std::sort(on.begin(), on.end());
            for (auto [t, v] : on) out.push_back(v);
        }
        f = out;
    }
    return m;
}

// Faces that run out to a point and straight back (… a b a …): a needle along an edge, which
// would put a third face on it. The needle goes; the face is the same without it.
void removeSpikes(Mesh& m) {
    for (auto& f : m.faces)
        for (bool again = true; again && f.size() >= 3;) {
            again = false;
            for (size_t i = 0; i < f.size() && f.size() >= 3; ++i) {
                const size_t n = f.size(), next = (i + 1) % n, after = (i + 2) % n;
                if (f[i] == f[next]) { // (a point twice in a row)
                    f.erase(f.begin() + long(next));
                    again = true;
                    break;
                }
                if (f[i] == f[after]) { // a b a: b, and one a, go
                    const size_t lo = std::min(next, after), hi = std::max(next, after);
                    f.erase(f.begin() + long(hi)), f.erase(f.begin() + long(lo));
                    again = true;
                    break;
                }
            }
        }
    std::erase_if(m.faces, [](const std::vector<int>& f) { return f.size() < 3; });
}

// Gaps the cutting leaves (a tenth of a mm, where one side's rounding cut a piece the other
// didn't): an edge with no face back along it. A small loop of them, or a sliver of next to no
// area, gets a face of its own; the shape is closed again.
void closeSmallGaps(Mesh& m) {
    // Two faces back to back on the same corners (a wall of no thickness): both go.
    std::map<std::vector<int>, std::vector<size_t>> byCorners;
    auto key = [](std::vector<int> f) {
        std::rotate(f.begin(), std::min_element(f.begin(), f.end()), f.end());
        return f;
    };
    for (size_t i = 0; i < m.faces.size(); ++i) byCorners[key(m.faces[i])].push_back(i);
    std::vector<bool> drop(m.faces.size(), false);
    for (size_t i = 0; i < m.faces.size(); ++i) {
        if (drop[i]) continue;
        std::vector<int> r(m.faces[i].rbegin(), m.faces[i].rend());
        if (auto it = byCorners.find(key(r)); it != byCorners.end())
            for (size_t j : it->second)
                if (j != i && !drop[j]) {
                    drop[i] = drop[j] = true;
                    break;
                }
    }
    for (size_t i = m.faces.size(); i-- > 0;)
        if (drop[i]) m.faces.erase(m.faces.begin() + long(i));

    std::set<std::pair<int, int>> have;
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) have.insert({f[i], f[(i + 1) % f.size()]});
    std::map<int, std::vector<int>> next; // (the open edges, a to b)
    for (auto [a, b] : have)
        if (!have.contains({b, a})) next[a].push_back(b);
    auto fill = [&](std::vector<int> loop) {
        if (loop.size() < 3) return;
        float around = 0.0f;
        glm::vec3 area(0.0f);
        for (size_t i = 0; i < loop.size(); ++i) {
            const glm::vec3 p = m.verts[size_t(loop[i])], q = m.verts[size_t(loop[(i + 1) % loop.size()])];
            around += glm::distance(p, q), area += glm::cross(p, q);
        }
        // A small one, or one next to flat: a piece the cutting lost. Else a real opening: left.
        const glm::vec3 n = glm::length(area) > 0.0f ? glm::normalize(area) : glm::vec3(0.0f);
        float bulge = 0.0f;
        for (int v : loop) bulge = std::max(bulge, std::abs(glm::dot(m.verts[size_t(v)] - m.verts[size_t(loop[0])], n)));
        if (around > 2e-3f && glm::length(area) > 2e-9f && bulge > 1e-4f) return;
        std::reverse(loop.begin(), loop.end());
        m.faces.push_back(loop);
    };
    while (!next.empty()) {
        std::vector<int> loop{next.begin()->first};
        while (!loop.empty() && loop.size() <= 64) {
            auto it = next.find(loop.back());
            if (it == next.end()) break; // (an open run, not a loop)
            const int b = it->second.back();
            it->second.pop_back();
            if (it->second.empty()) next.erase(it);
            // Back at a point already passed: the loop from there closes (two gaps meeting at a corner, a figure 8).
            if (auto at = std::find(loop.begin(), loop.end(), b); at != loop.end()) {
                fill(std::vector<int>(at, loop.end()));
                loop.erase(at + 1, loop.end());
                if (loop.size() == 1 && !next.contains(loop[0])) break;
            } else
                loop.push_back(b);
        }
    }
}

// Every edge has as many faces along it one way as the other (closed; edges where two parts
// only touch count too).
bool sealed(const Mesh& m) {
    std::map<std::pair<int, int>, int> e;
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) {
            const int a = f[i], b = f[(i + 1) % f.size()];
            a < b ? ++e[{a, b}] : --e[{b, a}];
        }
    for (auto [k, n] : e)
        if (n) return false;
    return true;
}

Mesh combineOnce(const Mesh& am, const Mesh& b, Combine op) {
    Node A, B;
    A.build(polysOf(am));
    B.build(polysOf(b));
    if (!A.hasPlane || !B.hasPlane) return {};
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
    Mesh a = meshOf(out);
    // Points the cutting left a hair apart (closer than 0.03 mm: nothing a printer or a model
    // could show) are one; else they fold tiny faces back over their neighbours.
    mergeByDistance(a, 3e-5f);
    removeSpikes(a);
    mergeFlatFaces(a);
    removeSpikes(a);
    closeSmallGaps(a);
    return a;
}


// Manifold's way, for two closed shapes: exact, and doesn't go wrong where faces just touch.
// Each triangle carries the face it came from, so its faces come back whole (not triangles).
// Nothing if either isn't closed all round the way it needs (then the BSP's way).
std::optional<Mesh> combineManifold(const Mesh& a, const Mesh& b, Combine op) {
    auto toManifold = [](const Mesh& m, uint32_t firstFace) -> std::optional<manifold::Manifold> {
        manifold::MeshGL g;
        g.numProp = 3;
        g.vertProperties.reserve(m.verts.size() * 3);
        for (const glm::vec3& v : m.verts) g.vertProperties.insert(g.vertProperties.end(), {v.x, v.y, v.z});
        for (size_t f = 0; f < m.faces.size(); ++f) {
            if (m.faces[f].size() < 3) return std::nullopt; // (a line: not a solid)
            for (const auto& t : triangulate(m, int(f)))
                g.triVerts.insert(g.triVerts.end(), {uint32_t(t[0]), uint32_t(t[1]), uint32_t(t[2])}), g.faceID.push_back(firstFace + uint32_t(f));
        }
        if (g.triVerts.empty()) return std::nullopt;
        g.runOriginalID = {manifold::Manifold::ReserveIDs(1)}; // (so it keeps the faces given, not its own numbering)
        g.runIndex = {0, uint32_t(g.triVerts.size())};
        g.Merge(); // (points in the same place, kept apart by a seam: one)
        manifold::Manifold out(g);
        if (out.Status() != manifold::Manifold::Error::NoError) return std::nullopt;
        return out;
    };
    const auto A = toManifold(a, 0), B = toManifold(b, uint32_t(a.faces.size()));
    if (!A || !B) return std::nullopt;
    const manifold::Manifold r = op == Combine::Add ? *A + *B : op == Combine::Subtract ? *A - *B : *A ^ *B;
    if (r.Status() != manifold::Manifold::Error::NoError) return std::nullopt;
    const manifold::MeshGL g = r.GetMeshGL();
    Mesh out;
    const size_t n = g.NumVert();
    out.verts.reserve(n);
    for (size_t i = 0; i < n; ++i) out.verts.push_back({g.vertProperties[i * g.numProp], g.vertProperties[i * g.numProp + 1], g.vertProperties[i * g.numProp + 2]});
    // The triangles of each face, joined back into it: its outline, walked round (a face cut into
    // two pieces, or with a hole in it: as it comes, triangles).
    // A face of one shape lying flat against a face of the other, side by side (both cubes' tops
    // in a row): one face. A shape's own faces stay as they were.
    const size_t tris = g.triVerts.size() / 3;
    const uint32_t split = uint32_t(a.faces.size());
    std::vector<uint32_t> id(tris);
    for (size_t t = 0; t < tris; ++t) id[t] = t < g.faceID.size() ? g.faceID[t] : uint32_t(0x80000000u + t);
    std::map<uint32_t, uint32_t> parent;
    std::function<uint32_t(uint32_t)> root = [&](uint32_t x) {
        auto it = parent.find(x);
        if (it == parent.end() || it->second == x) return x;
        return it->second = root(it->second);
    };
    std::map<uint32_t, glm::vec4> plane; // (each face's: its normal and how far along it)
    for (size_t t = 0; t < tris; ++t) {
        if (plane.count(id[t])) continue;
        const glm::vec3 p0 = out.verts[g.triVerts[t * 3]], p1 = out.verts[g.triVerts[t * 3 + 1]], p2 = out.verts[g.triVerts[t * 3 + 2]];
        const glm::vec3 c = glm::cross(p1 - p0, p2 - p0);
        if (glm::length(c) > 1e-12f) plane[id[t]] = glm::vec4(glm::normalize(c), glm::dot(glm::normalize(c), p0));
    }
    float size = 0;
    for (const glm::vec3& v : out.verts) size = std::max(size, std::max(std::abs(v.x), std::max(std::abs(v.y), std::abs(v.z))));
    std::map<std::pair<int, int>, size_t> edgeTri;
    for (size_t t = 0; t < tris; ++t)
        for (int k = 0; k < 3; ++k) edgeTri[{int(g.triVerts[t * 3 + size_t(k)]), int(g.triVerts[t * 3 + size_t((k + 1) % 3)])}] = t;
    for (const auto& [e, t] : edgeTri) {
        auto o = edgeTri.find({e.second, e.first});
        if (o == edgeTri.end()) continue;
        const uint32_t x = id[t], y = id[o->second];
        if (x == y || (x < split) == (y < split) || !plane.count(x) || !plane.count(y)) continue;
        const glm::vec4 px = plane[x], py = plane[y];
        if (glm::dot(glm::vec3(px), glm::vec3(py)) > 1 - 1e-5f && std::abs(px.w - py.w) < 1e-5f * std::max(size, 1.0f)) {
            const uint32_t rx = root(x), ry = root(y);
            if (rx != ry) parent[rx] = ry, parent.emplace(ry, ry);
        }
    }
    std::map<uint32_t, std::vector<std::array<int, 3>>> faces;
    for (size_t t = 0; t < tris; ++t) faces[root(id[t])].push_back({int(g.triVerts[t * 3]), int(g.triVerts[t * 3 + 1]), int(g.triVerts[t * 3 + 2])});
    for (const auto& [id, ts] : faces) {
        std::set<std::pair<int, int>> inside;
        for (const auto& t : ts)
            for (int k = 0; k < 3; ++k) inside.insert({t[size_t(k)], t[size_t((k + 1) % 3)]});
        std::map<int, int> next;
        bool simple = true;
        for (const auto& [p, q] : inside)
            if (!inside.count({q, p})) simple &= next.emplace(p, q).second; // (an outline edge)
        std::vector<int> loop;
        if (simple && !next.empty()) {
            int at = next.begin()->first;
            do {
                loop.push_back(at);
                auto it = next.find(at);
                if (it == next.end() || loop.size() > next.size()) break;
                at = it->second;
            } while (at != loop.front());
        }
        if (simple && loop.size() == next.size() && loop.size() >= 3) out.faces.push_back(loop);
        else
            for (const auto& t : ts) out.faces.push_back({t[0], t[1], t[2]});
    }
    return out;
}

} // namespace

bool combine(Mesh& a, const Mesh& b, Combine op) {
    if (auto m = combineManifold(a, b, op)) {
        a = std::move(*m);
        return !a.faces.empty();
    }
    Mesh out = combineOnce(a, b, op);
    // Closed going in but not coming out: the rounding went wrong somewhere. Again with the
    // other shape moved a few µm (nothing that shows), and the first that comes out closed kept.
    if (!out.faces.empty() && !sealed(out) && sealed(a) && sealed(b)) {
        static const glm::vec3 nudge[] = {{3e-6f, 5e-6f, 7e-6f}, {-6e-6f, 4e-6f, -3e-6f}, {5e-6f, -7e-6f, 4e-6f}};
        for (const glm::vec3& d : nudge) {
            Mesh moved = b;
            for (glm::vec3& v : moved.verts) v += d;
            if (Mesh again = combineOnce(a, moved, op); !again.faces.empty() && sealed(again)) {
                out = std::move(again);
                break;
            }
        }
    }
    a = std::move(out);
    return !a.faces.empty();
}

} // namespace fry
