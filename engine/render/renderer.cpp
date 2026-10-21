// The renderer's API-independent half: collects draws, lights and the camera each frame,
// works out shadow cascades, light clusters, culling and sorting, and hands the result to
// the backend (Vulkan or OpenGL) as a FramePacket.
#include <optional>
#include "render/renderer.h"
#include "core/time.h"
#include "asset/ksky.h"
#include "core/cvar.h"
#include "fs/vfs.h"
#include "core/log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace keel {
namespace {
// Planes (xyz·p + w ≥ 0 inside) of a view-projection's frustum. `sides` only: no near/far
// (sun cascades keep casters between the sun and the slice they shadow).
struct Frustum {
    glm::vec4 planes[6];
    int count = 0;
    Frustum(const glm::mat4& m, bool sidesOnly) {
        const glm::mat4 t = glm::transpose(m);
        planes[count++] = t[3] + t[0];
        planes[count++] = t[3] - t[0];
        planes[count++] = t[3] + t[1];
        planes[count++] = t[3] - t[1];
        if (!sidesOnly) planes[count++] = t[2]; // near (depth 0..1); no far: the sky is there
    }
    // The world box of a transformed model-space box, tested against every plane.
    bool sees(const glm::mat4& xf, const glm::vec3& lo, const glm::vec3& hi) const {
        const glm::vec3 c = glm::vec3(xf * glm::vec4((lo + hi) * 0.5f, 1.0f));
        const glm::vec3 h = (hi - lo) * 0.5f;
        const glm::mat3 a(glm::abs(glm::vec3(xf[0])), glm::abs(glm::vec3(xf[1])), glm::abs(glm::vec3(xf[2])));
        const glm::vec3 e = a * h;
        for (int i = 0; i < count; ++i) {
            const glm::vec3 n(planes[i]);
            if (glm::dot(n, c) + planes[i].w + glm::dot(glm::abs(n), e) < 0.0f) return false;
        }
        return true;
    }
};

Cvar r_vsync("r_vsync", "1", CVAR_ARCHIVE, "Wait for vertical sync (0 = uncapped)");
Cvar r_shadowres("r_shadowres", "4096", CVAR_ARCHIVE, "Sun shadow map size: 2048, 4096 or 8192 (2×2 cascades)");
Cvar r_cull("r_cull", "1", CVAR_NONE, "Skip what the camera (or a shadow) can't see");
Cvar r_occlusion("r_occlusion", "1", CVAR_ARCHIVE, "Skip what's hidden behind other things (hierarchical Z of a frame ago)");
Cvar r_skyintensity("r_skyintensity", "1", CVAR_ARCHIVE, "Sky brightness (times the map's)");
Cvar r_fog("r_fog", "1", CVAR_ARCHIVE, "Height fog / aerial perspective");
Cvar r_gi("r_gi", "1", CVAR_ARCHIVE, "Real-time GI (Vulkan, ray-tracing GPUs): light from flashlights, fires and lamps bounces off what's around (2: show only that)");
Cvar r_mirrors("r_mirrors", "1", CVAR_ARCHIVE, "Mirrors show the room (the nearest one in view; Vulkan)");
Cvar r_volumetricfog("r_volumetricfog", "1", CVAR_ARCHIVE, "Fog in a 3D grid (Vulkan): lit by the sun, its shadows and the lamps");
Cvar r_fogscale("r_fogscale", "1", CVAR_CHEAT, "Multiplies the map's fog density (testing)");
Cvar r_skycolor("r_skycolor", "0.45 0.6 0.8", CVAR_ARCHIVE, "Sky colour, \"r g b\" in sRGB");
Cvar r_exposure("r_exposure", "1.4", CVAR_ARCHIVE, "Scene brightness before tonemapping");
Cvar r_ssao("r_ssao", "1", CVAR_ARCHIVE, "Ambient occlusion: soft shadow in corners and under things (0 off .. 1 full)");
Cvar r_ssao_radius("r_ssao_radius", "24", CVAR_ARCHIVE, "Ambient occlusion reach (units)");
Cvar r_bloom("r_bloom", "0.035", CVAR_ARCHIVE, "Bloom: how much bright light glows over its surroundings (0 off)");
Cvar r_saturation("r_saturation", "1.05", CVAR_ARCHIVE, "Colour saturation after tonemapping");
Cvar r_taa("r_taa", "1", CVAR_ARCHIVE, "Temporal anti-aliasing: smooth edges using the last frames");
Cvar r_sharpen("r_sharpen", "0.3", CVAR_ARCHIVE, "Sharpening after TAA (0 off)");
Cvar r_shadowsoft("r_shadowsoft", "1", CVAR_ARCHIVE, "Soft sun shadows (PCSS): sharp where they touch, softer further away; the sun's size (0 = a fixed small blur)");
Cvar r_volumetric("r_volumetric", "0", CVAR_ARCHIVE, "Volumetric sunlight: shafts of sun in the air, shadowed by what's in the way (0 off)");
Cvar r_autoexposure("r_autoexposure", "1", CVAR_ARCHIVE, "Eye adaptation: brighter in the dark, darker in the sun, over a second or two (0 off)");
Cvar r_renderscale("r_renderscale", "1", CVAR_ARCHIVE, "The 3D scene's resolution as a fraction of the window's (0.5 to 1; TAA scales it up)");
Cvar r_dynres("r_dynres", "0", CVAR_ARCHIVE, "Dynamic resolution: lower the render scale as needed to hold this frame rate (0 off)");
Cvar r_gpuculling("r_gpuculling", "1", CVAR_ARCHIVE, "Occlusion culling on the GPU (Vulkan): hidden things aren't drawn, tested a frame sooner than on the CPU");
Cvar r_prepass("r_prepass", "1", CVAR_ARCHIVE, "Depth pre-pass: the scene's depth first, so each pixel is lit once (faster when things overlap)");
Cvar r_ssr("r_ssr", "0", CVAR_ARCHIVE, "Screen-space reflections: what's on screen mirrored in glossy floors, metal and water (0 off)");
Cvar r_shafts("r_shafts", "0.5", CVAR_ARCHIVE, "Sun shafts: light scattering in the air around the sun where the sky shows through (0 off)");
Cvar r_vignette("r_vignette", "0.25", CVAR_ARCHIVE, "Darkening towards the screen's corners");
Cvar r_dynamic("r_dynamic", "1", CVAR_ARCHIVE, "Real-time lights (flashlights, map light_dynamic/light_spot, scripts)");
Cvar r_maxlights("r_maxlights", "256", CVAR_ARCHIVE, "Most dynamic lights drawn per frame (nearest win)");
Cvar r_lightshadows("r_lightshadows", "4", CVAR_ARCHIVE, "Most dynamic lights casting shadows per frame (0 = none)");
Cvar r_lightshadowres("r_lightshadowres", "512", CVAR_ARCHIVE, "Shadow resolution per light face: 256, 512, 1024 or 2048");
Cvar r_lightdebug("r_lightdebug", "0", 0, "1 = tint the scene by how many lights touch each cluster");
Cvar r_shadowdist("r_shadowdist", "4000", CVAR_ARCHIVE, "How far sun shadows reach (units)");
Cvar r_drawlights("r_drawlights", "0", 0, "Draw a small box at every dynamic light");

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

uint32_t packColor(const glm::vec4& c) {
    auto b = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(c.a) << 24;
}

glm::vec3 forwardOf(const Camera& c) {
    const float yaw = glm::radians(c.yaw), pitch = glm::radians(c.pitch);
    return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
}

glm::mat4 viewProjection(const Camera& c, float aspect) {
    const glm::vec3 forward = forwardOf(c);
    const glm::vec3 up = glm::vec3(glm::rotate(glm::mat4(1), glm::radians(c.roll), forward) * glm::vec4(0, 0, 1, 0));
    glm::mat4 view = glm::lookAt(c.origin, c.origin + forward, up);
    glm::mat4 proj = glm::perspective(glm::radians(c.fovY), aspect, c.zNear, c.zFar);
    proj[1][1] *= -1.0f; // clip-space Y points down (Vulkan; OpenGL is set up to match)
    proj[2][0] -= c.shift.x, proj[2][1] -= c.shift.y;
    return proj * view;
}
} // namespace

