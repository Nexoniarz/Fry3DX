// The fallback renderer for old GPUs: OpenGL 4.0 or 3.1, GLSL 1.40, nothing newer — bound
// textures, vertex attributes, one uniform block for the frame, a texture buffer for skinning.
// Two tiers, cut down from the main renderer:
//   4.0  lightmaps, sun with cascaded shadows (PCF), HDR sky, fog, normal maps, GGX highlights,
//        sky reflections, the 16 nearest dynamic lights (no shadows)
//   3.1  lightmaps, sun with shadows (one tap), sky, fog; flat surfaces, no highlights or lights
// Neither has clustered lights, light shadows or occlusion culling. Depth is GL's -1..1 here
// (no glClipControl), so vertex shaders turn the 0..1 matrices' z into it.
#include "render/backend.h"
#include "core/log.h"
#include "platform/window.h"
#include "render/screenshot.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace keel {
namespace {

// Entry points from OpenGL 3.1 (no DSA, no sampler objects, no storage buffers).
#define KEEL_GLX_FUNCTIONS(X)                                                                                        \
    X(const GLubyte*, GetString, (GLenum))                                                                           \
    X(void, GetIntegerv, (GLenum, GLint*))                                                                           \
    X(void, Enable, (GLenum))                                                                                        \
    X(void, Disable, (GLenum))                                                                                       \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                              \
    X(void, Clear, (GLbitfield))                                                                                     \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                        \
    X(void, ClearDepth, (GLdouble))                                                                                  \
    X(void, DepthFunc, (GLenum))                                                                                     \
    X(void, DepthMask, (GLboolean))                                                                                  \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                     \
    X(void, PolygonOffset, (GLfloat, GLfloat))                                                                       \
    X(void, PixelStorei, (GLenum, GLint))                                                                            \
    X(void, DrawBuffer, (GLenum))                                                                                    \
    X(void, ReadBuffer, (GLenum))                                                                                    \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                                     \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                                          \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                                                 \
    X(void, BindBuffer, (GLenum, GLuint))                                                                            \
    X(void, BindBufferBase, (GLenum, GLuint, GLuint))                                                                \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                                   \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                                              \
    X(void, GenTextures, (GLsizei, GLuint*))                                                                         \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                                \
    X(void, BindTexture, (GLenum, GLuint))                                                                           \
    X(void, ActiveTexture, (GLenum))                                                                                 \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))                \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                                  \
    X(void, TexBuffer, (GLenum, GLenum, GLuint))                                                                     \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                                     \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                                            \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                                       \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                           \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                                      \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                                     \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                                            \
    X(void, BindVertexArray, (GLuint))                                                                               \
    X(void, EnableVertexAttribArray, (GLuint))                                                                       \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                           \
    X(void, VertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void*))                                     \
    X(void, VertexAttribI4ui, (GLuint, GLuint, GLuint, GLuint, GLuint))                                              \
    X(void, VertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat))                                            \
    X(void, DrawElements, (GLenum, GLsizei, GLenum, const void*))                                                    \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                                                    \
    X(GLuint, CreateShader, (GLenum))                                                                                \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                                     \
    X(void, CompileShader, (GLuint))                                                                                 \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                                   \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                  \
    X(void, DeleteShader, (GLuint))                                                                                  \
    X(GLuint, CreateProgram, (void))                                                                                 \
    X(void, AttachShader, (GLuint, GLuint))                                                                          \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar*))                                                     \
    X(void, BindFragDataLocation, (GLuint, GLuint, const GLchar*))                                                   \
    X(void, LinkProgram, (GLuint))                                                                                   \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                                  \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                 \
    X(void, DeleteProgram, (GLuint))                                                                                 \
    X(void, UseProgram, (GLuint))                                                                                    \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                                            \
    X(GLuint, GetUniformBlockIndex, (GLuint, const GLchar*))                                                         \
    X(void, UniformBlockBinding, (GLuint, GLuint, GLuint))                                                           \
    X(void, Uniform1i, (GLint, GLint))                                                                               \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*))                                                            \
    X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))

namespace glx {
#define KEEL_GLX_DECLARE(ret, name, params) using name##_t = ret(APIENTRY*) params; name##_t name = nullptr;
KEEL_GLX_FUNCTIONS(KEEL_GLX_DECLARE)
#undef KEEL_GLX_DECLARE
bool load() {
    std::string missing;
#define KEEL_GLX_LOAD(ret, name, params)                                                \
    name = reinterpret_cast<name##_t>(SDL_GL_GetProcAddress("gl" #name));               \
    if (!name) missing += " gl" #name;
    KEEL_GLX_FUNCTIONS(KEEL_GLX_LOAD)
#undef KEEL_GLX_LOAD
    if (!missing.empty()) log::warn("OpenGL (legacy): missing functions:{}", missing);
    return missing.empty();
}
} // namespace glx

// ── Shaders (GLSL 1.40; TIER is 1 for GL 3.1, 2 for 4.0) ──
const char* kFrameBlock = R"(
layout(std140) uniform Frame { // (GLSL 1.40: no instance name, so the members say F_)
    mat4 F_cascades[4];
    vec4 F_splits, F_cascadeTexel, F_sunDir, F_sunColor, F_shadow, F_camPos, F_sky, F_fog, F_lightCount;
    vec4 F_lights[64]; // 16 × (pos radius, colour, spot dir cos-outer, cos-inner)
};
)";

