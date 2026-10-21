// The bigger mesh tools: rounding edges, smoothing, loop cuts, and finding loops and rings.
#include "mesh.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace fry {

namespace {
using Edge = std::pair<int, int>;
Edge key(int a, int b) { return a < b ? Edge{a, b} : Edge{b, a}; }

void tidy(std::vector<int>& f) {
    f.erase(std::unique(f.begin(), f.end()), f.end());
    while (f.size() > 1 && f.front() == f.back()) f.pop_back();
}

// Each edge as its faces run along it: (from, to) → (face, the corner it starts at).
std::map<Edge, std::pair<int, int>> directed(const Mesh& m) {
    std::map<Edge, std::pair<int, int>> d;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) d[{fv[i], fv[(i + 1) % fv.size()]}] = {int(f), int(i)};
    }
    return d;
}

glm::vec3 normalOf(const std::vector<glm::vec3>& v, const std::vector<int>& f) { // (Newell's)
    glm::vec3 n(0.0f);
    for (size_t i = 0; i < f.size(); ++i) {
        const glm::vec3 a = v[size_t(f[i])], b = v[size_t(f[(i + 1) % f.size()])];
        n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    return glm::length(n) > 1e-20f ? glm::normalize(n) : n;
}

std::map<int, std::set<int>> neighbours(const Mesh& m) {
    std::map<int, std::set<int>> n;
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) {
            const int a = f[i], b = f[(i + 1) % f.size()];
            n[a].insert(b), n[b].insert(a);
        }
    return n;
}
} // namespace

std::set<std::pair<int, int>> sharpEdges(const Mesh& m, float angle) {
    const auto d = directed(m);
    const float c = std::cos(glm::radians(angle));
    std::set<Edge> out;
    for (const auto& [e, fc] : d) {
        if (e.first > e.second) continue;
        auto twin = d.find({e.second, e.first});
        if (twin != d.end() && glm::dot(faceNormal(m, fc.first), faceNormal(m, twin->second.first)) < c) out.insert(e);
    }
    return out;
}