glm::mat4 Renderer::cameraViewProjection(float aspect) const { return viewProjection(camera_, aspect); }

bool Renderer::init(const Window& window, bool validation, RenderApi api) {
    switch (api) {
    case RenderApi::OpenGL: backend_ = createOpenGLBackend(); break;
    case RenderApi::OpenGL40: backend_ = createLegacyOpenGLBackend(4, 0); break;
    case RenderApi::OpenGL31: backend_ = createLegacyOpenGLBackend(3, 1); break;
    case RenderApi::GLES30: backend_ = createLegacyOpenGLBackend(3, 0, true); break;
    default: backend_ = createVulkanBackend(); break;
    }
    if (!backend_->init(window, validation)) {
        backend_->shutdown();
        backend_.reset();
        return false;
    }
    log::info("Renderer: {}", backend_->name());
    // Fixed slots: 0 white, 1 a "missing" checker. (2-4 are the backend's render targets.)
    const std::vector<uint8_t> white(4, 255);
    backend_->createTexture(kWhiteTexture, 1, 1, false, {white});
    std::vector<uint8_t> missing(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const bool on = ((x / 8) ^ (y / 8)) & 1;
            uint8_t* p = &missing[(y * 64 + x) * 4];
            p[0] = on ? 255 : 0; p[1] = 0; p[2] = on ? 255 : 0; p[3] = 255;
        }
    backend_->createTexture(kMissingTexture, 64, 64, false, {missing});
    return true;
}

void Renderer::shutdown() {
    if (backend_) backend_->shutdown();
    backend_.reset();
    models_.clear();
    modelCache_.clear();
    textureCache_.clear();
    nextTexture_ = kFirstTextureSlot;
}

void Renderer::beginUi() { backend_->beginUi(); }

void Renderer::drawBox(const glm::vec3& mins, const glm::vec3& maxs, const glm::vec4& color, TextureId texture) {
    packet_.boxes.push_back({glm::vec4((mins + maxs) * 0.5f, float(texture)), glm::vec4((maxs - mins) * 0.5f, 0), color});
}

const Renderer::Sky* Renderer::loadSky(const std::string& path) {
    if (auto it = skies_.find(path); it != skies_.end()) return &it->second;
    const auto data = fs::read(path);
    KskyHeader h{};
    if (!data || data->size() < sizeof h) {
        log::warn("Sky {} not found", path);
        return nullptr;
    }
    std::memcpy(&h, data->data(), sizeof h);
    if (std::memcmp(h.magic, "KSKY", 4) != 0 || h.version != kKskyVersion) {
        log::warn("{}: not a sky (version {})", path, h.version);
        return nullptr;
    }
    std::vector<std::vector<uint8_t>> mips;
    size_t at = sizeof h;
    for (uint32_t k = 0; k < h.levels; ++k) {
        const size_t bytes = size_t(h.width >> k) * size_t(h.height >> k) * 4;
        if (at + bytes > data->size()) return nullptr;
        mips.emplace_back(data->begin() + ptrdiff_t(at), data->begin() + ptrdiff_t(at + bytes));
        at += bytes;
    }
    Sky sky;
    sky.texture = newTexture(h.width, h.height, false, mips);
    sky.levels = h.levels;
    sky.sunDir = glm::vec3(h.sunDir[0], h.sunDir[1], h.sunDir[2]);
    for (int i = 0; i < 9; ++i) sky.sh[i] = glm::vec3(h.sh[i * 3], h.sh[i * 3 + 1], h.sh[i * 3 + 2]);
    return &(skies_[path] = sky);
}

void Renderer::setAtmosphere(const Atmosphere& a) {
    atmosphere_ = a;
    sky_ = {};
    if (const Sky* s = a.sky.empty() ? nullptr : loadSky(a.sky)) sky_ = *s;
}

void Renderer::setProbes(const std::vector<std::pair<glm::vec3, std::string>>& probes) {
    if (probes == probeList_) return;
    probeList_ = probes;
    probes_.clear();
    for (const auto& [at, path] : probes)
        if (const Sky* s = loadSky(path)) probes_.push_back({at, s->texture, float(s->levels - 1)});
}

void Renderer::setSun(bool enabled, const glm::vec3& dir, const glm::vec3& color, bool realtime) {
    sunEnabled_ = enabled;
    sunRealtime_ = enabled && realtime;
    sunDir_ = glm::normalize(dir);
    sunColor_ = color;
}

void Renderer::drawSprite(const glm::vec3& at, float size, const glm::vec4& color, TextureId texture, float rotation, bool additive) {
    auto& list = additive ? packet_.glowSprites : packet_.sprites;
    if (list.size() >= 8192) return;
    uint32_t bits = texture;
    float tex;
    std::memcpy(&tex, &bits, 4);
    list.push_back({glm::vec4(at, size * 0.5f), color, glm::vec4(0, 0, 0, rotation), glm::vec4(0, 0, 0, tex)});
}

