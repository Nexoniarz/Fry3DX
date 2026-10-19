#ifndef KEEL_MODEL_GLSL
#define KEEL_MODEL_GLSL
// Shared by mesh.vert, skinned.vert and mesh.frag: per-draw and per-frame data. The layouts are
// identical on both backends (render/backend.h mirrors them); only how they're reached differs.
#include "backend.glsl"

struct Vertex { float px, py, pz, nx, ny, nz, u, v, lu, lv; };
// Dynamic lights (clustered, see render/clusters.h).
struct Light {
    vec4 posRadius;
    vec4 color;  // rgb, w = first shadow view or -1
    vec4 spot;   // xyz direction, w = cos(outer cone); -2 = point light
    vec4 extra;  // x = cos(inner cone), y = the light's size (area light radius)
};
struct ShadowView { mat4 viewProj; vec4 rect; }; // rect: atlas uv offset, size
// How much of a spot light reaches a direction at cos c from its axis. A plain spot fades from
// its inner cone to its outer one; a reflector (a flashlight, extra.z = 1) has a hot centre, a
// faint bright ring where its mirror ends, faint rings in the beam and a dim spill out to the edge.
float spotShape(Light l, float c) {
    if (l.extra.z < 0.5) return smoothstep(l.spot.w, l.extra.x, c);
    float outer = acos(clamp(l.spot.w, -1.0, 1.0)), inner = acos(clamp(l.extra.x, -1.0, 1.0));
    float t = acos(clamp(c, -1.0, 1.0)) / max(outer, 1e-3), k = inner / max(outer, 1e-3);
    float core = mix(0.14, 1.0, 1.0 - smoothstep(k * 0.5, k * 1.1, t));
    float ring = exp(-pow((t - k * 1.15) * 16.0, 2.0)) * 0.22;
    float rings = 0.95 + 0.05 * sin(t * 60.0);
    return (core + ring) * rings * (1.0 - smoothstep(0.7, 1.0, t));
}

// Per draw: transform, baked ambient cube (+X -X +Y -Y +Z -Z), params x = sun visibility,
// y = lit by probes, z = liquid (cut at `plane`: xyz normal, w distance, world space),
// w = liquid style (1 film, 2 body; see LiquidStyle).
#define DRAW_FIELDS mat4 model; vec4 cube[6]; vec4 params; vec4 plane; vec4 probe;
// Per frame: sun and its shadow map, camera, dynamic lights.
#define FRAME_FIELDS                                                                         \
    mat4 cascades[4];  /* sun shadow cascades, each a quarter of the shadow map */           \
    vec4 splits;       /* view distance where each cascade ends */                           \
    vec4 cascadeTexel; /* world size of a shadow texel per cascade */                        \
    vec4 sunDir;       /* xyz towards the sun, w = shadow map texture index */               \
    vec4 sunColor;     /* rgb, w = 1 when there is a sun */                                  \
    vec4 shadow;       /* x = texel size, y = strength on baked surfaces, z = 1 real-time */ \
    vec4 volumetric;   /* volumetric light: x strength, y reach, z air density, w sun too */ \
    vec4 camPos;       /* xyz, w = dynamic light count */                                    \
    vec4 camFwd;       /* xyz, w = debug view (1 = lights per cluster) */                    \
    vec4 screen;       /* width, height, shadow atlas texture index, atlas size */           \
    vec4 sky;          /* x sky texture (0 = none), y intensity, z top level, w yaw */       \
    vec4 fog;          /* x density per unit, y height falloff per unit, z max, w base z */  \
    vec4 skySH[9];     /* the sky's light: rgb spherical harmonics, irradiance / pi */      \
    vec4 froxel;       /* the fog grid (froxel.glsl): x 1 = on (Vulkan), y near, z far */  \
    vec4 gi;           /* real-time GI probes (gi.glsl): x 1 = on, y spacing */            \
    vec4 giOrigin;     /* the probe grid's first cell (whole numbers) */                   \
    vec4 mirror;       /* x: the mirror's picture's texture (a mirror surface shows it); y 1: \
                          the mirror's own pass (every light, not this view's clusters) */  \
    vec4 clip;         /* the mirror's pass: what's behind its plane (xyz·p < w) is cut */ \
    vec4 weather;      /* x wet (rain), y snow cover, z night (0 day .. 1), w lightning flash */

