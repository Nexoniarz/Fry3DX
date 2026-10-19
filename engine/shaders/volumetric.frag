#version 460
// Volumetric light (half resolution): the sun and the dynamic lights scattered towards the eye
// by the air along each view ray, where they reach it (12 jittered steps between the eye and the
// depth buffer; each takes the sun's shadow and, for every light in that spot's cluster, its
// falloff, cone and a one-tap shadow; Henyey-Greenstein phase), so window frames cut shafts out of
// the sun and lamps glow in the haze (like Source 2's volumetric fog). Once per pixel, whatever overdraw the scene had. Added onto the reflections
// target (TAA, or the tonemap, adds that to the scene). Rows: the targets' own, both backends.
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
#define KEEL_FRAME_ONLY
#include "model.glsl"
PUSH_BLOCK {
#ifndef KEEL_GL
    FrameData frame;
#endif
    uint depth;
    uint w, h, outW, outH;
    float pad0, pad1, pad2;
    vec4 proj; // tan(half fov x), tan(half fov y), near, far
} pc;
#ifndef KEEL_GL
#define FRAME pc.frame
#endif
layout(location = 0) out vec4 o_light;

#include "airlight.glsl"

void main() {
    o_light = vec4(0.0);
    if (FRAME.volumetric.x <= 0.0) return;
    const bool sun = FRAME.sunColor.w > 0.5 && FRAME.volumetric.w > 0.5;
    vec2 uv = gl_FragCoord.xy / vec2(pc.outW, pc.outH);
    ivec2 px = clamp(ivec2(uv * vec2(pc.w, pc.h)), ivec2(0), ivec2(pc.w - 1, pc.h - 1));
    float d = fetchTex(pc.depth, px).r;
    float z = d >= 1.0 ? 1e9 : pc.proj.z * pc.proj.w / (pc.proj.w - d * (pc.proj.w - pc.proj.z));
    // The view ray in world space (view: x right, y down the screen, z forward).
    vec3 fwd = FRAME.camFwd.xyz, right = normalize(cross(fwd, vec3(0.0, 0.0, 1.0))), up = cross(right, fwd);
    vec2 s = (uv * 2.0 - 1.0) * pc.proj.xy;
    vec3 rd = normalize(fwd + right * s.x - up * s.y);
    float dist = z * length(vec3(s, 1.0));
    float len = min(dist, FRAME.volumetric.y);
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float lit = 0.0;
    vec3 lamps = vec3(0.0);
    const int N = 12;
    for (int i = 0; i < N; ++i) {
        vec3 w = FRAME.camPos.xyz + rd * len * ((float(i) + jitter) / float(N));
        if (sun) lit += sunlitAt(w);
        lamps += lightsAt(w, rd, uv);
    }
    float scattered = 1.0 - exp(-len * FRAME.volumetric.z);
    vec3 sunPart = sun ? FRAME.sunColor.rgb * (lit / float(N)) * phaseHG(dot(rd, FRAME.sunDir.xyz), 0.6) : vec3(0.0);
    // Lamps: each step's light over the stretch it stands for (the air between them is lit too).
    vec3 lampPart = lamps / float(N) * scattered * 7.0;
    o_light = vec4((sunPart * scattered + lampPart) * FRAME.volumetric.x, 0.0);
}
