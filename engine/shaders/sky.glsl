#ifndef KEEL_SKY_GLSL
#define KEEL_SKY_GLSL
// The sky (.ksky, engine/asset/ksky.h): equirectangular RGBM; level k is the sky as a surface
// of roughness k / top reflects it.
const float kSkyRange = 16.0;

// Radiance from direction `dir` (world space). yaw turns the photo so its sun lines up with
// the map's.
vec3 skyRadiance(uint tex, float intensity, float yaw, vec3 dir, float level) {
    float c = cos(yaw), s = sin(yaw);
    dir = vec3(c * dir.x + s * dir.y, -s * dir.x + c * dir.y, dir.z);
    vec2 uv = vec2(atan(dir.y, dir.x) * 0.15915494 + 0.5, acos(clamp(dir.z, -1.0, 1.0)) * 0.31830989);
    vec4 m = sampleTexLod(tex, uv, level);
    return m.rgb * m.a * kSkyRange * intensity;
}
#endif
