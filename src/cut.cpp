// The knife, pulling pieces apart, and holes.
#include "mesh.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace fry {
namespace {

using Edge = std::pair<int, int>;
Edge key(int a, int b) { return {std::min(a, b), std::max(a, b)}; }

std::optional<float> rayTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-14f) return std::nullopt;
    const glm::vec3 s = o - a;
    const float u = glm::dot(s, p) / det;
    if (u < 0 || u > 1) return std::nullopt;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(d, q) / det;
    if (v < 0 || u + v > 1) return std::nullopt;
    const float t = glm::dot(e2, q) / det;
    return t > 0 ? std::optional<float>(t) : std::nullopt;
}

float sizeOf(const Mesh& m) {
    const glm::vec3 d = boundsMax(m) - boundsMin(m);
    return std::max({d.x, d.y, d.z, 1e-6f});
}

// Puts point `p` on every face's edge (a, b), between them.
void putOnEdge(Mesh& m, int a, int b, int p) {
    for (auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) {
            const int u = f[i], v = f[(i + 1) % f.size()];
            if ((u == a && v == b) || (u == b && v == a)) {
                f.insert(f.begin() + long(i) + 1, p);
                break;
            }
        }
}

// Two faces from one, cut between two of its corners (false if they're next to each other).
bool splitFace(Mesh& m, int face, int p, int q, int* other = nullptr) {
    std::vector<int> f = m.faces[size_t(face)];
    auto i = std::find(f.begin(), f.end(), p) - f.begin(), j = std::find(f.begin(), f.end(), q) - f.begin();
    const long n = long(f.size());
    if (i == n || j == n || i == j) return false;
    if (i > j) std::swap(i, j);
    if (j - i < 2 || (i == 0 && j == n - 1)) return false;
    std::vector<int> a(f.begin() + i, f.begin() + j + 1), b(f.begin() + j, f.end());
    b.insert(b.end(), f.begin(), f.begin() + i + 1);
    m.faces[size_t(face)] = a;
    m.faces.push_back(b);
    if (other) *other = int(m.faces.size()) - 1;
    return true;
}

} // namespace

