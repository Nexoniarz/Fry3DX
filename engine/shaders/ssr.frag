#version 460
// Screen-space reflections (half resolution): from each pixel, the view ray mirrored about its
// normal (the mesh pass's G-buffer) is marched through view space against the depth buffer;
// on a hit (refined by bisection) it takes this frame's lit scene there. Fades with roughness,
// distance, the screen's edges and rays towards the camera, so it blends into the sky and
// probe reflections the mesh pass already gave. TAA (or the tonemap) adds it to the scene.
// Rows are the targets' own (uv from gl_FragCoord), on both backends.
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
PUSH_BLOCK {
    uint depth, gbuf, scene;
    uint w, h;       // full size
    uint outW, outH; // this target's
    float strength;
    vec4 proj;       // tan(half fov x), tan(half fov y), near, far
} pc;
layout(location = 0) out vec4 o_ssr;

float viewZ(vec2 uv) {
    ivec2 p = clamp(ivec2(uv * vec2(pc.w, pc.h)), ivec2(0), ivec2(pc.w - 1, pc.h - 1));
    float d = fetchTex(pc.depth, p).r;
    return d >= 1.0 ? 1e9 : pc.proj.z * pc.proj.w / (pc.proj.w - d * (pc.proj.w - pc.proj.z));
}
vec3 viewPos(vec2 uv) { // across the screen from the centre of the pixel its depth came from (as ssao.frag)
    float z = viewZ(uv);
    ivec2 p = clamp(ivec2(uv * vec2(pc.w, pc.h)), ivec2(0), ivec2(pc.w - 1, pc.h - 1));
    vec2 at = (vec2(p) + 0.5) / vec2(pc.w, pc.h);
    return vec3((at * 2.0 - 1.0) * pc.proj.xy * z, z);
}
vec2 toUv(vec3 v) { return v.xy / (v.z * pc.proj.xy) * 0.5 + 0.5; }
vec3 octDecode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(pc.outW, pc.outH);
    o_ssr = vec4(0.0);
    vec4 g = fetchTex(pc.gbuf, clamp(ivec2(uv * vec2(pc.w, pc.h)), ivec2(0), ivec2(pc.w - 1, pc.h - 1)));
    float rough = g.z, weight = g.w;
    if (weight <= 0.01 || rough > 0.65) return;
    vec3 p = viewPos(uv);
    if (p.z > 1e8) return;
    vec3 n = octDecode(g.xy);
    vec3 v = normalize(p);
    vec3 r = reflect(v, n);
    if (r.z < -0.2) return; // back at the camera: nothing on screen to see there
    // March: steps grow with distance; thickness grows with them.
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float t = 4.0 + 6.0 * jitter, stepLen = 6.0;
    const int STEPS = 40;
    vec2 hit = vec2(-1.0);
    float hitT = 0.0;
    for (int i = 0; i < STEPS; ++i) {
        vec3 q = p + r * t;
        if (q.z <= pc.proj.z) break;
        vec2 quv = toUv(q);
        if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) break;
        float sceneZ = viewZ(quv);
        float behind = q.z - sceneZ;
        if (behind > 0.0 && behind < stepLen * 2.0 + 8.0) {
            float lo = t - stepLen, hi = t; // refine between the last two steps
            for (int k = 0; k < 5; ++k) {
                float mid = (lo + hi) * 0.5;
                vec3 m = p + r * mid;
                if (m.z - viewZ(toUv(m)) > 0.0) hi = mid; else lo = mid;
            }
            hitT = hi;
            hit = toUv(p + r * hi);
            break;
        }
        t += stepLen;
        stepLen *= 1.12;
    }
    if (hit.x < 0.0) return;
    vec2 edge = smoothstep(vec2(0.0), vec2(0.12), hit) * smoothstep(vec2(0.0), vec2(0.12), 1.0 - hit);
    float fade = edge.x * edge.y * (1.0 - smoothstep(0.3, 0.65, rough)) * (1.0 - smoothstep(1500.0, 3000.0, hitT)) * smoothstep(-0.2, 0.1, r.z + 0.1);
    vec3 c = min(sampleTexLod(pc.scene, hit, 0.0).rgb, vec3(16.0));
    o_ssr = vec4(c * weight * fade * pc.strength, fade);
}
