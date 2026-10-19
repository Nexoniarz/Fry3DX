#include "render/clusters.h"

#include <algorithm>
#include <cmath>

namespace keel {
namespace {
const float kLogRatio = std::log(kClusterFar / kClusterNear);

float sliceStart(uint32_t k) { return k == 0 ? 0.0f : kClusterNear * std::exp(kLogRatio * float(k) / kClusterZ); }

float distSq(float v, float lo, float hi) {
    const float d = v < lo ? lo - v : v > hi ? v - hi : 0.0f;
    return d * d;
}
} // namespace

uint32_t clusterSlice(float depth) {
    if (depth <= kClusterNear) return 0;
    return std::min(uint32_t(std::log(depth / kClusterNear) / kLogRatio * kClusterZ), kClusterZ - 1);
}

void buildClusters(const ClusterView& v, const std::vector<DynamicLight>& lights, size_t maxIndices, ClusterGrid& out) {
    const uint32_t cellCount = kClusterX * kClusterY * kClusterZ;
    out.cells.assign(cellCount, glm::uvec2(0));
    out.indices.clear();
    const float tanY = v.tanHalfY, tanX = v.tanHalfY * v.aspect;
    std::vector<uint32_t> pairs; // cell << 12 | light
    for (uint32_t li = 0; li < lights.size() && li < 4096; ++li) {
        const DynamicLight& l = lights[li];
        const glm::vec3 d = l.origin - v.origin;
        const glm::vec3 c(glm::dot(d, v.right), glm::dot(d, v.up), glm::dot(d, v.forward)); // view space, z forward
        const float r = l.radius;
        if (c.z + r <= 0.0f) continue;
        const uint32_t z0 = clusterSlice(std::max(c.z - r, 0.0f)), z1 = clusterSlice(c.z + r);
        for (uint32_t z = z0; z <= z1; ++z) {
            const float dn = std::max(sliceStart(z), 0.05f), df = z + 1 == kClusterZ ? kClusterFar : sliceStart(z + 1);
            // Conservative tile range from the sphere's extent at both ends of the slice.
            auto tileU = [&](float x, float depth) { return (x / (depth * tanX) + 1.0f) * 0.5f * kClusterX; };
            auto tileV = [&](float y, float depth) { return (1.0f - y / (depth * tanY)) * 0.5f * kClusterY; };
            float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
            for (float depth : {dn, df}) {
                for (float x : {c.x - r, c.x + r}) u0 = std::min(u0, tileU(x, depth)), u1 = std::max(u1, tileU(x, depth));
                for (float y : {c.y - r, c.y + r}) v0 = std::min(v0, tileV(y, depth)), v1 = std::max(v1, tileV(y, depth));
            }
            const int x0 = std::max(int(std::floor(u0)), 0), x1 = std::min(int(std::floor(u1)), int(kClusterX) - 1);
            const int y0 = std::max(int(std::floor(v0)), 0), y1 = std::min(int(std::floor(v1)), int(kClusterY) - 1);
            for (int y = y0; y <= y1; ++y) {
                // Cell bounds in view space: y shrinks downwards (v grows), x grows rightwards.
                const float yt = 1.0f - 2.0f * y / kClusterY, yb = 1.0f - 2.0f * (y + 1) / kClusterY;
                const float ylo = std::min({yb * dn, yb * df}) * tanY, yhi = std::max({yt * dn, yt * df}) * tanY;
                for (int x = x0; x <= x1; ++x) {
                    const float xl = 2.0f * x / kClusterX - 1.0f, xr = 2.0f * (x + 1) / kClusterX - 1.0f;
                    const float xlo = std::min({xl * dn, xl * df}) * tanX, xhi = std::max({xr * dn, xr * df}) * tanX;
                    if (distSq(c.x, xlo, xhi) + distSq(c.y, ylo, yhi) + distSq(c.z, dn, df) > r * r) continue;
                    pairs.push_back(((z * kClusterY + y) * kClusterX + x) << 12 | li);
                }
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());
    if (pairs.size() > maxIndices) pairs.resize(maxIndices);
    out.indices.reserve(pairs.size());
    for (uint32_t p : pairs) {
        glm::uvec2& cell = out.cells[p >> 12];
        if (cell.y == 0) cell.x = uint32_t(out.indices.size());
        ++cell.y;
        out.indices.push_back(p & 0xFFF);
    }
}

} // namespace keel
