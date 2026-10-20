#include "mesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace fry {

namespace {
constexpr float kPi = 3.14159265358979f;

// Removes repeated neighbours from a face (and the last if it's the first again).
void tidyFace(std::vector<int>& f) {
    f.erase(std::unique(f.begin(), f.end()), f.end());
    while (f.size() > 1 && f.front() == f.back()) f.pop_back();
}

// Renumbers points through `to` (old → new, -1: gone) and tidies the faces, dropping any left
// with fewer than three corners.
void remapFaces(Mesh& m, const std::vector<int>& to) {
    std::vector<std::vector<int>> faces;
    for (auto f : m.faces) {
        bool gone = false;
        for (int& v : f) gone |= (v = to[size_t(v)]) < 0;
        tidyFace(f);
        if (!gone && f.size() >= 3) faces.push_back(std::move(f));
    }
    m.faces = std::move(faces);
}
} // namespace

// ── Shapes ──

Mesh makeCube(float size) {
    const float h = size * 0.5f;
    Mesh m;
    for (int i = 0; i < 8; ++i) m.verts.emplace_back(i & 1 ? h : -h, i & 2 ? h : -h, i & 4 ? h : -h);
    m.faces = {{4, 5, 7, 6}, {0, 2, 3, 1}, {1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}};
    return m;
}

Mesh makePlane(float size) {
    const float h = size * 0.5f;
    return {{{-h, -h, 0}, {h, -h, 0}, {h, h, 0}, {-h, h, 0}}, {{0, 1, 2, 3}}};
}

Mesh makeCylinder(int sides, float radius, float height) {
    sides = std::max(sides, 3);
    Mesh m;
    for (int z = 0; z < 2; ++z)
        for (int i = 0; i < sides; ++i) {
            const float a = 2 * kPi * float(i) / float(sides);
            m.verts.emplace_back(radius * std::cos(a), radius * std::sin(a), (z ? 0.5f : -0.5f) * height);
        }
    std::vector<int> top, bottom;
    for (int i = 0; i < sides; ++i) {
        const int j = (i + 1) % sides;
        m.faces.push_back({i, j, sides + j, sides + i});
        top.push_back(sides + i), bottom.push_back(sides - 1 - i);
    }
    m.faces.push_back(top), m.faces.push_back(bottom);
    return m;
}

Mesh makeCone(int sides, float radius, float height) {
    sides = std::max(sides, 3);
    Mesh m;
    for (int i = 0; i < sides; ++i) {
        const float a = 2 * kPi * float(i) / float(sides);
        m.verts.emplace_back(radius * std::cos(a), radius * std::sin(a), -0.5f * height);
    }
    m.verts.emplace_back(0, 0, 0.5f * height);
    std::vector<int> bottom;
    for (int i = 0; i < sides; ++i) m.faces.push_back({i, (i + 1) % sides, sides}), bottom.push_back(sides - 1 - i);
    m.faces.push_back(bottom);
    return m;
}

Mesh makeSphere(int segments, int rings, float radius) {
    segments = std::max(segments, 3), rings = std::max(rings, 2);
    Mesh m;
    m.verts.emplace_back(0, 0, radius); // the top
    for (int k = 1; k < rings; ++k) {
        const float t = kPi * float(k) / float(rings);
        for (int i = 0; i < segments; ++i) {
            const float a = 2 * kPi * float(i) / float(segments);
            m.verts.emplace_back(radius * std::sin(t) * std::cos(a), radius * std::sin(t) * std::sin(a), radius * std::cos(t));
        }
    }
    const int bottom = int(m.verts.size());
    m.verts.emplace_back(0, 0, -radius);
    auto at = [&](int k, int i) { return 1 + (k - 1) * segments + (i % segments); };
    for (int i = 0; i < segments; ++i) {
        m.faces.push_back({0, at(1, i), at(1, i + 1)});
        for (int k = 1; k + 1 < rings; ++k) m.faces.push_back({at(k, i), at(k + 1, i), at(k + 1, i + 1), at(k, i + 1)});
        m.faces.push_back({at(rings - 1, i), bottom, at(rings - 1, i + 1)});
    }
    return m;
}

