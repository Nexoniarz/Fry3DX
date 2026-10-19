// Checking and fixing models for 3D printing.
#include "mesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace fry {

bool overhangs(const Mesh& m, int face, float overhang, float bedZ) {
    if (faceNormal(m, face).z > -std::sin(glm::radians(90.0f - overhang))) return false;
    for (int v : m.faces[size_t(face)])
        if (m.verts[size_t(v)].z > bedZ + 1e-4f) return true;
    return false; // (lying on the bed)
}

PrintCheck checkForPrint(const Mesh& m, float overhang, float bedZ) {
    PrintCheck c;
    std::map<std::pair<int, int>, int> directed; // (how many faces go a -> b)
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) ++directed[{f[i], f[(i + 1) % f.size()]}];
    std::set<std::pair<int, int>> seen;
    for (const auto& [e, n] : directed) {
        const std::pair<int, int> key{std::min(e.first, e.second), std::max(e.first, e.second)};
        if (!seen.insert(key).second) continue;
        const int ab = n, ba = directed.count({e.second, e.first}) ? directed.at({e.second, e.first}) : 0;
        if (ab + ba == 1) ++c.openEdges;
        else if (ab + ba > 2) ++c.crowdedEdges;
        else if (ab == 2 || ba == 2) ++c.flippedEdges;
    }
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const float a = faceArea(m, int(f));
        c.area += a;
        if (a < 1e-10f) ++c.tinyFaces;
        else if (overhangs(m, int(f), overhang, bedZ)) c.overhangArea += a;
        for (const auto& t : triangulate(m, int(f))) c.volume += glm::dot(m.verts[size_t(t[0])], glm::cross(m.verts[size_t(t[1])], m.verts[size_t(t[2])])) / 6.0f;
    }
    std::set<int> left;
    for (size_t f = 0; f < m.faces.size(); ++f) left.insert(int(f));
    while (!left.empty()) {
        const std::set<int> piece = linkedFaces(m, {*left.begin()});
        for (int f : piece) left.erase(f);
        ++c.pieces;
    }
    c.insideOut = c.volume < 0;
    return c;
}

int closeHoles(Mesh& m) {
    std::set<std::pair<int, int>> directed;
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) directed.insert({f[i], f[(i + 1) % f.size()]});
    std::map<int, int> next; // (round each hole: a boundary edge a -> b, then on from b)
    for (const auto& [a, b] : directed)
        if (!directed.count({b, a})) next[a] = b;
    int holes = 0;
    while (!next.empty()) {
        std::vector<int> loop;
        int v = next.begin()->first;
        while (next.count(v)) {
            loop.push_back(v);
            const int n = next[v];
            next.erase(v);
            v = n;
        }
        if (loop.size() < 3 || v != loop.front()) continue; // (not a clean loop)
        std::reverse(loop.begin(), loop.end()); // (the cap goes round the other way to the faces beside it)
        m.faces.push_back(loop);
        ++holes;
    }
    return holes;
}

int faceOutward(Mesh& m) {
    std::map<std::pair<int, int>, std::vector<int>> byEdge;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) byEdge[{std::min(fv[i], fv[(i + 1) % fv.size()]), std::max(fv[i], fv[(i + 1) % fv.size()])}].push_back(int(f));
    }
    auto goes = [&](int f, int a, int b) { // (face f has the edge a -> b that way round)
        const auto& fv = m.faces[size_t(f)];
        for (size_t i = 0; i < fv.size(); ++i)
            if (fv[i] == a && fv[(i + 1) % fv.size()] == b) return true;
        return false;
    };
    int turned = 0;
    std::vector<char> done(m.faces.size(), 0);
    for (size_t start = 0; start < m.faces.size(); ++start) {
        if (done[start]) continue;
        std::vector<int> piece{int(start)}, todo{int(start)};
        int flips = 0;
        done[start] = 1;
        while (!todo.empty()) {
            const int f = todo.back();
            todo.pop_back();
            const auto fv = m.faces[size_t(f)];
            for (size_t i = 0; i < fv.size(); ++i) {
                const int a = fv[i], b = fv[(i + 1) % fv.size()];
                const auto& around = byEdge[{std::min(a, b), std::max(a, b)}];
                if (around.size() != 2) continue; // (only across clean edges)
                for (int g : around) {
                    if (done[size_t(g)]) continue;
                    done[size_t(g)] = 1;
                    if (goes(g, a, b)) std::reverse(m.faces[size_t(g)].begin(), m.faces[size_t(g)].end()), ++flips;
                    piece.push_back(g), todo.push_back(g);
                }
            }
        }
        float v = 0;
        for (int f : piece)
            for (const auto& t : triangulate(m, f)) v += glm::dot(m.verts[size_t(t[0])], glm::cross(m.verts[size_t(t[1])], m.verts[size_t(t[2])]));
        if (v < 0) { // (all of it inside out: turned round, those just turned are back as they were)
            for (int f : piece) std::reverse(m.faces[size_t(f)].begin(), m.faces[size_t(f)].end());
            flips = int(piece.size()) - flips;
        }
        turned += flips;
    }
    return turned;
}

} // namespace fry
