#version 460
// Depth pre-pass (r_prepass): only the depth, with alpha-tested texels cut out like mesh.frag
// does, so the scene pass shades each pixel once (what's hidden behind isn't lit for nothing).
#include "backend.glsl"
#include "model.glsl"

layout(location = 1) in vec2 v_uv;

void main() {
    if (sampleTex(pc.textures & 0xFFFFu, v_uv).a < 0.5) discard;
}