const char* kMeshVert = R"(
in vec3 a_pos; in vec3 a_normal; in vec2 a_uv; in vec2 a_lmuv; in uvec4 a_joints; in vec4 a_weights;
uniform mat4 u_viewProj, u_model;
uniform int u_skinned, u_firstJoint;
uniform samplerBuffer u_joints;
out vec3 v_normal, v_world; out vec2 v_uv, v_lmuv;
mat4 joint(uint j) {
    int b = (u_firstJoint + int(j)) * 4;
    return mat4(texelFetch(u_joints, b), texelFetch(u_joints, b + 1), texelFetch(u_joints, b + 2), texelFetch(u_joints, b + 3));
}
void main() {
    mat4 m = u_model;
    if (u_skinned != 0)
        m = joint(a_joints.x) * a_weights.x + joint(a_joints.y) * a_weights.y + joint(a_joints.z) * a_weights.z + joint(a_joints.w) * a_weights.w;
    vec4 w = m * vec4(a_pos, 1.0);
    v_world = w.xyz;
    v_normal = mat3(m) * a_normal;
    v_uv = a_uv;
    v_lmuv = a_lmuv;
    gl_Position = u_viewProj * w;
    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w; // 0..1 → GL's -1..1
}
)";

const char* kDepthFrag = R"(
out vec4 o_color;
void main() { o_color = vec4(1.0); }
)";

const char* kMeshFrag = R"(
in vec3 v_normal, v_world; in vec2 v_uv, v_lmuv;
uniform sampler2D u_albedo, u_lightmap, u_material, u_skyVis, u_sky;
uniform sampler2DShadow u_sunShadow;
uniform vec4 u_tint, u_params, u_plane, u_flags; // flags: x lightmap, y material, z sky visibility
uniform vec4 u_cube[6];
out vec4 o_color;

vec3 ambientCube(vec3 n) {
    vec3 w = n * n;
    return w.x * (n.x > 0.0 ? u_cube[0].rgb : u_cube[1].rgb) + w.y * (n.y > 0.0 ? u_cube[2].rgb : u_cube[3].rgb)
         + w.z * (n.z > 0.0 ? u_cube[4].rgb : u_cube[5].rgb);
}
float cascadeShadow(vec3 n, vec3 fwd) {
    if (F_sunColor.w < 0.5) return 1.0;
    float depth = dot(v_world - F_camPos.xyz, fwd);
    if (depth > F_splits.w) return 1.0;
    int c = depth < F_splits.x ? 0 : depth < F_splits.y ? 1 : depth < F_splits.z ? 2 : 3;
    vec4 p = F_cascades[c] * vec4(v_world + n * F_cascadeTexel[c] * 1.5, 1.0);
    vec2 uv = p.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || p.z > 1.0) return 1.0;
    vec2 base = vec2(c & 1, c >> 1) * 0.5;
    vec2 at = base + uv * 0.5;
    float t = F_shadow.x;
#if TIER >= 2
    float lit = 0.0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) lit += texture(u_sunShadow, vec3(clamp(at + (vec2(x, y) - 0.5) * t, base + t, base + 0.5 - t), p.z - 0.0002));
    return lit * 0.25;
#else
    return texture(u_sunShadow, vec3(at, p.z - 0.0003));
#endif
}
vec3 sky(vec3 dir, float level) {
    float c = cos(F_sky.w), s = sin(F_sky.w);
    dir = vec3(c * dir.x + s * dir.y, -s * dir.x + c * dir.y, dir.z);
    vec4 m = textureLod(u_sky, vec2(atan(dir.y, dir.x) * 0.15915494 + 0.5, acos(clamp(dir.z, -1.0, 1.0)) * 0.31830989), level);
    return m.rgb * m.a * 16.0 * F_sky.y;
}
#if TIER >= 2
mat3 cotangentFrame(vec3 n, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x, b = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    return mat3(t * invmax, b * invmax, n);
}
vec3 g_f0 = vec3(0.04); // reflectance at normal incidence: 0.04, or a metal's albedo
vec3 specGGX(vec3 n, vec3 l, vec3 v, float rough) {
    vec3 h = normalize(l + v);
    float a = max(rough * rough, 0.002), a2 = a * a;
    float ndl = max(dot(n, l), 1e-4), ndv = max(dot(n, v), 1e-4), ndh = max(dot(n, h), 0.0), vdh = max(dot(v, h), 0.0);
    float q = ndh * ndh * (a2 - 1.0) + 1.0;
    float vis = 0.5 / (ndl * sqrt(ndv * ndv * (1.0 - a2) + a2) + ndv * sqrt(ndl * ndl * (1.0 - a2) + a2));
    return min(a2 / (3.14159265 * q * q) * vis * (g_f0 + (1.0 - g_f0) * pow(1.0 - vdh, 5.0)) * 3.14159265, vec3(64.0));
}
vec2 envBRDF(float rough, float ndv) {
    vec4 r = rough * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}
#endif

