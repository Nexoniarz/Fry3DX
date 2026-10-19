#version 460
// Axis-aligned boxes generated from the vertex index: 36 vertices, no vertex buffer.
#include "backend.glsl"
PUSH_BLOCK {
    mat4 viewProj;
    vec4 center;  // xyz, w = texture index
    vec4 extents; // xyz = half size
    vec4 color;
} pc;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_world;

const vec3 kNormals[6] = vec3[](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0), vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
const vec2 kQuad[6] = vec2[](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));

void main() {
    int face = VERTEX_ID / 6;
    vec3 n = kNormals[face];
    vec3 t = abs(n.z) > 0.5 ? vec3(1,0,0) : vec3(0,0,1);
    vec3 b = cross(n, t);
    vec2 q = kQuad[VERTEX_ID % 6];
    v_world = pc.center.xyz + (n + t * q.x + b * q.y) * pc.extents.xyz;
    v_normal = n;
    gl_Position = pc.viewProj * vec4(v_world, 1.0);
}
