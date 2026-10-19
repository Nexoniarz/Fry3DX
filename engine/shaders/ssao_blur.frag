#version 460
// Smooths the occlusion (4×4, weighted by how close each sample's depth is to this pixel's).
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
PUSH_BLOCK {
    uint ao, depth;
    uint w, h;       // depth size
    uint outW, outH; // this target's (= the occlusion's)
    float pad0, pad1;
    vec4 proj;
} pc;
layout(location = 0) out vec4 o_ao;

float linearDepth(vec2 uv) {
    ivec2 p = clamp(ivec2(uv * vec2(pc.w, pc.h)), ivec2(0), ivec2(pc.w - 1, pc.h - 1));
    float d = fetchTex(pc.depth, p).r;
    return pc.proj.z * pc.proj.w / (pc.proj.w - d * (pc.proj.w - pc.proj.z));
}

void main() {
    ivec2 o = ivec2(gl_FragCoord.xy);
    vec2 uv = gl_FragCoord.xy / vec2(pc.outW, pc.outH);
    float z = linearDepth(uv);
    float sum = 0.0, weight = 0.0;
    for (int y = -2; y < 2; ++y)
        for (int x = -2; x < 2; ++x) {
            ivec2 q = clamp(o + ivec2(x, y), ivec2(0), ivec2(pc.outW - 1, pc.outH - 1));
            float zq = linearDepth((vec2(q) + 0.5) / vec2(pc.outW, pc.outH));
            float wgt = max(0.0, 1.0 - abs(zq - z) / (0.05 * z + 1.0));
            sum += fetchTex(pc.ao, q).r * wgt;
            weight += wgt;
        }
    o_ao = vec4(weight > 0.0 ? sum / weight : 1.0);
}
