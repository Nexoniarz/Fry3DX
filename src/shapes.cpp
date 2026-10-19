#include "shapes.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace fry {

namespace {
constexpr float kPi = 3.14159265358979f;

glm::mat4 at(glm::vec3 p, glm::vec3 size = glm::vec3(1.0f)) { return glm::scale(glm::translate(glm::mat4(1.0f), p), size); }

// A box from (corner) lo to hi, as its own six faces.
void addBox(Mesh& m, glm::vec3 lo, glm::vec3 hi) { append(m, makeCube(1.0f), at((lo + hi) * 0.5f, hi - lo)); }

// An outline drawn on the X–Z plane (x forward, z up), made `depth` thick across Y.
Mesh sideProfile(const std::vector<glm::vec2>& xz, float depth) {
    std::vector<glm::vec3> pts;
    for (glm::vec2 p : xz) pts.emplace_back(p.x, 0.0f, p.y);
    return extrudeOutline(pts, glm::vec3(0, 1, 0), depth);
}

// Grid box: a box cut along the given coordinates on each axis (points shared between sides).
Mesh gridBox(const std::vector<float> (&c)[3]) {
    Mesh m;
    std::map<std::array<int, 3>, int> index;
    auto point = [&](int ix, int iy, int iz) {
        const std::array<int, 3> k{ix, iy, iz};
        auto [it, fresh] = index.try_emplace(k, int(m.verts.size()));
        if (fresh) m.verts.emplace_back(c[0][size_t(ix)], c[1][size_t(iy)], c[2][size_t(iz)]);
        return it->second;
    };
    // Each side: its axis, which end, and the two axes across it (so their cross is outwards).
    const int sides[6][4] = {{0, 1, 1, 2}, {0, 0, 2, 1}, {1, 1, 2, 0}, {1, 0, 0, 2}, {2, 1, 0, 1}, {2, 0, 1, 0}};
    for (const auto& s : sides) {
        const int axis = s[0], u = s[2], v = s[3];
        const int fixed = s[1] ? int(c[axis].size()) - 1 : 0;
        for (size_t i = 0; i + 1 < c[u].size(); ++i)
            for (size_t j = 0; j + 1 < c[v].size(); ++j) {
                auto P = [&](size_t a, size_t b) {
                    int idx[3];
                    idx[axis] = fixed, idx[u] = int(a), idx[v] = int(b);
                    return point(idx[0], idx[1], idx[2]);
                };
                m.faces.push_back({P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)});
            }
    }
    return m;
}
} // namespace

Mesh makeRoundedBox(glm::vec3 size, float radius, int segments) {
    const glm::vec3 h = size * 0.5f;
    radius = std::clamp(radius, 0.0f, std::min({h.x, h.y, h.z}));
    segments = std::clamp(segments, 1, 16);
    std::vector<float> c[3];
    for (int a = 0; a < 3; ++a) {
        // Across the rounded strip at each end: steps at even angles (the 45° line always there,
        // where it turns from one side onto the next).
        std::vector<float> angles;
        for (int k = 0; k <= segments; ++k)
            if (float(k) * 90.0f / float(segments) < 45.0f - 1e-3f) angles.push_back(float(k) * 90.0f / float(segments));
        angles.push_back(45.0f);
        std::vector<float> lo;
        for (auto it = angles.rbegin(); it != angles.rend(); ++it) lo.push_back(-h[a] + radius - radius * std::tan(glm::radians(*it)));
        std::vector<float>& v = c[a];
        for (float x : lo) v.push_back(x);
        for (auto it = lo.rbegin(); it != lo.rend(); ++it) v.push_back(-*it);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end(), [](float p, float q) { return std::abs(p - q) < 1e-6f; }), v.end());
    }
    Mesh m = gridBox(c);
    if (radius > 1e-6f) {
        const glm::vec3 inner = h - glm::vec3(radius);
        for (glm::vec3& p : m.verts) {
            const glm::vec3 core = glm::clamp(p, -inner, inner);
            const glm::vec3 d = p - core;
            if (glm::length(d) > 1e-9f) p = core + glm::normalize(d) * radius;
        }
    }
    mergeByDistance(m, 1e-6f);
    return m;
}

