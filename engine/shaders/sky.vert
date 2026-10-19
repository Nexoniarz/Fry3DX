#version 460
// The sky: a full-screen triangle on the far plane, drawn where nothing else is (depth test
// less-or-equal against the cleared 1.0).
#include "backend.glsl"
layout(location = 0) out vec2 v_ndc;
void main() {
    vec2 uv = vec2((VERTEX_ID << 1) & 2, VERTEX_ID & 2);
    v_ndc = uv * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 1.0, 1.0);
}
