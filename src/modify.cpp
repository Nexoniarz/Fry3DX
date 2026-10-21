// Whole-shape changes: flat faces joined up, and a surface made thick.
#include "mesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace fry {

int mergeFlatFaces(Mesh& m, float angle) {
    const float same = std::cos(glm::radians(angle));
    std::map<std::pair<int, int>, std::vector<int>> byEdge;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) byEdge[{std::min(fv[i], fv[(i + 1) % fv.size()]), std::max(fv[i], fv[(i + 1) % fv.size()])}].push_back(int(f));
    }
    std::vector<glm::vec3> normal(m.faces.size());
    for (size_t f = 0; f < m.faces.size(); ++f) normal[f] = faceNormal(m, int(f));
    std::vector<int> root(m.faces.size());
    std::iota(root.begin(), root.end(), 0);
    auto find = [&](int f) {
        while (root[size_t(f)] != f) f = root[size_t(f)] = root[size_t(root[size_t(f)])];
        return f;
    };
    for (const auto& [e, fs] : byEdge)
        if (fs.size() == 2 && glm::dot(normal[size_t(fs[0])], normal[size_t(fs[1])]) > same &&
            std::abs(glm::dot(normal[size_t(fs[0])], faceCenter(m, fs[1]) - faceCenter(m, fs[0]))) < 1e-5f)
            root[size_t(find(fs[0]))] = find(fs[1]);
    std::map<int, std::vector<int>> groups;
    for (size_t f = 0; f < m.faces.size(); ++f) groups[find(int(f))].push_back(int(f));
    std::vector<std::vector<int>> out;
    int merged = 0;
    for (const auto& [r, fs] : groups) {
        if (fs.size() == 1) {
            out.push_back(m.faces[size_t(fs[0])]);
            continue;
        }
        // Its outline: the edges no other face of it has the other way round. One loop that
        // doesn't touch itself makes one face; otherwise the faces stay as they were.
        std::set<std::pair<int, int>> inside;
        for (int f : fs)
            for (size_t i = 0; i < m.faces[size_t(f)].size(); ++i) inside.insert({m.faces[size_t(f)][i], m.faces[size_t(f)][(i + 1) % m.faces[size_t(f)].size()]});
        std::map<int, int> next;
        bool ok = true;
        for (auto [a, b] : inside)
            if (!inside.count({b, a})) ok &= next.emplace(a, b).second;
        std::vector<int> loop;
        if (ok && !next.empty()) {
            int v = next.begin()->first;
            do {
                loop.push_back(v);
                v = next.count(v) ? next[v] : -1;
            } while (v >= 0 && v != loop.front() && loop.size() <= next.size());
            ok = v == loop.front() && loop.size() == next.size();
        }
        if (ok && loop.size() >= 3) out.push_back(loop), merged += int(fs.size()) - 1;
        else
            for (int f : fs) out.push_back(m.faces[size_t(f)]);
    }
    m.faces = out;
    return merged;
}

void thicken(Mesh& m, float thickness) {
    const size_t n = m.verts.size();
    // Each point's way out: its faces' normals, weighted by their corners' angles; pushed out
    // further where faces meet at an angle, so the walls come out evenly thick.
    std::vector<glm::vec3> out(n, glm::vec3(0.0f));
    std::vector<std::vector<glm::vec3>> around(n);
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        const glm::vec3 fn = faceNormal(m, int(f));
        for (size_t i = 0; i < fv.size(); ++i) {
            const glm::vec3 p = m.verts[size_t(fv[i])], a = m.verts[size_t(fv[(i + fv.size() - 1) % fv.size()])], b = m.verts[size_t(fv[(i + 1) % fv.size()])];
            const float ang = std::acos(std::clamp(glm::dot(glm::normalize(a - p), glm::normalize(b - p)), -1.0f, 1.0f));
            out[size_t(fv[i])] += fn * ang;
            around[size_t(fv[i])].push_back(fn);
        }
    }
    for (size_t v = 0; v < n; ++v) {
        if (glm::length(out[v]) < 1e-12f) continue;
        out[v] = glm::normalize(out[v]);
        float c = 1.0f;
        for (const glm::vec3& fn : around[v]) c = std::min(c, glm::dot(fn, out[v]));
        out[v] /= std::max(c, 0.35f);
    }
    // The other side: the points moved in (thickness > 0: in, against the faces' way), its faces
    // turned round; then walls along the open edges.
    for (size_t v = 0; v < n; ++v) m.verts.push_back(m.verts[v] - out[v] * thickness);
    std::set<std::pair<int, int>> directed;
    const size_t faces = m.faces.size();
    for (size_t f = 0; f < faces; ++f)
        for (size_t i = 0; i < m.faces[f].size(); ++i) directed.insert({m.faces[f][i], m.faces[f][(i + 1) % m.faces[f].size()]});
    for (size_t f = 0; f < faces; ++f) {
        std::vector<int> back;
        for (auto it = m.faces[f].rbegin(); it != m.faces[f].rend(); ++it) back.push_back(*it + int(n));
        m.faces.push_back(back);
    }
    for (auto [a, b] : directed)
        if (!directed.count({b, a})) m.faces.push_back({b, a, a + int(n), b + int(n)});
    if (thickness < 0) // (made outwards: the new side is the outside)
        for (auto& f : m.faces) std::reverse(f.begin(), f.end());
}