Mesh lathe(const std::vector<glm::vec2>& profile, int sides, bool closed) {
    sides = std::max(sides, 3);
    Mesh m;
    std::vector<std::vector<int>> rings;
    for (glm::vec2 p : profile) {
        std::vector<int> ring;
        if (p.x < 1e-6f) {
            ring.assign(size_t(sides), int(m.verts.size()));
            m.verts.emplace_back(0.0f, 0.0f, p.y);
        } else
            for (int i = 0; i < sides; ++i) {
                const float a = 2 * kPi * float(i) / float(sides);
                ring.push_back(int(m.verts.size()));
                m.verts.emplace_back(p.x * std::cos(a), p.x * std::sin(a), p.y);
            }
        rings.push_back(ring);
    }
    const size_t n = rings.size();
    for (size_t k = 0; k + (closed ? 0 : 1) < n; ++k) {
        const auto &lo = rings[k], &hi = rings[(k + 1) % n];
        for (int i = 0; i < sides; ++i) {
            const size_t a = size_t(i), b = size_t((i + 1) % sides);
            std::vector<int> f{lo[a], lo[b], hi[b], hi[a]};
            f.erase(std::unique(f.begin(), f.end()), f.end());
            while (f.size() > 1 && f.front() == f.back()) f.pop_back();
            if (f.size() >= 3) m.faces.push_back(f);
        }
    }
    if (!closed) {
        if (profile.front().x >= 1e-6f) m.faces.emplace_back(rings.front().rbegin(), rings.front().rend());
        if (profile.back().x >= 1e-6f) m.faces.push_back(rings.back());
    }
    return m;
}