std::set<Edge> knifeCut(Mesh& m, const glm::vec3& eye, const std::vector<glm::vec3>& path, bool through) {
    std::set<Edge> made;
    const float eps = sizeOf(m) * 1e-5f;
    for (size_t s = 0; s + 1 < path.size(); ++s) {
        const glm::vec3 da = path[s] - eye, db = path[s + 1] - eye;
        glm::vec3 n = glm::cross(da, db);
        if (glm::length(n) < 1e-12f) continue;
        n = glm::normalize(n);
        // Between the step's two rays: both of these >= 0 (linear along a line, so easy to clip).
        auto side1 = [&](const glm::vec3& x) { return glm::dot(glm::cross(da, x - eye), n); };
        auto side2 = [&](const glm::vec3& x) { return glm::dot(glm::cross(x - eye, db), n); };
        struct Tri {
            glm::vec3 a, b, c;
            int face;
        };
        std::vector<Tri> tris;
        if (!through)
            for (size_t f = 0; f < m.faces.size(); ++f)
                for (const auto& t : triangulate(m, int(f))) tris.push_back({m.verts[size_t(t[0])], m.verts[size_t(t[1])], m.verts[size_t(t[2])], int(f)});
        struct Crossing {
            int vert = -1, a = -1, b = -1;
            glm::vec3 p{0.0f};
        };
        struct Cut {
            int face;
            Crossing c[2];
        };
        std::vector<Cut> cuts;
        for (size_t f = 0; f < m.faces.size(); ++f) {
            const auto& fv = m.faces[f];
            std::vector<float> d(fv.size());
            for (size_t k = 0; k < fv.size(); ++k) d[k] = glm::dot(n, m.verts[size_t(fv[k])] - eye);
            std::vector<Crossing> xs;
            for (size_t k = 0; k < fv.size(); ++k) {
                const size_t k1 = (k + 1) % fv.size();
                if (std::abs(d[k]) <= eps) xs.push_back({fv[k], -1, -1, m.verts[size_t(fv[k])]});
                else if (std::abs(d[k1]) > eps && (d[k] > 0) != (d[k1] > 0)) {
                    const float t = d[k] / (d[k] - d[k1]);
                    xs.push_back({-1, fv[k], fv[k1], glm::mix(m.verts[size_t(fv[k])], m.verts[size_t(fv[k1])], t)});
                }
            }
            if (xs.size() != 2) continue; // (misses it, touches a corner, lies in it, or a bent face)
            // The part of the chord inside the step's wedge.
            float t0 = 0, t1 = 1;
            for (auto sideFn : {0, 1}) {
                const float g0 = sideFn ? side2(xs[0].p) : side1(xs[0].p), g1 = sideFn ? side2(xs[1].p) : side1(xs[1].p);
                if (std::abs(g1 - g0) < 1e-12f) {
                    if (g0 < 0) t0 = 1, t1 = 0;
                    continue;
                }
                const float at = -g0 / (g1 - g0);
                if (g1 > g0) t0 = std::max(t0, at);
                else t1 = std::min(t1, at);
            }
            if (t1 - t0 < 1e-5f) continue;
            if (!through) {
                const glm::vec3 c = faceCenter(m, int(f));
                if (glm::dot(faceNormal(m, int(f)), eye - c) <= 0) continue;
                const glm::vec3 mid = glm::mix(xs[0].p, xs[1].p, (t0 + t1) * 0.5f), dir = mid - eye;
                const float len = glm::length(dir);
                bool hidden = false;
                for (const Tri& t : tris)
                    if (t.face != int(f))
                        if (auto h = rayTriangle(eye, dir / len, t.a, t.b, t.c); h && *h < len - eps * 10) {
                            hidden = true;
                            break;
                        }
                if (hidden) continue;
            }
            cuts.push_back({int(f), {xs[0], xs[1]}});
        }
        // New points on the edges crossed (shared with the faces either side), then the splits.
        std::map<Edge, int> onEdge;
        for (Cut& c : cuts)
            for (Crossing& x : c.c)
                if (x.vert < 0) {
                    auto [it, fresh] = onEdge.try_emplace(key(x.a, x.b), int(m.verts.size()));
                    if (fresh) {
                        m.verts.push_back(x.p);
                        putOnEdge(m, x.a, x.b, it->second);
                    }
                    x.vert = it->second;
                }
        for (const Cut& c : cuts)
            if (splitFace(m, c.face, c.c[0].vert, c.c[1].vert)) made.insert(key(c.c[0].vert, c.c[1].vert));
            else if (std::abs(c.c[0].vert - c.c[1].vert) > 0) {
                // Already an edge of the face (it ran along one): still part of the cut.
                const auto& f = m.faces[size_t(c.face)];
                for (size_t i = 0; i < f.size(); ++i)
                    if (key(f[i], f[(i + 1) % f.size()]) == key(c.c[0].vert, c.c[1].vert)) made.insert(key(c.c[0].vert, c.c[1].vert));
            }
    }
    return made;
}

