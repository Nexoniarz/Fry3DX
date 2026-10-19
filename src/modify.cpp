// Whole-shape changes: flat faces joined up, and a surface made thick.
#include "mesh.h"

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

} // namespace fry
