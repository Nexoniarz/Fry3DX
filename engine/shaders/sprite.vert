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
const vec2 kQuad[6] = vec2[](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));
void main() {
    vec2 q = kQuad[VERTEX_ID];
    float c = cos(pc.right.w), s = sin(pc.right.w);
    vec2 r = vec2(c * q.x - s * q.y, s * q.x + c * q.y);
    vec3 world = pc.posSize.xyz + (pc.right.xyz * r.x + pc.up.xyz * r.y) * pc.posSize.w;
    v_uv = q * 0.5 + 0.5;
    v_uv.y = 1.0 - v_uv.y;
    gl_Position = pc.viewProj * vec4(world, 1.0);
}
