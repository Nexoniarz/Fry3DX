#version 460
// Ambient occlusion from the depth buffer alone (Alchemy-style, half resolution): for each
// pixel, view-space points around it in a spiral; those above the surface's plane and close
// enough darken it. Rows are the depth buffer's own (uv from gl_FragCoord), on both backends.
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
PUSH_BLOCK {
    uint depth;        // scene depth slot
    uint w, h;         // its size
    uint outW, outH;   // this target's
    float radius;      // world units
    float strength;
    float pad;
    vec4 proj;         // tan(half fov x), tan(half fov y), near, far
} pc;
layout(location = 0) out vec4 o_ao;

// The point seen at uv: its depth from the pixel there, and its place across the screen from
// that same pixel's centre (not uv itself: on a slanted wall the two would disagree by a part of
// a pixel, in a pattern that depends on how the window's size halves; stripes on walls).
vec3 viewPos(vec2 uv) {
    ivec2 p = clamp(ivec2(uv * vec2(pc.w, pc.h)), ivec2(0), ivec2(pc.w - 1, pc.h - 1));
    float d = fetchTex(pc.depth, p).r;
    float z = pc.proj.z * pc.proj.w / (pc.proj.w - d * (pc.proj.w - pc.proj.z));
    vec2 at = (vec2(p) + 0.5) / vec2(pc.w, pc.h);
    return vec3((at * 2.0 - 1.0) * pc.proj.xy * z, z);
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(pc.outW, pc.outH);
    ivec2 full = clamp(ivec2(uv * vec2(pc.w, pc.h)), ivec2(0), ivec2(pc.w - 1, pc.h - 1));
    if (fetchTex(pc.depth, full).r >= 1.0) { o_ao = vec4(1.0); return; } // sky
    vec3 p = viewPos(uv);
    // The surface normal from neighbouring depths (the smaller step on each axis, so edges hold).
    vec2 texel = 1.0 / vec2(pc.outW, pc.outH);
    vec3 px1 = viewPos(uv + vec2(texel.x, 0)) - p, px2 = p - viewPos(uv - vec2(texel.x, 0));
    vec3 py1 = viewPos(uv + vec2(0, texel.y)) - p, py2 = p - viewPos(uv - vec2(0, texel.y));
    // The smaller step, but never one off the screen's edge (there it clamps onto this pixel,
    // a zero step, and the normal goes wrong: a dark line along the border).
    bool right = uv.x + texel.x < 1.0, left = uv.x - texel.x > 0.0, down = uv.y + texel.y < 1.0, up = uv.y - texel.y > 0.0;
    vec3 dx = !right ? px2 : !left ? px1 : dot(px1, px1) < dot(px2, px2) ? px1 : px2;
    vec3 dy = !down ? py2 : !up ? py1 : dot(py1, py1) < dot(py2, py2) ? py1 : py2;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n; // towards the camera
    // Interleaved gradient noise turns the spiral per pixel; the blur evens it out.
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    const int kSamples = 12;
    float occlusion = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        float t = (float(i) + noise) / float(kSamples);
        float a = (float(i) * 2.39996 + noise * 6.2831853);
        float r = pc.radius * t;
        vec2 off = vec2(cos(a), sin(a)) * r / (p.z * pc.proj.xy * 2.0);
        vec2 q = uv + off;
        if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, vec2(1.0)))) continue; // off screen: nothing known there
        vec3 v = viewPos(q) - p;
        float vv = dot(v, v);
        float falloff = max(0.0, 1.0 - vv / (pc.radius * pc.radius));
        occlusion += max(0.0, dot(v, n) - 0.01 * p.z) / (vv + 1.0) * falloff;
    }
    float ao = max(0.0, 1.0 - 2.0 * occlusion / float(kSamples) * pc.radius * 0.6);
    o_ao = vec4(mix(1.0, ao * ao, pc.strength));
}