// ── Bevel ──
//
// Every face corner at a point of a rounded edge is replaced: where both its edges are rounded,
// by a point inside the face `width` from both; where one is, by a point slid along the other
// (shared with the face across it); where neither, by the point itself plus any slid points on
// its edges. Each rounded edge then gets a strip between its two faces' new edges, curved along a
// circle; whatever holes are left round a point (the corners where several strips meet) are
// filled, as part of a sphere when they're rounded corners, flat otherwise.
void bevelEdges(Mesh& m, const std::set<std::pair<int, int>>& want, float width, int segments) {
    if (width <= 0.0f) return;
    segments = std::clamp(segments, 1, 16);
    const auto d = directed(m);
    std::set<Edge> bev;
    for (auto [a, b] : want)
        if (d.count({a, b}) && d.count({b, a})) bev.insert(key(a, b));
    if (bev.empty()) return;
    auto isBev = [&](int a, int b) { return bev.count(key(a, b)) > 0; };
    std::set<int> at;
    for (auto [a, b] : bev) at.insert(a), at.insert(b);
    const int nOrig = int(m.verts.size());
    const std::vector<glm::vec3> orig = m.verts;
    std::map<int, int> owner; // new point → the point it came from
    auto add = [&](glm::vec3 p, int from) {
        owner[int(m.verts.size())] = from;
        m.verts.push_back(p);
        return int(m.verts.size()) - 1;
    };
    auto dirTo = [&](int v, int w) {
        const glm::vec3 x = orig[size_t(w)] - orig[size_t(v)];
        const float l = glm::length(x);
        return l > 1e-12f ? x / l : glm::vec3(0.0f);
    };
    auto sinOf = [](glm::vec3 a, glm::vec3 b) { return std::max(glm::length(glm::cross(a, b)), 0.05f); };

    // Points slid along edges, shared by the faces either side.
    std::map<Edge, std::pair<glm::vec3, int>> slideSum; // (point, towards) → sum, count
    for (const auto& fv : m.faces)
        for (size_t i = 0; i < fv.size(); ++i) {
            const int v = fv[i], prev = fv[(i + fv.size() - 1) % fv.size()], next = fv[(i + 1) % fv.size()];
            if (!at.count(v)) continue;
            const bool in = isBev(prev, v), out = isBev(v, next);
            if (in == out) continue;
            const glm::vec3 a = dirTo(v, prev), b = dirTo(v, next);
            const int along = in ? next : prev;
            const float t = std::min(width / sinOf(a, b), glm::distance(orig[size_t(v)], orig[size_t(along)]) * 0.49f);
            auto& s = slideSum[{v, along}];
            s.first += orig[size_t(v)] + (in ? b : a) * t, ++s.second;
        }
    std::map<Edge, int> slide;
    for (const auto& [k, s] : slideSum) slide[k] = add(s.first / float(s.second), k.first);

    // What each face corner becomes.
    std::vector<std::vector<std::vector<int>>> rep(m.faces.size());
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        rep[f].resize(fv.size());
        for (size_t i = 0; i < fv.size(); ++i) {
            const int v = fv[i], prev = fv[(i + fv.size() - 1) % fv.size()], next = fv[(i + 1) % fv.size()];
            auto& r = rep[f][i];
            if (!at.count(v)) {
                r = {v};
                continue;
            }
            const bool in = isBev(prev, v), out = isBev(v, next);
            if (in && out) {
                const glm::vec3 a = dirTo(v, prev), b = dirTo(v, next);
                const float most = std::min(glm::distance(orig[size_t(v)], orig[size_t(prev)]), glm::distance(orig[size_t(v)], orig[size_t(next)])) * 0.49f;
                glm::vec3 off = (a + b) * (width / sinOf(a, b));
                if (glm::length(off) > most * 1.42f) off *= most * 1.42f / glm::length(off);
                r = {add(orig[size_t(v)] + off, v)};
            } else if (in) r = {slide[{v, next}]};
            else if (out) r = {slide[{v, prev}]};
            else {
                const auto sp = slide.find({v, prev}), sn = slide.find({v, next});
                if (sp != slide.end()) r.push_back(sp->second);
                if (sp == slide.end() || sn == slide.end()) r.push_back(v);
                if (sn != slide.end()) r.push_back(sn->second);
            }
        }
    }
    std::vector<std::vector<int>> faces;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        std::vector<int> nf;
        for (const auto& r : rep[f]) nf.insert(nf.end(), r.begin(), r.end());
        faces.push_back(std::move(nf));
    }

    // The strips, curved: a conic from one side to the other, its middle towards where the old
    // edge was (a true circle when both sides are the same distance from it).
    struct Round {
        glm::vec3 centre;
        float radius;
    };
    std::map<int, std::vector<Round>> rounds; // by original point
    std::map<Edge, std::vector<int>> arcs;
    auto arc = [&](int p, int q, int v, glm::vec3 dir) {
        if (p == q) return std::vector<int>(size_t(segments + 1), p);
        if (auto it = arcs.find({p, q}); it != arcs.end()) return it->second;
        if (auto it = arcs.find({q, p}); it != arcs.end()) return std::vector<int>(it->second.rbegin(), it->second.rend());
        const glm::vec3 A = m.verts[size_t(p)], B = m.verts[size_t(q)], V = orig[size_t(v)];
        const glm::vec3 C = V + dir * std::max(glm::dot(A - V, dir), glm::dot(B - V, dir));
        const glm::vec3 la = A - C, lb = B - C;
        const float lenA = glm::length(la), lenB = glm::length(lb);
        const float cosPhi = lenA > 1e-9f && lenB > 1e-9f ? std::clamp(glm::dot(la, lb) / (lenA * lenB), -1.0f, 1.0f) : -1.0f;
        const float phi = std::acos(cosPhi), w = std::sin(phi * 0.5f);
        std::vector<int> pts{p};
        for (int k = 1; k < segments; ++k) {
            const float t = float(k) / float(segments), b0 = (1 - t) * (1 - t), b1 = 2 * t * (1 - t) * w, b2 = t * t;
            pts.push_back(add((A * b0 + C * b1 + B * b2) / (b0 + b1 + b2), v));
        }
        pts.push_back(q);
        if (segments > 1 && 1.0f + cosPhi > 1e-3f) rounds[v].push_back({C + (la + lb) / (1.0f + cosPhi), (lenA + lenB) * 0.5f * std::tan(phi * 0.5f)});
        arcs[{p, q}] = pts;
        return pts;
    };
    for (auto [a, b] : bev) {
        const auto [f, ia] = d.at({a, b});
        const auto [g, ib] = d.at({b, a});
        const size_t nf = rep[size_t(f)].size(), ng = rep[size_t(g)].size();
        const int Aa = rep[size_t(f)][size_t(ia)].back(), Ab = rep[size_t(f)][(size_t(ia) + 1) % nf].front();
        const int Bb = rep[size_t(g)][size_t(ib)].back(), Ba = rep[size_t(g)][(size_t(ib) + 1) % ng].front();
        const glm::vec3 dir = dirTo(a, b);
        const auto atA = arc(Aa, Ba, a, dir), atB = arc(Ab, Bb, b, -dir);
        for (int k = 0; k < segments; ++k) faces.push_back({atB[size_t(k)], atA[size_t(k)], atA[size_t(k) + 1], atB[size_t(k) + 1]});
    }
    for (auto& f : faces) tidy(f);
    std::erase_if(faces, [](const std::vector<int>& f) { return f.size() < 3; });

    // Fill the holes round the points.
    auto relevant = [&](int x) { return x >= nOrig || at.count(x); };
    std::set<Edge> has;
    for (const auto& f : faces)
        for (size_t i = 0; i < f.size(); ++i) has.insert({f[i], f[(i + 1) % f.size()]});
    std::map<int, int> nextOf;
    std::set<int> unclear;
    for (auto [x, y] : has)
        if (!has.count({y, x}) && relevant(x) && relevant(y)) {
            if (nextOf.count(y)) unclear.insert(y);
            nextOf[y] = x;
        }
    std::set<int> used;
    std::map<Edge, int> faceOfEdge;
    for (size_t f = 0; f < faces.size(); ++f)
        for (size_t i = 0; i < faces[f].size(); ++i) faceOfEdge[{faces[f][i], faces[f][(i + 1) % faces[f].size()]}] = int(f);
    std::set<int> mergedInto;
    for (const auto& [start, unused] : nextOf) {
        if (used.count(start) || unclear.count(start)) continue;
        std::vector<int> loop{start};
        bool ok = false;
        for (int x = nextOf[start], n = 0; n < 4096; ++n) {
            if (x == start) {
                ok = true;
                break;
            }
            if (!nextOf.count(x) || unclear.count(x) || used.count(x)) break;
            loop.push_back(x);
            x = nextOf[x];
        }
        if (!ok || loop.size() < 3) continue;
        used.insert(loop.begin(), loop.end());
        const bool hasOrig = std::any_of(loop.begin(), loop.end(), [&](int x) { return x < nOrig; });
        const int v = hasOrig ? -1 : owner[loop[0]];
        if (!hasOrig && segments > 1 && rounds.count(v)) { // a rounded corner: part of a sphere
            glm::vec3 O(0.0f), c(0.0f);
            float r = 0.0f;
            for (const Round& rd : rounds[v]) O += rd.centre, r += rd.radius;
            O /= float(rounds[v].size()), r /= float(rounds[v].size());
            for (int x : loop) c += m.verts[size_t(x)];
            c /= float(loop.size());
            auto onBall = [&](glm::vec3 p) { return glm::length(p - O) > 1e-9f ? O + glm::normalize(p - O) * r : p; };
            const int rings = std::max(1, segments / 2);
            std::vector<int> prev = loop;
            for (int j = 1; j < rings; ++j) {
                std::vector<int> ring;
                for (int x : loop) ring.push_back(add(onBall(glm::mix(m.verts[size_t(x)], c, float(j) / float(rings))), v));
                for (size_t i = 0; i < loop.size(); ++i) faces.push_back({prev[i], prev[(i + 1) % loop.size()], ring[(i + 1) % loop.size()], ring[i]});
                prev = ring;
            }
            const int mid = add(onBall(c), v);
            for (size_t i = 0; i < loop.size(); ++i) faces.push_back({prev[i], prev[(i + 1) % loop.size()], mid});
            continue;
        }
        // Flat: joined into the face beside it when that's flat with it (one face, not two).
        const glm::vec3 n = normalOf(m.verts, loop);
        bool merged = false;
        for (size_t i = 0; i < loop.size() && !merged; ++i) {
            const int x = loop[i], y = loop[(i + 1) % loop.size()];
            auto it = faceOfEdge.find({y, x});
            if (it == faceOfEdge.end() || mergedInto.count(it->second)) continue;
            auto& g = faces[size_t(it->second)];
            int shared = 0;
            for (size_t k = 0; k < loop.size(); ++k) shared += faceOfEdge.count({loop[(k + 1) % loop.size()], loop[k]}) && faceOfEdge[{loop[(k + 1) % loop.size()], loop[k]}] == it->second;
            if (shared != 1 || glm::dot(normalOf(m.verts, g), n) < 0.999f) continue;
            // g runs y → x; put the loop's way from y round to x between them.
            std::vector<int> path;
            for (size_t k = 1; k + 1 < loop.size(); ++k) path.push_back(loop[(i + 1 + k) % loop.size()]);
            const auto pos = size_t(std::find(g.begin(), g.end(), y) - g.begin());
            g.insert(g.begin() + std::ptrdiff_t(pos + 1), path.begin(), path.end());
            mergedInto.insert(it->second);
            merged = true;
        }
        if (!merged) faces.push_back(loop);
    }
    m.faces = std::move(faces);
    removeUnused(m);
}

