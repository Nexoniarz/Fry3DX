#version 460
#include "backend.glsl"
PUSH_BLOCK {
    mat4 viewProj;
    vec4 center;
    vec4 extents;
    vec4 color;
} pc;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_world;
layout(location = 0) out vec4 o_color;

void main() {
    // World-space UVs: textures tile every 128 units regardless of box size.
    vec3 n = abs(v_normal);
    vec2 uv = (n.x > 0.5 ? v_world.yz : n.y > 0.5 ? v_world.xz : v_world.xy) / 128.0;
    vec4 tex = sampleTex(uint(pc.center.w), uv);
    o_color = vec4(pc.color.rgb * tex.rgb * sunLight(v_normal), 1.0);
}
