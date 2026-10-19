#version 460
#include "backend.glsl"
#include "model.glsl"
#include "lights.glsl"
#include "sky.glsl"

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec2 v_lightmapUv;
layout(location = 3) in vec3 v_world;
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_gbuf; // view normal (octahedral), roughness, reflection weight: for ssr.frag

vec2 octEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy * 0.5 + 0.5;
}

// 1 = sunlit. Cascade by view distance, normal offset by the cascade's texel size,
// 3×3 bilinear PCF taps (a smooth 4×4 filter).
float sunShadow(vec3 n) {
    if (FRAME.sunColor.w < 0.5) return 1.0;
    float depth = dot(v_world - FRAME.camPos.xyz, FRAME.camFwd.xyz);
    if (depth > FRAME.splits.w) return 1.0;
    int c = depth < FRAME.splits.x ? 0 : depth < FRAME.splits.y ? 1 : depth < FRAME.splits.z ? 2 : 3;
    float texel = FRAME.cascadeTexel[c];
    vec4 p = FRAME.cascades[c] * vec4(v_world + n * texel * 1.5, 1.0);
    vec2 uv = p.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || p.z > 1.0) return 1.0;
    float atlasTexel = FRAME.shadow.x;
    vec2 base = vec2(c & 1, c >> 1) * 0.5;
    vec2 lo = base + atlasTexel, hi = base + 0.5 - atlasTexel;
    vec2 at = base + uv * 0.5;
    uint map = uint(FRAME.sunDir.w);
    float lit = 0.0;
    float soft = FRAME.shadow.w; // the sun's apparent size (r_shadowsoft); 0 = the plain 3x3 filter
    if (soft <= 0.0) {
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) lit += shadowTap(map, at + vec2(x, y) * atlasTexel, lo, hi, p.z - 0.0002);
        lit /= 9.0;
    } else {
        // PCSS: how far above this point the things shading it are (blocker search), then a
        // filter that widens with that gap, so shadows are sharp where they touch and soft away.
        const vec2 disk[12] = vec2[](vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621),
                                     vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893),
                                     vec2(0.507, 0.064), vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));
        float spin = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) * 6.2831853;
        mat2 rot = mat2(cos(spin), sin(spin), -sin(spin), cos(spin));
        float blockers = 0.0, found = 0.0;
        for (int i = 0; i < 12; ++i) {
            float d = sampleTexLod(map, clamp(at + rot * disk[i] * atlasTexel * 6.0, lo, hi), 0.0).r;
            if (d < p.z - 0.0002) blockers += d, found += 1.0;
        }
        if (found == 0.0) return 1.0;
        float gap = (p.z - blockers / found) * 16000.0; // world units (the cascades' depth range)
        float width = clamp(gap * 0.012 * soft / texel, 1.0, 8.0); // in shadow texels
        for (int i = 0; i < 12; ++i) lit += shadowTap(map, at + rot * disk[i] * atlasTexel * width, lo, hi, p.z - 0.0002);
        lit /= 12.0;
    }
    // Fade out over the last 10% of the shadow distance instead of a hard edge.
    float fade = clamp((FRAME.splits.w - depth) / (FRAME.splits.w * 0.1), 0.0, 1.0);
    return mix(1.0, lit, fade);
}

// Smooth value noise, for ripples and the ragged edges of puddles.
float hash3(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
float vnoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1, 0, 0)), f.x), mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), f.x), mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// Tangent frame from screen-space derivatives (no stored tangents needed): T along +u, B along +v.
mat3 cotangentFrame(vec3 n, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    return mat3(t * invmax, b * invmax, n);
}

// Image-based light: what the split-sum integral of GGX × Fresnel comes to (F0 scale, bias),
// fitted analytically (Karis) instead of a lookup texture.
vec2 envBRDF(float rough, float ndv) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022), c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

