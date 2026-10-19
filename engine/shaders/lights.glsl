// Clustered dynamic lights for mesh.frag. Grid constants match render/clusters.h.
const uvec3 kClusters = uvec3(16, 9, 24);
const float kClusterNear = 8.0, kClusterFar = 16384.0;

uint clusterIndex(vec3 world) {
    float depth = dot(world - FRAME.camPos.xyz, FRAME.camFwd.xyz);
    uint z = depth <= kClusterNear ? 0u : min(uint(log(depth / kClusterNear) / log(kClusterFar / kClusterNear) * float(kClusters.z)), kClusters.z - 1u);
    uvec2 tile = min(uvec2(gl_FragCoord.xy / FRAME.screen.xy * vec2(kClusters.xy)), kClusters.xy - 1u);
    return (z * kClusters.y + tile.y) * kClusters.x + tile.x;
}

// 1 = lit. 3×3 bilinear PCF taps inside the light's tile of the shadow atlas.
float atlasShadow(int view, vec3 world, vec3 n, float dist) {
    ShadowView sv = SHADOW_VIEW(view);
    vec4 c = sv.viewProj * vec4(world + n * (0.75 + dist * 0.006), 1.0);
    if (c.w <= 0.0) return 1.0;
    c.xyz /= c.w;
    if (any(greaterThan(abs(c.xy), vec2(1.0))) || c.z > 1.0) return 1.0;
    float texel = 1.0 / FRAME.screen.w;
    vec2 lo = sv.rect.xy + texel, hi = sv.rect.xy + sv.rect.zw - texel;
    vec2 at = sv.rect.xy + (c.xy * 0.5 + 0.5) * sv.rect.zw;
    uint atlas = uint(FRAME.screen.z);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) lit += shadowTap(atlas, at + vec2(x, y) * texel, lo, hi, c.z - 0.00002);
    return lit / 9.0;
}

// GGX specular: height-correlated Smith visibility, Schlick Fresnel (F0: g_f0).
// Times π, because our light values are "what a white wall shows" (diffuse = albedo × light).
// Multiply by n·l and the light. rough: perceptual roughness.
// Reflectance at normal incidence for this surface (mesh.frag sets it: 0.04 for dielectrics,
// the albedo for metals).
vec3 g_f0 = vec3(0.04);

vec3 specGGX(vec3 n, vec3 l, vec3 v, float rough) {
    vec3 h = normalize(l + v);
    float a = max(rough * rough, 0.002), a2 = a * a;
    float ndl = max(dot(n, l), 1e-4), ndv = max(dot(n, v), 1e-4), ndh = max(dot(n, h), 0.0), vdh = max(dot(v, h), 0.0);
    float q = ndh * ndh * (a2 - 1.0) + 1.0;
    float d = a2 / (3.14159265 * q * q);
    float vis = 0.5 / (ndl * sqrt(ndv * ndv * (1.0 - a2) + a2) + ndv * sqrt(ndl * ndl * (1.0 - a2) + a2));
    vec3 f = g_f0 + (1.0 - g_f0) * pow(1.0 - vdh, 5.0);
    return min(3.14159265 * d * vis * f, vec3(64.0));
}

// Light reaching the surface from every dynamic light in its cluster: diffuse (returned) and
// the highlights (spec), sharper and stronger the glossier the surface.
vec3 dynamicLights(vec3 world, vec3 n, float rough, vec3 v, out uint count, out vec3 spec) {
    count = 0u;
    spec = vec3(0.0);
    if (FRAME.camPos.w < 0.5) return vec3(0.0);
    // A mirror's pass sees from somewhere else than the clusters were made for: every light.
    bool every = FRAME.mirror.y > 0.5;
    uvec2 cell = every ? uvec2(0u, min(uint(FRAME.camPos.w), 64u)) : CELL(clusterIndex(world));
    count = cell.y;
    vec3 total = vec3(0.0);
    for (uint k = 0u; k < cell.y; ++k) {
        Light l = LIGHT(every ? k : LIGHT_INDEX(cell.x + k));
        vec3 toLight = l.posRadius.xyz - world;
        float dist = length(toLight);
        if (dist >= l.posRadius.w) continue;
        vec3 L = toLight / max(dist, 0.001);
        float ndl = dot(n, L);
        if (ndl <= 0.0) continue;
        float a = 1.0 - dist / l.posRadius.w;
        float atten = a * a;
        if (l.spot.w > -1.5) atten *= spotShape(l, dot(-L, l.spot.xyz));
        if (atten <= 0.0) continue;
        if (l.color.w >= 0.0) {
            int view = int(l.color.w);
            if (l.spot.w <= -1.5) { // point light: one atlas tile per cube face, +X -X +Y -Y +Z -Z
                vec3 d = -toLight, ad = abs(d);
                view += ad.x >= ad.y && ad.x >= ad.z ? (d.x > 0.0 ? 0 : 1) : ad.y >= ad.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5);
            }
            atten *= atlasShadow(view, world, n, dist);
        }
        total += l.color.rgb * atten * ndl;
        // Area light (a sphere of radius extra.y): the highlight comes from the point on it
        // nearest the reflected ray, and widens to match (Karis 2013, "representative point").
        vec3 Ls = L;
        float norm = 1.0;
        if (l.extra.y > 0.0) {
            vec3 r = reflect(-v, n);
            vec3 toRay = dot(toLight, r) * r - toLight;
            vec3 closest = toLight + toRay * clamp(l.extra.y / max(length(toRay), 1e-3), 0.0, 1.0);
            Ls = normalize(closest);
            float a = max(rough * rough, 0.002), a2 = clamp(a + l.extra.y / (2.0 * dist), 0.0, 1.0);
            norm = (a / a2) * (a / a2);
        }
        spec += l.color.rgb * atten * ndl * specGGX(n, Ls, v, rough) * norm;
    }
    return total;
}

vec3 heatmap(uint count) {
    float t = clamp(float(count) / 16.0, 0.0, 1.0);
    return count == 0u ? vec3(0.0, 0.0, 0.15) : mix(vec3(0.0, 0.4, 1.0), vec3(1.0, 0.1, 0.0), t);
}
