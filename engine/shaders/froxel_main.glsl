// The body of froxel.comp and froxel_rt.comp (the same, plus rays for the sky above each cell).
// The fog grid (froxel.glsl), in two dispatches:
//  mode 0, one thread per cell: the fog's density there (height fog, plus the air when
//    r_volumetric's on) and the light it scatters towards the eye (the sky's, the sun through its
//    shadow, the lamps of its cluster), at a point jittered along the slice each frame and blended
//    with last frame's cell at the same place in the world, so it's smooth and doesn't crawl.
//  mode 1, one thread per column: walked front to back, adding up light and transmittance.
#include "backend.glsl"
#define KEEL_FRAME_ONLY
#include "model.glsl"
#include "sky.glsl"
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(buffer_reference, std430) buffer Cells4 { vec4 c[]; };    // per cell: scattered light (per unit), extinction
layout(buffer_reference, std430) writeonly buffer Out { uvec2 f[]; };
layout(buffer_reference, std430) readonly buffer SmokeCells { uint v[]; }; // two cells a word
layout(buffer_reference, std430) writeonly buffer SmokeSky { uvec2 f[]; };  // a column's smoke alone
PUSH_BLOCK {
    FrameData frame;
    Cells4 cur, prev;
    Out integrated;
    vec4 proj;    // tan(half fov x), tan(half fov y)
    vec4 prevPos; // last frame's eye; w 1 = there's a last frame to blend with
    vec4 prevFwd; // and its view direction; w this frame's jitter along the slice (0..1)
    uint mode;
    uint frameIndex;
    uvec2 tlas;   // KEEL_FOG_RAYS: the scene's acceleration structure
    SmokeCells smoke; // the smoke grid (render/backend.h kSmokeDims): density | shade << 8 a cell
    SmokeSky smokeSky; // integrate: each column's smoke alone, end to end (smoke_sky.frag lays it over the sky)
    vec4 smokeOrigin; // its world corner; w a clock (seconds) for the billows, < 0: no smoke
} pc;
#define FRAME pc.frame
#define KEEL_FROXEL_WRITER
#include "froxel.glsl"
#include "airlight.glsl"

// ── Smoke (shared/smoke.h): the grid's cells, blended, roughened by drifting billows ──
const ivec3 kSmokeDims = ivec3(160, 160, 48);
const float kSmokeCell = 16.0;
vec2 smokeCell(ivec3 c) {
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, kSmokeDims))) return vec2(0.0);
    uint i = uint((c.z * kSmokeDims.y + c.y) * kSmokeDims.x + c.x);
    uint h = (pc.smoke.v[i >> 1] >> ((i & 1u) * 16u)) & 0xFFFFu;
    return vec2(float(h & 0xFFu), float(h >> 8)) / 255.0;
}
// x density, y shade (both blended between the 8 cells round w).
vec2 smokeAt(vec3 w) {
    vec3 g = (w - pc.smokeOrigin.xyz) / kSmokeCell - 0.5;
    ivec3 b = ivec3(floor(g));
    vec3 f = g - vec3(b);
    float d = 0.0, s = 0.0;
    for (int k = 0; k < 8; ++k) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2);
        vec3 tri = mix(1.0 - f, f, vec3(o));
        vec2 c = smokeCell(b + o);
        float wgt = tri.x * tri.y * tri.z;
        d += c.x * wgt;
        s += c.y * c.x * wgt;
    }
    return vec2(d, d > 1e-4 ? s / d : 0.5);
}
float hash3(vec3 p) {
    p = fract(p * 0.3183099 + 0.1) * 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float valueNoise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1, 0, 0)), f.x), mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), f.x), mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// The density there after the billows: thick inside, torn and drifting at the edges.
float smokeDensity(vec3 w, float d) {
    if (d <= 0.0) return 0.0;
    float t = pc.smokeOrigin.w;
    vec3 q = w * 0.03 + vec3(t * 0.04, t * 0.03, -t * 0.12);
    float n = 0.5 * valueNoise(q) + 0.3 * valueNoise(q * 2.3 + vec3(t * 0.07)) + 0.2 * valueNoise(q * 5.1 - vec3(t * 0.1));
    // Thin smoke (the rim) is torn most; the thick middle only darkens and lightens a little.
    return clamp(d * 1.6 - (1.0 - n) * mix(1.1, 0.5, d), 0.0, 1.0) * mix(0.75, 1.15, n);
}

vec3 viewRay(vec3 fwd, vec2 uv) { // not normalized: 1 along fwd
    vec3 right = normalize(cross(fwd, vec3(0.0, 0.0, 1.0))), up = cross(right, fwd);
    vec2 s = (uv * 2.0 - 1.0) * pc.proj.xy;
    return fwd + right * s.x - up * s.y;
}