bool hasSky() { return FRAME.sky.x > 0.5; }
vec3 sky(vec3 dir, float level) { return skyRadiance(uint(FRAME.sky.x), FRAME.sky.y, FRAME.sky.w, dir, level); }
// The sky's light on a surface facing n (irradiance / π), from its spherical harmonics.
vec3 skyLight(vec3 n) {
    return FRAME.skySH[0].rgb * 0.282095 + FRAME.skySH[1].rgb * 0.488603 * n.y + FRAME.skySH[2].rgb * 0.488603 * n.z
         + FRAME.skySH[3].rgb * 0.488603 * n.x + FRAME.skySH[4].rgb * 1.092548 * n.x * n.y + FRAME.skySH[5].rgb * 1.092548 * n.y * n.z
         + FRAME.skySH[6].rgb * 0.315392 * (3.0 * n.z * n.z - 1.0) + FRAME.skySH[7].rgb * 1.092548 * n.x * n.z
         + FRAME.skySH[8].rgb * 0.546274 * (n.x * n.x - n.y * n.y);
}
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Height fog / aerial perspective: denser low down, coloured by the sky behind (blurred) and
// brighter towards the sun. Returns the colour seen through `dist` of it.
#ifndef KEEL_GL
#include "froxel.glsl"
#define GI_PROBES FRAME.giProbes
#include "gi.glsl"
#endif
vec3 applyFog(vec3 color, vec3 world) {
    vec3 ray = world - FRAME.camPos.xyz;
    float dist = length(ray);
    vec3 rd = ray / max(dist, 1e-3);
    if (!hasSky() || FRAME.fog.x <= 0.0) return color;
    float k = FRAME.fog.y;
    float start = FRAME.fog.x * exp(-k * (FRAME.camPos.z - FRAME.fog.w));
    float kz = k * rd.z;
    float amount = start * (abs(kz) > 1e-6 ? (1.0 - exp(-kz * dist)) / kz : dist);
    float f = min(1.0 - exp(-amount), FRAME.fog.z);
    vec3 fogColor = sky(normalize(vec3(rd.xy, max(rd.z, 0.05))), FRAME.sky.z) * 0.9;
    if (FRAME.sunColor.w > 0.5) fogColor += FRAME.sunColor.rgb * pow(max(dot(rd, FRAME.sunDir.xyz), 0.0), 8.0) * 0.25;
    return mix(color, fogColor, f);
}

vec3 ambientCube(vec3 n) {
    vec3 w = n * n;
    return w.x * (n.x > 0.0 ? DRAW.cube[0].rgb : DRAW.cube[1].rgb)
         + w.y * (n.y > 0.0 ? DRAW.cube[2].rgb : DRAW.cube[3].rgb)
         + w.z * (n.z > 0.0 ? DRAW.cube[4].rgb : DRAW.cube[5].rgb);
}

