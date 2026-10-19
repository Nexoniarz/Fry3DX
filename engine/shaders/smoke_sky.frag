#version 460
// Smoke in front of the sky (Vulkan): the sky pass has no fog (its photo has its own haze), so
// smoke against open sky is laid over it here, from the fog grid's smoke-only column totals
// (froxel_main.glsl: light, and how much of what's behind gets through). Drawn on the far plane
// after the sky, where nothing else is: colour × through + light.
#include "backend.glsl"
#define KEEL_FRAME_ONLY
#include "model.glsl"
layout(buffer_reference, std430) readonly buffer SmokeSky { uvec2 f[]; };
PUSH_BLOCK {
    FrameData frame;
    SmokeSky sky; // 160 × 90 (the fog grid's columns)
} pc;
#define FRAME pc.frame
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

const ivec2 kCols = ivec2(160, 90);
vec4 column(ivec2 c) {
    uvec2 v = pc.sky.f[clamp(c.y, 0, kCols.y - 1) * kCols.x + clamp(c.x, 0, kCols.x - 1)];
    return vec4(unpackHalf2x16(v.x), unpackHalf2x16(v.y));
}

void main() {
    vec2 g = gl_FragCoord.xy / FRAME.screen.xy * vec2(kCols) - 0.5;
    ivec2 b = ivec2(floor(g));
    vec2 f = g - vec2(b);
    vec4 s = mix(mix(column(b), column(b + ivec2(1, 0)), f.x), mix(column(b + ivec2(0, 1)), column(b + ivec2(1, 1)), f.x), f.y);
    o_color = vec4(s.rgb, s.a); // (blended: src + dst × src.a)
}
