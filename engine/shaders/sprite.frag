#version 460
// Particles: unlit, the texture times the colour (fire, sparks, flashes added on; smoke blended).
#include "backend.glsl"
PUSH_BLOCK {
    mat4 viewProj;
    vec4 posSize;
    vec4 color;
    vec4 right;
    vec4 up;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
// A grid (a sprite with colour.a < 0), worked out for each pixel where it lies on its plane:
// four sets of lines each `first` × 10 apart, each set showing by how far apart its lines are on
// the screen (none where they crowd together, stronger where they're far apart), lines about a
// pixel wide, smooth; the axes through the middle in the colour of the world's axis they run along.
// look: x the lines' shade (0 dark, 1 light), y how faint, z how strong; reach: it fades out there.
vec3 gridAxisColor(vec3 d) {
    d = abs(d);
    return d.x > 0.5 ? vec3(0.9, 0.25, 0.25) : d.y > 0.5 ? vec3(0.3, 0.75, 0.25) : vec3(0.25, 0.45, 0.95);
}
vec4 gridColor(vec3 w, vec3 centre, vec3 u, vec3 v, float first, vec3 look, float reach) {
    vec2 c = vec2(dot(w, u), dot(w, v));
    vec2 fw = max(fwidth(c), vec2(1e-9));
    float lines = 0.0;
    for (int k = 0; k < 4; ++k) {
        float s = first * pow(10.0, float(k));
        vec2 apart = s / fw; // (pixels between this set's lines, each way)
        vec2 off = abs(fract(c / s + 0.5) - 0.5) * apart; // (pixels to the nearest of them)
        vec2 strong = smoothstep(20.0, 160.0, apart);
        vec2 cover = clamp(0.5 * (1.0 + 0.6 * strong) + 0.5 - off, 0.0, 1.0);
        vec2 a = cover * smoothstep(5.0, 20.0, apart) * mix(vec2(look.y), vec2(look.z), strong);
        lines = max(lines, max(a.x, a.y));
    }
    vec2 axis = clamp(1.25 - abs(c) / fw, 0.0, 1.0) * 0.9; // (x: the line along v, y: along u)
    float fade = smoothstep(reach, reach * 0.6, length(w - centre));
    vec3 col = vec3(look.x) * lines;
    float a = lines;
    col = mix(col, gridAxisColor(v), axis.x), a = mix(a, 1.0, axis.x);
    col = mix(col, gridAxisColor(u), axis.y), a = mix(a, 1.0, axis.y);
    return vec4(col / max(a, 1e-6), a * fade);
}

layout(location = 0) out vec4 o_color;
void main() {
    if (pc.color.a < 0.0) { // a grid: right.w its finest step, color.rgb its look, posSize.w how far it reaches
        o_color = gridColor(v_world, pc.posSize.xyz, normalize(pc.right.xyz), normalize(pc.up.xyz), pc.right.w, pc.color.rgb, pc.posSize.w);
        return;
    }
    vec4 t = sampleTex(floatBitsToUint(pc.up.w), v_uv);
    // Block compression leaves a little alpha where there should be none; bright sprites
    // would show it as a square: cut it.
    float a = clamp((t.a - 0.03) / 0.97, 0.0, 1.0);
    o_color = vec4(t.rgb * pc.color.rgb, a * pc.color.a);
}
