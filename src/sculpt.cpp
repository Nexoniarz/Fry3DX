#include "sculpt.h"

#include <algorithm>
#include <cmath>

namespace fry {

SculptMesh::SculptMesh(const Mesh& m) : next(m.verts.size()), faces(m.verts.size()) {
    for (const auto& [a, b] : edges(m)) next[size_t(a)].push_back(b), next[size_t(b)].push_back(a);
    for (size_t f = 0; f < m.faces.size(); ++f)
        for (int v : m.faces[f]) faces[size_t(v)].push_back(int(f));
}

float sculptFalloff(float d, float radius) {
    if (d >= radius) return 0.0f;
    const float s = 1.0f - d / radius;
    return s * s * (3.0f - 2.0f * s);
}

glm::vec3 pointNormal(const Mesh& m, const SculptMesh& s, int point) {
    glm::vec3 n(0.0f);
    for (int f : s.faces[size_t(point)]) n += faceNormal(m, f) * std::max(faceArea(m, f), 1e-12f);
    const float len = glm::length(n);
    return len > 1e-20f ? n / len : glm::vec3(0, 0, 1);
}

void sculptDab(Mesh& m, const SculptMesh& s, SculptBrush brush, const Dab& dab) {
    if (brush == SculptBrush::Grab || dab.radius <= 0.0f) return;
    // The points it reaches, how much, and the surface under it: its normal and middle.
    std::vector<std::pair<int, float>> under;
    glm::vec3 normal(0.0f), middle(0.0f);
    float total = 0.0f;
    for (size_t i = 0; i < m.verts.size(); ++i) {
        const float w = sculptFalloff(glm::distance(m.verts[i], dab.at), dab.radius);
        if (w <= 0.0f || s.faces[i].empty()) continue;
        under.push_back({int(i), w});
        normal += pointNormal(m, s, int(i)) * w;
        middle += m.verts[i] * w, total += w;
    }
    if (under.empty()) return;
    middle /= total;
    normal = glm::length(normal) > 1e-12f ? glm::normalize(normal) : glm::vec3(0, 0, 1);
    const float sign = dab.invert ? -1.0f : 1.0f;
    // A sixth of the brush per dab at full strength (dabs come every quarter brush, so a stroke
    // over a point raises it about half the brush's size).
    const float step = dab.radius * 0.16f * std::clamp(dab.strength, 0.0f, 1.0f);

    // (Worked out from where the points were before this dab, so it doesn't depend on order.)
    std::vector<glm::vec3> moved(under.size());
    for (size_t k = 0; k < under.size(); ++k) {
        const auto [i, w] = under[k];
        const glm::vec3 p = m.verts[size_t(i)];
        switch (brush) {
        case SculptBrush::Draw: moved[k] = p + normal * (sign * step * w); break;
        case SculptBrush::Inflate: moved[k] = p + pointNormal(m, s, i) * (sign * step * w); break;
        case SculptBrush::Smooth: {
            glm::vec3 avg(0.0f);
            for (int n : s.next[size_t(i)]) avg += m.verts[size_t(n)];
            if (s.next[size_t(i)].empty()) avg = p;
            else avg /= float(s.next[size_t(i)].size());
            moved[k] = glm::mix(p, avg, std::min(1.0f, w * dab.strength));
            break;
        }
        case SculptBrush::Flatten: moved[k] = p - normal * (glm::dot(p - middle, normal) * std::min(1.0f, w * dab.strength * 0.5f)); break;
        case SculptBrush::Pinch: {
            glm::vec3 in = dab.at - p;
            in -= normal * glm::dot(in, normal); // along the surface, not into it
            moved[k] = p + in * (sign * std::min(0.5f, w * dab.strength * 0.15f));
            break;
        }
        case SculptBrush::Grab: break;
        }
    }
    for (size_t k = 0; k < under.size(); ++k) m.verts[size_t(under[k].first)] = moved[k];
}

std::vector<std::pair<int, float>> grabbed(const Mesh& m, const glm::vec3& at, float radius) {
    std::vector<std::pair<int, float>> out;
    for (size_t i = 0; i < m.verts.size(); ++i)
        if (const float w = sculptFalloff(glm::distance(m.verts[i], at), radius); w > 0.0f) out.push_back({int(i), w});
    return out;
}

void moveGrabbed(Mesh& m, const std::vector<std::pair<int, float>>& points, const std::vector<glm::vec3>& from, const glm::vec3& offset) {
    for (size_t k = 0; k < points.size() && k < from.size(); ++k) m.verts[size_t(points[k].first)] = from[k] + offset * points[k].second;
}

int finerForSculpting(Mesh& m, size_t points, size_t limit) {
    int times = 0;
    while (m.verts.size() < points) {
        size_t after = 0; // (each face becomes as many as it has corners)
        for (const auto& f : m.faces) after += f.size();
        if (after > limit || m.faces.empty()) break;
        subdivide(m, false);
        ++times;
    }
    return times;
}

} // namespace fry