// ── Subdivide (Catmull–Clark) ──

void subdivide(Mesh& m, bool smooth, float keepAngle, std::set<std::pair<int, int>>* creases) {
    if (m.faces.empty()) return;
    const auto d = directed(m);
    const std::set<Edge> sharp = keepAngle < 179.9f ? sharpEdges(m, keepAngle) : std::set<Edge>{};
    auto crease = [&](int a, int b) { return !d.count({a, b}) || !d.count({b, a}) || sharp.count(key(a, b)) || (creases && creases->count(key(a, b))); };
    Mesh out;
    out.verts = m.verts;
    std::vector<int> facePt(m.faces.size());
    for (size_t f = 0; f < m.faces.size(); ++f) facePt[f] = int(out.verts.size()), out.verts.push_back(faceCenter(m, int(f)));
    std::map<Edge, int> edgePt;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) {
            const int a = fv[i], b = fv[(i + 1) % fv.size()];
            if (edgePt.count(key(a, b))) continue;
            glm::vec3 p = (m.verts[size_t(a)] + m.verts[size_t(b)]) * 0.5f;
            if (smooth && !crease(a, b))
                p = (m.verts[size_t(a)] + m.verts[size_t(b)] + out.verts[size_t(facePt[size_t(d.at({a, b}).first)])] +
                     out.verts[size_t(facePt[size_t(d.at({b, a}).first)])]) *
                    0.25f;
            edgePt[key(a, b)] = int(out.verts.size());
            out.verts.push_back(p);
        }
    }
    if (smooth) {
        std::vector<glm::vec3> faceSum(m.verts.size(), glm::vec3(0.0f));
        std::vector<int> faceCount(m.verts.size(), 0);
        for (size_t f = 0; f < m.faces.size(); ++f)
            for (int v : m.faces[f]) faceSum[size_t(v)] += out.verts[size_t(facePt[f])], ++faceCount[size_t(v)];
        for (const auto& [v, nb] : neighbours(m)) {
            const glm::vec3 P = m.verts[size_t(v)];
            std::vector<int> creases;
            glm::vec3 mid(0.0f);
            for (int w : nb) {
                mid += (P + m.verts[size_t(w)]) * 0.5f;
                if (crease(v, w)) creases.push_back(w);
            }
            const float n = float(nb.size());
            if (creases.size() >= 3) continue; // a corner: stays
            if (creases.size() == 2) out.verts[size_t(v)] = P * 0.75f + (m.verts[size_t(creases[0])] + m.verts[size_t(creases[1])]) * 0.125f;
            else if (faceCount[size_t(v)] > 0 && n >= 3)
                out.verts[size_t(v)] = (faceSum[size_t(v)] / float(faceCount[size_t(v)]) + 2.0f * mid / n + (n - 3.0f) * P) / n;
        }
    }
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        const size_t n = fv.size();
        for (size_t i = 0; i < n; ++i)
            out.faces.push_back({fv[i], edgePt[key(fv[i], fv[(i + 1) % n])], facePt[f], edgePt[key(fv[(i + n - 1) % n], fv[i])]});
    }
    if (creases) { // (each crease is now two edges, through its new middle point; numbered as removeUnused will)
        std::vector<int> to(out.verts.size(), -1);
        int next = 0;
        std::vector<bool> used(out.verts.size());
        for (const auto& f : out.faces)
            for (int v : f) used[size_t(v)] = true;
        for (size_t v = 0; v < used.size(); ++v)
            if (used[v]) to[v] = next++;
        std::set<std::pair<int, int>> now;
        for (const auto& [a, b] : *creases) {
            const auto it = edgePt.find(key(a, b));
            if (it == edgePt.end()) continue; // (not an edge any more)
            for (const auto& [p, q] : {std::pair{a, it->second}, std::pair{it->second, b}})
                if (to[size_t(p)] >= 0 && to[size_t(q)] >= 0) now.insert(key(to[size_t(p)], to[size_t(q)]));
        }
        *creases = std::move(now);
    }
    m = std::move(out);
    removeUnused(m);
}