Mesh spin(const Mesh& m, const glm::vec3& origin, const glm::vec3& axis, float degrees, int steps, float rise) {
    if (m.verts.empty() || glm::length(axis) < 1e-9f) return m;
    const glm::vec3 ax = glm::normalize(axis);
    steps = std::clamp(steps, 1, 4096);
    const bool round = std::abs(std::abs(degrees) - 360.0f) < 1e-3f && std::abs(rise) < 1e-9f; // (back where it started: no ends)
    const int copies = round ? steps : steps + 1;
    const int n = int(m.verts.size());
    Mesh out;
    out.verts.reserve(size_t(n * copies));
    for (int k = 0; k < copies; ++k) {
        const float t = float(k) / float(steps);
        const glm::mat4 turn = glm::translate(glm::mat4(1.0f), origin + ax * (rise * t)) * glm::rotate(glm::mat4(1.0f), glm::radians(degrees * t), ax) * glm::translate(glm::mat4(1.0f), -origin);
        for (const glm::vec3& v : m.verts) out.verts.push_back(glm::vec3(turn * glm::vec4(v, 1.0f)));
    }
    auto at = [&](int k, int v) { return (k % copies) * n + v; };
    // What sweeps: edges with one face (a surface's rim, a flat shape's outline), each once.
    std::map<std::pair<int, int>, int> count;
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) ++count[{std::min(f[i], f[(i + 1) % f.size()]), std::max(f[i], f[(i + 1) % f.size()])}];
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) {
            const int a = f[i], b = f[(i + 1) % f.size()];
            if (count[{std::min(a, b), std::max(a, b)}] != 1) continue;
            for (int k = 0; k < steps; ++k) out.faces.push_back({at(k, b), at(k, a), at(k + 1, a), at(k + 1, b)});
        }
    // A closed shape turned: its own faces at each end too (it sweeps a solid). A surface: its two ends.
    if (!round)
        for (const auto& f : m.faces) {
            std::vector<int> first, last;
            for (int v : f) first.push_back(at(0, v)), last.push_back(at(steps, v));
            out.faces.push_back(first);
            std::reverse(last.begin(), last.end());
            out.faces.push_back(last);
        }
    // Points on the axis come to the same place each step: one point, and faces left with fewer than three gone.
    mergeByDistance(out, 1e-6f);
    for (auto& f : out.faces) {
        f.erase(std::unique(f.begin(), f.end()), f.end());
        while (f.size() > 1 && f.front() == f.back()) f.pop_back();
    }
    std::erase_if(out.faces, [](const std::vector<int>& f) { return f.size() < 3; });
    removeUnused(out);
    faceOutward(out);
    return out;
}

} // namespace fry