#ifdef KEEL_FRAME_ONLY
// Post passes that need the frame (volumetric.frag): it and the dynamic lights. Vulkan: a
// FrameData reference in the pass's own push block (#define FRAME pc.frame there); OpenGL: the
// frame's and the lights' bindings.
#ifdef KEEL_GL
layout(std430, binding = 4) readonly buffer FrameBlock { FRAME_FIELDS uvec2 unusedRefs[6]; } u_frame;
layout(std430, binding = 5) readonly buffer Lights { Light l[]; } u_lights;
layout(std430, binding = 6) readonly buffer Cells { uvec2 c[]; } u_cells;
layout(std430, binding = 7) readonly buffer LightIndices { uint i[]; } u_lightIndices;
layout(std430, binding = 8) readonly buffer ShadowViews { ShadowView v[]; } u_shadowViews;
#define FRAME u_frame
#define LIGHT(n_) u_lights.l[n_]
#define CELL(n_) u_cells.c[n_]
#define LIGHT_INDEX(n_) u_lightIndices.i[n_]
#define SHADOW_VIEW(n_) u_shadowViews.v[n_]
#else
layout(buffer_reference, std430) readonly buffer Lights { Light l[]; };
layout(buffer_reference, std430) readonly buffer Cells { uvec2 c[]; };
layout(buffer_reference, std430) readonly buffer LightIndices { uint i[]; };
layout(buffer_reference, std430) readonly buffer ShadowViews { ShadowView v[]; };
layout(buffer_reference, std430) readonly buffer Froxels { uvec2 f[]; }; // the fog grid, integrated: rgb light, a transmittance (halves)
// A GI probe (gi.glsl): the light reaching it as L1 spherical harmonics (sh[0..3].rgb: band 0,
// then y, z, x; sh[0].w how much to trust it), the mean distance to what's around it and its
// square along ±x ±y ±z, and the world cell it holds (w 1 = filled).
struct GiProbe { vec4 sh[4]; vec4 dist[3]; vec4 tag; };
layout(buffer_reference, std430) readonly buffer GiProbes { GiProbe p[]; };
layout(buffer_reference, std430) readonly buffer FrameData {
    FRAME_FIELDS
    Lights lights;
    Cells cells;
    LightIndices lightIndices;
    ShadowViews shadowViews;
    Froxels froxels;
    GiProbes giProbes;
};
#define LIGHT(n_) FRAME.lights.l[n_]
#define CELL(n_) FRAME.cells.c[n_]
#define LIGHT_INDEX(n_) FRAME.lightIndices.i[n_]
#define SHADOW_VIEW(n_) FRAME.shadowViews.v[n_]
#endif
#elif defined(KEEL_GL)
struct DrawData { DRAW_FIELDS };
layout(std430, binding = 0) readonly buffer Vertices { Vertex v[]; } u_vertices;
layout(std430, binding = 1) readonly buffer Skin { uvec2 s[]; } u_skin; // joints (4×u8), weights (4×u8)
layout(std430, binding = 2) readonly buffer Joints { mat4 m[]; } u_joints;
layout(std430, binding = 3) readonly buffer Draws { DrawData d[]; } u_draws;
layout(std430, binding = 4) readonly buffer FrameBlock { FRAME_FIELDS uvec2 unusedRefs[6]; } u_frame;
layout(std430, binding = 5) readonly buffer Lights { Light l[]; } u_lights;
layout(std430, binding = 6) readonly buffer Cells { uvec2 c[]; } u_cells; // first index, count
layout(std430, binding = 7) readonly buffer LightIndices { uint i[]; } u_lightIndices;
layout(std430, binding = 8) readonly buffer ShadowViews { ShadowView v[]; } u_shadowViews;
PUSH_BLOCK {
    mat4 viewProj;
    uint textures;   // albedo | lightmap << 16
    uint tint;       // RGBA8
    uint material;   // texture: rgb tangent-space normal, a gloss (0 = flat, matte-ish) | sky visibility << 16
    uint drawIndex;  // into Draws
    uint firstJoint; // into Joints (skinned draws)
} pc;
#define VERTEX(n_) u_vertices.v[n_]
#define SKIN(n_) u_skin.s[n_]
#define JOINT(n_) u_joints.m[pc.firstJoint + (n_)]
#define DRAW u_draws.d[pc.drawIndex]
#define FRAME u_frame
#define LIGHT(n_) u_lights.l[n_]
#define CELL(n_) u_cells.c[n_]
#define LIGHT_INDEX(n_) u_lightIndices.i[n_]
#define SHADOW_VIEW(n_) u_shadowViews.v[n_]
#else
layout(buffer_reference, std430) readonly buffer Vertices { Vertex v[]; };
layout(buffer_reference, std430) readonly buffer Skin { uvec2 s[]; }; // joints (4×u8), weights (4×u8)
layout(buffer_reference, std430) readonly buffer Joints { mat4 m[]; };
layout(buffer_reference, std430) readonly buffer DrawData { DRAW_FIELDS };
layout(buffer_reference, std430) readonly buffer Lights { Light l[]; };
layout(buffer_reference, std430) readonly buffer Cells { uvec2 c[]; }; // first index, count
layout(buffer_reference, std430) readonly buffer LightIndices { uint i[]; };
layout(buffer_reference, std430) readonly buffer ShadowViews { ShadowView v[]; };
layout(buffer_reference, std430) readonly buffer Froxels { uvec2 f[]; }; // the fog grid, integrated: rgb light, a transmittance (halves)
// A GI probe (gi.glsl): the light reaching it as L1 spherical harmonics (sh[0..3].rgb: band 0,
// then y, z, x; sh[0].w how much to trust it), the mean distance to what's around it and its
// square along ±x ±y ±z, and the world cell it holds (w 1 = filled).
struct GiProbe { vec4 sh[4]; vec4 dist[3]; vec4 tag; };
layout(buffer_reference, std430) readonly buffer GiProbes { GiProbe p[]; };
layout(buffer_reference, std430) readonly buffer FrameData {
    FRAME_FIELDS
    Lights lights;
    Cells cells;
    LightIndices lightIndices;
    ShadowViews shadowViews;
    Froxels froxels;
    GiProbes giProbes;
};
PUSH_BLOCK {
    mat4 viewProj;
    Vertices vertices;
    Skin skin;       // skinned draws only
    Joints joints;   // skinned draws only; already include the model transform
    DrawData draw;
    FrameData frame;
    uint textures;   // albedo | lightmap << 16
    uint tint;       // RGBA8
    uint material;   // texture: rgb tangent-space normal, a gloss (0 = flat, matte-ish) | sky visibility << 16
} pc;
#define VERTEX(n_) pc.vertices.v[n_]
#define SKIN(n_) pc.skin.s[n_]
#define JOINT(n_) pc.joints.m[n_]
#define DRAW pc.draw
#define FRAME pc.frame
#define LIGHT(n_) pc.frame.lights.l[n_]
#define CELL(n_) pc.frame.cells.c[n_]
#define LIGHT_INDEX(n_) pc.frame.lightIndices.i[n_]
#define SHADOW_VIEW(n_) pc.frame.shadowViews.v[n_]
#endif
#endif
