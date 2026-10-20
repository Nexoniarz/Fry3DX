#pragma once
// A tree of boxes round a mesh's triangles (a bounding volume hierarchy): what a ray hits first,
// or whether anything's in its way, without trying every triangle.
#include "mesh.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace fry {

class TriangleTree {
public:
    TriangleTree() = default;
    // The mesh's triangles, its points moved by `matrix` (into the world).
    TriangleTree(const Mesh& m, const glm::mat4& matrix);

    struct Hit {
        float t = 0;
        int face = -1;           // (the mesh's face it's part of)
        std::array<int, 3> tri{}; // (its points)
    };
    // The nearest hit along the ray (dir needn't be unit length: t is in its lengths), before maxT.
    std::optional<Hit> nearest(const glm::vec3& origin, const glm::vec3& dir, float maxT = 1e30f) const;
    // Anything in the way before maxT, but triangles with point `skip` (the point being looked at).
    bool blocked(const glm::vec3& origin, const glm::vec3& dir, float maxT, int skip = -1) const;
    bool empty() const { return tris_.empty(); }
    const std::vector<glm::vec3>& points() const { return points_; } // (in the world)

private:
    struct Tri {
        std::array<int, 3> v;
        int face;
    };
    struct Node {
        glm::vec3 lo, hi;
        int first, count; // (a leaf: its triangles; else count 0 and `first` is the right child, the left one next)
    };
    std::vector<glm::vec3> points_;
    std::vector<Tri> tris_;
    std::vector<Node> nodes_;
    int build(int first, int count);
};

// Where a ray meets a triangle (Möller–Trumbore), in the ray's lengths.
std::optional<float> rayTriangle(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);

} // namespace fry