std::vector<std::set<int>> splitAlong(Mesh& m, const std::set<Edge>& cut, bool fill) {
    std::map<Edge, std::vector<int>> byEdge;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) byEdge[key(fv[i], fv[(i + 1) % fv.size()])].push_back(int(f));
    }
    // The pieces: faces reached from each other without crossing the cut.
    std::vector<int> piece(m.faces.size(), -1);
    int pieces = 0;
    for (size_t start = 0; start < m.faces.size(); ++start) {
        if (piece[start] >= 0) continue;
        std::vector<int> todo{int(start)};
        piece[start] = pieces;
        while (!todo.empty()) {
            const int f = todo.back();
            todo.pop_back();
            const auto& fv = m.faces[size_t(f)];
            for (size_t i = 0; i < fv.size(); ++i) {
                const Edge e = key(fv[i], fv[(i + 1) % fv.size()]);
                if (cut.count(e)) continue;
                for (int g : byEdge[e])
                    if (piece[size_t(g)] < 0) piece[size_t(g)] = pieces, todo.push_back(g);
            }
        }
        ++pieces;
    }
    std::vector<std::set<int>> out(static_cast<size_t>(pieces));
    for (size_t f = 0; f < m.faces.size(); ++f) out[size_t(piece[f])].insert(int(f));
    if (pieces < 2) return out;
    // Points shared between pieces: one copy each.
    std::vector<int> owner(m.verts.size(), -1), original;
    for (size_t v = 0; v < m.verts.size(); ++v) original.push_back(int(v));
    std::map<std::pair<int, int>, int> copyOf; // (point, piece) → its copy
    for (size_t f = 0; f < m.faces.size(); ++f)
        for (int& v : m.faces[f]) {
            const int p = piece[f];
            if (owner[size_t(v)] < 0) owner[size_t(v)] = p;
            if (owner[size_t(v)] == p) continue;
            auto [it, fresh] = copyOf.try_emplace({v, p}, int(m.verts.size()));
            if (fresh) m.verts.push_back(m.verts[size_t(v)]), original.push_back(v);
            v = it->second;
        }
    if (!fill) return out;
    // Close each piece's openings along the cut.
    for (int p = 0; p < pieces; ++p) {
        std::map<Edge, int> uses;
        for (int f : out[size_t(p)]) {
            const auto& fv = m.faces[size_t(f)];
            for (size_t i = 0; i < fv.size(); ++i) ++uses[key(fv[i], fv[(i + 1) % fv.size()])];
        }
        std::multimap<int, int> next; // the cap's edges: each open edge the other way round
        for (int f : out[size_t(p)]) {
            const auto& fv = m.faces[size_t(f)];
            for (size_t i = 0; i < fv.size(); ++i) {
                const int a = fv[i], b = fv[(i + 1) % fv.size()];
                if (uses[key(a, b)] == 1 && cut.count(key(original[size_t(a)], original[size_t(b)]))) next.insert({b, a});
            }
        }
        while (!next.empty()) {
            std::vector<int> loop{next.begin()->first};
            int at = next.begin()->second;
            next.erase(next.begin());
            while (at != loop.front()) {
                loop.push_back(at);
                auto it = next.find(at);
                if (it == next.end()) break;
                at = it->second;
                next.erase(it);
            }
            if (at == loop.front() && loop.size() >= 3) {
                m.faces.push_back(loop);
                out[size_t(p)].insert(int(m.faces.size()) - 1);
            }
        }
    }
    return out;
}

float volumeOf(const Mesh& m, const std::set<int>& faces) {
    float v = 0;
    for (int f : faces)
        for (const auto& t : triangulate(m, f)) v += glm::dot(m.verts[size_t(t[0])], glm::cross(m.verts[size_t(t[1])], m.verts[size_t(t[2])])) / 6.0f;
    return std::abs(v);
}

std::vector<glm::vec3> holeOutline(const Mesh& m, int face, const glm::vec3& centre, HoleShape shape, float across, float tall, int sides, float turn) {
    const auto& fv = m.faces[size_t(face)];
    const glm::vec3 n = faceNormal(m, face);
    glm::vec3 u(0.0f);
    for (size_t i = 0; i < fv.size(); ++i) {
        const glm::vec3 d = m.verts[size_t(fv[(i + 1) % fv.size()])] - m.verts[size_t(fv[i])];
        if (glm::length(d) > glm::length(u)) u = d;
    }
    u -= n * glm::dot(u, n);
    if (glm::length(u) < 1e-9f) u = glm::cross(n, std::abs(n.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
    u = glm::normalize(u);
    const float a0 = glm::radians(turn);
    const glm::vec3 v0 = glm::cross(n, u), ut = u * std::cos(a0) + v0 * std::sin(a0), vt = glm::cross(n, ut);
    const glm::vec3 c = centre - n * glm::dot(centre - faceCenter(m, face), n);
    std::vector<glm::vec3> out;
    auto ring = [&](int count, float r, float start) { // (counter-clockwise about the normal)
        for (int k = 0; k < count; ++k) {
            const float a = start + glm::two_pi<float>() * float(k) / float(count);
            out.push_back(c + (ut * std::cos(a) + vt * std::sin(a)) * r);
        }
    };
    switch (shape) {
    case HoleShape::Round: ring(std::max(sides, 3), across * 0.5f, 0.0f); break;
    case HoleShape::Rectangle: {
        const float hw = across * 0.5f, hh = (tall > 0 ? tall : across) * 0.5f;
        for (glm::vec2 k : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)}) out.push_back(c + ut * (hw * k.x) + vt * (hh * k.y));
        break;
    }
    case HoleShape::Triangle: ring(3, across / std::sqrt(3.0f), glm::half_pi<float>()); break;
    case HoleShape::Hexagon: ring(6, across / std::sqrt(3.0f), 0.0f); break; // (corners left and right: flats top and bottom)
    }
    return out;
}