void main() {
    vec4 albedo = texture(u_albedo, v_uv);
    if (albedo.a < 0.5) discard;
    if (u_params.z > 0.5 && dot(v_world, u_plane.xyz) > u_plane.w) discard; // liquid cut
    vec3 base = albedo.rgb * u_tint.rgb;
    vec3 n = normalize(v_normal), geo = n;
    vec3 view = normalize(F_camPos.xyz - v_world);
    vec3 fwd = F_lightCount.yzw; // the camera's forward (packed here)
    float gloss = 0.15;
#if TIER >= 2
    mat3 tbn = cotangentFrame(n, v_world, v_uv);
    if (u_flags.y > 0.5) {
        vec4 m = texture(u_material, v_uv);
        vec2 xy = m.xy * 2.0 - 1.0; // Z rebuilt; B is metalness
        n = normalize(tbn * vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0))));
        gloss = m.w;
        g_f0 = mix(vec3(0.04), base, m.z);
        base *= 1.0 - m.z;
    }
#endif
    vec3 sunDir = F_sunDir.xyz;
    float facing = dot(geo, sunDir) > 0.0 ? max(dot(n, sunDir), 0.0) : 0.0;
    vec3 sun = F_sunColor.rgb * F_sunColor.w * facing;
    float vis = facing > 0.0 ? cascadeShadow(geo, fwd) : 0.0;
    bool realtime = F_shadow.z > 0.5;
    vec3 light, env = vec3(0.0);
    float skyVis = 0.0;
    if (u_flags.x > 0.5) {
        vec4 rgbm = texture(u_lightmap, v_lmuv);
        env = rgbm.rgb * rgbm.a * 8.0;
        light = realtime ? env + sun * vis : env * (1.0 - F_shadow.y * (1.0 - vis) * facing * F_sunColor.w);
        if (u_flags.z > 0.5) skyVis = texture(u_skyVis, v_lmuv).r;
    } else if (u_params.y > 0.5) {
        env = ambientCube(n);
        light = env + sun * (realtime ? vis : u_params.x * vis);
        skyVis = 0.5 * u_params.x;
    } else {
        light = vec3(0.3 + 0.9 * max(dot(n, normalize(vec3(0.4, 0.3, 0.85))), 0.0));
    }
    vec3 color = base * light;
#if TIER >= 2
    float rough = clamp(1.0 - gloss, 0.05, 1.0);
    color += F_sunColor.rgb * F_sunColor.w * facing * vis * specGGX(n, sunDir, view, rough);
    for (int i = 0; i < int(F_lightCount.x); ++i) {
        vec4 pr = F_lights[i * 4], lc = F_lights[i * 4 + 1], sp = F_lights[i * 4 + 2], ex = F_lights[i * 4 + 3];
        vec3 toLight = pr.xyz - v_world;
        float dist = length(toLight);
        if (dist >= pr.w) continue;
        vec3 L = toLight / max(dist, 0.001);
        float ndl = dot(n, L);
        if (ndl <= 0.0) continue;
        float a = 1.0 - dist / pr.w, atten = a * a;
        if (sp.w > -1.5) atten *= smoothstep(sp.w, ex.x, dot(-L, sp.xyz));
        color += lc.rgb * atten * ndl * (base + specGGX(n, L, view, rough));
    }
    if (F_sky.x > 0.5) {
        vec3 r = reflect(-view, n);
        vec2 ab = envBRDF(rough, clamp(dot(n, view), 1e-4, 1.0));
        color += mix(env, sky(r, rough * F_sky.z), skyVis * skyVis) * (g_f0 * ab.x + ab.y);
    }
#endif
    float alpha = u_tint.a;
    if (alpha < 0.5) { // glass: mostly what's behind, a little reflection
        float fresnel = pow(1.0 - clamp(abs(dot(n, view)), 0.0, 1.0), 4.0);
        color = color * 0.12 + (F_sky.x > 0.5 ? sky(reflect(-view, n), 1.0) : vec3(0.45, 0.6, 0.8)) * fresnel * 0.8;
        alpha = clamp(alpha * 0.6 + fresnel * 0.5, 0.0, 1.0);
    }
    if (F_sky.x > 0.5 && F_fog.x > 0.0) { // height fog coloured by the sky
        vec3 ray = v_world - F_camPos.xyz;
        float dist = length(ray);
        vec3 rd = ray / max(dist, 1e-3);
        float k = F_fog.y, kz = k * rd.z;
        float amount = F_fog.x * exp(-k * (F_camPos.z - F_fog.w)) * (abs(kz) > 1e-6 ? (1.0 - exp(-kz * dist)) / kz : dist);
        color = mix(color, sky(normalize(vec3(rd.xy, max(rd.z, 0.05))), F_sky.z) * 0.9, min(1.0 - exp(-amount), F_fog.z));
    }
    o_color = vec4(color, alpha);
}
)";

const char* kSkyVert = R"(
out vec2 v_ndc;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_ndc = uv * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 1.0, 1.0);
}
)";
const char* kSkyFrag = R"(
in vec2 v_ndc;
uniform mat4 u_invViewProj;
uniform sampler2D u_sky;
out vec4 o_color;
void main() {
    vec4 far = u_invViewProj * vec4(v_ndc, 1.0, 1.0), near = u_invViewProj * vec4(v_ndc, 0.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - near.xyz / near.w);
    float c = cos(F_sky.w), s = sin(F_sky.w);
    vec3 d = vec3(c * dir.x + s * dir.y, -s * dir.x + c * dir.y, dir.z);
    vec4 m = textureLod(u_sky, vec2(atan(d.y, d.x) * 0.15915494 + 0.5, acos(clamp(d.z, -1.0, 1.0)) * 0.31830989), 0.0);
    vec3 col = m.rgb * m.a * 16.0 * F_sky.y;
    if (F_sunColor.w > 0.5) {
        float k = dot(dir, F_sunDir.xyz);
        col += F_sunColor.rgb * (smoothstep(0.99995, 0.99999, k) * 60.0 + pow(max(k, 0.0), 600.0) * 1.5 + pow(max(k, 0.0), 40.0) * 0.08);
    }
    o_color = vec4(col, 1.0);
}
)";