Mesh makeTorus(int segments, int sides, float radius, float tube) {
    segments = std::max(segments, 3), sides = std::max(sides, 3);
    Mesh m;
    for (int u = 0; u < segments; ++u)
        for (int v = 0; v < sides; ++v) {
            const float a = 2 * kPi * float(u) / float(segments), b = 2 * kPi * float(v) / float(sides);
            const float r = radius + tube * std::cos(b);
            m.verts.emplace_back(r * std::cos(a), r * std::sin(a), tube * std::sin(b));
        }
    auto at = [&](int u, int v) { return (u % segments) * sides + (v % sides); };
    for (int u = 0; u < segments; ++u)
        for (int v = 0; v < sides; ++v) m.faces.push_back({at(u, v), at(u + 1, v), at(u + 1, v + 1), at(u, v + 1)});
    return m;
}

Mesh extrudeOutline(const std::vector<glm::vec3>& outline, const glm::vec3& planeNormal, float thickness) {
    std::vector<glm::vec3> pts;
    for (const glm::vec3& p : outline)
        if (pts.empty() || glm::distance(pts.back(), p) > 1e-5f) pts.push_back(p);
    while (pts.size() > 1 && glm::distance(pts.front(), pts.back()) < 1e-5f) pts.pop_back();
    Mesh m;
    if (pts.size() < 3) return m;
    const glm::vec3 n = glm::normalize(planeNormal);
    Mesh flat{pts, {std::vector<int>(pts.size())}};
    std::iota(flat.faces[0].begin(), flat.faces[0].end(), 0);
    if (glm::dot(faceNormal(flat, 0), n) < 0.0f) std::reverse(pts.begin(), pts.end()); // counter-clockwise round n
    const int c = int(pts.size());
    const glm::vec3 half = n * (thickness * 0.5f);
    for (const glm::vec3& p : pts) m.verts.push_back(p + half);
    for (const glm::vec3& p : pts) m.verts.push_back(p - half);
    std::vector<int> front(static_cast<size_t>(c)), back(static_cast<size_t>(c));
    for (int i = 0; i < c; ++i) front[size_t(i)] = i, back[size_t(i)] = c + (c - 1 - i);
    m.faces.push_back(front);
    if (thickness > 0.0f) {
        m.faces.push_back(back);
        for (int i = 0; i < c; ++i) {
            const int j = (i + 1) % c;
            m.faces.push_back({c + i, c + j, j, i});
        }
    } else {
        m.verts.resize(size_t(c)); // (no thickness: just the flat shape)
    }
    return m;
}

// ── Looking at it ──