void Renderer::drawStreak(const glm::vec3& from, const glm::vec3& to, float width, const glm::vec4& color, TextureId texture, bool additive) {
    auto& list = additive ? packet_.glowSprites : packet_.sprites;
    if (list.size() >= 8192) return;
    uint32_t bits = texture;
    float tex;
    std::memcpy(&tex, &bits, 4);
    // up.xyz: half its length along it (non-zero marks a streak; its side is found per frame).
    list.push_back({glm::vec4((from + to) * 0.5f, width * 0.5f), color, glm::vec4(0.0f), glm::vec4((to - from) * 0.5f, tex)});
}

void Renderer::drawModel(ModelId model, const glm::mat4& transform, const glm::vec4& tint, const std::vector<glm::mat4>* skin,
                         const ModelLighting* lighting, bool castShadow, const glm::vec4* liquidPlane, LiquidStyle liquid, const Surface* surface) {
    if (!model.valid() || packet_.draws.size() >= kMaxDraws) return;
    ModelDraw d{model.index, transform, tint};
    d.castShadow = castShadow || shadowOnly_;
    d.shadowOnly = shadowOnly_;
    d.viewModel = viewModel_;
    d.texture = textureOverride_;
    d.liquid = liquid != LiquidStyle::None;
    const Model& mdl = models_[model.index];
    d.boundsMin = glm::vec3(1e30f);
    d.boundsMax = glm::vec3(-1e30f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner(i & 1 ? mdl.maxs.x : mdl.mins.x, i & 2 ? mdl.maxs.y : mdl.mins.y, i & 4 ? mdl.maxs.z : mdl.mins.z);
        const glm::vec3 w = glm::vec3(transform * glm::vec4(corner, 1.0f));
        d.boundsMin = glm::min(d.boundsMin, w);
        d.boundsMax = glm::max(d.boundsMax, w);
    }
    if (skin) d.boundsMin -= glm::vec3(32.0f), d.boundsMax += glm::vec3(32.0f); // animation leaves the bind pose
    d.drawIndex = uint32_t(packet_.draws.size());
    gpu::DrawData dd{};
    dd.model = transform;
    if (lighting) {
        for (int i = 0; i < 6; ++i) dd.cube[i] = glm::vec4(lighting->cube[i], 0);
        dd.params = glm::vec4(lighting->sunVisibility, 1, 0, 0);
    }
    if (liquidPlane) {
        dd.params.z = 1.0f;
        dd.plane = *liquidPlane;
    }
    dd.params.w = float(int(liquid));
    if (!probes_.empty()) { // the nearest reflection probe to the middle of it
        const glm::vec3 mid = (d.boundsMin + d.boundsMax) * 0.5f;
        const Probe* best = &probes_[0];
        for (const Probe& pr : probes_)
            if (glm::distance(pr.at, mid) < glm::distance(best->at, mid)) best = &pr;
        dd.probe = glm::vec4(float(best->texture), best->top, 0, 0);
    }
    static const auto t0 = std::chrono::steady_clock::now(); // water's waves move with this
    dd.probe.z = hasClock_ ? float(std::fmod(clock_, 3600.0)) : std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(), 3600.0f);
    if (surface)
        dd.probe.w = 1.0f + std::round(std::clamp(surface->metal, 0.0f, 1.0f) * 255.0f) + std::round(std::clamp(surface->rough, 0.0f, 1.0f) * 255.0f) * 256.0f;
    packet_.draws.push_back(dd);
    if (skin && mdl.skinned && packet_.joints.size() + skin->size() <= kMaxJointsPerFrame) {
        d.firstJoint = uint32_t(packet_.joints.size());
        for (const glm::mat4& m : *skin) packet_.joints.push_back(transform * m);
    }
    modelDraws_.push_back(d);
}

void Renderer::clearFrame() {
    modelDraws_.clear();
    packet_.smoke.clear();
    lights_.clear();
    packet_.draws.clear();
    packet_.joints.clear();
    packet_.boxes.clear();
    packet_.sprites.clear();
    packet_.glowSprites.clear();
}

void Renderer::drawFrame() {
    ImGui::Render();
    if (r_vsync.consumeModified()) backend_->setVsync(r_vsync.b());
    uint32_t width = 0, height = 0;
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<float, std::milli>(b - a).count(); };
    const auto t0 = clock::now();
    if (!backend_->beginFrame(width, height)) return clearFrame();
    const auto t1 = clock::now();
    buildPacket(width, height);
    const auto t2 = clock::now();
    backend_->render(packet_);
    const auto t3 = clock::now();
    timings_.wait = ms(t0, t1), timings_.build = ms(t1, t2), timings_.record = ms(t2, t3);
    backend_->gpuTimes(timings_.gpu);
    screenshotPath_.clear();
    clearFrame();
}

void Renderer::takeOcclusion() {
    HzbReadback back;
    if (!backend_->takeHzb(back) || back.depth.size() != size_t(kHzbWidth) * kHzbHeight) return;
    auto it = std::find_if(hzbPending_.begin(), hzbPending_.end(), [&](const auto& f) { return f.first == back.tag; });
    if (it == hzbPending_.end()) return;
    hzbFrame_ = it->second;
    std::erase_if(hzbPending_, [&](const auto& f) { return f.first <= back.tag; });
    // The pyramid: each level the farthest of the 2×2 below it (odd edges keep their last row).
    hzbLevels_.clear();
    hzbLevels_.push_back(std::move(back.depth));
    uint32_t w = kHzbWidth, h = kHzbHeight;
    while (w > 1 || h > 1) {
        const uint32_t nw = std::max(1u, (w + 1) / 2), nh = std::max(1u, (h + 1) / 2);
        const std::vector<float>& src = hzbLevels_.back();
        std::vector<float> dst(size_t(nw) * nh);
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x) {
                float m = 0.0f;
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx)
                        m = std::max(m, src[size_t(std::min(2 * y + dy, h - 1)) * w + std::min(2 * x + dx, w - 1)]);
                dst[size_t(y) * nw + x] = m;
            }
        hzbLevels_.push_back(std::move(dst));
        w = nw, h = nh;
    }
    hzbValid_ = true;
}

