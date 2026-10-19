// Depth-aware ("bilateral") upsampling of a half-size target (ambient occlusion, reflections,
// volumetric light): the four nearest texels, each weighted by how close its depth is to this
// pixel's, so what's behind an edge doesn't bleed onto it (a plain bilinear read leaves dark or
// bright lines along corners and silhouettes).
float linearDepth(float d) {
    const float n = 2.0, f = 16384.0; // the camera's near and far planes (render/renderer.cpp)
    return d >= 1.0 ? f : n * f / (f - d * (f - n));
}
vec4 upsampleDepthAware(uint tex, uint depthTex, ivec2 px) {
    ivec2 full = texSize(depthTex), half_ = texSize(tex);
    vec2 hp = (vec2(px) + 0.5) * vec2(half_) / vec2(full) - 0.5;
    ivec2 b = ivec2(floor(hp));
    vec2 f = hp - vec2(b);
    float z = linearDepth(fetchTex(depthTex, clamp(px, ivec2(0), full - 1)).r);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 t = clamp(b + o, ivec2(0), half_ - 1);
        ivec2 tf = clamp((t * full) / half_ + (full / half_) / 2, ivec2(0), full - 1); // the full-size texel it stands for
        float zt = linearDepth(fetchTex(depthTex, tf).r);
        float bil = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float w = bil / (0.002 + abs(zt - z) / max(z, 1.0));
        sum += fetchTex(tex, t) * w;
        wsum += w;
    }
    return wsum > 0.0 ? sum / wsum : fetchTex(tex, clamp(b, ivec2(0), half_ - 1));
}
