// OpenGL 4.6 + ARB_bindless_texture (enabled by the loader's preamble): every texture is a
// 64-bit handle (sampler state included) in a table, indexed like Vulkan's descriptor array.
// Depth textures also get a comparison handle in a second table, for shadows.
layout(std430, binding = 9) readonly buffer TextureHandles { uvec2 handles[]; } u_textureHandles;
layout(std430, binding = 10) readonly buffer ShadowHandles { uvec2 handles[]; } u_shadowHandles;

#define PUSH_BLOCK layout(std140, binding = 0) uniform Push
#define VERTEX_ID gl_VertexID

vec4 sampleTex(uint index, vec2 uv) {
    return texture(sampler2D(u_textureHandles.handles[index]), uv);
}

ivec2 texSize(uint index) { return textureSize(sampler2D(u_textureHandles.handles[index]), 0); }

vec4 fetchTex(uint index, ivec2 p) {
    return texelFetch(sampler2D(u_textureHandles.handles[index]), p, 0);
}

vec4 sampleTexLod(uint index, vec2 uv, float lod) {
    return textureLod(sampler2D(u_textureHandles.handles[index]), uv, lod);
}

float shadowTap(uint index, vec2 uv, vec2 lo, vec2 hi, float ref) {
    return textureLod(sampler2DShadow(u_shadowHandles.handles[index]), vec3(clamp(uv, lo, hi), ref), 0.0);
}