const char* kBoxVert = R"(
uniform mat4 u_viewProj;
uniform vec4 u_center, u_extents;
out vec3 v_normal;
const vec3 kNormals[6] = vec3[](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0), vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
const vec2 kQuad[6] = vec2[](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));
void main() {
    vec3 n = kNormals[gl_VertexID / 6];
    vec3 t = abs(n.z) > 0.5 ? vec3(1,0,0) : vec3(0,0,1);
    vec2 q = kQuad[gl_VertexID % 6];
    vec3 w = u_center.xyz + (n + t * q.x + cross(n, t) * q.y) * u_extents.xyz;
    v_normal = n;
    gl_Position = u_viewProj * vec4(w, 1.0);
    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;
}
)";
const char* kBoxFrag = R"(
in vec3 v_normal;
uniform vec4 u_color;
out vec4 o_color;
void main() { o_color = vec4(u_color.rgb * (0.5 + 0.5 * max(dot(v_normal, normalize(vec3(0.4, 0.3, 0.85))), 0.0)), u_color.a); }
)";

const char* kTonemapVert = R"(
out vec2 v_uv;
void main() {
    v_uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
    v_uv.y = 1.0 - v_uv.y; // the scene's row 0 is the view's top (as in the main renderer)
}
)";
const char* kTonemapFrag = R"(
in vec2 v_uv;
uniform sampler2D u_scene;
uniform vec4 u_exposure;
out vec4 o_color;
void main() {
    vec3 x = texture(u_scene, v_uv).rgb * u_exposure.x;
    x = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
    o_color = vec4(mix(x * 12.92, 1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, x)), 1.0);
}
)";

// What the Frame block holds (std140: matches the GLSL above).
struct FrameBlock {
    glm::mat4 cascades[4];
    glm::vec4 splits, cascadeTexel, sunDir, sunColor, shadow, camPos, sky, fog, lightCount;
    glm::vec4 lights[64];
};
constexpr int kMaxLegacyLights = 16;

struct Program {
    GLuint id = 0;
    GLint viewProj = -1, model = -1, skinned = -1, firstJoint = -1, tint = -1, params = -1, plane = -1, flags = -1, cube = -1,
          invViewProj = -1, center = -1, extents = -1, color = -1, exposure = -1;
};

class LegacyGLBackend final : public RenderBackend {
public:
    explicit LegacyGLBackend(int major, int minor) : major_(major), minor_(minor), tier_(major >= 4 ? 2 : 1) {
        name_ = "OpenGL " + std::to_string(major) + "." + std::to_string(minor) + " (fallback)";
    }
    const char* name() const override { return name_.c_str(); }
    GpuInfo gpuInfo() const override { return gpu_; }
    GpuInfo gpu_;

    bool init(const Window& window, bool) override {
        window_ = window.handle();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_);
        // 3.1 has no profiles; 4.0 asks for core.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, major_ >= 4 ? SDL_GL_CONTEXT_PROFILE_CORE : 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, major_ >= 4 ? 0 : SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        context_ = SDL_GL_CreateContext(window_);
        if (!context_) {
            log::warn("OpenGL {}.{} context: {}", major_, minor_, SDL_GetError());
            return false;
        }
        SDL_GL_MakeCurrent(window_, context_);
        if (!glx::load()) return false;
        log::info("GPU: {} ({})", reinterpret_cast<const char*>(glx::GetString(GL_RENDERER)), reinterpret_cast<const char*>(glx::GetString(GL_VERSION)));
        gpu_.name = reinterpret_cast<const char*>(glx::GetString(GL_RENDERER)), gpu_.driver = reinterpret_cast<const char*>(glx::GetString(GL_VERSION));
        glx::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glx::PixelStorei(GL_PACK_ALIGNMENT, 1);

        const std::string header = "#version 140\n#define TIER " + std::to_string(tier_) + "\n" + kFrameBlock;
        mesh_ = link(header + kMeshVert, header + kMeshFrag);
        depth_ = link(header + kMeshVert, header + kDepthFrag);
        sky_ = link(header + kSkyVert, header + kSkyFrag);
        box_ = link(header + kBoxVert, header + kBoxFrag);
        tonemap_ = link(header + kTonemapVert, header + kTonemapFrag);
        if (!mesh_.id || !depth_.id || !sky_.id || !box_.id || !tonemap_.id) return false;