namespace {
float S(const std::vector<float>& s, size_t i) { return s[i]; }
int N(const std::vector<float>& s, size_t i) { return int(std::lround(s[i])); }

std::vector<glm::vec2> arc(glm::vec2 centre, float r, float from, float to, int steps) {
    std::vector<glm::vec2> p;
    for (int k = 0; k <= steps; ++k) {
        const float a = glm::radians(from + (to - from) * float(k) / float(steps));
        p.push_back(centre + glm::vec2(std::cos(a), std::sin(a)) * r);
    }
    return p;
}

const std::vector<ShapeRecipe> kRecipes = {
    // ── Basic ──
    {"cube", "Cube", false, false, {{"Width", 1, 0.001f, 100}, {"Depth", 1, 0.001f, 100}, {"Height", 1, 0.001f, 100}},
     [](const std::vector<float>& s) {
         Mesh m = makeCube(1.0f);
         transform(m, at({0, 0, 0}, {S(s, 1), S(s, 0), S(s, 2)}));
         return m;
     }},
    {"rounded_cube", "Rounded cube", false, true,
     {{"Width", 1, 0.001f, 100}, {"Depth", 1, 0.001f, 100}, {"Height", 1, 0.001f, 100}, {"Roundness", 0.1f, 0, 50}, {"Steps", 4, 1, 12, true}},
     [](const std::vector<float>& s) { return makeRoundedBox({S(s, 1), S(s, 0), S(s, 2)}, S(s, 3), N(s, 4)); }},
    {"plane", "Plane", false, false, {{"Width", 2, 0.001f, 1000}, {"Depth", 2, 0.001f, 1000}, {"Cuts", 0, 0, 64, true}},
     [](const std::vector<float>& s) {
         Mesh m;
         const int n = N(s, 2) + 1;
         for (int j = 0; j <= n; ++j)
             for (int i = 0; i <= n; ++i) m.verts.emplace_back((float(j) / float(n) - 0.5f) * S(s, 1), (float(i) / float(n) - 0.5f) * S(s, 0), 0.0f);
         for (int j = 0; j < n; ++j)
             for (int i = 0; i < n; ++i) {
                 const int a = j * (n + 1) + i;
                 m.faces.push_back({a, a + n + 1, a + n + 2, a + 1});
             }
         return m;
     }},
    {"circle", "Circle", false, false, {{"Sides", 32, 3, 256, true}, {"Radius", 0.5f, 0.001f, 100}},
     [](const std::vector<float>& s) {
         Mesh m;
         std::vector<int> f;
         for (int i = 0; i < N(s, 0); ++i) {
             const float a = 2 * kPi * float(i) / float(N(s, 0));
             f.push_back(int(m.verts.size()));
             m.verts.emplace_back(std::cos(a) * S(s, 1), std::sin(a) * S(s, 1), 0.0f);
         }
         m.faces.push_back(f);
         return m;
     }},
    {"cylinder", "Cylinder", false, true, {{"Sides", 24, 3, 256, true}, {"Radius", 0.5f, 0.001f, 100}, {"Height", 1, 0.001f, 100}},
     [](const std::vector<float>& s) { return makeCylinder(N(s, 0), S(s, 1), S(s, 2)); }},
    {"tube", "Tube (pipe)", false, true,
     {{"Sides", 24, 3, 256, true}, {"Outside radius", 0.5f, 0.001f, 100}, {"Wall", 0.08f, 0.001f, 100}, {"Height", 1, 0.001f, 100}},
     [](const std::vector<float>& s) {
         const float r = S(s, 1), in = std::max(r - S(s, 2), 0.001f), h = S(s, 3);
         return lathe({{r, -h / 2}, {r, h / 2}, {in, h / 2}, {in, -h / 2}}, N(s, 0), true);
     }},
    {"cone", "Cone", false, true,
     {{"Sides", 24, 3, 256, true}, {"Bottom radius", 0.5f, 0, 100}, {"Top radius", 0, 0, 100}, {"Height", 1, 0.001f, 100}},
     [](const std::vector<float>& s) { return makeFrustum(N(s, 0), std::max(S(s, 1), 0.001f), S(s, 2), S(s, 3)); }},
    {"pyramid", "Pyramid", false, false, {{"Width", 1, 0.001f, 100}, {"Height", 1, 0.001f, 100}},
     [](const std::vector<float>& s) {
         Mesh m = makeCone(4, S(s, 0) * 0.70710678f, S(s, 1));
         transform(m, glm::rotate(glm::mat4(1.0f), kPi / 4, glm::vec3(0, 0, 1)));
         return m;
     }},
    {"sphere", "Sphere", false, true, {{"Segments", 24, 3, 256, true}, {"Rings", 12, 2, 128, true}, {"Radius", 0.5f, 0.001f, 100}},
     [](const std::vector<float>& s) { return makeSphere(N(s, 0), N(s, 1), S(s, 2)); }},
    {"capsule", "Capsule", false, true, {{"Sides", 24, 3, 256, true}, {"Radius", 0.25f, 0.001f, 100}, {"Height", 1, 0.002f, 100}},
     [](const std::vector<float>& s) {
         const float r = std::min(S(s, 1), S(s, 2) / 2), h = S(s, 2);
         std::vector<glm::vec2> p;
         for (auto q : arc({0, -h / 2 + r}, r, -90, 0, 6)) p.push_back(q);
         for (auto q : arc({0, h / 2 - r}, r, 0, 90, 6)) p.push_back(q);
         for (auto& q : p) q.x = std::max(q.x, 0.0f);
         return lathe(p, N(s, 0));
     }},
    {"torus", "Torus (ring)", false, true,
     {{"Segments", 32, 3, 256, true}, {"Sides", 12, 3, 128, true}, {"Radius", 0.5f, 0.001f, 100}, {"Thickness", 0.15f, 0.001f, 100}},
     [](const std::vector<float>& s) { return makeTorus(N(s, 0), N(s, 1), S(s, 2), S(s, 3)); }},
    {"wedge", "Wedge (ramp)", false, false, {{"Width", 1, 0.001f, 100}, {"Length", 1, 0.001f, 100}, {"Height", 0.5f, 0.001f, 100}},
     [](const std::vector<float>& s) {
         const float l = S(s, 1), h = S(s, 2);
         return sideProfile({{-l / 2, 0}, {l / 2, 0}, {-l / 2, h}}, S(s, 0));
     }},
    {"stairs", "Stairs", false, false,
     {{"Steps", 6, 1, 100, true}, {"Width", 1, 0.001f, 100}, {"Length", 1.6f, 0.001f, 100}, {"Height", 1.0f, 0.001f, 100}},
     [](const std::vector<float>& s) {
         const int n = N(s, 0);
         const float L = S(s, 2), H = S(s, 3), d = L / float(n), h = H / float(n);
         std::vector<glm::vec2> p{{0, 0}, {L, 0}, {L, H}};
         for (int k = n - 1; k >= 1; --k) p.push_back({float(k) * d, float(k + 1) * h}), p.push_back({float(k) * d, float(k) * h});
         p.push_back({0, h});
         for (auto& q : p) q.x -= L / 2;
         return sideProfile(p, S(s, 1));
     }},
    {"arch", "Arch", false, false,
     {{"Width", 1.2f, 0.01f, 100}, {"Height", 2.2f, 0.01f, 100}, {"Thickness", 0.15f, 0.001f, 50}, {"Depth", 0.3f, 0.001f, 100}, {"Steps", 12, 2, 128, true}},
     [](const std::vector<float>& s) {
         const float W = S(s, 0) / 2, H = S(s, 1), t = std::min(S(s, 2), W * 0.9f), r = W - t, spring = std::max(H - t - r, 0.0f);
         std::vector<glm::vec2> p{{-W, 0}, {-W, H}, {W, H}, {W, 0}, {W - t, 0}};
         for (auto q : arc({0, spring}, r, 0, 180, N(s, 4))) p.push_back(q);
         p.push_back({-W + t, 0});
         // (drawn across X; turned so the opening faces forward)
         Mesh m = sideProfile(p, S(s, 3));
         transform(m, glm::rotate(glm::mat4(1.0f), kPi / 2, glm::vec3(0, 0, 1)));
         return m;
     }},
    // ── Starter models ──
    {"table", "Table", true, false,
     {{"Width", 1.2f, 0.05f, 20}, {"Depth", 0.7f, 0.05f, 20}, {"Height", 0.75f, 0.05f, 5}, {"Top thickness", 0.04f, 0.005f, 1}, {"Legs", 0.05f, 0.005f, 1}},
     [](const std::vector<float>& s) {
         const float w = S(s, 0) / 2, d = S(s, 1) / 2, h = S(s, 2), t = S(s, 3), l = S(s, 4), in = l;
         Mesh m;
         addBox(m, {-d, -w, h - t}, {d, w, h});
         for (int i = 0; i < 4; ++i) {
             const float x = (i & 1 ? 1 : -1) * (d - in - l / 2), y = (i & 2 ? 1 : -1) * (w - in - l / 2);
             addBox(m, {x - l / 2, y - l / 2, 0}, {x + l / 2, y + l / 2, h - t});
         }
         return m;
     }},
    {"chair", "Chair", true, false,
     {{"Width", 0.45f, 0.05f, 5}, {"Depth", 0.45f, 0.05f, 5}, {"Seat height", 0.46f, 0.05f, 3}, {"Back height", 0.45f, 0, 3}, {"Legs", 0.04f, 0.005f, 1}},
     [](const std::vector<float>& s) {
         const float w = S(s, 0) / 2, d = S(s, 1) / 2, h = S(s, 2), b = S(s, 3), l = S(s, 4), t = 0.04f;
         Mesh m;
         addBox(m, {-d, -w, h - t}, {d, w, h});
         for (int i = 0; i < 4; ++i) {
             const float x = (i & 1 ? 1 : -1) * (d - l / 2), y = (i & 2 ? 1 : -1) * (w - l / 2);
             addBox(m, {x - l / 2, y - l / 2, 0}, {x + l / 2, y + l / 2, h - t});
         }
         if (b > 0) { // the back, at the chair's back (−X): two posts and a board
             for (int i = 0; i < 2; ++i) {
                 const float y = (i ? 1 : -1) * (w - l / 2);
                 addBox(m, {-d, y - l / 2, h}, {-d + l, y + l / 2, h + b});
             }
             addBox(m, {-d, -w, h + b * 0.55f}, {-d + l * 0.8f, w, h + b});
         }
         return m;
     }},
    {"shelf", "Shelf", true, false,
     {{"Width", 0.8f, 0.05f, 20}, {"Depth", 0.3f, 0.05f, 5}, {"Height", 1.8f, 0.05f, 10}, {"Shelves", 5, 2, 30, true}, {"Board", 0.02f, 0.003f, 0.5f}},
     [](const std::vector<float>& s) {
         const float w = S(s, 0) / 2, d = S(s, 1) / 2, h = S(s, 2), t = S(s, 4);
         const int n = N(s, 3);
         Mesh m;
         addBox(m, {-d, -w, 0}, {d, -w + t, h});
         addBox(m, {-d, w - t, 0}, {d, w, h});
         addBox(m, {-d, -w + t, 0}, {-d + t, w - t, h}); // the back
         for (int k = 0; k < n; ++k) {
             const float z = (h - t) * float(k) / float(n - 1);
             addBox(m, {-d + t, -w + t, z}, {d, w - t, z + t});
         }
         return m;
     }},
    {"barrel", "Barrel", true, true, {{"Sides", 24, 6, 128, true}, {"Radius", 0.3f, 0.01f, 10}, {"Height", 0.9f, 0.01f, 10}, {"Bulge", 0.05f, 0, 1}},
     [](const std::vector<float>& s) {
         std::vector<glm::vec2> p;
         const float r = S(s, 1), h = S(s, 2);
         p.push_back({0, 0});
         for (int k = 0; k <= 8; ++k) {
             const float t = float(k) / 8.0f;
             p.push_back({r + S(s, 3) * std::sin(t * kPi), t * h});
         }
         p.push_back({0, h});
         Mesh m = lathe(p, N(s, 0));
         transform(m, at({0, 0, -h / 2}));
         return m;
     }},
    {"bottle", "Bottle", true, true, {{"Sides", 24, 6, 128, true}, {"Radius", 0.04f, 0.005f, 2}, {"Height", 0.3f, 0.02f, 5}, {"Neck", 0.013f, 0.002f, 1}},
     [](const std::vector<float>& s) {
         const float r = S(s, 1), h = S(s, 2), n = std::min(S(s, 3), r);
         std::vector<glm::vec2> p{{0, 0}, {r * 0.92f, 0}, {r, h * 0.03f}, {r, h * 0.6f}, {r * 0.75f, h * 0.7f}, {n, h * 0.8f}, {n, h * 0.97f},
                                  {n * 1.2f, h * 0.975f}, {n * 1.2f, h}, {0, h}};
         Mesh m = lathe(p, N(s, 0));
         transform(m, at({0, 0, -h / 2}));
         return m;
     }},
    {"pillar", "Pillar", true, true, {{"Sides", 16, 4, 128, true}, {"Radius", 0.2f, 0.01f, 10}, {"Height", 3, 0.1f, 50}},
     [](const std::vector<float>& s) {
         const float r = S(s, 1), h = S(s, 2), slab = r * 0.5f;
         Mesh m;
         addBox(m, {-r * 1.4f, -r * 1.4f, 0}, {r * 1.4f, r * 1.4f, slab});
         addBox(m, {-r * 1.4f, -r * 1.4f, h - slab}, {r * 1.4f, r * 1.4f, h});
         append(m, makeCylinder(N(s, 0), r, h - 2 * slab), at({0, 0, h / 2}));
         return m;
     }},
    {"door_frame", "Door frame", true, false,
     {{"Width", 0.9f, 0.1f, 10}, {"Height", 2.1f, 0.1f, 10}, {"Frame", 0.08f, 0.005f, 1}, {"Depth", 0.15f, 0.005f, 2}},
     [](const std::vector<float>& s) {
         const float W = S(s, 0) / 2 + S(s, 2), H = S(s, 1) + S(s, 2), t = S(s, 2);
         Mesh m = sideProfile({{-W, 0}, {-W, H}, {W, H}, {W, 0}, {W - t, 0}, {W - t, H - t}, {-W + t, H - t}, {-W + t, 0}}, S(s, 3));
         transform(m, glm::rotate(glm::mat4(1.0f), kPi / 2, glm::vec3(0, 0, 1)));
         return m;
     }},
    {"crate", "Crate", true, false, {{"Size", 0.6f, 0.05f, 10}, {"Frame", 0.06f, 0.005f, 1}},
     [](const std::vector<float>& s) {
         const float h = S(s, 0) / 2, f = std::min(S(s, 1), h * 0.45f);
         Mesh m = makeCube(S(s, 0));
         std::set<int> all;
         for (int i = 0; i < 6; ++i) all.insert(i);
         insetFaces(m, all, 1.0f - (h - f) / h); // a frame round each side, the panels set in
         extrudeFaces(m, {0}, -f * 0.3f);
         for (int i = 1; i < 6; ++i) extrudeFaces(m, {i}, -f * 0.3f);
         return m;
     }},
};
} // namespace

const std::vector<ShapeRecipe>& shapeRecipes() { return kRecipes; }

const ShapeRecipe* findRecipe(std::string_view id) {
    for (const ShapeRecipe& r : kRecipes)
        if (id == r.id) return &r;
    return nullptr;
}

std::vector<float> defaultSettings(const ShapeRecipe& r) {
    std::vector<float> s;
    for (const ShapeSetting& p : r.settings) s.push_back(p.value);
    return s;
}

Mesh makeFromRecipe(const ShapeRecipe& r, std::vector<float> s) {
    for (size_t i = 0; i < r.settings.size(); ++i) {
        if (i >= s.size()) s.push_back(r.settings[i].value);
        s[i] = std::clamp(s[i], r.settings[i].min, r.settings[i].max);
        if (r.settings[i].whole) s[i] = std::round(s[i]);
    }
    Mesh m = r.make(s);
    // Standing on the ground at the origin.
    const glm::vec3 lo = boundsMin(m), hi = boundsMax(m);
    transform(m, at({-(lo.x + hi.x) / 2, -(lo.y + hi.y) / 2, -lo.z}));
    return m;
}

} // namespace fry