void main() {
    // A mirror's pass: nothing behind the mirror (the wall it's on, the room beyond).
    if (dot(FRAME.clip.xyz, FRAME.clip.xyz) > 0.5 && dot(v_world, FRAME.clip.xyz) < FRAME.clip.w) discard;
    // The mirror itself: the picture its pass made, where this pixel is (slightly darkened, as
    // real mirrors are), and nothing for the screen-space reflections to add.
    if (FRAME.mirror.x > 0.5 && (pc.textures & 0xFFFFu) == uint(FRAME.mirror.x)) {
        o_color = vec4(fetchTex(uint(FRAME.mirror.x), ivec2(gl_FragCoord.xy)).rgb * vec3(0.9, 0.92, 0.92), 1.0);
        o_gbuf = vec4(0.5, 0.5, 1.0, 0.0);
        return;
    }
    vec4 albedo = sampleTex(pc.textures & 0xFFFFu, v_uv);
    if (albedo.a < 0.5) discard; // alpha-tested texture (fences, foliage)
    vec4 tint = unpackUnorm4x8(pc.tint);
    vec3 base = albedo.rgb * tint.rgb;
    vec3 n = normalize(v_normal);
    vec3 geo = n; // the surface itself, before relief
    vec3 view = normalize(FRAME.camPos.xyz - v_world);
    // Relief and gloss from the material map. Derivatives first, while control flow is uniform.
    mat3 tbn = cotangentFrame(n, v_world, v_uv);
    float gloss = 0.15, metal = 0.0;
    uint materialMap = pc.material & 0xFFFFu, skyVisMap = pc.material >> 16;
    if (materialMap != 0u) {
        vec4 m = sampleTex(materialMap, v_uv);
        vec2 xy = m.xy * 2.0 - 1.0; // X, Y; Z rebuilt (B holds metalness)
        n = normalize(tbn * vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0))));
        metal = m.z;
        gloss = m.w;
    }
    // Weather: how open to the sky this spot is (the lightmap's sky map; moving things: their
    // sun visibility), then rain wetting it (darker, glossy) and snow lying on what faces up.
    float exposed = skyVisMap != 0u ? sampleTex(skyVisMap, v_lightmapUv).r : DRAW.params.y > 0.5 ? DRAW.params.x : 0.0;
    exposed = smoothstep(0.25, 0.7, exposed);
    if (FRAME.weather.x > 0.0) {
        float wet = FRAME.weather.x * exposed * (0.45 + 0.55 * max(geo.z, 0.0));
        base *= 1.0 - 0.45 * wet;
        gloss = mix(gloss, 0.93, wet * 0.85);
    }
    float snowCover = 0.0;
    if (FRAME.weather.y > 0.0) {
        float snow = FRAME.weather.y * exposed * smoothstep(0.35, 0.8, geo.z) * smoothstep(0.2, 0.6, vnoise(v_world * 0.05) * 0.5 + FRAME.weather.y);
        snowCover = snow;
        // Snow isn't flat white: drifts a little brighter, hollows a little bluer.
        float drift = vnoise(v_world * 0.012) * 0.6 + vnoise(v_world * 0.05) * 0.4;
        base = mix(base, mix(vec3(0.78, 0.82, 0.9), vec3(0.9, 0.91, 0.93), drift), snow);
        gloss = mix(gloss, 0.25, snow);
        metal *= 1.0 - snow;
    }
    int liquid = int(DRAW.params.w + 0.5);
    float liquidAlpha = 1.0;
    if (liquid == 1) {
        // A film of liquid: thickest in the middle (UV v = 0), ragged at the edge, rippled.
        float thickness = 1.0 - v_uv.y;
        float ragged = vnoise(v_world * 0.35) * 0.6 + vnoise(v_world * 1.1) * 0.4;
        float edge = 0.12 + 0.45 * ragged;
        if (thickness < edge) discard;
        liquidAlpha = smoothstep(edge, edge + 0.2, thickness);
        float e = 0.05;
        vec3 q = v_world * 0.6;
        vec3 grad = vec3(vnoise(q + vec3(e, 0, 0)) - vnoise(q - vec3(e, 0, 0)), vnoise(q + vec3(0, e, 0)) - vnoise(q - vec3(0, e, 0)),
                         vnoise(q + vec3(0, 0, e)) - vnoise(q - vec3(0, 0, e))) / (2.0 * e);
        n = normalize(n - (grad - n * dot(grad, n)) * 0.12);
        // Deeper liquid is richer in colour (Beer–Lambert-ish); thin edges just wet the surface.
        base = mix(base * 0.35, base, 0.4 + 0.6 * thickness);
        gloss = 0.95;
    } else if (liquid == 2) {
        // A body of liquid: light passes through it, more of it where it's thicker. Looking
        // straight through the middle is the longest path (rich, dense); the edges are thin.
        float through = abs(dot(n, view));
        base = mix(base * 1.15, base * 0.5, through);
        liquidAlpha = mix(0.45, 1.0, through);
        gloss = 0.92;
    }
    if (liquid == 4) {
        // A decal (a burn mark) on a disc: matte, darkest in the middle, its edge soft and ragged.
        float thickness = 1.0 - v_uv.y;
        float ragged = vnoise(v_world * 0.12) * 0.6 + vnoise(v_world * 0.45) * 0.4;
        liquidAlpha = smoothstep(0.0, 0.9, thickness + (ragged - 0.5) * 0.6);
        gloss = 0.08;
    }
    if (liquid == 3) {
        // Open water: waves (a few travelling swells plus noise ripples) bend the normal; looking
        // straight down you see into it, at a grazing angle it mirrors the sky (Fresnel, below).
        float t = DRAW.probe.z;
        vec2 p = v_world.xy, g = vec2(0.0);
        vec2 d1 = vec2(0.8, 0.6), d2 = vec2(-0.5, 0.86), d3 = vec2(0.2, -0.98);
        g += d1 * cos(dot(p, d1) * 0.035 + t * 1.1) * 0.09;
        g += d2 * cos(dot(p, d2) * 0.07 + t * 1.7) * 0.06;
        g += d3 * cos(dot(p, d3) * 0.13 + t * 2.3) * 0.04;
        // Two layers of ripples drifting different ways (the wind, and the surface's own churn).
        float e = 0.5;
        vec3 q = vec3(p * 0.09 + vec2(t * 0.25, t * 0.1), t * 0.35), q2 = vec3(p * 0.21 - vec2(t * 0.12, t * 0.3), t * 0.6);
        g += vec2(vnoise(q + vec3(e, 0, 0)) - vnoise(q - vec3(e, 0, 0)), vnoise(q + vec3(0, e, 0)) - vnoise(q - vec3(0, e, 0))) * 0.3;
        g += vec2(vnoise(q2 + vec3(e, 0, 0)) - vnoise(q2 - vec3(e, 0, 0)), vnoise(q2 + vec3(0, e, 0)) - vnoise(q2 - vec3(0, e, 0))) * 0.18;
        n = normalize(geo + vec3(-g, 0.0));
        float through = abs(dot(n, view));
        base = mix(base * 1.25, base * 0.6, through);
        liquidAlpha = mix(0.95, 0.62, through);
        gloss = 0.97;
    }
    if (DRAW.params.z > 0.5) {
        // Liquid: cut away above the surface; looking in through the cut, the inside (back)
        // faces stand in for the flat surface.
        if (dot(v_world, DRAW.plane.xyz) > DRAW.plane.w) discard;
        if (!gl_FrontFacing) {
            n = DRAW.plane.xyz;
            base *= 1.2;
        }
    }
    // Metals reflect in their own colour and scatter nothing: F0 is the albedo, diffuse goes.
    g_f0 = mix(vec3(0.04), base, metal);
    base *= 1.0 - metal;
    vec3 sunDir = FRAME.sunDir.xyz;
    // Relief can't light what the surface itself faces away from (no light leaking round corners).
    float facing = dot(geo, sunDir) > 0.0 ? max(dot(n, sunDir), 0.0) : 0.0;
    vec3 sun = FRAME.sunColor.rgb * FRAME.sunColor.w * facing;
    float rough = clamp(1.0 - gloss, 0.05, 1.0);
    vec3 sunSpec = FRAME.sunColor.rgb * FRAME.sunColor.w * facing * specGGX(n, sunDir, view, rough);
    // How much open sky this spot sees (reflections of the sky fade with it).
    float skyVis = 0.0;
    // Cavities in the relief (grout, pores) get less of the soft, baked light.
    float cavity = clamp(1.0 - (1.0 - dot(n, geo)) * 2.5, 0.45, 1.0);
    vec3 specular = vec3(0.0), env = vec3(0.0);
    bool realtime = FRAME.shadow.z > 0.5;
    uint lightmap = pc.textures >> 16;
    vec3 light;
    if (lightmap != 0u) {
        vec4 rgbm = sampleTex(lightmap, v_lightmapUv); // RGBM, range 8
        vec3 baked = rgbm.rgb * rgbm.a * 8.0 * cavity;
        // Night: what the sky gave (most of the light where it's open) goes; lamps stay.
        baked *= 1.0 - FRAME.weather.z * 0.9 * smoothstep(0.0, 0.45, skyVisMap != 0u ? sampleTex(skyVisMap, v_lightmapUv).r : 0.0);
        env = baked;
        skyVis = skyVisMap != 0u ? sampleTex(skyVisMap, v_lightmapUv).r : 0.0;
        if (realtime) {
            float vis = facing > 0.0 ? sunShadow(geo) : 0.0;
            light = baked + sun * vis;
            specular = sunSpec * vis;
        } else { // old maps: the sun is baked in; players and props darken it where they block it
            float shade = 1.0 - FRAME.shadow.y * (1.0 - sunShadow(n)) * facing * FRAME.sunColor.w;
            light = baked * shade;
        }
    } else if (DRAW.params.y > 0.5) {
        // Moving things: baked ambient cube plus the sun. With a real-time sun the shadow map
        // already has the world in it; otherwise the probe's sun visibility stands in for it.
        float vis = facing > 0.0 ? sunShadow(geo) : 0.0;
        vis = realtime ? vis : DRAW.params.x * vis;
        env = ambientCube(n) * cavity * (1.0 - FRAME.weather.z * 0.8 * max(DRAW.params.x, 0.35));
        light = env + sun * vis;
        // Probes don't know where the sky is: compare their light with the open sky's.
        if (hasSky()) skyVis = clamp(luma(env) / max(luma(skyLight(n)), 1e-3), 0.0, 1.0) * mix(0.6, 1.0, DRAW.params.x);
        specular = sunSpec * vis;
    } else if (hasSky()) {
        // No baked light of its own (water, map-less things): the sky's light here and the real
        // sun or moon, through its shadow — so it darkens at night like everything else.
        float vis = facing > 0.0 ? sunShadow(geo) : 0.0;
        env = skyLight(n);
        light = env + sun * vis;
        specular = sunSpec * vis;
    } else {
        light = vec3(sunLight(n));
    }
    uint count;
    vec3 dynSpec;
    light += dynamicLights(v_world, n, rough, view, count, dynSpec);
