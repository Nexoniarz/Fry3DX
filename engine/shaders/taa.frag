#version 460
// Temporal anti-aliasing (as id Tech 6's TSSAA, in spirit): the scene is drawn a sub-pixel off
// each frame; each pixel finds where it was last frame (its depth, last frame's camera) and
// blends with what was there, after clamping that to the colours around it now (variance
// clipping in YCoCg) so moved things don't leave ghosts. Weighted by brightness so highlights
// don't flicker. Rows are the targets' own (uv from gl_FragCoord), on both backends.
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
#include "upsample.glsl"
PUSH_BLOCK {
    mat4 reproject;  // this frame's clip space → last frame's
    uint scene, history, depth;
    uint w, h;
    uint reset;      // no usable history (first frame, resized, teleported)
    uint ssr;        // reflections to add first (half size), 0 = none
    float pad1;
} pc;
layout(location = 0) out vec4 o_color;

// The scene with the half-size reflections / volumetric light on it: depth-aware for the pixel
// itself (no lines along edges), a plain read for its neighbours (only the colour clamp uses them).
vec3 sceneAt(ivec2 q) {
    vec3 c = fetchTex(pc.scene, q).rgb;
    if (pc.ssr != 0u) c += sampleTexLod(pc.ssr, (vec2(q) + 0.5) / vec2(pc.w, pc.h), 0.0).rgb;
    return c;
}
vec3 sceneAtCentre(ivec2 q) {
    vec3 c = fetchTex(pc.scene, q).rgb;
    if (pc.ssr != 0u) c += upsampleDepthAware(pc.ssr, pc.depth, q).rgb;
    return c;
}

vec3 toYCoCg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 fromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
float weight(vec3 c) { return 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722))); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 last = ivec2(pc.w - 1, pc.h - 1);
    vec3 cur = sceneAtCentre(p).rgb;
    if (pc.reset != 0u) { o_color = vec4(cur, 1.0); return; }
    // The colours around this pixel now (mean ± deviation), and the nearest depth among them
    // (so an edge's history comes from the thing in front).
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float nearest = 1.0;
    ivec2 at = p;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), last);
            vec3 c = toYCoCg(sceneAt(q).rgb);
            m1 += c;
            m2 += c * c;
            float d = fetchTex(pc.depth, q).r;
            if (d < nearest) nearest = d, at = q;
        }
    vec3 mu = m1 / 9.0, sigma = sqrt(abs(m2 / 9.0 - mu * mu));
    vec3 lo = mu - 1.25 * sigma, hi = mu + 1.25 * sigma;
    vec2 uv = (vec2(p) + 0.5) / vec2(pc.w, pc.h);
    vec4 prev = pc.reproject * vec4(((vec2(at) + 0.5) / vec2(pc.w, pc.h)) * 2.0 - 1.0, nearest, 1.0);
    vec2 puv = prev.xy / prev.w * 0.5 + 0.5 + (uv - (vec2(at) + 0.5) / vec2(pc.w, pc.h));
    if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) { o_color = vec4(cur, 1.0); return; }
    vec3 hist = sampleTex(pc.history, puv).rgb;
    hist = fromYCoCg(clamp(toYCoCg(hist), lo, hi));
    // Less history when things move a lot on screen (it smears), more when still.
    float moved = length((puv - uv) * vec2(pc.w, pc.h));
    float keep = mix(0.92, 0.7, clamp(moved / 24.0, 0.0, 1.0));
    float wc = (1.0 - keep) * weight(cur), wh = keep * weight(hist);
    o_color = vec4((cur * wc + hist * wh) / (wc + wh), 1.0);
}