std::vector<glm::vec3> holeOutline(const Mesh& m, int face, const glm::vec3& centre, float radius, int sides) {
    if (sides == 4) return holeOutline(m, face, centre, HoleShape::Rectangle, radius * 2.0f, radius * 2.0f);
    return holeOutline(m, face, centre, HoleShape::Round, radius * 2.0f, 0.0f, sides);
}

namespace {
// Sets a loop of new points (counter-clockwise about the face's normal, inside it) into a face:
// the face becomes a ring of faces round the loop. Returns the loop's points, or nothing if it
// doesn't fit (nothing changed).
std::optional<std::vector<int>> setIntoFace(Mesh& m, int face, const std::vector<glm::vec3>& loop) {
    const std::vector<int> fv = m.faces[size_t(face)];
    const glm::vec3 n = faceNormal(m, face);
    const glm::vec3 u = glm::normalize(std::abs(n.z) < 0.9f ? glm::cross(glm::vec3(0, 0, 1), n) : glm::cross(glm::vec3(1, 0, 0), n)), v = glm::cross(n, u);
    glm::vec3 c(0.0f);
    for (const glm::vec3& p : loop) c += p;
    c /= float(loop.size());
    auto flat = [&](const glm::vec3& p) { return glm::vec2(glm::dot(p - c, u), glm::dot(p - c, v)); };
    std::vector<glm::vec2> outer;
    for (int i : fv) outer.push_back(flat(m.verts[size_t(i)]));
    auto inside = [&](glm::vec2 p) {
        bool in = false;
        for (size_t i = 0, j = outer.size() - 1; i < outer.size(); j = i++)
            if ((outer[i].y > p.y) != (outer[j].y > p.y) && p.x < (outer[j].x - outer[i].x) * (p.y - outer[i].y) / (outer[j].y - outer[i].y) + outer[i].x) in = !in;
        return in;
    };
    for (const glm::vec3& p : loop)
        if (!inside(flat(p))) return std::nullopt;
    // The face's corners must go round the middle in order (so each can be joined to the loop).
    auto angle = [](glm::vec2 p) { return std::atan2(p.y, p.x); };
    // Angles round the middle, each further on than the last (a face that doubles back goes
    // more than once round, and doesn't fit).
    auto unwrap = [&](const std::vector<glm::vec2>& pts, float near) {
        std::vector<float> a;
        for (const glm::vec2& p : pts) {
            float x = angle(p);
            if (a.empty()) {
                while (x < near - glm::pi<float>()) x += glm::two_pi<float>();
                while (x >= near + glm::pi<float>()) x -= glm::two_pi<float>();
            } else {
                while (x <= a.back()) x += glm::two_pi<float>();
                while (x > a.back() + glm::two_pi<float>()) x -= glm::two_pi<float>();
            }
            a.push_back(x);
        }
        return a;
    };
    const std::vector<float> ao = unwrap(outer, angle(outer[0]));
    if (ao.back() - ao.front() >= glm::two_pi<float>()) return std::nullopt;
    std::vector<glm::vec2> inner;
    for (const glm::vec3& p : loop) inner.push_back(flat(p));
    const std::vector<float> ai = unwrap(inner, ao[0]);
    if (ai.back() - ai.front() >= glm::two_pi<float>()) return std::nullopt;
    // Each corner of the face joins the loop point nearest its way round.
    const int N = int(loop.size()), M = int(fv.size());
    auto loopAngle = [&](int k) { // (k may go past either end: whole turns)
        const int q = ((k % N) + N) % N, turns = (k - q) / N;
        return ai[size_t(q)] + glm::two_pi<float>() * float(turns);
    };
    std::vector<int> at(size_t(M) + 1);
    int k = -N;
    for (int i = 0; i < M; ++i) {
        while (std::abs(loopAngle(k + 1) - ao[size_t(i)]) <= std::abs(loopAngle(k) - ao[size_t(i)])) ++k;
        at[size_t(i)] = k;
    }
    at[size_t(M)] = at[0] + N;
    const int base = int(m.verts.size());
    for (const glm::vec3& p : loop) m.verts.push_back(p);
    auto L = [&](int k) { return base + ((k % N) + N) % N; };
    bool first = true;
    for (int i = 0; i < M; ++i) {
        std::vector<int> f{fv[size_t(i)], fv[size_t((i + 1) % M)]};
        for (int q = at[size_t(i) + 1]; q >= at[size_t(i)]; --q) f.push_back(L(q));
        if (first) m.faces[size_t(face)] = f, first = false;
        else m.faces.push_back(f);
    }
    std::vector<int> out;
    for (int q = 0; q < N; ++q) out.push_back(base + q);
    return out;
}
} // namespace

