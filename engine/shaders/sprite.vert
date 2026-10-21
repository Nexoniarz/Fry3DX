#version 460
// Particles: a camera-facing quad per sprite, from the vertex index (6 vertices, no buffer).
#include "backend.glsl"
PUSH_BLOCK {
    mat4 viewProj;
    vec4 posSize; // xyz centre, w half size
    vec4 color;   // rgb (HDR: can be bright), a
    vec4 right;   // camera right, w = rotation (radians)
    vec4 up;      // camera up, w = texture index (bits)
} pc;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_rel; // (a grid: where on it, from its middle)
const vec2 kQuad[6] = vec2[](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));
void main() {
    vec2 q = kQuad[VERTEX_ID];
    float c = cos(pc.right.w), s = sin(pc.right.w);
    vec2 r = vec2(c * q.x - s * q.y, s * q.x + c * q.y);
    vec3 world = pc.posSize.xyz + (pc.right.xyz * r.x + pc.up.xyz * r.y) * pc.posSize.w;
    if (pc.color.a < 0.0) world = pc.posSize.xyz + (pc.right.xyz * q.x + pc.up.xyz * q.y) * pc.posSize.w; // a grid: a square on its plane
    else if (pc.posSize.w < 0.0) { // a streak: right.xyz the eye, w its half width at the far end; each end's side faces the eye
        vec3 end = pc.posSize.xyz + pc.up.xyz * q.y, side = cross(pc.up.xyz, pc.right.xyz - end);
        world = end + (length(side) > 1e-9 ? normalize(side) : vec3(0.0, 0.0, 1.0)) * q.x * (q.y < 0.0 ? -pc.posSize.w : pc.right.w);
    }
    v_uv = q * 0.5 + 0.5;
    v_uv.y = 1.0 - v_uv.y;
    v_rel = pc.color.a < 0.0 ? q * pc.posSize.w : vec2(0.0);
    gl_Position = pc.viewProj * vec4(world, 1.0);
}
