#version 460
// Bloom, going down: a 13-tap filter (Jimenez, "Next generation post processing in Call of
// Duty: Advanced Warfare") halving the size each step. The first step clamps fireflies.
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
PUSH_BLOCK {
    uint src;
    uint srcW, srcH;
    uint outW, outH;
    uint first;
} pc;
layout(location = 0) out vec4 o_color;

vec3 tap(vec2 uv) {
    vec2 h = 0.5 / vec2(pc.srcW, pc.srcH);
    return sampleTex(pc.src, clamp(uv, h, 1.0 - h)).rgb;
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(pc.outW, pc.outH);
    vec2 t = 1.0 / vec2(pc.srcW, pc.srcH);
    vec3 a = tap(uv + t * vec2(-2, -2)), b = tap(uv + t * vec2(0, -2)), c = tap(uv + t * vec2(2, -2));
    vec3 d = tap(uv + t * vec2(-1, -1)), e = tap(uv + t * vec2(1, -1));
    vec3 f = tap(uv + t * vec2(-2, 0)), g = tap(uv), hh = tap(uv + t * vec2(2, 0));
    vec3 i = tap(uv + t * vec2(-1, 1)), j = tap(uv + t * vec2(1, 1));
    vec3 k = tap(uv + t * vec2(-2, 2)), l = tap(uv + t * vec2(0, 2)), m = tap(uv + t * vec2(2, 2));
    vec3 sum = (d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f + hh + l) * 0.0625 + g * 0.125;
    if (pc.first != 0u) sum = min(sum, vec3(32.0)); // no single-pixel sparkle blooming the screen
    o_color = vec4(sum, 1.0);
}