#ifndef KEEL_GL
    if (FRAME.gi.x > 0.5) {
        vec3 bounced = giIrradiance(v_world, n, view) * cavity; // their light, bounced
        if (FRAME.gi.z > 0.5) { o_color = vec4(bounced * 4.0, 1.0); o_gbuf = vec4(0.0); return; }
        light += bounced;
    }
#endif
    // Reflections (split-sum image-based light): the sky, prefiltered for this roughness, where
    // the sky is open; the light arriving here stands in for the room where it isn't.
    float ndv = clamp(dot(n, view), 1e-4, 1.0);
    vec3 r = reflect(-view, n);
    r = normalize(r + geo * max(-dot(r, geo), 0.0) * 1.05); // keep it above the surface
    vec2 ab = envBRDF(rough, ndv);
    float occlusion = skyVis * skyVis;
    // What's around: the nearest reflection probe (env_cubemap) where there is one, else the
    // light arriving here; the sky takes over as far as this spot sees it.
    // (probes were baked in daylight: at night what they show goes dark too)
    vec3 near = DRAW.probe.x > 0.5 ? skyRadiance(uint(DRAW.probe.x), 1.0 - 0.92 * FRAME.weather.z, 0.0, r, rough * DRAW.probe.y) : env;
    vec3 surroundings = hasSky() ? mix(near, sky(r, rough * FRAME.sky.z), occlusion) : near;
    vec3 reflection = surroundings * (g_f0 * ab.x + ab.y) * cavity;
    vec3 color = base * light + specular + dynSpec + reflection;
    // Snow glitters: here and there a crystal catches the sun (or a lamp) towards the eye.
    if (snowCover > 0.05) {
        vec3 cell = floor(v_world * 1.7);
        float h = fract(sin(dot(cell, vec3(12.9898, 78.233, 45.164))) * 43758.5453);
        float glint = step(0.975, h) * pow(max(dot(n, normalize(sunDir + view)), 0.0), 40.0);
        color += (FRAME.sunColor.rgb * FRAME.sunColor.w * (facing > 0.0 ? 1.0 : 0.0) + dynSpec * 4.0) * glint * snowCover * 1.2;
    }
    float alpha = tint.a * liquidAlpha;
    if (liquid == 3) {
        // Open water barely scatters light of its own: it's what's mirrored (more the flatter
        // you look: Fresnel) over what's seen through it (the bottom, more looking straight
        // down), tinted by a dim veil of its own colour. Blended over what's behind it, so:
        //   alpha = the share it covers, colour = what covers it.
        vec3 mirrored = hasSky() ? mix(env, sky(r, 0.0), occlusion) : vec3(0.45, 0.6, 0.8);
        float fres = 0.02 + 0.98 * pow(1.0 - clamp(abs(dot(n, view)), 0.0, 1.0), 5.0);
        float clear = 0.72 * clamp(abs(dot(geo, view)), 0.0, 1.0); // (how much of the bottom shows through)
        vec3 veil = base * (light * 0.3 + vec3(0.015));
        float cover = fres + (1.0 - fres) * (1.0 - clear);
        color = (mirrored * fres + veil * (1.0 - fres) * (1.0 - clear)) / max(cover, 1e-3);
        color += FRAME.sunColor.rgb * FRAME.sunColor.w * facing * specGGX(n, sunDir, view, 0.06) + dynSpec; // (the sun's glint, lamps')
        alpha = clamp(cover, 0.0, 1.0);
    } else if (liquid != 0 && liquid != 4) {
        // Wet: a sharp sun glint and the sky mirrored at grazing angles, on top of what's lit.
        vec3 mirrored = hasSky() ? mix(env, sky(r, 0.0), occlusion) : vec3(0.45, 0.6, 0.8);
        float fres = 0.02 + 0.98 * pow(1.0 - clamp(dot(n, view), 0.0, 1.0), 5.0);
        color += mirrored * fres * 0.9 + FRAME.sunColor.rgb * FRAME.sunColor.w * facing * specGGX(n, sunDir, view, 0.08);
        if (liquid == 1) alpha = clamp(alpha * (0.75 + fres), 0.0, 0.95);
    }
    if (alpha < 0.5 && liquid != 4 && liquid != 3) {
        // Glass (clear materials; denser translucent things like liquid or cracks shade normally): more reflective at grazing angles (Fresnel), plus a sharp sun glint.
        vec3 v = normalize(FRAME.camPos.xyz - v_world);
        vec3 nf = dot(n, v) < 0.0 ? -n : n; // the side facing us, whatever the winding
        float fresnel = pow(1.0 - clamp(dot(nf, v), 0.0, 1.0), 4.0);
        float glint = pow(max(dot(nf, normalize(sunDir + v)), 0.0), 150.0) * FRAME.sunColor.w;
        // Glass barely scatters light itself: mostly what's behind it, plus reflections.
        vec3 rg = reflect(-v, nf);
        vec3 mirrored = hasSky() ? mix(env, sky(rg, 0.0), max(occlusion, 0.25)) : vec3(0.45, 0.6, 0.8);
        color = color * 0.12 + FRAME.sunColor.rgb * glint * 3.0 + mirrored * fresnel * 0.8;
        // A neutral sheen where it turns away from you (no colour: grey to white), so glass reads
        // as glass at an angle.
        color += vec3(0.06 + fresnel * 0.35) * (hasSky() ? max(occlusion, 0.4) : 0.6);
        alpha = clamp(alpha * 0.6 + fresnel * 0.5 + glint, 0.0, 1.0);
    }
#ifndef KEEL_GL
    if (FRAME.froxel.x > 0.5) { // the fog grid: lit, shadowed fog (and the air's light)
        vec4 f = froxelFog(gl_FragCoord.xy / FRAME.screen.xy, dot(v_world - FRAME.camPos.xyz, FRAME.camFwd.xyz));
        color = color * f.a + f.rgb;
    } else
#endif
        color = applyFog(color, v_world);
    o_color = vec4(color, alpha);
    // View space as the post passes see it: x right, y down the screen, z forward.
    vec3 fwd = FRAME.camFwd.xyz, right = normalize(cross(fwd, vec3(0.0, 0.0, 1.0))), up = cross(right, fwd);
    vec3 nv = vec3(dot(n, right), -dot(n, up), dot(n, fwd));
    float reflectWeight = dot(g_f0 * ab.x + ab.y, vec3(0.333)) * cavity * (liquid != 0 ? 1.5 : 1.0);
    o_gbuf = vec4(octEncode(nv), rough, reflectWeight);
    if (FRAME.camFwd.w > 0.5) o_color.rgb = mix(o_color.rgb, heatmap(count), 0.6);
}