// Hidden: the box, seen from the grid's camera, is farther than everything already drawn over
// the whole patch of screen it covers. Anything touching the near plane or the screen's edge
// counts as seen.
bool Renderer::occluded(const glm::mat4& xf, const glm::vec3& lo, const glm::vec3& hi) const {
    glm::vec2 a(1e9f), b(-1e9f);
    float nearest = 1.0f;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
        const glm::vec4 c = hzbFrame_.viewProj * (xf * glm::vec4(corner, 1.0f));
        if (c.w < 2.0f) return false; // at or behind the camera
        const glm::vec3 ndc = glm::vec3(c) / c.w;
        a = glm::min(a, glm::vec2(ndc)), b = glm::max(b, glm::vec2(ndc));
        nearest = std::min(nearest, ndc.z);
    }
    if (a.x < -1.0f || a.y < -1.0f || b.x > 1.0f || b.y > 1.0f) return false;
    // Texels covered at level 0, one extra round (the camera moved a little since).
    int x0 = int((a.x * 0.5f + 0.5f) * kHzbWidth) - 1, x1 = int((b.x * 0.5f + 0.5f) * kHzbWidth) + 1;
    int y0 = int((a.y * 0.5f + 0.5f) * kHzbHeight) - 1, y1 = int((b.y * 0.5f + 0.5f) * kHzbHeight) + 1;
    size_t level = 0;
    while (level + 1 < hzbLevels_.size() && (x1 - x0 > 3 || y1 - y0 > 3)) x0 >>= 1, x1 >>= 1, y0 >>= 1, y1 >>= 1, ++level;
    const int w = std::max(1, int(kHzbWidth >> level)), h = std::max(1, int(kHzbHeight >> level));
    const std::vector<float>& d = hzbLevels_[level];
    for (int y = std::max(y0, 0); y <= std::min(y1, h - 1); ++y)
        for (int x = std::max(x0, 0); x <= std::min(x1, w - 1); ++x)
            if (d[size_t(y) * size_t(w) + size_t(x)] >= nearest) return false;
    return true;
}