// ── Rings and loops ──

namespace {
// The four-sided faces across an edge, each with the corner its way in starts at: the way in is
// corners (i, i+1), the way out (i+3, i+2), so each "first" point is on the same side.
std::vector<std::pair<int, int>> ringSteps(const Mesh& m, Edge e) {
    const auto d = directed(m);
    std::vector<std::pair<int, int>> steps;
    std::set<int> seen;
    auto walk = [&](int a, int b, bool front) {
        std::vector<std::pair<int, int>> got;
        for (;;) {
            auto it = d.find({a, b});
            if (it == d.end()) break;
            const auto [f, i] = it->second;
            const auto& fv = m.faces[size_t(f)];
            if (fv.size() != 4 || seen.count(f)) break;
            seen.insert(f);
            got.push_back({f, i});
            a = fv[(size_t(i) + 3) % 4], b = fv[(size_t(i) + 2) % 4];
        }
        if (front) steps = got;
        else steps.insert(steps.begin(), got.rbegin(), got.rend());
    };
    walk(e.first, e.second, true);
    walk(e.second, e.first, false);
    return steps;
}
} // namespace

std::vector<int> faceRing(const Mesh& m, std::pair<int, int> edge) {
    std::vector<int> out;
    for (auto [f, i] : ringSteps(m, edge)) out.push_back(f);
    return out;
}

