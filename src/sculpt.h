#pragma once
// Sculpting: brushes that push a mesh's points about like clay — the points under the brush
// move most in its middle, less towards its edge. It only moves points (no new ones): a shape
// with few points is made finer first (finerForSculpting).
#include "mesh.h"

#include <utility>
#include <vector>

namespace fry {

enum class SculptBrush {
    Draw,    // raise (or, inverted, dig into) the surface under it
    Inflate, // puff out along each point's own normal (inverted: shrink)
    Smooth,  // even out bumps
    Flatten, // press onto the plane the surface under it is closest to
    Pinch,   // pull points in to the middle (inverted: push apart) — sharpens creases
    Grab,    // take hold of a patch and drag it (grabbed / moveGrabbed)
};

// What a stroke needs to know about the mesh, made once when it starts (the faces don't change
// while sculpting, only where the points are).
struct SculptMesh {
    explicit SculptMesh(const Mesh& m);
    std::vector<std::vector<int>> next;  // each point's neighbours along edges
    std::vector<std::vector<int>> faces; // the faces round each point
};

// One dab, in the mesh's own space. strength: 0..1; invert: the other way.
struct Dab {
    glm::vec3 at{0.0f};
    float radius = 0.1f, strength = 0.5f;
    bool invert = false;
};

// How much of a dab reaches a point `d` away: 1 in the middle, 0 at the edge, smoothly.
float sculptFalloff(float d, float radius);
// A point's normal: its faces', averaged.
glm::vec3 pointNormal(const Mesh& m, const SculptMesh& s, int point);
void sculptDab(Mesh& m, const SculptMesh& s, SculptBrush brush, const Dab& dab);

// Grab: the points under the brush and how much each follows it; then, as it's dragged, each
// put `offset` × its share away from where it was when grabbed (`from`).
std::vector<std::pair<int, float>> grabbed(const Mesh& m, const glm::vec3& at, float radius);
void moveGrabbed(Mesh& m, const std::vector<std::pair<int, float>>& points, const std::vector<glm::vec3>& from, const glm::vec3& offset);

// Made finer without changing its shape (each face split into four, flat) until it has at least
// `points` points, or would have more than `limit` faces. Returns how many times it was split.
int finerForSculpting(Mesh& m, size_t points = 6000, size_t limit = 400000);

} // namespace fry