glm::vec3 faceNormal(const Mesh& m, int face) {
    const auto& f = m.faces[size_t(face)];
    glm::vec3 n(0.0f);
    for (size_t i = 0; i < f.size(); ++i) {
        const glm::vec3 &a = m.verts[size_t(f[i])], &b = m.verts[size_t(f[(i + 1) % f.size()])];
        n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    const float l = glm::length(n);
    return l > 1e-12f ? n / l : glm::vec3(0, 0, 1);
}

glm::vec3 faceCenter(const Mesh& m, int face) {
    glm::vec3 c(0.0f);
    for (int v : m.faces[size_t(face)]) c += m.verts[size_t(v)];
    return c / float(std::max<size_t>(m.faces[size_t(face)].size(), 1));
}

float faceArea(const Mesh& m, int face) {
    const auto& f = m.faces[size_t(face)];
    glm::vec3 n(0.0f);
    for (size_t i = 0; i < f.size(); ++i) n += glm::cross(m.verts[size_t(f[i])], m.verts[size_t(f[(i + 1) % f.size()])]);
    return glm::length(n) * 0.5f;
}

std::vector<std::array<int, 3>> triangulate(const Mesh& m, int face) {
    const auto& f = m.faces[size_t(face)];
    std::vector<std::array<int, 3>> out;
    if (f.size() < 3) return out;
    if (f.size() == 3) return {{f[0], f[1], f[2]}};
    // Onto the face's plane (u × v = its normal, so counter-clockwise stays so), then cut off
    // ears: corners that bulge out with nothing of the face inside them.
    const glm::vec3 n = faceNormal(m, face);
    const glm::vec3 u = glm::normalize(std::abs(n.z) < 0.9f ? glm::cross(glm::vec3(0, 0, 1), n) : glm::cross(glm::vec3(1, 0, 0), n));
    const glm::vec3 v = glm::cross(n, u);
    std::vector<glm::vec2> p;
    for (int i : f) p.emplace_back(glm::dot(m.verts[size_t(i)], u), glm::dot(m.verts[size_t(i)], v));
    // (the tests go by the face's own size: a letter 5 mm tall the same as a wall 5 m wide)
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (const glm::vec2& q : p) lo = glm::min(lo, q), hi = glm::max(hi, q);
    const float span = std::max(hi.x - lo.x, hi.y - lo.y), eps = std::max(span * span * 1e-9f, 1e-30f);
    auto cross2 = [](glm::vec2 a, glm::vec2 b, glm::vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
    std::vector<int> left(f.size());
    std::iota(left.begin(), left.end(), 0);
    while (left.size() > 3) {
        bool cut = false;
        for (size_t i = 0; i < left.size() && !cut; ++i) {
            const int a = left[(i + left.size() - 1) % left.size()], b = left[i], c = left[(i + 1) % left.size()];
            const glm::vec2 pa = p[size_t(a)], pb = p[size_t(b)], pc = p[size_t(c)];
            if (cross2(pa, pb, pc) <= eps) continue; // dents in, or flat
            bool inside = false;
            for (int o : left) {
                if (o == a || o == b || o == c) continue;
                const glm::vec2 q = p[size_t(o)];
                if (q == pa || q == pb || q == pc) continue; // (the same place: where a hole's cut joins)
                // (strictly inside, or on the new edge a-c: a point on the outline's own edges a-b, b-c is no harm)
                const float ab = cross2(pa, pb, q), bc = cross2(pb, pc, q), ca = cross2(pc, pa, q);
                if (ab > eps && bc > eps && ca >= -eps) {
                    inside = true;
                    break;
                }
            }
            if (inside) continue;
            out.push_back({f[size_t(a)], f[size_t(b)], f[size_t(c)]});
            left.erase(left.begin() + std::ptrdiff_t(i));
            cut = true;
        }
        if (!cut) // (no ear: a point in a straight line between its neighbours goes, it covers nothing)
            for (size_t i = 0; i < left.size() && !cut; ++i) {
                const int a = left[(i + left.size() - 1) % left.size()], b = left[i], c = left[(i + 1) % left.size()];
                if (std::abs(cross2(p[size_t(a)], p[size_t(b)], p[size_t(c)])) <= eps) left.erase(left.begin() + std::ptrdiff_t(i)), cut = true;
            }
        if (!cut) { // (a tangled face: a fan of what's left)
            for (size_t i = 1; i + 1 < left.size(); ++i) out.push_back({f[size_t(left[0])], f[size_t(left[i])], f[size_t(left[i + 1])]});
            return out;
        }
    }
    out.push_back({f[size_t(left[0])], f[size_t(left[1])], f[size_t(left[2])]});
    return out;
}

std::vector<std::pair<int, int>> edges(const Mesh& m) {
    std::set<std::pair<int, int>> s;
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) {
            const int a = f[i], b = f[(i + 1) % f.size()];
            s.insert({std::min(a, b), std::max(a, b)});
        }
    return {s.begin(), s.end()};
}

std::vector<std::vector<glm::vec3>> cornerNormals(const Mesh& m, bool smooth, float angle) {
    std::vector<glm::vec3> fn(m.faces.size());
    std::vector<float> area(m.faces.size());
    for (size_t f = 0; f < m.faces.size(); ++f) fn[f] = faceNormal(m, int(f)), area[f] = faceArea(m, int(f));
    std::vector<std::vector<glm::vec3>> out(m.faces.size());
    std::vector<std::vector<int>> round(m.verts.size());
    if (smooth)
        for (size_t f = 0; f < m.faces.size(); ++f)
            for (int v : m.faces[f]) round[size_t(v)].push_back(int(f));
    const float minDot = std::cos(glm::radians(angle));
    for (size_t f = 0; f < m.faces.size(); ++f)
        for (int v : m.faces[f]) {
            glm::vec3 n = fn[f];
            if (smooth) {
                n = glm::vec3(0.0f);
                for (int g : round[size_t(v)])
                    if (glm::dot(fn[f], fn[size_t(g)]) >= minDot - 1e-4f) n += fn[size_t(g)] * std::max(area[size_t(g)], 1e-8f);
                n = glm::length(n) > 1e-12f ? glm::normalize(n) : fn[f];
            }
            out[f].push_back(n);
        }
    return out;
}

glm::vec3 boundsMin(const Mesh& m) {
    glm::vec3 lo(1e30f);
    for (const glm::vec3& v : m.verts) lo = glm::min(lo, v);
    return m.verts.empty() ? glm::vec3(0.0f) : lo;
}

glm::vec3 boundsMax(const Mesh& m) {
    glm::vec3 hi(-1e30f);
    for (const glm::vec3& v : m.verts) hi = glm::max(hi, v);
    return m.verts.empty() ? glm::vec3(0.0f) : hi;
}

// ── Changing it ──

std::vector<int> extrudeFaces(Mesh& m, const std::set<int>& faces, float distance) {
    std::set<std::pair<int, int>> inSel;
    std::map<int, glm::vec3> along; // each point of the piece: which way it moves
    glm::vec3 region(0.0f);
    for (int f : faces) {
        const auto& fv = m.faces[size_t(f)];
        const glm::vec3 n = faceNormal(m, f);
        region += n * std::max(faceArea(m, f), 1e-8f);
        for (size_t i = 0; i < fv.size(); ++i) {
            inSel.insert({fv[i], fv[(i + 1) % fv.size()]});
            along[fv[i]] += n;
        }
    }
    // One flat piece moves as one, along its normal; a bent one, each point along its own.
    region = glm::length(region) > 1e-12f ? glm::normalize(region) : glm::vec3(0, 0, 1);
    bool flat = true;
    for (int f : faces) flat &= glm::dot(faceNormal(m, f), region) > 0.99f;
    std::map<int, int> moved;
    std::vector<int> made;
    for (auto& [v, n] : along) {
        const glm::vec3 dir = flat ? region : glm::normalize(n);
        moved[v] = int(m.verts.size());
        made.push_back(moved[v]);
        m.verts.push_back(m.verts[size_t(v)] + dir * distance);
    }
    for (int f : faces)
        for (int& v : m.faces[size_t(f)]) v = moved[v];
    for (auto [a, b] : inSel) // the piece's outline: a wall under each of its edges
        if (!inSel.count({b, a})) m.faces.push_back({a, b, moved[b], moved[a]});
    return made;
}

void insetFaces(Mesh& m, const std::set<int>& faces, float amount) {
    for (int f : faces) {
        std::vector<int> outer = m.faces[size_t(f)];
        const glm::vec3 c = faceCenter(m, f);
        std::vector<int> inner;
        for (int v : outer) {
            inner.push_back(int(m.verts.size()));
            m.verts.push_back(glm::mix(m.verts[size_t(v)], c, amount));
        }
        m.faces[size_t(f)] = inner;
        for (size_t i = 0; i < outer.size(); ++i) {
            const size_t j = (i + 1) % outer.size();
            m.faces.push_back({outer[i], outer[j], inner[j], inner[i]});
        }
    }
}

void flipFaces(Mesh& m, const std::set<int>& faces) {
    for (int f : faces) std::reverse(m.faces[size_t(f)].begin(), m.faces[size_t(f)].end());
}

void deleteFaces(Mesh& m, const std::set<int>& faces) {
    std::vector<std::vector<int>> keep;
    for (size_t f = 0; f < m.faces.size(); ++f)
        if (!faces.count(int(f))) keep.push_back(m.faces[f]);
    m.faces = std::move(keep);
    removeUnused(m);
}

void deleteVerts(Mesh& m, const std::set<int>& verts) {
    std::vector<int> to(m.verts.size());
    std::vector<glm::vec3> kept;
    for (size_t v = 0; v < m.verts.size(); ++v) {
        to[v] = verts.count(int(v)) ? -1 : int(kept.size());
        if (to[v] >= 0) kept.push_back(m.verts[v]);
    }
    remapFaces(m, to);
    m.verts = std::move(kept);
}

bool makeFace(Mesh& m, const std::vector<int>& verts, const glm::vec3& towards) {
    if (verts.size() < 3) return false;
    // Round their middle, in order, on the plane they lie on.
    glm::vec3 c(0.0f);
    for (int v : verts) c += m.verts[size_t(v)];
    c /= float(verts.size());
    glm::vec3 n(0.0f);
    for (size_t i = 0; i < verts.size(); ++i)
        for (size_t j = i + 1; j < verts.size(); ++j) {
            glm::vec3 k = glm::cross(m.verts[size_t(verts[i])] - c, m.verts[size_t(verts[j])] - c);
            if (glm::dot(k, n) < 0.0f) k = -k;
            n += k;
        }
    if (glm::length(n) < 1e-12f) return false; // (all in a line)
    n = glm::normalize(n);
    if (glm::dot(n, towards) < 0.0f) n = -n;
    const glm::vec3 u = glm::normalize(std::abs(n.z) < 0.9f ? glm::cross(glm::vec3(0, 0, 1), n) : glm::cross(glm::vec3(1, 0, 0), n));
    const glm::vec3 w = glm::cross(n, u);
    std::vector<int> f = verts;
    std::sort(f.begin(), f.end(), [&](int a, int b) {
        const glm::vec3 pa = m.verts[size_t(a)] - c, pb = m.verts[size_t(b)] - c;
        return std::atan2(glm::dot(pa, w), glm::dot(pa, u)) < std::atan2(glm::dot(pb, w), glm::dot(pb, u));
    });
    m.faces.push_back(f);
    return true;
}

int mergeByDistance(Mesh& m, float distance) {
    std::vector<int> to(m.verts.size(), -1);
    std::vector<glm::vec3> kept;
    for (size_t v = 0; v < m.verts.size(); ++v) {
        for (size_t k = 0; k < kept.size(); ++k)
            if (glm::distance(kept[k], m.verts[v]) <= distance) {
                to[v] = int(k);
                break;
            }
        if (to[v] < 0) to[v] = int(kept.size()), kept.push_back(m.verts[v]);
    }
    const int gone = int(m.verts.size() - kept.size());
    remapFaces(m, to);
    m.verts = std::move(kept);
    return gone;
}

void mergeToCenter(Mesh& m, const std::set<int>& verts) {
    if (verts.size() < 2) return;
    glm::vec3 c(0.0f);
    for (int v : verts) c += m.verts[size_t(v)];
    c /= float(verts.size());
    std::vector<int> to(m.verts.size());
    std::iota(to.begin(), to.end(), 0);
    const int first = *verts.begin();
    m.verts[size_t(first)] = c;
    for (int v : verts) to[size_t(v)] = first;
    remapFaces(m, to);
    removeUnused(m);
}

void removeUnused(Mesh& m) {
    std::vector<bool> used(m.verts.size());
    for (const auto& f : m.faces)
        for (int v : f) used[size_t(v)] = true;
    std::vector<int> to(m.verts.size(), -1);
    std::vector<glm::vec3> kept;
    for (size_t v = 0; v < m.verts.size(); ++v)
        if (used[v]) to[v] = int(kept.size()), kept.push_back(m.verts[v]);
    remapFaces(m, to);
    m.verts = std::move(kept);
}

void transform(Mesh& m, const glm::mat4& matrix) {
    for (glm::vec3& v : m.verts) v = glm::vec3(matrix * glm::vec4(v, 1.0f));
    if (glm::determinant(glm::mat3(matrix)) < 0.0f) // (mirrored: keep the faces facing out)
        for (auto& f : m.faces) std::reverse(f.begin(), f.end());
}

void append(Mesh& m, const Mesh& other, const glm::mat4& matrix) {
    const int base = int(m.verts.size());
    for (const glm::vec3& v : other.verts) m.verts.push_back(glm::vec3(matrix * glm::vec4(v, 1.0f)));
    const bool flip = glm::determinant(glm::mat3(matrix)) < 0.0f;
    for (auto f : other.faces) {
        for (int& v : f) v += base;
        if (flip) std::reverse(f.begin(), f.end());
        m.faces.push_back(std::move(f));
    }
}

Mesh makeFrustum(int sides, float bottom, float top, float height) {
    if (top <= 1e-6f) return makeCone(sides, bottom, height);
    Mesh m = makeCylinder(sides, bottom, height);
    for (glm::vec3& v : m.verts)
        if (v.z > 0) v.x *= top / bottom, v.y *= top / bottom;
    return m;
}

} // namespace fry
