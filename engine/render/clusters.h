#pragma once
// Clustered lighting: the view frustum is cut into a 3D grid (screen tiles × depth
// slices) and each cell lists the lights that can reach it, so a pixel only shades
// the few lights near it. Built on the CPU each frame; no GPU code here, so it is
// shared by every render backend and testable headless.
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace keel {

struct DynamicLight {
    glm::vec3 origin{0.0f};
    float radius = 300.0f;        // light reaches zero here
    glm::vec3 color{1.0f};        // linear, already scaled by intensity
    glm::vec3 direction{0, 0, -1}; // spot lights
    float innerCone = 0.0f;       // spot half-angles in degrees; outerCone 0 = point light
    float outerCone = 0.0f;
    bool shadows = false;
    float size = 0.0f;            // the light's own radius (a bulb, a lamp, a window): softer, wider highlights
    bool reflector = false;       // a flashlight's beam (spotShape in model.glsl): hot centre, a ring, a dim spill
    bool spot() const { return outerCone > 0.0f; }
};

// Must match lights.glsl.
inline constexpr uint32_t kClusterX = 16, kClusterY = 9, kClusterZ = 24;
inline constexpr float kClusterNear = 8.0f, kClusterFar = 16384.0f;

struct ClusterView {
    glm::vec3 origin, forward, right, up; // up is the screen's up (after roll)
    float tanHalfY = 1.0f, aspect = 1.0f;
};

struct ClusterGrid {
    std::vector<glm::uvec2> cells;  // per cell: first index, light count; cell = (z * Y + y) * X + x
    std::vector<uint32_t> indices;  // light indices, grouped by cell
};

// Depth slice for a distance along the view direction.
uint32_t clusterSlice(float depth);
// Lights past maxIndices total cell entries are dropped (farthest cells last).
void buildClusters(const ClusterView& view, const std::vector<DynamicLight>& lights, size_t maxIndices, ClusterGrid& out);

} // namespace keel
