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

Repair repairForPrint(Mesh& m) {
    Repair r;
    if (m.verts.empty()) return r;
    // (how near is "the same place": a millionth of its size, a micron at least)
    const float size = glm::length(boundsMax(m) - boundsMin(m));
    const size_t before = m.verts.size(), faces = m.faces.size();
    mergeByDistance(m, std::max(size * 1e-6f, 1e-6f)); // (it drops faces left with fewer than three points: counted below)
    r.joinedPoints = int(before - m.verts.size());
    // Faces with a point twice in a row, fewer than three points, or no area.
    const float tiny = std::max(size * size * 1e-12f, 1e-16f);
    for (auto& f : m.faces) {
        f.erase(std::unique(f.begin(), f.end()), f.end());
        while (f.size() > 1 && f.front() == f.back()) f.pop_back();
    }
    std::erase_if(m.faces, [&](const std::vector<int>& f) { return f.size() < 3; });
    for (size_t i = m.faces.size(); i-- > 0;)
        if (faceArea(m, int(i)) <= tiny) m.faces.erase(m.faces.begin() + std::ptrdiff_t(i));
    r.emptyFaces = int(faces - m.faces.size());
    // The same face twice: once. Back to back (the same points, the other way round): neither.
    std::map<std::vector<int>, std::vector<size_t>> same;
    for (size_t i = 0; i < m.faces.size(); ++i) {
        std::vector<int> key = m.faces[i];
        std::sort(key.begin(), key.end());
        same[key].push_back(i);
    }
    std::vector<char> drop(m.faces.size(), 0);
    auto turn = [](const std::vector<int>& f) { // (the face started at its lowest point: two that turn the same way match)
        std::vector<int> g = f;
        std::rotate(g.begin(), std::min_element(g.begin(), g.end()), g.end());
        return g;
    };
    for (const auto& [key, list] : same) {
        if (list.size() < 2) continue;
        int ways = 0, other = 0;
        const std::vector<int> first = turn(m.faces[list[0]]);
        for (size_t i : list) (turn(m.faces[i]) == first ? ways : other)++;
        if (ways == other) // (back to back: an inside wall)
            for (size_t i : list) drop[i] = 1;
        else
            for (size_t k = 1; k < list.size(); ++k) drop[list[k]] = 1;
    }
    size_t at = 0;
    for (size_t i = 0; i < m.faces.size(); ++i)
        if (!drop[i]) {
            if (at != i) m.faces[at] = std::move(m.faces[i]); // (not onto itself: that empties it)
            ++at;
        }
    r.doubledFaces = int(m.faces.size() - at);
    m.faces.resize(at);
    removeUnused(m);
    r.turnedFaces = faceOutward(m);
    r.closedHoles = closeHoles(m);
    if (r.closedHoles) r.turnedFaces += faceOutward(m);
    return r;
}

std::vector<std::pair<glm::vec3, glm::vec3>> sliceAt(const Mesh& m, float z) {
    std::vector<std::pair<glm::vec3, glm::vec3>> out;
    for (size_t f = 0; f < m.faces.size(); ++f)
        for (const auto& t : triangulate(m, int(f))) { // (a triangle crosses it twice or not at all; a dented face can more)
            glm::vec3 at[2];
            int n = 0;
            for (int i = 0; i < 3; ++i) {
                const glm::vec3 a = m.verts[size_t(t[size_t(i)])], b = m.verts[size_t(t[size_t((i + 1) % 3)])];
                const float da = a.z - z, db = b.z - z;
                if ((da < 0) != (db < 0) && n < 2) at[n++] = glm::mix(a, b, da / (da - db));
            }
            if (n == 2) out.push_back({at[0], at[1]});
        }
    return out;
}

} // namespace fry