std::vector<std::array<int, 4>> ringQuads(const Mesh& m, std::pair<int, int> edge) {
    std::vector<std::array<int, 4>> out;
    for (auto [f, i] : ringSteps(m, edge)) {
        const auto& fv = m.faces[size_t(f)];
        out.push_back({fv[size_t(i)], fv[(size_t(i) + 1) % 4], fv[(size_t(i) + 2) % 4], fv[(size_t(i) + 3) % 4]});
    }
    return out;
}

std::vector<int> loopCut(Mesh& m, std::pair<int, int> edge, int cuts) {
    cuts = std::clamp(cuts, 1, 64);
    const auto steps = ringSteps(m, edge);
    if (steps.empty()) return {};
    std::map<Edge, std::vector<int>> pts; // cut edge (lower first) → its new points, lower end first
    std::vector<int> made;
    auto along = [&](int p, int q) {
        const Edge k = key(p, q);
        auto it = pts.find(k);
        if (it == pts.end()) {
            std::vector<int> v;
            for (int c = 1; c <= cuts; ++c) {
                v.push_back(int(m.verts.size()));
                made.push_back(v.back());
                m.verts.push_back(glm::mix(m.verts[size_t(k.first)], m.verts[size_t(k.second)], float(c) / float(cuts + 1)));
            }
            it = pts.emplace(k, v).first;
        }
        std::vector<int> v = it->second;
        if (p > q) std::reverse(v.begin(), v.end());
        return v;
    };
    std::set<int> ring;
    std::vector<std::vector<int>> added;
    for (auto [f, i] : steps) {
        ring.insert(f);
        const auto fv = m.faces[size_t(f)];
        const int x0 = fv[size_t(i)], x1 = fv[(size_t(i) + 1) % 4], x2 = fv[(size_t(i) + 2) % 4], x3 = fv[(size_t(i) + 3) % 4];
        std::vector<int> l0{x0}, l1{x3};
        for (int p : along(x0, x1)) l0.push_back(p);
        for (int p : along(x3, x2)) l1.push_back(p);
        l0.push_back(x1), l1.push_back(x2);
        for (size_t k = 0; k + 1 < l0.size(); ++k) added.push_back({l0[k], l0[k + 1], l1[k + 1], l1[k]});
    }
    std::vector<std::vector<int>> faces;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        if (ring.count(int(f))) continue;
        const auto& fv = m.faces[f];
        std::vector<int> nf;
        for (size_t i = 0; i < fv.size(); ++i) {
            const int p = fv[i], q = fv[(i + 1) % fv.size()];
            nf.push_back(p);
            if (pts.count(key(p, q))) // (a face at the ring's end: the cut points go into its edge)
                for (int x : along(p, q)) nf.push_back(x);
        }
        faces.push_back(std::move(nf));
    }
    faces.insert(faces.end(), added.begin(), added.end());
    m.faces = std::move(faces);
    return made;
}

