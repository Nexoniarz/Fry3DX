#version 460
#include "backend.glsl"
layout(location = 0) out vec2 v_uv;
void main() {
    v_uv = vec2((VERTEX_ID << 1) & 2, VERTEX_ID & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
#ifdef KEEL_GL
    // The window shows row 0 at the bottom in OpenGL; our images keep the view's top in row 0 (as in Vulkan).
    v_uv.y = 1.0 - v_uv.y;
#endif
}
