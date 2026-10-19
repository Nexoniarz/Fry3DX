#pragma once
// Shapes to start from, with settings you can change after adding one (until you edit its
// shape by hand): the basic ones, and starter models (a table, a chair...). All stand on the
// ground at their origin (z = 0 is their bottom), centred on it the other ways.
#include "mesh.h"

#include <string>
#include <string_view>
#include <vector>

namespace fry {

struct ShapeSetting {
    const char* name;
    float value, min, max;
    bool whole = false; // a count (sides, steps...)
};

struct ShapeRecipe {
    const char* id;    // as saved
    const char* name;  // as shown
    bool starter;      // a starter model (else a basic shape)
    bool smooth;       // looks best smooth-shaded
    std::vector<ShapeSetting> settings;
    Mesh (*make)(const std::vector<float>& s);
};

const std::vector<ShapeRecipe>& shapeRecipes();
const ShapeRecipe* findRecipe(std::string_view id);
// The recipe's mesh for these settings (missing ones take their defaults).
Mesh makeFromRecipe(const ShapeRecipe& r, std::vector<float> settings);
std::vector<float> defaultSettings(const ShapeRecipe& r);

// A box with rounded edges and corners (`radius`, in `segments` steps round each).
Mesh makeRoundedBox(glm::vec3 size, float radius, int segments);
// A shape turned round the Z axis: `profile` is (distance out, height) from the bottom up.
// Points at distance 0 become a single point; open ends get a lid; `closed` joins the last
// profile point back to the first (a ring, like a tube's cross-section).
Mesh lathe(const std::vector<glm::vec2>& profile, int sides, bool closed = false);

} // namespace fry
