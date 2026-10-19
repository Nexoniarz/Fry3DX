#version 460
// The hierarchical Z-buffer's base: the scene's depth shrunk to a small grid, each texel the
// farthest depth under it. The CPU reads it back and builds the rest of the pyramid (see
// Renderer::occluded). Rows match the depth buffer's on both backends.
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
PUSH_BLOCK {
    uint depth;      // texture slot of the scene's depth
    uint srcW, srcH; // its size
    uint dstW, dstH; // this grid's
} pc;

layout(location = 0) out vec4 o_depth;

void main() {
    ivec2 o = ivec2(gl_FragCoord.xy);
    vec2 scale = vec2(pc.srcW, pc.srcH) / vec2(pc.dstW, pc.dstH);
    ivec2 lo = ivec2(floor(vec2(o) * scale));
    ivec2 hi = min(ivec2(ceil(vec2(o + 1) * scale)), ivec2(pc.srcW, pc.srcH));
    float far = 0.0;
    for (int y = lo.y; y < hi.y; ++y)
        for (int x = lo.x; x < hi.x; ++x) far = max(far, fetchTex(pc.depth, ivec2(x, y)).r);
    o_depth = vec4(far);
}
