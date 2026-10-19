#version 460
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
#include "upsample.glsl"
// HDR scene → display: ambient occlusion, bloom, exposure, ACES filmic curve, a light grade
// (saturation, vignette), then sRGB encoding.
PUSH_BLOCK {
    uint scene;
    float exposure;
    uint ao;          // 0 = none
    uint bloom;       // 0 = none
    float bloomStrength;
    float saturation;
    float vignette;
    float sharpen;    // after TAA: give back a little crispness (0 = none)
    uint depth;       // scene depth (1 = sky)
    uint ssr;         // reflections to add (when TAA didn't), 0 = none
    uint adapt;       // eye adaptation (1×1 exposure multiplier), 0 = none
    uint pad2;
    vec4 shafts;      // sun shafts: the sun's uv, strength (0 = none)
    vec4 sunColor;
} pc;

// Sun shafts (screen-space light scattering, GPU Gems 3 ch. 13): march towards the sun's spot
// on screen, adding up how much open sky lies on the way; air lit by the sun glows there,
// fading with distance from the sun. Started at a per-pixel offset so the steps don't band.
vec3 sunShafts(vec2 uv) {
    const int N = 32;
    vec2 toSun = pc.shafts.xy - uv;
    vec2 stepUv = toSun * (0.85 / float(N));
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec2 p = uv + stepUv * jitter;
    vec3 lit = vec3(0.0);
    float w = 1.0, total = 0.0;
    for (int i = 0; i < N; ++i, p += stepUv) {
        bool inside = all(greaterThanEqual(p, vec2(0.0))) && all(lessThanEqual(p, vec2(1.0)));
        if (inside && sampleTexLod(pc.depth, p, 0.0).r >= 1.0) // open sky: its light, the sun's brightest
            lit += max(sampleTexLod(pc.scene, p, 0.0).rgb - 1.0, vec3(0.0)) * w;
        total += w;
        w *= 0.97;
    }
    vec2 aspect = vec2(float(texSize(pc.scene).x) / float(texSize(pc.scene).y), 1.0);
    float fall = exp(-length(toSun * aspect) * 1.5);
    return min(lit / total, vec3(8.0)) * fall * pc.shafts.z;
}

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 toSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    vec3 hdr = sampleTex(pc.scene, v_uv).rgb;
    const ivec2 px = ivec2(v_uv * vec2(texSize(pc.depth)));
    if (pc.ssr != 0u) hdr += upsampleDepthAware(pc.ssr, pc.depth, px).rgb;
    if (pc.sharpen > 0.0) { // unsharp mask against the 4 neighbours, limited so edges don't ring
        vec2 t = 1.0 / vec2(texSize(pc.scene));
        vec3 n = sampleTex(pc.scene, v_uv + vec2(t.x, 0)).rgb + sampleTex(pc.scene, v_uv - vec2(t.x, 0)).rgb
               + sampleTex(pc.scene, v_uv + vec2(0, t.y)).rgb + sampleTex(pc.scene, v_uv - vec2(0, t.y)).rgb;
        hdr = max(hdr + clamp((hdr - n * 0.25) * pc.sharpen, -0.1 * (hdr + 0.05), 0.1 * (hdr + 0.05)), 0.0);
    }
    if (pc.ao != 0u) hdr *= upsampleDepthAware(pc.ao, pc.depth, px).r;
    if (pc.bloom != 0u) hdr = mix(hdr, sampleTex(pc.bloom, v_uv).rgb, pc.bloomStrength);
    if (pc.shafts.z > 0.0) hdr += sunShafts(v_uv);
    float exposure = pc.exposure * (pc.adapt != 0u ? sampleTexLod(pc.adapt, vec2(0.5), 0.0).r : 1.0);
    vec3 c = aces(hdr * exposure);
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = max(mix(vec3(l), c, pc.saturation), 0.0);
    vec2 d = v_uv - 0.5;
    c *= 1.0 - pc.vignette * dot(d, d) * 2.0;
    o_color = vec4(toSrgb(c), 1.0);
}