namespace fry {

std::vector<glm::vec3> pathOf(const Mesh& m, bool& closed) {
    closed = false;
    std::map<std::pair<int, int>, int> count;
    for (const auto& f : m.faces)
        if (f.size() == 2) count[{std::min(f[0], f[1]), std::max(f[0], f[1])}] = 1; // (a line's piece)
        else
            for (size_t i = 0; i < f.size(); ++i) ++count[{std::min(f[i], f[(i + 1) % f.size()]), std::max(f[i], f[(i + 1) % f.size()])}];
    std::map<int, std::vector<int>> next;
    for (const auto& [e, n] : count)
        if (n == 1 && e.first != e.second) next[e.first].push_back(e.second), next[e.second].push_back(e.first);
    if (next.empty()) { // (closed all round: the biggest face's outline)
        int best = -1;
        float area = 0;
        for (size_t f = 0; f < m.faces.size(); ++f) {
            if (m.faces[f].size() < 3) continue;
            glm::vec3 n(0.0f);
            const auto& fv = m.faces[f];
            for (size_t i = 0; i < fv.size(); ++i) n += glm::cross(m.verts[size_t(fv[i])], m.verts[size_t(fv[(i + 1) % fv.size()])]);
            if (glm::length(n) > area) area = glm::length(n), best = int(f);
        }
        std::vector<glm::vec3> out;
        if (best >= 0)
            for (int v : m.faces[size_t(best)]) out.push_back(m.verts[size_t(v)]);
        closed = out.size() >= 3;
        return out;
    }
    // Each run of edges, walked from an end (or anywhere on a loop); the longest kept.
    std::set<std::pair<int, int>> used;
    std::vector<glm::vec3> best;
    bool bestClosed = false;
    float bestLength = -1;
    auto walk = [&](int from) {
        std::vector<int> chain{from};
        int at = from, prev = -1;
        bool loop = false;
        for (;;) {
            int go = -1;
            for (int n : next[at])
                if (n != prev && !used.count({std::min(at, n), std::max(at, n)})) {
                    go = n;
                    break;
                }
            if (go < 0) break;
            used.insert({std::min(at, go), std::max(at, go)});
            if (go == from) {
                loop = true;
                break;
            }
            chain.push_back(go), prev = at, at = go;
        }
        float length = 0;
        for (size_t i = 1; i < chain.size(); ++i) length += glm::distance(m.verts[size_t(chain[i - 1])], m.verts[size_t(chain[i])]);
        if (loop) length += glm::distance(m.verts[size_t(chain.back())], m.verts[size_t(from)]);
        if (chain.size() >= 2 && length > bestLength) {
            bestLength = length, bestClosed = loop, best.clear();
            for (int v : chain) best.push_back(m.verts[size_t(v)]);
        }
    };
    for (const auto& [v, ns] : next)
        if (ns.size() == 1) walk(v); // (ends first: open runs from their start)
    for (const auto& [v, ns] : next)
        for (int n : ns)
            if (!used.count({std::min(v, n), std::max(v, n)})) walk(v);
    closed = bestClosed;
    return best;
}

std::vector<PathPoint> alongPath(const std::vector<glm::vec3>& path, bool closed, int count) {
    std::vector<PathPoint> out;
    if (path.size() < 2 || count < 1) return out;
    std::vector<glm::vec3> p = path;
    if (closed) p.push_back(path.front());
    std::vector<float> at(p.size(), 0.0f); // (the length to each point)
    for (size_t i = 1; i < p.size(); ++i) at[i] = at[i - 1] + glm::distance(p[i - 1], p[i]);
    const float total = at.back();
    if (total < 1e-12f) return out;
    for (int k = 0; k < count; ++k) {
        const float want = closed ? total * float(k) / float(count) : count == 1 ? 0.0f : total * float(k) / float(count - 1);
        size_t i = size_t(std::upper_bound(at.begin(), at.end(), want) - at.begin());
        i = std::clamp<size_t>(i, 1, p.size() - 1);
        while (i < p.size() - 1 && at[i] - at[i - 1] < 1e-12f) ++i; // (no way along a piece with no length)
        const float piece = std::max(at[i] - at[i - 1], 1e-12f), t = std::clamp((want - at[i - 1]) / piece, 0.0f, 1.0f);
        const glm::vec3 d = p[i] - p[i - 1];
        out.push_back({glm::mix(p[i - 1], p[i], t), glm::length(d) > 1e-12f ? glm::normalize(d) : glm::vec3(1, 0, 0)});
    }
    return out;
}

} // namespace fry