        glx::GenBuffers(1, &frameUbo_);
        glx::BindBuffer(GL_UNIFORM_BUFFER, frameUbo_);
        glx::BufferData(GL_UNIFORM_BUFFER, sizeof(FrameBlock), nullptr, GL_DYNAMIC_DRAW);
        glx::GenBuffers(1, &jointBuffer_);
        glx::BindBuffer(GL_TEXTURE_BUFFER, jointBuffer_);
        glx::BufferData(GL_TEXTURE_BUFFER, kMaxJointsPerFrame * sizeof(glm::mat4), nullptr, GL_DYNAMIC_DRAW);
        glx::GenTextures(1, &jointTexture_);
        glx::BindTexture(GL_TEXTURE_BUFFER, jointTexture_);
        glx::TexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, jointBuffer_);
        glx::GenVertexArrays(1, &emptyVao_);

        // The sun's shadow map: 2×2 cascades, smaller on the 3.1 tier.
        shadowSize_ = tier_ >= 2 ? kShadowSize : kShadowSize / 2;
        glx::GenTextures(1, &shadowMap_);
        glx::BindTexture(GL_TEXTURE_2D, shadowMap_);
        glx::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, GLsizei(shadowSize_), GLsizei(shadowSize_), 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glx::GenFramebuffers(1, &shadowFbo_);
        glx::BindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
        glx::FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowMap_, 0);
        glx::DrawBuffer(GL_NONE);
        glx::ReadBuffer(GL_NONE);
        glx::BindFramebuffer(GL_FRAMEBUFFER, 0);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        imgui_ = ImGui_ImplSDL3_InitForOpenGL(window_, context_) && ImGui_ImplOpenGL3_Init("#version 140");
        log::info("Fallback renderer: tier {} ({})", tier_, tier_ >= 2 ? "shadows, normal maps, highlights, sky reflections, 16 lights" : "lightmaps, sun shadows, sky");
        return imgui_;
    }

    void shutdown() override {
        if (!context_) return;
        if (imgui_) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            imgui_ = false;
        }
        if (glx::DeleteTextures) {
            for (GLuint t : textures_)
                if (t) glx::DeleteTextures(1, &t);
            for (Model& m : models_) releaseModel(m);
            releaseTargets();
            for (GLuint* t : {&shadowMap_, &jointTexture_}) glx::DeleteTextures(1, t);
            glx::DeleteFramebuffers(1, &shadowFbo_);
            for (GLuint* b : {&frameUbo_, &jointBuffer_}) glx::DeleteBuffers(1, b);
            glx::DeleteVertexArrays(1, &emptyVao_);
            for (Program* p : {&mesh_, &depth_, &sky_, &box_, &tonemap_}) glx::DeleteProgram(p->id);
        }
        SDL_GL_DestroyContext(context_);
        context_ = nullptr;
    }

    void beginUi() override {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }
    void setVsync(bool on) override { SDL_GL_SetSwapInterval(on ? 1 : 0); }
    void onResize() override {}

    // No block compression here (old GPUs): the renderer decodes BC7 to RGBA8 first.
    void createTexture(TextureId slot, uint32_t w, uint32_t h, bool srgb, const std::vector<std::vector<uint8_t>>& mips,
                       TexFormat = TexFormat::RGBA8) override {
        if (slot >= textures_.size()) textures_.resize(slot + 1, 0);
        GLuint& t = textures_[slot];
        if (!t) glx::GenTextures(1, &t);
        glx::BindTexture(GL_TEXTURE_2D, t);
        for (size_t l = 0; l < mips.size(); ++l)
            glx::TexImage2D(GL_TEXTURE_2D, GLint(l), srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, GLsizei(std::max(1u, w >> l)), GLsizei(std::max(1u, h >> l)), 0, GL_RGBA,
                            GL_UNSIGNED_BYTE, mips[l].data());
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(mips.size()) - 1);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips.size() > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    void uploadModel(uint32_t index, const KmdlModel& kmdl) override {
        if (index >= models_.size()) models_.resize(index + 1);
        releaseModel(models_[index]);
        Model& m = models_[index];
        glx::GenVertexArrays(1, &m.vao);
        glx::BindVertexArray(m.vao);
        glx::GenBuffers(1, &m.vertices);
        glx::BindBuffer(GL_ARRAY_BUFFER, m.vertices);
        glx::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(kmdl.vertices.size() * sizeof(KmdlVertex)), kmdl.vertices.data(), GL_STATIC_DRAW);
        const GLsizei stride = sizeof(KmdlVertex);
        const struct { GLuint loc; GLint n; size_t off; } attrs[] = {{0, 3, offsetof(KmdlVertex, position)}, {1, 3, offsetof(KmdlVertex, normal)},
                                                                     {2, 2, offsetof(KmdlVertex, uv)}, {3, 2, offsetof(KmdlVertex, lightmapUv)}};
        for (const auto& a : attrs) {
            glx::EnableVertexAttribArray(a.loc);
            glx::VertexAttribPointer(a.loc, a.n, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(a.off));
        }
        if (!kmdl.skin.empty()) {
            glx::GenBuffers(1, &m.skin);
            glx::BindBuffer(GL_ARRAY_BUFFER, m.skin);
            glx::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(kmdl.skin.size() * sizeof(KmdlSkin)), kmdl.skin.data(), GL_STATIC_DRAW);
            glx::EnableVertexAttribArray(4);
            glx::VertexAttribIPointer(4, 4, GL_UNSIGNED_BYTE, sizeof(KmdlSkin), reinterpret_cast<const void*>(offsetof(KmdlSkin, joints)));
            glx::EnableVertexAttribArray(5);
            glx::VertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(KmdlSkin), reinterpret_cast<const void*>(offsetof(KmdlSkin, weights)));
        }
        glx::GenBuffers(1, &m.indices);
        glx::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.indices);
        glx::BufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(kmdl.indices.size() * 4), kmdl.indices.data(), GL_STATIC_DRAW);
        glx::BindVertexArray(0);
        m.skinned = !kmdl.skin.empty();
    }

    uint64_t uiTexture(TextureId slot) override { return slot < textures_.size() ? uint64_t(textures_[slot]) : 0; }

    bool beginFrame(uint32_t& width, uint32_t& height) override {
        int w = 0, h = 0;
        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) return false;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        if (w <= 0 || h <= 0) return false;
        ensureTargets(uint32_t(w), uint32_t(h));
        width = uint32_t(w);
        height = uint32_t(h);
        return true;
    }

    bool takeHzb(HzbReadback&) override { return false; } // no occlusion culling here

    void render(FramePacket& p) override;