void Renderer::buildPacket(uint32_t width, uint32_t height) {
    FramePacket& p = packet_;
    p.outWidth = width;
    p.outHeight = height;
    // Render scale: the 3D scene at a fraction of the window (TAA and the tonemap scale it up).
    // r_dynres <fps> moves it to hold that frame rate, a step at a time with a pause between.
    float scale = 1.0f;
    if (backend_->renderScale()) {
        scale = std::clamp(r_renderscale.f(), 0.5f, 1.0f);
        if (r_dynres.f() > 0.0f) {
            static double last = keel::now(), lastChange = 0;
            const double t = keel::now(), frame = t - last;
            last = t;
            dynFrame_ = dynFrame_ * 0.9 + frame * 0.1; // smoothed
            const double target = 1.0 / r_dynres.f();
            if (t - lastChange > 0.5) {
                if (dynFrame_ > target * 1.08 && dynScale_ > 0.5f) dynScale_ = std::max(0.5f, dynScale_ - 0.05f), lastChange = t;
                else if (dynFrame_ < target * 0.8 && dynScale_ < scale) dynScale_ = std::min(scale, dynScale_ + 0.05f), lastChange = t;
            }
            scale = std::min(scale, dynScale_);
        } else {
            dynScale_ = scale;
        }
    }
    width = std::max(1u, uint32_t(std::lround(float(width) * scale)));
    height = std::max(1u, uint32_t(std::lround(float(height) * scale)));
    p.width = width;
    p.height = height;
    p.viewProj = viewProjection(camera_, float(width) / float(height));
    p.shadowSize = r_shadowres.i() >= 8192 ? 8192u : r_shadowres.i() <= 2048 ? 2048u : 4096u;
    // TAA: a different sub-pixel offset each frame (Halton 2,3), and how to find last frame's
    // pixels. History is dropped on resize, toggling, or a jump (teleport, respawn).
    {
        const glm::mat4 unjittered = p.viewProj;
        const bool taa = r_taa.b();
        p.post.taa = taa;
        p.post.taaReset = !taa || !taaPrevValid_ || width != taaPrevW_ || height != taaPrevH_ || glm::length(camera_.origin - taaPrevOrigin_) > 128.0f;
        p.post.reproject = taaPrevViewProj_ * glm::inverse(unjittered);
        if (taa) {
            auto halton = [](uint32_t i, uint32_t b) {
                float f = 1.0f, r = 0.0f;
                for (; i > 0; i /= b) f /= float(b), r += f * float(i % b);
                return r;
            };
            const uint32_t i = (taaFrame_++ % 8) + 1;
            const glm::vec2 jitter((halton(i, 2) - 0.5f) * 2.0f / float(width), (halton(i, 3) - 0.5f) * 2.0f / float(height));
            glm::mat4 shift(1.0f); // clip space: x' = x + jx·w, y' = y + jy·w
            shift[3][0] = jitter.x;
            shift[3][1] = jitter.y;
            p.viewProj = shift * unjittered;
        }
        taaPrevViewProj_ = unjittered;
        taaPrevValid_ = taa;
        taaPrevW_ = width, taaPrevH_ = height;
        taaPrevOrigin_ = camera_.origin;
    }
    p.screenshot = screenshotPath_;

    // ── Sun cascades: each fits a slice of the view frustum in a bounding sphere (so its size
    // never changes as the camera turns) and snaps to whole texels (so edges don't shimmer).
    gpu::FrameData& frame = p.frame;
    frame = {};
    const glm::vec3 camFwd = forwardOf(camera_);
    {
        const float maxDist = std::clamp(r_shadowdist.f(), 500.0f, 16000.0f);
        // "Practical" splits (Zhang et al.): mostly logarithmic, so the nearest cascade stays small
        // (sharp) however far the shadows reach; the far ones take the distance.
        float splits[4];
        for (int c = 0; c < 4; ++c) {
            const float k = float(c + 1) / 4.0f, n = 8.0f;
            splits[c] = 0.92f * n * std::pow(maxDist / n, k) + 0.08f * (n + (maxDist - n) * k);
        }
        splits[3] = maxDist;
        const glm::vec3 right = glm::normalize(glm::cross(camFwd, glm::vec3(0, 0, 1) + glm::vec3(0.0001f, 0, 0))), upv = glm::cross(right, camFwd);
        const float tanY = std::tan(glm::radians(camera_.fovY) * 0.5f), tanX = tanY * float(width) / float(height);
        const glm::vec3 up = std::abs(sunDir_.z) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
        const float cascadeSize = float(p.shadowSize) / 2.0f;
        for (int c = 0; c < 4; ++c) {
            const float n = c == 0 ? 2.0f : splits[c - 1], f = splits[c];
            glm::vec3 corners[8], center(0.0f);
            for (int k = 0; k < 8; ++k) {
                const float d = k < 4 ? n : f;
                corners[k] = camera_.origin + camFwd * d + right * (k & 1 ? tanX : -tanX) * d + upv * (k & 2 ? tanY : -tanY) * d;
                center += corners[k] / 8.0f;
            }
            float radius = 0.0f;
            for (const glm::vec3& q : corners) radius = std::max(radius, glm::length(q - center));
            radius = std::ceil(radius / 16.0f) * 16.0f;
            const float texel = 2.0f * radius / cascadeSize;
            glm::mat4 lightView = glm::lookAt(glm::vec3(0), -sunDir_, up);
            glm::vec4 lc = lightView * glm::vec4(center, 1);
            lc.x = std::floor(lc.x / texel) * texel;
            lc.y = std::floor(lc.y / texel) * texel;
            center = glm::vec3(glm::inverse(lightView) * lc);
            lightView = glm::lookAt(center + sunDir_ * 8000.0f, center, up);
            frame.cascades[c] = glm::ortho(-radius, radius, -radius, radius, 1.0f, 16000.0f) * lightView;
            frame.splits[c] = f;
            frame.cascadeTexel[c] = texel;
        }
    }
    frame.sunDir = glm::vec4(sunDir_, float(kSunShadowSlot));
    frame.sunColor = glm::vec4(sunColor_, sunEnabled_ ? 1.0f : 0.0f);
    frame.shadow = glm::vec4(1.0f / float(p.shadowSize), 0.55f, sunRealtime_ ? 1.0f : 0.0f, std::max(r_shadowsoft.f(), 0.0f));
    // Strength, reach, air density; w: the sun takes part (a real-time sun; lamps always do).
    frame.volumetric = glm::vec4(std::max(r_volumetric.f(), 0.0f), 2500.0f, 0.00015f, sunRealtime_ ? 1.0f : 0.0f);


    // ── Dynamic lights: cull, pick shadow casters, cluster ──
    const glm::vec3 camUp0 = glm::vec3(glm::rotate(glm::mat4(1), glm::radians(camera_.roll), camFwd) * glm::vec4(0, 0, 1, 0));
    const glm::vec3 camRight = glm::normalize(glm::cross(camFwd, camUp0)), camUp = glm::cross(camRight, camFwd);
    if (!r_dynamic.b()) lights_.clear();
    std::erase_if(lights_, [&](const DynamicLight& l) { return l.radius <= 0 || glm::dot(l.origin - camera_.origin, camFwd) < -l.radius; });
    auto nearness = [&](const DynamicLight& l) { return glm::length(l.origin - camera_.origin) - l.radius; };
    std::stable_sort(lights_.begin(), lights_.end(), [&](const DynamicLight& a, const DynamicLight& b) { return nearness(a) < nearness(b); });
    lights_.resize(std::min<size_t>(lights_.size(), std::min<size_t>(std::max(r_maxlights.i(), 0), kMaxLights)));
    const uint32_t tile = std::clamp(uint32_t(r_lightshadowres.i()), 128u, 2048u);
    const uint32_t tilesPerRow = kAtlasSize / tile, tileBudget = std::min(tilesPerRow * tilesPerRow, kMaxShadowViews);
    p.lights.assign(lights_.size(), {});
    p.views.clear();
    struct ViewSphere { glm::vec3 center; float radius; };
    std::vector<ViewSphere> viewSpheres;
    int shadowLights = 0;
    for (size_t i = 0; i < lights_.size(); ++i) {
        const DynamicLight& l = lights_[i];
        gpu::Light& g = p.lights[i];
        g.posRadius = glm::vec4(l.origin, l.radius);
        const glm::vec3 dir = glm::length(l.direction) > 0 ? glm::normalize(l.direction) : glm::vec3(0, 0, -1);
        const float outer = std::clamp(l.outerCone, 1.0f, 89.0f), inner = std::clamp(l.innerCone, 0.0f, outer - 0.5f);
        g.spot = glm::vec4(dir, l.spot() ? std::cos(glm::radians(outer)) : -2.0f);
        g.extra = glm::vec4(std::cos(glm::radians(inner)), std::max(l.size, 0.0f), l.reflector && l.spot() ? 1.0f : 0.0f, 0);
        g.color = glm::vec4(l.color, -1.0f);
        const uint32_t need = l.spot() ? 1 : 6;
        if (!l.shadows || shadowLights >= r_lightshadows.i() || p.views.size() + need > tileBudget) continue;
        ++shadowLights;
        g.color.w = float(p.views.size());
        auto addView = [&](const glm::vec3& fwd, float fovDeg) {
            const glm::vec3 up = std::abs(fwd.z) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
            const uint32_t t = uint32_t(p.views.size());
            const glm::vec4 rect(float(t % tilesPerRow * tile) / kAtlasSize, float(t / tilesPerRow * tile) / kAtlasSize, float(tile) / kAtlasSize,
                                 float(tile) / kAtlasSize);
            p.views.push_back({glm::perspective(glm::radians(fovDeg), 1.0f, 1.0f, l.radius) * glm::lookAt(l.origin, l.origin + fwd, up), rect});
            viewSpheres.push_back({l.origin, l.radius});
        };
        if (l.spot()) addView(dir, std::min(2.0f * outer + 4.0f, 170.0f));
        else
            for (const glm::vec3& f : {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)})
                addView(f, 95.0f); // a little over 90° so PCF near face edges stays inside the frustum
    }
    ClusterView cv{camera_.origin, camFwd, camRight, camUp, std::tan(glm::radians(camera_.fovY) * 0.5f), float(width) / float(height)};
    buildClusters(cv, lights_, kMaxLightIndices, clusters_);
    p.clusters = &clusters_;
    lightStats_ = {uint32_t(lights_.size()), uint32_t(shadowLights), uint32_t(p.views.size()), uint32_t(clusters_.indices.size())};
    frame.camPos = glm::vec4(camera_.origin, float(lights_.size()));
    frame.weather = weather_;
    p.skyCloud = cloud_;
    weather_ = glm::vec4(0.0f), cloud_ = 0.0f; // (set again next frame by whoever has weather)
    frame.camFwd = glm::vec4(camFwd, r_lightdebug.f());
    frame.screen = glm::vec4(float(width), float(height), float(kAtlasSlot), float(kAtlasSize));
    if (r_drawlights.b())
        for (const DynamicLight& l : lights_) drawBox(l.origin - glm::vec3(4), l.origin + glm::vec3(4), glm::vec4(glm::min(l.color, glm::vec3(1)), 1));

    // ── Draw lists ──
    auto translucent = [](const ModelDraw& d, const Submesh& s) { return d.tint.a * s.color.a < 0.99f; };
    auto mesh = [&](const ModelDraw& d, const Submesh& s, bool shadow) {
        return MeshDraw{d.model, s.firstIndex, s.indexCount, d.drawIndex, d.firstJoint, (d.texture ? d.texture : s.texture) | (s.lightmap << 16),
                        packColor(d.tint * s.color), shadow ? 0u : s.material | (s.skyVisibility << 16), d.liquid};
    };
    p.opaque.clear();
    p.translucent.clear();
    for (auto& list : sunCasters_) list.clear();
    drawStats_ = {};
    const bool cull = r_cull.b();
    // Occlusion: ask for this frame's grid, take back an older one, and use it only while the
    // camera is still close to where it was drawn from (else things could pop in late).
    takeOcclusion();
    p.hzbTag = 0;
    if (r_occlusion.b()) {
        p.hzbTag = ++hzbTag_;
        hzbPending_.push_back({p.hzbTag, {p.viewProj, camera_.origin, camFwd}});
        if (hzbPending_.size() > 8) hzbPending_.erase(hzbPending_.begin());
    }
    // On the GPU when it can (a frame fresher, no read-back); else here, from an older grid.
    p.gpuCull = cull && r_occlusion.b() && r_gpuculling.b() && backend_->gpuCulling();
    p.cullOcclusion = p.gpuCull && lastCull_.valid && glm::length(camera_.origin - lastCull_.origin) < 48.0f && glm::dot(camFwd, lastCull_.forward) > 0.985f;
    p.cullViewProj = lastCull_.viewProj;
    lastCull_ = {true, p.viewProj, camera_.origin, camFwd};
    const uint32_t gpuHidden = p.gpuCull ? backend_->gpuCulled() : 0;
    // (The CPU's test still covers translucent things when the GPU does the opaque ones.)
    const bool cpuOcclusion = cull && r_occlusion.b() && hzbValid_ && glm::length(camera_.origin - hzbFrame_.origin) < 48.0f &&
                              glm::dot(camFwd, hzbFrame_.forward) > 0.985f;
    const Frustum view(p.viewProj, false);
    const Frustum cascade[4] = {Frustum(frame.cascades[0], true), Frustum(frame.cascades[1], true), Frustum(frame.cascades[2], true),
                                Frustum(frame.cascades[3], true)};
    atlasCasters_.resize(p.views.size());
    for (auto& list : atlasCasters_) list.clear();
    std::vector<std::pair<float, MeshDraw>> sorted;
    drawStats_.occluded += gpuHidden; // the GPU's (a frame or two ago)
    // Real-time GI: everything solid and still-shaped, in view or not, for the rays to hit.
    const bool rays = r_gi.b() && backend_->rayTracing();
    p.rayInstances.clear();
    p.frame.gi = rays ? glm::vec4(1.0f, 64.0f, r_gi.i() == 2 ? 1.0f : 0.0f, 0.0f) : glm::vec4(0.0f); // z: r_gi 2 shows only the bounced light
    // Mirrors: the nearest one in view that faces you gets the room drawn again, reflected in it.
    const ModelDraw* mirrorDraw = nullptr;
    const Submesh* mirrorSub = nullptr;
    glm::vec4 mirrorPlane(0.0f);
    if (r_mirrors.b() && backend_->mirrors()) {
        float best = 1e30f;
        for (const ModelDraw& d : modelDraws_) {
            if (d.shadowOnly) continue;
            for (const Submesh& s : models_[d.model].submeshes) {
                if (!s.mirror || !view.sees(d.transform, s.lo, s.hi)) continue;
                const glm::vec3 n = glm::normalize(glm::transpose(glm::inverse(glm::mat3(d.transform))) * glm::vec3(s.plane));
                const glm::vec3 on = glm::vec3(d.transform * glm::vec4(glm::vec3(s.plane) * s.plane.w, 1.0f));
                const float dist = glm::dot(n, on);
                if (glm::dot(n, camera_.origin) - dist < 0.5f) continue; // behind it
                const glm::vec3 centre = glm::vec3(d.transform * glm::vec4((s.lo + s.hi) * 0.5f, 1.0f));
                if (const float far = glm::length(centre - camera_.origin); far < best) best = far, mirrorDraw = &d, mirrorSub = &s, mirrorPlane = glm::vec4(n, dist);
            }
        }
    }
    p.mirror = mirrorDraw != nullptr;
    p.mirrorOpaque.clear();
    p.mirrorTranslucent.clear();
    p.frame.mirror = p.frame.clip = glm::vec4(0.0f);
    std::optional<Frustum> mirrorView;
    if (p.mirror) {
        // Reflection in the plane n·x = d: x − 2(n·x − d)n.
        const glm::vec3 n(mirrorPlane);
        glm::mat4 reflect(1.0f);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) reflect[c][r] -= 2.0f * n[c] * n[r];
        reflect[3] = glm::vec4(2.0f * mirrorPlane.w * n, 1.0f);
        p.mirrorViewProj = p.viewProj * reflect;
        p.mirrorInvViewProj = glm::inverse(p.mirrorViewProj);
        p.frame.mirror = glm::vec4(float(kMirrorSlot), 0, 0, 0);
        mirrorView.emplace(p.mirrorViewProj, false);
        // Only where the mirror is on screen gets drawn (its box's corners; all of it when the
        // box reaches behind the eye).
        glm::vec2 lo(1.0f), hi(-1.0f);
        bool behind = false;
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 c(k & 1 ? mirrorSub->hi.x : mirrorSub->lo.x, k & 2 ? mirrorSub->hi.y : mirrorSub->lo.y, k & 4 ? mirrorSub->hi.z : mirrorSub->lo.z);
            const glm::vec4 q = p.viewProj * mirrorDraw->transform * glm::vec4(c, 1.0f);
            if (q.w < 0.1f) { behind = true; break; }
            lo = glm::min(lo, glm::vec2(q) / q.w), hi = glm::max(hi, glm::vec2(q) / q.w);
        }
        if (behind) lo = glm::vec2(-1.0f), hi = glm::vec2(1.0f);
        lo = glm::clamp(lo, -1.0f, 1.0f), hi = glm::clamp(hi, -1.0f, 1.0f);
        // NDC to pixels (y down, as rows run), a pixel of margin.
        const glm::vec2 size(float(p.width), float(p.height));
        const glm::ivec2 a = glm::max(glm::ivec2(glm::floor((lo * 0.5f + 0.5f) * size)) - 2, glm::ivec2(0));
        const glm::ivec2 b = glm::min(glm::ivec2(glm::ceil((hi * 0.5f + 0.5f) * size)) + 2, glm::ivec2(size));
        p.mirrorRect = glm::ivec4(a, glm::max(b - a, glm::ivec2(1)));

        mirrorReflect_ = reflect;
        mirrorClip_ = glm::vec4(n, mirrorPlane.w - 0.25f); // (the wall it hangs on is cut too)
    }
    for (const ModelDraw& d : modelDraws_) {
        const Model& m = models_[d.model];
        for (const Submesh& s : m.submeshes) {
            // In the mirror: everything in front of it (your own body too), but not your hands.
            if (mirrorView && !d.viewModel && !s.mirror && mirrorView->sees(d.transform, s.lo, s.hi))
                (translucent(d, s) ? p.mirrorTranslucent : p.mirrorOpaque).push_back(mesh(d, s, false));
            if (rays && !d.liquid && d.firstJoint == UINT32_MAX && !translucent(d, s))
                p.rayInstances.push_back({d.transform, d.model, s.firstIndex, s.texture | (s.lightmap << 16), packColor(d.tint * s.color)});
            bool seen = !cull || view.sees(d.transform, s.lo, s.hi);
            const bool occlusion = cpuOcclusion && (!p.gpuCull || translucent(d, s));
            if (seen && occlusion && !d.shadowOnly && occluded(d.transform, s.lo, s.hi)) {
                seen = false;
                ++drawStats_.occluded;
            }
            if (translucent(d, s)) {
                if (!seen) continue;
                ++drawStats_.translucent;
                if (!d.shadowOnly) sorted.emplace_back(glm::length((d.boundsMin + d.boundsMax) * 0.5f - camera_.origin) + (d.liquid ? 2.0f : 0.0f),
                                       mesh(d, s, false));
                continue;
            }
            if (!d.shadowOnly && seen) {
                p.opaque.push_back(mesh(d, s, false));
                if (&d == mirrorDraw && &s == mirrorSub) p.opaque.back().textures = kMirrorSlot | (s.lightmap << 16); // shows its picture
                if (p.gpuCull) { // its world box, for the GPU's test
                    glm::vec3 lo(1e30f), hi(-1e30f);
                    for (int k = 0; k < 8; ++k) {
                        const glm::vec3 c(k & 1 ? s.hi.x : s.lo.x, k & 2 ? s.hi.y : s.lo.y, k & 4 ? s.hi.z : s.lo.z);
                        const glm::vec3 w = glm::vec3(d.transform * glm::vec4(c, 1.0f));
                        lo = glm::min(lo, w), hi = glm::max(hi, w);
                    }
                    p.opaque.back().lo = lo, p.opaque.back().hi = hi;
                }
                ++drawStats_.drawn;
                drawStats_.triangles += s.indexCount / 3;
            } else if (!d.shadowOnly) {
                ++drawStats_.culled;
            }
            if (d.castShadow && sunEnabled_)
                for (int c = 0; c < 4; ++c)
                    if (!cull || cascade[c].sees(d.transform, s.lo, s.hi)) sunCasters_[c].push_back(mesh(d, s, true)), ++drawStats_.sunShadow;
            // Light shadows: only things inside the light's sphere (and everything casts, the world too,
            // but not first person's arms and tool: they're drawn where the camera is, not in the world).
            for (size_t v = 0; v < viewSpheres.size() && !d.viewModel; ++v) {
                const glm::vec3 q = glm::clamp(viewSpheres[v].center, d.boundsMin, d.boundsMax);
                if (glm::dot(q - viewSpheres[v].center, q - viewSpheres[v].center) <= viewSpheres[v].radius * viewSpheres[v].radius)
                    atlasCasters_[v].push_back(mesh(d, s, true)), ++drawStats_.lightShadow;
            }
        }
    }
    // Shadow passes only write depth (no materials, no alpha test), so neighbouring submeshes of
    // one model drawn the same way merge into one draw when their indices follow on.
    auto merge = [](std::vector<MeshDraw>& list) {
        size_t out = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            if (out > 0) {
                MeshDraw& last = list[out - 1];
                const MeshDraw& d = list[i];
                if (d.model == last.model && d.draw == last.draw && d.firstJoint == last.firstJoint && d.firstIndex == last.firstIndex + last.indexCount) {
                    last.indexCount += d.indexCount;
                    continue;
                }
            }
            list[out++] = list[i];
        }
        list.resize(out);
    };
    drawStats_.sunShadow = drawStats_.lightShadow = 0;
    for (auto& list : sunCasters_) merge(list), drawStats_.sunShadow += uint32_t(list.size());
    for (auto& list : atlasCasters_) merge(list), drawStats_.lightShadow += uint32_t(list.size());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; }); // back to front
    for (auto& [dist, m] : sorted) p.translucent.push_back(m);

    p.sunPasses.clear();
    if (sunEnabled_) {
        const int half = int(p.shadowSize / 2);
        for (int c = 0; c < 4; ++c) p.sunPasses.push_back({frame.cascades[c], glm::ivec4((c & 1) * half, (c >> 1) * half, half, half), &sunCasters_[c]});
    }
    p.atlasPasses.clear();
    for (size_t v = 0; v < p.views.size(); ++v) {
        const glm::vec4 r = p.views[v].rect * float(kAtlasSize);
        p.atlasPasses.push_back({p.views[v].viewProj, glm::ivec4(r), &atlasCasters_[v]});
    }

    // ── Sky and air ──
    // The sky only needs directions: build its matrix with the camera at the origin, so it
    // stays exact however far from the map the camera is (a full one loses all precision there).
    {
        Camera at0 = camera_;
        at0.origin = glm::vec3(0.0f);
        p.invViewProj = glm::inverse(viewProjection(at0, float(width) / float(height)));
    }
    p.skyPhotoSunZ = 2.0f;
    if (sky_.texture) {
        const float intensity = atmosphere_.skyIntensity * std::max(r_skyintensity.f(), 0.0f);
        // Turn the photo so its sun stands where the map's does.
        float yaw = 0.0f;
        if (sunEnabled_ && glm::length(glm::vec2(sunDir_)) > 1e-3f && glm::length(glm::vec2(sky_.sunDir)) > 1e-3f)
            yaw = std::atan2(sunDir_.y, sunDir_.x) - std::atan2(sky_.sunDir.y, sky_.sunDir.x);
        frame.sky = glm::vec4(float(sky_.texture), intensity, float(sky_.levels - 1), yaw);
        p.skyPhotoSunZ = glm::length(sky_.sunDir) > 0.5f ? sky_.sunDir.z : 2.0f;
        // The SH rotate with the photo: bands 0/2 (z) stay, x/y pairs turn by yaw (and 2·yaw).
        const float c = std::cos(yaw), sn = std::sin(yaw), c2 = std::cos(2 * yaw), s2 = std::sin(2 * yaw);
        glm::vec3 sh[9];
        for (int i = 0; i < 9; ++i) sh[i] = sky_.sh[i] * intensity;
        // Band 1: (y, z, x) — rotating the world by yaw about z: x' = c·x − s·y, y' = s·x + c·y.
        const glm::vec3 y1 = sh[1], x1 = sh[3];
        sh[1] = c * y1 + sn * x1;
        sh[3] = c * x1 - sn * y1;
        // Band 2: (xy, yz, z², xz, x²−y²): (xz, yz) turn by yaw, (x²−y², xy) by 2·yaw.
        const glm::vec3 yz = sh[5], xz = sh[7], xy = sh[4], xx = sh[8];
        sh[5] = c * yz + sn * xz;
        sh[7] = c * xz - sn * yz;
        sh[4] = c2 * xy + s2 * xx;
        sh[8] = c2 * xx - s2 * xy;
        for (int i = 0; i < 9; ++i) frame.skySH[i] = glm::vec4(sh[i], 0.0f);
        if (r_fog.b()) frame.fog = glm::vec4(atmosphere_.fogDensity * std::max(r_fogscale.f(), 0.0f), atmosphere_.fogFalloff, atmosphere_.fogMax, atmosphere_.fogBaseZ);
    }
    // The fog grid, when there's fog or light in the air to fill it with.
    const bool airLit = frame.fog.x > 0.0f || frame.volumetric.x > 0.0f || !p.smoke.empty();
    frame.froxel = airLit && r_volumetricfog.b() && backend_->froxelFog() ? glm::vec4(1.0f, 16.0f, 16384.0f, 0.0f) : glm::vec4(0.0f);

    // Sky is given in display colours; undo exposure so it shows as typed.
    glm::vec3 sky(0.5f);
    std::sscanf(r_skycolor.str().c_str(), "%f %f %f", &sky.r, &sky.g, &sky.b);
    p.exposure = std::max(r_exposure.f(), 0.01f);
    // Night stays night: the eye adapts, but not all the way back to day.
    p.exposure *= glm::mix(1.0f, 0.3f, std::clamp(p.frame.weather.z, 0.0f, 1.0f));
    const float tanY = std::tan(glm::radians(camera_.fovY) * 0.5f);
    p.post.aoStrength = std::clamp(r_ssao.f(), 0.0f, 1.0f); // (the TAA fields were set with the camera above)
    p.post.aoRadius = std::max(r_ssao_radius.f(), 0.001f); // (small for close-up work: a modeller sets it to its view)
    p.post.bloomStrength = std::clamp(r_bloom.f(), 0.0f, 1.0f);
    p.post.saturation = r_saturation.f();
    p.post.vignette = r_vignette.f();
    p.post.sharpen = std::max(r_sharpen.f(), 0.0f);
    p.post.ssr = std::max(r_ssr.f(), 0.0f);
    p.prepass = r_prepass.b();
    { // Particles face the camera; blended ones are drawn back to front.
        const glm::vec3 f = forwardOf(camera_), right = glm::normalize(glm::cross(f, glm::vec3(0, 0, 1) + glm::vec3(0.0001f, 0, 0))),
                        up = glm::cross(right, f);
        for (auto* list : {&p.sprites, &p.glowSprites})
            for (gpu::Sprite& s : *list) {
                if (const glm::vec3 half(s.up); glm::dot(half, half) > 0.0f) { // a streak: its side faces the eye
                    glm::vec3 side = glm::cross(half, camera_.origin - glm::vec3(s.posSize));
                    side = glm::length(side) > 1e-6f ? glm::normalize(side) : right;
                    s.right = glm::vec4(side * s.posSize.w, 0.0f);
                    s.posSize.w = 1.0f;
                    continue;
                }
                s.right = glm::vec4(right, s.right.w), s.up = glm::vec4(up, s.up.w);
            }
        std::sort(p.sprites.begin(), p.sprites.end(), [&](const gpu::Sprite& a, const gpu::Sprite& b) {
            return glm::dot(glm::vec3(a.posSize) - camera_.origin, f) > glm::dot(glm::vec3(b.posSize) - camera_.origin, f);
        });
    }
    {
        static double lastTime = keel::now();
        const double t = keel::now();
        p.post.adaptDt = float(std::clamp(t - lastTime, 0.0, 0.25));
        lastTime = t;
        const bool on = r_autoexposure.b() && p.post.bloomStrength > 0.0f; // (it reads the bloom chain)
        p.post.adaptReset = on && !adaptOn_;
        p.post.adapt = adaptOn_ = on;
    }
    p.post.shafts = glm::vec4(0.0f);
    if (sunEnabled_ && sunDir_.z > -0.05f && r_shafts.f() > 0.0f) { // the sun on screen, or just off it
        const glm::vec4 c = p.viewProj * glm::vec4(camera_.origin + sunDir_ * 10000.0f, 1.0f);
        if (c.w > 0.0f) {
            const glm::vec2 uv = glm::vec2(c) / c.w * 0.5f + 0.5f;
            const float onScreen = 1.0f - glm::smoothstep(0.6f, 1.4f, glm::length((uv - 0.5f) * 2.0f) - 0.4f);
            p.post.shafts = glm::vec4(uv, r_shafts.f() * onScreen * glm::smoothstep(-0.05f, 0.15f, sunDir_.z) * (1.0f - p.skyCloud), 0.0f); // (none through cloud)
            p.post.sunColor = glm::vec4(sunColor_, 0.0f);
        }
    }
    p.post.proj = glm::vec4(tanY * float(width) / float(height), tanY, camera_.zNear, camera_.zFar);
    p.clearColor = glm::vec3(srgbToLinear(sky.r), srgbToLinear(sky.g), srgbToLinear(sky.b)) / p.exposure;
    // The mirror's pass: this frame's data, but seen from the reflected eye, cut at the mirror,
    // lit by every light (the clusters are this view's), and no fog grid (that's this view's too).
    if (p.mirror) {
        p.mirrorFrame = p.frame;
        const float lights = p.frame.camPos.w, debug = p.frame.camFwd.w;
        p.mirrorFrame.camPos = glm::vec4(glm::vec3(mirrorReflect_ * glm::vec4(glm::vec3(p.frame.camPos), 1.0f)), lights);
        p.mirrorFrame.camFwd = glm::vec4(glm::normalize(glm::vec3(mirrorReflect_ * glm::vec4(glm::vec3(p.frame.camFwd), 0.0f))), debug);
        p.mirrorFrame.mirror = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        p.mirrorFrame.froxel = glm::vec4(0.0f);
        p.mirrorFrame.clip = mirrorClip_;
    }
}

} // namespace keel
