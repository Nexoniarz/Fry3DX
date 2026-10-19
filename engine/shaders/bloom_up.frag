#version 460
// Bloom, coming up: a 3×3 tent of the smaller level, added onto the larger one (blended).
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
PUSH_BLOCK {
    uint src;
    uint srcW, srcH;
    uint outW, outH;
    float radius;
} pc;
layout(location = 0) out vec4 o_color;

vec3 tap(vec2 uv) {
    vec2 h = 0.5 / vec2(pc.srcW, pc.srcH);
    return sampleTex(pc.src, clamp(uv, h, 1.0 - h)).rgb;
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(pc.outW, pc.outH);
    vec2 t = pc.radius / vec2(pc.srcW, pc.srcH);
    vec3 s = tap(uv) * 4.0;
    s += (tap(uv + vec2(-t.x, 0)) + tap(uv + vec2(t.x, 0)) + tap(uv + vec2(0, -t.y)) + tap(uv + vec2(0, t.y))) * 2.0;
    s += tap(uv + vec2(-t.x, -t.y)) + tap(uv + vec2(t.x, -t.y)) + tap(uv + vec2(-t.x, t.y)) + tap(uv + vec2(t.x, t.y));
    o_color = vec4(s / 16.0, 1.0);
}
