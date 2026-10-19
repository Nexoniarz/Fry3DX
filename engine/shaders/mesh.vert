#version 460
#include "model.glsl"

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec2 v_lightmapUv;
layout(location = 3) out vec3 v_world;

void main() {
    Vertex vx = VERTEX(VERTEX_ID);
    mat4 model = DRAW.model;
    vec4 world = model * vec4(vx.px, vx.py, vx.pz, 1.0);
    v_normal = normalize(mat3(model) * vec3(vx.nx, vx.ny, vx.nz));
    v_uv = vec2(vx.u, vx.v);
    v_lightmapUv = vec2(vx.lu, vx.lv);
    v_world = world.xyz;
    gl_Position = pc.viewProj * world;
}
