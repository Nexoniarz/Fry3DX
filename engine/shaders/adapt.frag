#version 460
// Eye adaptation (1×1): the scene's average brightness (geometric mean of 16 taps of the
// smallest bloom level), turned into an exposure that keeps a mid grey where it should be,
// eased towards (0.6x to 1.8x, so nothing burns out) from last frame's at `speed` per second (faster into the dark is slower,
// as eyes are). The tonemap multiplies its exposure by it.
#define KEEL_POST // screen-space: render targets, sampled clamped
#include "backend.glsl"
PUSH_BLOCK {
    uint src;      // the smallest bloom level
    uint prev;     // last frame's (0 = start fresh)
    float dt;
    float speed;
    float key;     // the brightness to aim for
    float minE, maxE;
    float pad;
} pc;
layout(location = 0) out vec4 o_exposure;

void main() {
    float sum = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            vec3 c = sampleTexLod(pc.src, (vec2(x, y) + 0.5) / 4.0, 0.0).rgb;
            sum += log(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-4));
        }
    float average = exp(sum / 16.0);
    float target = clamp(pc.key / average, pc.minE, pc.maxE);
    if (pc.prev == 0u) { o_exposure = vec4(target); return; }
    float last = sampleTexLod(pc.prev, vec2(0.5), 0.0).r;
    float rate = target > last ? pc.speed * 0.6 : pc.speed; // brightening (into the dark) is slower
    o_exposure = vec4(last + (target - last) * (1.0 - exp(-pc.dt * rate)));
}
