#ifndef KEEL_BACKEND_GLSL
#define KEEL_BACKEND_GLSL
// What every shader includes: textures and the push block, for the backend being built.
// Vulkan: bindless descriptor array + push constants. OpenGL 4.6: ARB_bindless_texture handles
// in storage buffers + a uniform block. Shader code only uses the names defined here.
#ifdef KEEL_GL
#include "backend_gl.glsl"
#else
#include "backend_vk.glsl"
#endif

float sunLight(vec3 n) {
    const vec3 sun = normalize(vec3(0.4, 0.3, 0.85));
    return 0.3 + 0.9 * max(dot(n, sun), 0.0);
}
#endif