std::set<std::pair<int, int>> edgeLoop(const Mesh& m, std::pair<int, int> edge) {
    const auto nb = neighbours(m);
    std::map<Edge, std::set<int>> facesOf;
    for (size_t f = 0; f < m.faces.size(); ++f)
        for (size_t i = 0; i < m.faces[f].size(); ++i) facesOf[key(m.faces[f][i], m.faces[f][(i + 1) % m.faces[f].size()])].insert(int(f));
    std::set<Edge> out{key(edge.first, edge.second)};
    for (int dir = 0; dir < 2; ++dir) {
        int prev = dir ? edge.second : edge.first, cur = dir ? edge.first : edge.second;
        for (int n = 0; n < 100000; ++n) {
            auto it = nb.find(cur);
            if (it == nb.end() || it->second.size() != 4) break;
            const auto& besideFaces = facesOf[key(prev, cur)];
            int next = -1;
            for (int w : it->second) {
                if (w == prev) continue;
                bool touches = false;
                for (int f : facesOf[key(cur, w)]) touches |= besideFaces.count(f) > 0;
                if (!touches) next = w;
            }
            if (next < 0 || !out.insert(key(cur, next)).second) break;
            prev = cur, cur = next;
        }
    }
    return out;
}

std::set<int> linkedVerts(const Mesh& m, const std::set<int>& verts) {
    const auto nb = neighbours(m);
    std::set<int> out = verts;
    std::vector<int> todo(verts.begin(), verts.end());
    while (!todo.empty()) {
        const int v = todo.back();
        todo.pop_back();
        if (auto it = nb.find(v); it != nb.end())
            for (int w : it->second)
                if (out.insert(w).second) todo.push_back(w);
    }
    return out;
}

std::set<int> linkedFaces(const Mesh& m, const std::set<int>& faces) {
    std::set<int> verts;
    for (int f : faces)
        if (f < int(m.faces.size())) verts.insert(m.faces[size_t(f)].begin(), m.faces[size_t(f)].end());
    verts = linkedVerts(m, verts);
    std::set<int> out;
    for (size_t f = 0; f < m.faces.size(); ++f)
        if (verts.count(m.faces[f][0])) out.insert(int(f));
    return out;
}

