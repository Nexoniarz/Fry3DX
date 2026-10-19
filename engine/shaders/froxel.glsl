// The fog grid ("froxels", as in Frostbite / id Tech): kFroxels cells over the screen and along
// the view, slices spaced exponentially from FRAME.froxel.y to .z. froxel.comp fills it; entry k
// of a column holds the light the fog adds (rgb) and how much of what's behind gets through (a)
// from the eye to the far side of slice k.
const uvec3 kFroxels = uvec3(160, 90, 64);
uint froxelIndex(uvec3 c) { return (c.z * kFroxels.y + c.y) * kFroxels.x + c.x; }
// Where slice boundary j lies (view depth); slice k spans boundaries k and k + 1.
float froxelBoundary(float j) { return FRAME.froxel.y * pow(FRAME.froxel.z / FRAME.froxel.y, j / float(kFroxels.z)); }
float froxelSlice(float depth) { return log(max(depth, 1e-3) / FRAME.froxel.y) / log(FRAME.froxel.z / FRAME.froxel.y) * float(kFroxels.z); }

#ifndef KEEL_FROXEL_WRITER
vec4 froxelEntry(ivec2 xy, int z) {
    uvec2 v = FRAME.froxels.f[froxelIndex(uvec3(clamp(xy, ivec2(0), ivec2(kFroxels.xy) - 1), clamp(z, 0, int(kFroxels.z) - 1)))];
    return vec4(unpackHalf2x16(v.x), unpackHalf2x16(v.y));
}
vec4 froxelColumn(ivec2 xy, float c) { // c: entry index, fractional
    if (c < 0.0) return mix(vec4(0.0, 0.0, 0.0, 1.0), froxelEntry(xy, 0), clamp(c + 1.0, 0.0, 1.0));
    int z = int(c);
    return mix(froxelEntry(xy, z), froxelEntry(xy, z + 1), c >= float(kFroxels.z - 1) ? 0.0 : fract(c));
}
// The fog between the eye and a point at uv on screen, depth along the view: (light, transmittance).
vec4 froxelFog(vec2 uv, float depth) {
    vec2 g = uv * vec2(kFroxels.xy) - 0.5;
    ivec2 b = ivec2(floor(g));
    vec2 f = g - vec2(b);
    // Entry k ends at boundary k + 1; nearer than the first slice's far side fades in from nothing.
    float c = max(froxelSlice(depth) - 1.0, -1.0);
    return mix(mix(froxelColumn(b, c), froxelColumn(b + ivec2(1, 0), c), f.x), mix(froxelColumn(b + ivec2(0, 1), c), froxelColumn(b + ivec2(1, 1), c), f.x), f.y);
}
#endif
