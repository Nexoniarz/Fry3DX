#version 460
#include "model.glsl"

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec2 v_lightmapUv;
layout(location = 3) out vec3 v_world;
// The same depth in the depth pre-pass and the scene pass (else the compiler, dropping what the pre-pass
// doesn't use, can round it differently and the scene pass fails its depth test in patches).
invariant gl_Position;

void main() {
    Vertex vx = VERTEX(VERTEX_ID);
    uvec2 sk = SKIN(VERTEX_ID);
    vec4 w = unpackUnorm4x8(sk.y);
    mat4 m = JOINT(sk.x & 0xFFu) * w.x + JOINT((sk.x >> 8) & 0xFFu) * w.y
           + JOINT((sk.x >> 16) & 0xFFu) * w.z + JOINT(sk.x >> 24) * w.w;
    vec4 world = m * vec4(vx.px, vx.py, vx.pz, 1.0);
    v_normal = normalize(mat3(m) * vec3(vx.nx, vx.ny, vx.nz));
    v_uv = vec2(vx.u, vx.v);
    v_lightmapUv = vec2(0.0);
    v_world = world.xyz;
    gl_Position = pc.viewProj * world;
}