std::set<int> growVerts(const Mesh& m, const std::set<int>& verts) {
    const auto nb = neighbours(m);
    std::set<int> out = verts;
    for (int v : verts)
        if (auto it = nb.find(v); it != nb.end()) out.insert(it->second.begin(), it->second.end());
    return out;
}

} // namespace fry

namespace fry {

std::set<std::pair<int, int>> edgeRing(const Mesh& m, std::pair<int, int> edge) {
    auto key = [](int a, int b) { return std::pair{std::min(a, b), std::max(a, b)}; };
    std::set<std::pair<int, int>> out{key(edge.first, edge.second)};
    for (const auto& q : ringQuads(m, edge)) out.insert(key(q[0], q[1])), out.insert(key(q[3], q[2]));
    return out;
}

void smoothPoints(Mesh& m, const std::set<int>& verts, float amount, int times) {
    std::vector<std::vector<int>> next(m.verts.size());
    for (const auto& [a, b] : edges(m)) next[size_t(a)].push_back(b), next[size_t(b)].push_back(a);
    for (int t = 0; t < times; ++t) {
        std::vector<std::pair<int, glm::vec3>> moved; // (from where they all were before this pass)
        for (int v : verts) {
            if (v < 0 || size_t(v) >= m.verts.size() || next[size_t(v)].empty()) continue;
            glm::vec3 mid(0.0f);
            for (int n : next[size_t(v)]) mid += m.verts[size_t(n)];
            mid /= float(next[size_t(v)].size());
            moved.push_back({v, glm::mix(m.verts[size_t(v)], mid, std::clamp(amount, 0.0f, 1.0f))});
        }
        for (const auto& [v, p] : moved) m.verts[size_t(v)] = p;
    }
}

void pushPoints(Mesh& m, const std::set<int>& verts, float distance) {
    std::vector<glm::vec3> normal(m.verts.size(), glm::vec3(0.0f));
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const glm::vec3 n = faceNormal(m, int(f)) * faceArea(m, int(f));
        for (int v : m.faces[f]) normal[size_t(v)] += n;
    }
    for (int v : verts)
        if (v >= 0 && size_t(v) < m.verts.size() && glm::length(normal[size_t(v)]) > 1e-20f) m.verts[size_t(v)] += glm::normalize(normal[size_t(v)]) * distance;
}

std::vector<int> bridgeFaces(Mesh& m, int a, int b) {
    if (a == b || a < 0 || b < 0 || size_t(a) >= m.faces.size() || size_t(b) >= m.faces.size()) return {};
    const std::vector<int> A = m.faces[size_t(a)];
    std::vector<int> B = m.faces[size_t(b)];
    const size_t n = A.size();
    if (B.size() != n || n < 3) return {};
    for (int v : A)
        if (std::find(B.begin(), B.end(), v) != B.end()) return {}; // (sharing a point: nothing to bridge)
    // B the other way round (seen down the tunnel, the same way as A), turned to fit A best.
    std::reverse(B.begin(), B.end());
    size_t shift = 0;
    float best = INFINITY;
    for (size_t s = 0; s < n; ++s) {
        float d = 0.0f;
        for (size_t i = 0; i < n; ++i) d += glm::distance(m.verts[size_t(A[i])], m.verts[size_t(B[(i + s) % n])]);
        if (d < best) best = d, shift = s;
    }
    m.faces.erase(m.faces.begin() + std::max(a, b)), m.faces.erase(m.faces.begin() + std::min(a, b));
    std::vector<int> made;
    for (size_t i = 0; i < n; ++i) {
        // Along A's edge the way A went (it takes A's place there; A's neighbour runs it the other way).
        made.push_back(int(m.faces.size()));
        m.faces.push_back({A[i], A[(i + 1) % n], B[(i + 1 + shift) % n], B[(i + shift) % n]});
    }
    return made;
}

} // namespace fry
