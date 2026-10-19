#version 460
// Particles: unlit, the texture times the colour (fire, sparks, flashes added on; smoke blended).
#include "backend.glsl"
PUSH_BLOCK {
    mat4 viewProj;
    vec4 posSize;
    vec4 color;
    vec4 right;
    vec4 up;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 t = sampleTex(floatBitsToUint(pc.up.w), v_uv);
    // Block compression leaves a little alpha where there should be none; bright sprites
    // would show it as a square: cut it.
    float a = clamp((t.a - 0.03) / 0.97, 0.0, 1.0);
    o_color = vec4(t.rgb * pc.color.rgb, a * pc.color.a);
}
