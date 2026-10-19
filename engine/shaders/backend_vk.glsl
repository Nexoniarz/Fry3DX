// Vulkan: one global descriptor array of textures, separate samplers, push constants.
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 1) uniform sampler u_sampler;
layout(set = 0, binding = 2) uniform sampler u_shadowSampler; // depth compare, linear: bilinear PCF
layout(set = 0, binding = 3) uniform sampler u_clampSampler;  // render targets, for the screen-space passes
// Post passes (#define KEEL_POST before including): every lookup clamps at the edges.
#ifdef KEEL_POST
#define KEEL_SAMPLER u_clampSampler
#else
#define KEEL_SAMPLER u_sampler
#endif

#define PUSH_BLOCK layout(push_constant) uniform Push
#define VERTEX_ID gl_VertexIndex

vec4 sampleTex(uint index, vec2 uv) {
    return texture(sampler2D(u_textures[nonuniformEXT(index)], KEEL_SAMPLER), uv);
}

ivec2 texSize(uint index) { return textureSize(sampler2D(u_textures[nonuniformEXT(index)], KEEL_SAMPLER), 0); }

vec4 fetchTex(uint index, ivec2 p) {
    return texelFetch(sampler2D(u_textures[nonuniformEXT(index)], KEEL_SAMPLER), p, 0);
}

vec4 sampleTexLod(uint index, vec2 uv, float lod) {
    return textureLod(sampler2D(u_textures[nonuniformEXT(index)], KEEL_SAMPLER), uv, lod);
}

// 1 = lit. Hardware-filtered depth compare at uv (clamped inside [lo, hi]).
float shadowTap(uint index, vec2 uv, vec2 lo, vec2 hi, float ref) {
    return textureLod(sampler2DShadow(u_textures[nonuniformEXT(index)], u_shadowSampler), vec3(clamp(uv, lo, hi), ref), 0.0);
}