// Last frame's cell (blended) at a world point; w < 0 when it was off that grid.
vec4 history(vec3 w) {
    vec3 fwd = pc.prevFwd.xyz, right = normalize(cross(fwd, vec3(0.0, 0.0, 1.0))), up = cross(right, fwd);
    vec3 v = w - pc.prevPos.xyz;
    float depth = dot(v, fwd);
    if (depth < FRAME.froxel.y) return vec4(-1.0);
    vec2 uv = (vec2(dot(v, right), -dot(v, up)) / depth / pc.proj.xy) * 0.5 + 0.5;
    vec3 g = vec3(uv * vec2(kFroxels.xy), froxelSlice(depth)) - 0.5;
    if (any(lessThan(g, vec3(0.0))) || any(greaterThan(g, vec3(kFroxels) - 1.0))) return vec4(-1.0);
    ivec3 b = ivec3(floor(g));
    vec3 f = g - vec3(b);
    vec4 r = vec4(0.0);
    for (int k = 0; k < 8; ++k) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2);
        float wgt = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y) * (o.z != 0 ? f.z : 1.0 - f.z);
        r += pc.prev.c[froxelIndex(uvec3(min(b + o, ivec3(kFroxels) - 1)))] * wgt;
    }
    return r;
}

void inject(uvec3 c) {
    vec2 uv = (vec2(c.xy) + 0.5) / vec2(kFroxels.xy);
    vec3 ray = viewRay(FRAME.camFwd.xyz, uv), rd = normalize(ray);
    vec3 w = FRAME.camPos.xyz + ray * froxelBoundary(float(c.z) + pc.prevFwd.w);
    float fog = 0.0;
    if (FRAME.sky.x > 0.5 && FRAME.fog.x > 0.0) fog = FRAME.fog.x * exp(-FRAME.fog.y * (w.z - FRAME.fog.w));
    float air = FRAME.volumetric.x > 0.0 ? FRAME.volumetric.z : 0.0;
    vec2 smoke = pc.smokeOrigin.w >= 0.0 ? smokeAt(w) : vec2(0.0);
    float smokeExt = smokeDensity(w, smoke.x) * 0.035; // (thick smoke: ~30 units hide what's behind)
    vec3 light = vec3(0.0);
    if (fog + air + smokeExt > 0.0) {
        const bool sunOn = FRAME.sunColor.w > 0.5;
        float lit = sunOn ? sunlitAt(w) : 0.0;
        float mu = dot(rd, FRAME.sunDir.xyz);
        vec3 lamps = lightsAt(w, rd, uv);
        // The fog: lit by the sky (blurred, as the old height fog was) and, where the sun reaches
        // it, glowing towards the sun; the lamps light it too.
        vec3 sky = FRAME.sky.x > 0.5 ? skyRadiance(uint(FRAME.sky.x), FRAME.sky.y, FRAME.sky.w, normalize(vec3(rd.xy, max(rd.z, 0.05))), FRAME.sky.z) * 0.9 : vec3(0.0);
        vec3 smokeSky = sky; // (the smoke's own: steady, no per-frame noise to average away)
#ifdef KEEL_FOG_RAYS
        // How much sky this point sees: one ray a frame into the sky above it, in a different
        // direction each frame (the blend with last frame's cells averages them). Under a roof
        // the fog gets next to no sky light, so rooms stay clear instead of filling with grey.
        uint h = (c.x * 73856093u) ^ (c.y * 19349663u) ^ (c.z * 83492791u) ^ (pc.frameIndex * 2654435761u);
        h = h * 747796405u + 2891336453u;
        float r1 = float(h & 0xFFFFu) / 65535.0, r2 = float(h >> 16) / 65535.0;
        vec3 up = normalize(vec3(cos(r1 * 6.2831853), sin(r1 * 6.2831853), 0.0) * sqrt(r2) * 1.2 + vec3(0.0, 0.0, 1.0));
        rayQueryEXT q;
        rayQueryInitializeEXT(q, accelerationStructureEXT(pc.tlas), gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, w, 1.0, up, 6000.0);
        while (rayQueryProceedEXT(q)) {}
        float open = rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
        sky *= mix(0.06, 1.0, open);
        if (smokeExt > 0.0) { // under a roof or not: one ray straight up
            rayQueryEXT q2;
            rayQueryInitializeEXT(q2, accelerationStructureEXT(pc.tlas), gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, w, 1.0, vec3(0.0, 0.0, 1.0), 6000.0);
            while (rayQueryProceedEXT(q2)) {}
            smokeSky *= rayQueryGetIntersectionTypeEXT(q2, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.3;
        }
#else
        // Where the sun can't reach (under a roof, mostly) the sky's light is dimmer too.
        if (sunOn) sky *= mix(0.3, 1.0, lit);
        smokeSky = sky;
#endif
        vec3 fogLight = sky + (sunOn ? FRAME.sunColor.rgb * lit * pow(max(mu, 0.0), 8.0) * 0.25 : vec3(0.0)) + lamps * 12.0;
        // The air (r_volumetric): shafts of sun and haloes round lamps; it adds light only.
        vec3 airLight = (sunOn && FRAME.volumetric.w > 0.5 ? FRAME.sunColor.rgb * lit * phaseHG(mu, 0.6) : vec3(0.0)) + lamps * 7.0;
        light = fog * fogLight + air * airLight * FRAME.volumetric.x;
        if (smokeExt > 0.0) {
            // Smoke: the sky's light all round, the sun through its shadows and through the smoke
            // between it and the sun (a few steps that way), the lamps; tinted by its shade.
            float toward = sunOn ? smokeAt(w + FRAME.sunDir.xyz * 24.0).x + smokeAt(w + FRAME.sunDir.xyz * 64.0).x + smokeAt(w + FRAME.sunDir.xyz * 128.0).x : 0.0;
            vec3 sunIn = sunOn ? FRAME.sunColor.rgb * lit * exp(-toward * 1.4) * 0.5 : vec3(0.0);
            // The smoke above shades what's under it (the sky's light comes mostly from above).
            float above = smokeAt(w + vec3(0.0, 0.0, 32.0)).x + smokeAt(w + vec3(0.0, 0.0, 80.0)).x;
            light += smokeExt * smoke.y * (smokeSky * mix(1.0, 0.35, clamp(above * 0.5, 0.0, 1.0)) + sunIn + lamps * 4.0);
        }
    }
    vec4 now = vec4(light, fog + smokeExt);
    uint i = froxelIndex(c);
    if (pc.prevPos.w > 0.5) {
        vec4 h = history(w);
        if (h.w >= 0.0) now = mix(h, now, 0.12);
    }
    pc.cur.c[i] = now;
}

void integrate(uvec2 xy) {
    vec2 uv = (vec2(xy) + 0.5) / vec2(kFroxels.xy);
    float rayScale = length(viewRay(FRAME.camFwd.xyz, uv)); // ray length per unit of depth
    vec3 ray = viewRay(FRAME.camFwd.xyz, uv);
    vec3 light = vec3(0.0);
    // The fog never hides more than its max (FRAME.fog.z; 0 or 1: no cap): once the fog alone has
    // hidden that much, the cells further on keep only their smoke (and its share of the light).
    float through = 1.0, fogThrough = 1.0, keep = 1.0 - FRAME.fog.z;
    vec3 smokeLight = vec3(0.0);
    float smokeThrough = 1.0;
    bool capped = FRAME.fog.z > 0.0 && FRAME.fog.z < 1.0;
    float near = 0.0;
    for (uint z = 0u; z < kFroxels.z; ++z) {
        float far = froxelBoundary(float(z + 1u)), len = (far - near) * rayScale;
        vec3 w = FRAME.camPos.xyz + ray * (near + far) * 0.5;
        near = far;
        vec4 cell = pc.cur.c[froxelIndex(uvec3(xy, z))];
        // The smoke here, sampled again (exact: it doesn't move with the slices' jitter as the fog
        // would); the rest of the cell's extinction is the fog.
        float smokeHere = pc.smokeOrigin.w >= 0.0 ? min(smokeDensity(w, smokeAt(w).x) * 0.035, cell.w) : 0.0;
        float fog = max(cell.w - smokeHere, 0.0);
        if (capped && fogThrough <= keep && cell.w > 1e-9) {
            cell.rgb *= (cell.w - fog) / cell.w;
            cell.w -= fog;
            fog = 0.0;
        }
        fogThrough *= exp(-fog * len);
        if (smokeHere > 1e-9) { // the smoke's share of the cell
            float se = smokeHere, st = exp(-se * len);
            smokeLight += smokeThrough * cell.rgb * (se / cell.w) * (1.0 - st) / se;
            smokeThrough *= st;
        }
        float t = exp(-cell.w * len);
        light += through * (cell.w > 1e-9 ? cell.rgb * (1.0 - t) / cell.w : cell.rgb * len);
        through *= t;
        pc.integrated.f[froxelIndex(uvec3(xy, z))] = uvec2(packHalf2x16(light.rg), packHalf2x16(vec2(light.b, through)));
    }
    if (pc.smokeOrigin.w >= 0.0) pc.smokeSky.f[xy.y * kFroxels.x + xy.x] = uvec2(packHalf2x16(smokeLight.rg), packHalf2x16(vec2(smokeLight.b, smokeThrough)));
}

void main() {
    uvec3 id = gl_GlobalInvocationID;
    if (any(greaterThanEqual(id.xy, kFroxels.xy))) return;
    if (pc.mode == 0u) inject(id);
    else integrate(id.xy);
}
