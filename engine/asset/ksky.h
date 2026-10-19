#pragma once
// .ksky: a sky for the renderer, compiled by assetc from an equirectangular .hdr.
//   KskyHeader
//   `levels` images of width >> k × height >> k RGBM8 pixels (rows top-down), largest first
// Directions are world space (+Z up): u = atan2(d.y, d.x) / 2π + 0.5, v = acos(d.z) / π.
// A pixel's linear radiance is rgb · a · kSkyRange. The sky is normalised so its upper half
// averages 1 (the engine scales it by the map's sky intensity), and the sun is taken out of
// it (the engine draws its own, matching the shadows). Level k is the sky as a surface of
// roughness k / (levels - 1) reflects it (GGX prefiltered), for image-based reflections.
#include <cstdint>

namespace keel {

inline constexpr uint32_t kKskyVersion = 1;
inline constexpr float kSkyRange = 16.0f;

struct KskyHeader {
    char magic[4]; // "KSKY"
    uint32_t version;
    uint32_t width, height, levels;
    float sunDir[3]; // towards where the sun was in the photo
    float sh[27];    // 9 RGB spherical harmonics of the light it casts: Σ shᵢ·Yᵢ(n) = irradiance / π
};

} // namespace keel
