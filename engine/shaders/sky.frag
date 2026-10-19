#version 460
#include "backend.glsl"
PUSH_BLOCK {
    mat4 invViewProj;
    vec4 sunDir;   // xyz towards the sun
    vec4 sunColor; // rgb, w = 1 when there is a sun
    vec4 sky;      // x texture, y intensity, z top level, w yaw
    vec4 weather;  // x cloud cover, y the photo's own sun's height (z; 2 = none), z night, w lightning flash
} pc;
#include "sky.glsl"

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 far = pc.invViewProj * vec4(v_ndc, 1.0, 1.0), near = pc.invViewProj * vec4(v_ndc, 0.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - near.xyz / near.w);
    vec3 c = skyRadiance(uint(pc.sky.x), pc.sky.y, pc.sky.w, dir, 0.0);
    // Overcast: the photo's sky greys over (a flat, brighter-low-down cloud deck).
    float cloud = pc.weather.x;
    // The photo's own sun (turned to the real sun's bearing, but at the height it was shot at):
    // when the time of day has moved the sun away from it, it's painted over with the sky beside it.
    if (pc.weather.y < 1.5 && length(pc.sunDir.xy) > 1e-3) {
        vec3 photoSun = vec3(normalize(pc.sunDir.xy) * sqrt(max(1.0 - pc.weather.y * pc.weather.y, 0.0)), pc.weather.y);
        float moved = 1.0 - smoothstep(0.9990, 0.9998, dot(normalize(pc.sunDir.xyz), photoSun));
        float near = smoothstep(0.975, 0.997, dot(dir, photoSun)) * moved;
        if (near > 0.0) { // the same height a little round to the side: clean sky like what's round it
            const float t = 0.38; // ~22°
            vec3 beside = vec3(dir.x * cos(t) - dir.y * sin(t), dir.x * sin(t) + dir.y * cos(t), dir.z);
            c = mix(c, skyRadiance(uint(pc.sky.x), pc.sky.y, pc.sky.w, beside, 1.0), near);
        }
    }
    // The photo blurred right out (its top level): no sun in it, for the cloud deck and the night.
    vec3 soft = (cloud > 0.0 || pc.weather.z > 0.0) ? skyRadiance(uint(pc.sky.x), pc.sky.y, pc.sky.w, dir, pc.sky.z) : c;
    if (cloud > 0.0) {
        float l = dot(soft, vec3(0.2126, 0.7152, 0.0722));
        vec3 deck = vec3(l) * vec3(0.92, 0.95, 1.0) * mix(1.0, 0.7, smoothstep(0.0, 0.6, dir.z));
        c = mix(c, deck, cloud);
    }
    if (pc.sunColor.w > 0.5 && cloud < 0.95) {
        // The sun itself (taken out of the photo so it matches the shadows): a disc and a glow.
        float d = dot(dir, pc.sunDir.xyz);
        c += pc.sunColor.rgb * (smoothstep(0.99995, 0.99999, d) * 60.0 + pow(max(d, 0.0), 600.0) * 1.5 + pow(max(d, 0.0), 40.0) * 0.08) * (1.0 - cloud) * (1.0 - cloud);
    }
    // Night: the photo's sky goes dark blue, and stars come out (fixed to the sky).
    float night = pc.weather.z;
    if (night > 0.0) {
        c = mix(c, vec3(0.004, 0.006, 0.014) + soft * 0.15, night);
        vec3 d = dir * 220.0;
        vec3 cell = floor(d), f = fract(d) - 0.5;
        float h = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        float star = step(0.985, h) * smoothstep(0.12, 0.0, length(f)) * smoothstep(-0.05, 0.25, dir.z);
        float bright = 0.3 + 0.7 * fract(h * 97.0); // stars of different brightness
        c += vec3(0.8, 0.85, 1.0) * star * bright * night * 0.6;
    }
    c *= 1.0 + pc.weather.w * 4.0; // lightning lights the clouds
    o_color = vec4(c, 1.0);
}