private:
    struct Model {
        GLuint vao = 0, vertices = 0, indices = 0, skin = 0;
        bool skinned = false;
    };

    Program link(const std::string& vs, const std::string& fs) {
        auto compile = [](GLenum type, const std::string& src) -> GLuint {
            const GLuint s = glx::CreateShader(type);
            const char* text = src.c_str();
            glx::ShaderSource(s, 1, &text, nullptr);
            glx::CompileShader(s);
            GLint ok = 0;
            glx::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char logText[4096];
                glx::GetShaderInfoLog(s, sizeof logText, nullptr, logText);
                log::error("OpenGL (legacy): shader failed to compile:\n{}", logText);
                glx::DeleteShader(s);
                return 0;
            }
            return s;
        };
        Program p;
        const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        if (!v || !f) return p;
        p.id = glx::CreateProgram();
        glx::AttachShader(p.id, v);
        glx::AttachShader(p.id, f);
        const char* attrs[] = {"a_pos", "a_normal", "a_uv", "a_lmuv", "a_joints", "a_weights"};
        for (GLuint i = 0; i < 6; ++i) glx::BindAttribLocation(p.id, i, attrs[i]);
        glx::BindFragDataLocation(p.id, 0, "o_color");
        glx::LinkProgram(p.id);
        glx::DeleteShader(v);
        glx::DeleteShader(f);
        GLint ok = 0;
        glx::GetProgramiv(p.id, GL_LINK_STATUS, &ok);
        if (!ok) {
            char logText[4096];
            glx::GetProgramInfoLog(p.id, sizeof logText, nullptr, logText);
            log::error("OpenGL (legacy): program failed to link:\n{}", logText);
            glx::DeleteProgram(p.id);
            p.id = 0;
            return p;
        }
        if (const GLuint block = glx::GetUniformBlockIndex(p.id, "Frame"); block != GL_INVALID_INDEX) glx::UniformBlockBinding(p.id, block, 0);
        auto u = [&](const char* n) { return glx::GetUniformLocation(p.id, n); };
        p.viewProj = u("u_viewProj"), p.model = u("u_model"), p.skinned = u("u_skinned"), p.firstJoint = u("u_firstJoint");
        p.tint = u("u_tint"), p.params = u("u_params"), p.plane = u("u_plane"), p.flags = u("u_flags"), p.cube = u("u_cube");
        p.invViewProj = u("u_invViewProj"), p.center = u("u_center"), p.extents = u("u_extents"), p.color = u("u_color"), p.exposure = u("u_exposure");
        // Fixed texture units.
        glx::UseProgram(p.id);
        const struct { const char* name; GLint unit; } units[] = {{"u_albedo", 0}, {"u_lightmap", 1}, {"u_material", 2}, {"u_skyVis", 3},
                                                                  {"u_sunShadow", 4}, {"u_sky", 5}, {"u_joints", 6}, {"u_scene", 0}};
        for (const auto& t : units)
            if (const GLint loc = u(t.name); loc >= 0) glx::Uniform1i(loc, t.unit);
        glx::UseProgram(0);
        return p;
    }

    void releaseModel(Model& m) {
        if (m.vao) glx::DeleteVertexArrays(1, &m.vao);
        for (GLuint* b : {&m.vertices, &m.indices, &m.skin})
            if (*b) glx::DeleteBuffers(1, b);
        m = {};
    }
    void releaseTargets() {
        if (sceneTex_) glx::DeleteTextures(1, &sceneTex_);
        if (sceneDepth_) glx::DeleteTextures(1, &sceneDepth_);
        if (sceneFbo_) glx::DeleteFramebuffers(1, &sceneFbo_);
        sceneTex_ = sceneDepth_ = sceneFbo_ = 0;
    }
    void ensureTargets(uint32_t w, uint32_t h) {
        if (sceneFbo_ && w == targetW_ && h == targetH_) return;
        releaseTargets();
        glx::GenTextures(1, &sceneTex_);
        glx::BindTexture(GL_TEXTURE_2D, sceneTex_);
        glx::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_FLOAT, nullptr);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glx::GenTextures(1, &sceneDepth_);
        glx::BindTexture(GL_TEXTURE_2D, sceneDepth_);
        glx::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, GLsizei(w), GLsizei(h), 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glx::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glx::GenFramebuffers(1, &sceneFbo_);
        glx::BindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
        glx::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneTex_, 0);
        glx::FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sceneDepth_, 0);
        if (glx::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) log::error("OpenGL (legacy): scene framebuffer incomplete");
        glx::BindFramebuffer(GL_FRAMEBUFFER, 0);
        targetW_ = w, targetH_ = h;
    }
    void bindTexture(GLint unit, TextureId slot) {
        glx::ActiveTexture(GL_TEXTURE0 + GLenum(unit));
        glx::BindTexture(GL_TEXTURE_2D, slot < textures_.size() && textures_[slot] ? textures_[slot] : textures_[kWhiteTexture]);
    }
    void drawMeshes(const FramePacket& p, const std::vector<MeshDraw>& draws, const glm::mat4& viewProj, const Program& prog, bool shadow);

    std::string name_;
    int major_, minor_, tier_;
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    bool imgui_ = false;
    Program mesh_, depth_, sky_, box_, tonemap_;
    std::vector<GLuint> textures_;
    std::vector<Model> models_;
    GLuint frameUbo_ = 0, jointBuffer_ = 0, jointTexture_ = 0, emptyVao_ = 0;
    GLuint shadowMap_ = 0, shadowFbo_ = 0;
    uint32_t shadowSize_ = kShadowSize;
    GLuint sceneTex_ = 0, sceneDepth_ = 0, sceneFbo_ = 0;
    uint32_t targetW_ = 0, targetH_ = 0;
};

