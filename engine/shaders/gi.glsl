// Real-time GI probes (gi.comp fills them, with ray queries): a grid of kGiDims probes,
// FRAME.gi.y apart, that follows the eye; a probe's slot is its world cell wrapped into the grid,
// so moving keeps what's already there. They hold the light that dynamic lights (flashlight,
// fire, explosions, lamps) bounce off what's around; the lightmaps already bounce the rest.
// GI_PROBES: the probes to read (#define before including).
const ivec3 kGiDims = ivec3(24, 24, 12);
// a mod kGiDims, also below zero (GLSL's % isn't defined there)
ivec3 giWrap(ivec3 a) { return a - kGiDims * ivec3(floor(vec3(a) / vec3(kGiDims))); }
uint giSlot(ivec3 cell) {
    ivec3 s = giWrap(cell);
    return uint((s.z * kGiDims.y + s.y) * kGiDims.x + s.x);
}
// Irradiance / π from L1 spherical harmonics of radiance, facing n.
vec3 giShIrradiance(GiProbe p, vec3 n) {
    return max(0.282095 * p.sh[0].rgb + 0.325735 * (p.sh[1].rgb * n.y + p.sh[2].rgb * n.z + p.sh[3].rgb * n.x), vec3(0.0));
}
// The light bounced onto a surface at world (normal n, towards the eye v): the 8 probes round it,
// weighted by nearness, whether they're in front of the surface, and whether they can see it
// (what's between, by the distances each one keeps: Chebyshev, as in DDGI).
vec3 giIrradiance(vec3 world, vec3 n, vec3 v) {
    float s = FRAME.gi.y;
    ivec3 origin = ivec3(FRAME.giOrigin.xyz);
    vec3 p = world + (n * 0.2 + v * 0.1) * s;
    vec3 g = p / s - 0.5; // probe c sits at (c + 0.5) * s
    ivec3 base = ivec3(floor(g));
    vec3 f = g - vec3(base);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int k = 0; k < 8; ++k) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2), c = base + o;
        if (any(lessThan(c, origin)) || any(greaterThanEqual(c, origin + kGiDims))) continue;
        GiProbe pr = GI_PROBES.p[giSlot(c)];
        if (pr.tag.w < 0.5 || ivec3(pr.tag.xyz) != c) continue;
        vec3 tri = mix(1.0 - f, f, vec3(o));
        float w = tri.x * tri.y * tri.z * pr.sh[0].w;
        vec3 at = (vec3(c) + 0.5) * s;
        vec3 toProbe = normalize(at - world);
        float wrap = (dot(toProbe, n) + 1.0) * 0.5;
        w *= wrap * wrap + 0.2;
        vec3 d = p - at;
        float r = length(d);
        d /= max(r, 1e-4);
        vec3 d2 = d * d;
        vec2 m = vec2(d.x > 0.0 ? pr.dist[0].x : pr.dist[0].y, d.x > 0.0 ? pr.dist[1].z : pr.dist[1].w) * d2.x
               + vec2(d.y > 0.0 ? pr.dist[0].z : pr.dist[0].w, d.y > 0.0 ? pr.dist[2].x : pr.dist[2].y) * d2.y
               + vec2(d.z > 0.0 ? pr.dist[1].x : pr.dist[1].y, d.z > 0.0 ? pr.dist[2].z : pr.dist[2].w) * d2.z;
        if (r > m.x) {
            float variance = abs(m.y - m.x * m.x), e = r - m.x;
            float cheb = variance / (variance + e * e);
            w *= max(cheb * cheb * cheb, 0.02);
        }
        sum += giShIrradiance(pr, n) * w;
        total += w;
    }
    return total > 1e-4 ? sum / total : vec3(0.0);
}