int imprintOutline(Mesh& m, int face, const std::vector<glm::vec3>& outline) {
    if (face < 0 || face >= int(m.faces.size()) || outline.size() < 3) return -1;
    auto inner = setIntoFace(m, face, outline);
    if (!inner) return -1;
    m.faces.push_back(*inner);
    return int(m.faces.size()) - 1;
}

bool cutHole(Mesh& m, int face, const std::vector<glm::vec3>& outline, float depth) {
    if (face < 0 || face >= int(m.faces.size()) || outline.size() < 3) return false;
    const Mesh before = m;
    const glm::vec3 n = faceNormal(m, face);
    glm::vec3 c(0.0f);
    for (const glm::vec3& p : outline) c += p;
    c /= float(outline.size());
    // The face behind, found before anything changes.
    int behind = -1;
    float tBehind = 1e30f;
    if (depth <= 0)
        for (size_t f = 0; f < m.faces.size(); ++f) {
            if (int(f) == face) continue;
            for (const auto& t : triangulate(m, int(f)))
                if (auto h = rayTriangle(c, -n, m.verts[size_t(t[0])], m.verts[size_t(t[1])], m.verts[size_t(t[2])]); h && *h > 1e-6f && *h < tBehind) tBehind = *h, behind = int(f);
        }
    if (depth <= 0 && behind < 0) return false;
    auto front = setIntoFace(m, face, outline);
    if (!front) return false;
    if (depth > 0) {
        m.faces.push_back(*front);
        extrudeFaces(m, {int(m.faces.size()) - 1}, -depth);
        return true;
    }
    // Through: the same outline, carried straight back onto the face behind.
    const glm::vec3 nb = faceNormal(m, behind), pb = faceCenter(m, behind);
    if (std::abs(glm::dot(nb, n)) < 1e-4f) return m = before, false;
    std::vector<glm::vec3> back;
    for (auto it = outline.rbegin(); it != outline.rend(); ++it) back.push_back(*it - n * (glm::dot(nb, pb - *it) / glm::dot(nb, -n)));
    auto rear = setIntoFace(m, behind, back);
    if (!rear) return m = before, false;
    const int N = int(outline.size());
    for (int k = 0; k < N; ++k) {
        const int k1 = (k + 1) % N;
        const int bk = (*rear)[size_t(N - 1 - k)], bk1 = (*rear)[size_t(N - 1 - k1)];
        m.faces.push_back({(*front)[size_t(k)], (*front)[size_t(k1)], bk1, bk});
    }
    return true;
}

} // namespace fry
