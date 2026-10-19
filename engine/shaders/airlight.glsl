// The light in the air at a point (volumetric.frag, froxel.comp): the sun through its shadow
// cascades, and the dynamic lights of that point's cluster with a one-tap shadow each. Needs
// FRAME, LIGHT, CELL, LIGHT_INDEX and SHADOW_VIEW (model.glsl with KEEL_FRAME_ONLY).
float sunlitAt(vec3 w) {
    float depth = dot(w - FRAME.camPos.xyz, FRAME.camFwd.xyz);
    if (depth > FRAME.splits.w) return 1.0;
    int c = depth < FRAME.splits.x ? 0 : depth < FRAME.splits.y ? 1 : depth < FRAME.splits.z ? 2 : 3;
    vec4 p = FRAME.cascades[c] * vec4(w, 1.0);
    vec2 uv = p.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || p.z > 1.0) return 1.0;
    vec2 base = vec2(c & 1, c >> 1) * 0.5;
    float t = FRAME.shadow.x;
    return shadowTap(uint(FRAME.sunDir.w), base + uv * 0.5, base + t, base + 0.5 - t, p.z - 0.0005);
}

// The light cluster a point in the air is in (render/clusters.h), from its pixel's place on screen.
const uvec3 kClusters = uvec3(16, 9, 24);
const float kClusterNear = 8.0, kClusterFar = 16384.0;
uint clusterAt(vec3 w, vec2 uv) {
    float depth = dot(w - FRAME.camPos.xyz, FRAME.camFwd.xyz);
    uint z = depth <= kClusterNear ? 0u : min(uint(log(depth / kClusterNear) / log(kClusterFar / kClusterNear) * float(kClusters.z)), kClusters.z - 1u);
    uvec2 tile = min(uvec2(uv * vec2(kClusters.xy)), kClusters.xy - 1u);
    return (z * kClusters.y + tile.y) * kClusters.x + tile.x;
}
float phaseHG(float mu, float g) { return (1.0 - g * g) / (12.566 * pow(1.0 + g * g - 2.0 * g * mu, 1.5)); }
// The dynamic lights lighting the air at w (towards the eye along -rd).
vec3 lightsAt(vec3 w, vec3 rd, vec2 uv) {
    if (FRAME.camPos.w < 0.5) return vec3(0.0);
    uvec2 cell = CELL(clusterAt(w, uv));
    vec3 total = vec3(0.0);
    for (uint k = 0u; k < min(cell.y, 8u); ++k) {
        Light l = LIGHT(LIGHT_INDEX(cell.x + k));
        vec3 toLight = l.posRadius.xyz - w;
        float dist = length(toLight);
        if (dist >= l.posRadius.w) continue;
        vec3 L = toLight / max(dist, 0.001);
        float a = 1.0 - dist / l.posRadius.w, atten = a * a;
        if (l.spot.w > -1.5) atten *= spotShape(l, dot(-L, l.spot.xyz));
        if (atten <= 0.0) continue;
        if (l.color.w >= 0.0) { // shadowed: one tap in its atlas tile
            int view = int(l.color.w);
            if (l.spot.w <= -1.5) {
                vec3 d = -toLight, ad = abs(d);
                view += ad.x >= ad.y && ad.x >= ad.z ? (d.x > 0.0 ? 0 : 1) : ad.y >= ad.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5);
            }
            ShadowView sv = SHADOW_VIEW(view);
            vec4 c = sv.viewProj * vec4(w, 1.0);
            if (c.w > 0.0) {
                c.xyz /= c.w;
                if (all(lessThanEqual(abs(c.xy), vec2(1.0))) && c.z <= 1.0) {
                    float texel = 1.0 / FRAME.screen.w;
                    vec2 at = sv.rect.xy + (c.xy * 0.5 + 0.5) * sv.rect.zw;
                    atten *= shadowTap(uint(FRAME.screen.z), at, sv.rect.xy + texel, sv.rect.xy + sv.rect.zw - texel, c.z - 0.00005);
                }
            }
        }
        total += l.color.rgb * atten * phaseHG(dot(rd, -L), 0.3);
    }
    return total;
}