void LegacyGLBackend::drawMeshes(const FramePacket& p, const std::vector<MeshDraw>& draws, const glm::mat4& viewProj, const Program& prog, bool shadow) {
    glx::UseProgram(prog.id);
    glx::UniformMatrix4fv(prog.viewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
    uint32_t boundModel = UINT32_MAX;
    for (const MeshDraw& d : draws) {
        if (d.model >= models_.size() || !models_[d.model].vao) continue;
        const Model& m = models_[d.model];
        if (d.model != boundModel) glx::BindVertexArray(m.vao), boundModel = d.model;
        const gpu::DrawData& dd = p.draws[d.draw];
        const bool skinned = d.firstJoint != UINT32_MAX && m.skinned;
        if (!skinned) glx::VertexAttribI4ui(4, 0, 0, 0, 0), glx::VertexAttrib4f(5, 1, 0, 0, 0);
        glx::UniformMatrix4fv(prog.model, 1, GL_FALSE, glm::value_ptr(dd.model));
        glx::Uniform1i(prog.skinned, skinned ? 1 : 0);
        glx::Uniform1i(prog.firstJoint, skinned ? GLint(d.firstJoint) : 0);
        bindTexture(0, d.textures & 0xFFFFu);
        if (!shadow) {
            const TextureId lightmap = d.textures >> 16, material = d.material & 0xFFFFu, skyVis = d.material >> 16;
            if (lightmap) bindTexture(1, lightmap);
            if (material && tier_ >= 2) bindTexture(2, material);
            if (skyVis) bindTexture(3, skyVis);
            const glm::vec4 tint = glm::unpackUnorm4x8(d.tint);
            glx::Uniform4fv(prog.tint, 1, glm::value_ptr(tint));
            glx::Uniform4fv(prog.params, 1, glm::value_ptr(dd.params));
            glx::Uniform4fv(prog.plane, 1, glm::value_ptr(dd.plane));
            const glm::vec4 flags(lightmap ? 1.0f : 0.0f, material ? 1.0f : 0.0f, skyVis ? 1.0f : 0.0f, 0.0f);
            glx::Uniform4fv(prog.flags, 1, glm::value_ptr(flags));
            glx::Uniform4fv(prog.cube, 6, glm::value_ptr(dd.cube[0]));
        }
        glx::DrawElements(GL_TRIANGLES, GLsizei(d.indexCount), GL_UNSIGNED_INT, reinterpret_cast<const void*>(uintptr_t(d.firstIndex) * 4));
    }
    glx::BindVertexArray(0);
}

void LegacyGLBackend::render(FramePacket& p) {
    // ── Per-frame data: the Frame block (the 16 nearest lights on tier 2), joints ──
    FrameBlock fb{};
    std::memcpy(fb.cascades, p.frame.cascades, sizeof fb.cascades);
    fb.splits = p.frame.splits, fb.cascadeTexel = p.frame.cascadeTexel, fb.sunDir = p.frame.sunDir, fb.sunColor = p.frame.sunColor;
    fb.shadow = glm::vec4(1.0f / float(shadowSize_), p.frame.shadow.y, p.frame.shadow.z, 0);
    fb.camPos = p.frame.camPos, fb.sky = p.frame.sky, fb.fog = p.frame.fog;
    std::vector<const gpu::Light*> closest;
    if (tier_ >= 2) {
        for (const gpu::Light& l : p.lights) closest.push_back(&l);
        const glm::vec3 cam(p.frame.camPos);
        std::sort(closest.begin(), closest.end(), [&](auto* a, auto* b) {
            return glm::length(glm::vec3(a->posRadius) - cam) - a->posRadius.w < glm::length(glm::vec3(b->posRadius) - cam) - b->posRadius.w;
        });
        closest.resize(std::min<size_t>(closest.size(), kMaxLegacyLights));
    }
    fb.lightCount = glm::vec4(float(closest.size()), glm::vec3(p.frame.camFwd));
    for (size_t i = 0; i < closest.size(); ++i)
        fb.lights[i * 4] = closest[i]->posRadius, fb.lights[i * 4 + 1] = closest[i]->color, fb.lights[i * 4 + 2] = closest[i]->spot, fb.lights[i * 4 + 3] = closest[i]->extra;
    glx::BindBuffer(GL_UNIFORM_BUFFER, frameUbo_);
    glx::BufferSubData(GL_UNIFORM_BUFFER, 0, sizeof fb, &fb);
    glx::BindBufferBase(GL_UNIFORM_BUFFER, 0, frameUbo_);
    if (!p.joints.empty()) {
        glx::BindBuffer(GL_TEXTURE_BUFFER, jointBuffer_);
        glx::BufferSubData(GL_TEXTURE_BUFFER, 0, GLsizeiptr(std::min(p.joints.size(), size_t(kMaxJointsPerFrame)) * sizeof(glm::mat4)), p.joints.data());
    }
    glx::ActiveTexture(GL_TEXTURE6);
    glx::BindTexture(GL_TEXTURE_BUFFER, jointTexture_);
    glx::Disable(GL_CULL_FACE);
    glx::Disable(GL_BLEND);
    glx::Enable(GL_DEPTH_TEST);
    glx::DepthFunc(GL_LESS);
    glx::DepthMask(GL_TRUE);

    // ── Sun shadows (light shadows aren't drawn on this renderer) ──
    glx::BindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glx::Viewport(0, 0, GLsizei(shadowSize_), GLsizei(shadowSize_));
    glx::ClearDepth(1.0);
    glx::Clear(GL_DEPTH_BUFFER_BIT);
    glx::Enable(GL_POLYGON_OFFSET_FILL);
    glx::PolygonOffset(2.5f, 2.0f);
    const float scale = float(shadowSize_) / float(p.shadowSize);
    for (const ShadowPass& pass : p.sunPasses) {
        glx::Viewport(GLint(pass.rect.x * scale), GLint(pass.rect.y * scale), GLsizei(pass.rect.z * scale), GLsizei(pass.rect.w * scale));
        drawMeshes(p, *pass.draws, pass.viewProj, depth_, true);
    }
    glx::Disable(GL_POLYGON_OFFSET_FILL);

    // ── Scene (HDR) ──
    glx::BindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glx::Viewport(0, 0, GLsizei(p.width), GLsizei(p.height));
    glx::ClearColor(p.clearColor.r, p.clearColor.g, p.clearColor.b, 1.0f);
    glx::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glx::ActiveTexture(GL_TEXTURE4);
    glx::BindTexture(GL_TEXTURE_2D, shadowMap_);
    const TextureId skyTex = TextureId(p.frame.sky.x);
    if (skyTex) bindTexture(5, skyTex);
    if (!p.boxes.empty()) {
        glx::UseProgram(box_.id);
        glx::UniformMatrix4fv(box_.viewProj, 1, GL_FALSE, glm::value_ptr(p.viewProj));
        glx::BindVertexArray(emptyVao_);
        for (const gpu::Box& b : p.boxes) {
            glx::Uniform4fv(box_.center, 1, glm::value_ptr(b.center));
            glx::Uniform4fv(box_.extents, 1, glm::value_ptr(b.extents));
            glx::Uniform4fv(box_.color, 1, glm::value_ptr(b.color));
            glx::DrawArrays(GL_TRIANGLES, 0, 36);
        }
    }
    drawMeshes(p, p.opaque, p.viewProj, mesh_, false);
    if (skyTex) {
        glx::UseProgram(sky_.id);
        glx::UniformMatrix4fv(sky_.invViewProj, 1, GL_FALSE, glm::value_ptr(p.invViewProj));
        glx::DepthFunc(GL_LEQUAL);
        glx::DepthMask(GL_FALSE);
        glx::BindVertexArray(emptyVao_);
        glx::DrawArrays(GL_TRIANGLES, 0, 3);
        glx::DepthMask(GL_TRUE);
        glx::DepthFunc(GL_LESS);
    }
    if (!p.translucent.empty()) {
        glx::Enable(GL_BLEND);
        glx::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glx::DepthMask(GL_FALSE);
        drawMeshes(p, p.translucent, p.viewProj, mesh_, false);
        glx::DepthMask(GL_TRUE);
        glx::Disable(GL_BLEND);
    }

    // ── Tonemap to the window, then UI ──
    glx::BindFramebuffer(GL_FRAMEBUFFER, 0);
    glx::Viewport(0, 0, GLsizei(p.width), GLsizei(p.height));
    glx::Disable(GL_DEPTH_TEST);
    glx::UseProgram(tonemap_.id);
    const glm::vec4 exposure(p.exposure, 0, 0, 0);
    glx::Uniform4fv(tonemap_.exposure, 1, glm::value_ptr(exposure));
    glx::ActiveTexture(GL_TEXTURE0);
    glx::BindTexture(GL_TEXTURE_2D, sceneTex_);
    glx::BindVertexArray(emptyVao_);
    glx::DrawArrays(GL_TRIANGLES, 0, 3);
    glx::BindVertexArray(0);
    glx::UseProgram(0);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (!p.screenshot.empty()) {
        std::vector<uint8_t> rows(size_t(p.width) * p.height * 4), px(rows.size());
        glx::ReadBuffer(GL_BACK);
        glx::ReadPixels(0, 0, GLsizei(p.width), GLsizei(p.height), GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        const size_t stride = size_t(p.width) * 4; // the window's row 0 is its bottom
        for (uint32_t y = 0; y < p.height; ++y) std::memcpy(&px[y * stride], &rows[(p.height - 1 - y) * stride], stride);
        saveScreenshot(p.screenshot, std::move(px), p.width, p.height);
    }
    SDL_GL_SwapWindow(window_);
}
} // namespace

std::unique_ptr<RenderBackend> createLegacyOpenGLBackend(int major, int minor) { return std::make_unique<LegacyGLBackend>(major, minor); }

} // namespace keel
