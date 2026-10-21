#pragma once
#include "render/backend.h"
#include "render/clusters.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace keel {

class Window;
struct KmdlModel;

struct Camera {
    glm::vec3 origin{0.0f};
    float yaw = 0.0f;   // degrees, 0 = +X, counter-clockwise
    float pitch = 0.0f; // degrees, positive looks down (Quake convention)
    float roll = 0.0f;  // degrees, positive tilts right
    float fovY = 74.0f; // degrees
    glm::vec2 shift{0.0f}; // where the picture's middle sits on the screen (-1..1 across, down): a view beside panels
    float zNear = 2.0f, zFar = 16384.0f; // what's nearer or further isn't drawn (units)
};

// Baked light for a moving model (from the map's probes): ambient cube + sun visibility.
// A whole model's surface where it has no material map: how metal (0..1) and how rough (0..1).
struct Surface {
    float metal = 0.0f, rough = 0.85f;
};

struct ModelLighting {
    glm::vec3 cube[6]; // light arriving from +X -X +Y -Y +Z -Z
    float sunVisibility = 1.0f;
};

// Liquid shading: a thin film on a surface (puddles, splats; thicker towards the middle of the
// model's UV v = 0 end, with an uneven edge), or a glossy body (drops, what's in a bottle).
enum class LiquidStyle { None = 0, Film = 1, Body = 2, Water = 3, Decal = 4 }; // Water: open water's surface (waves, deep colour)

struct ModelId {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
};

// World space is Quake-like: Z up, units roughly inches. The scene renders in HDR and is
// tonemapped to the screen; UI draws on top. The graphics API sits behind a RenderBackend
// (Vulkan or OpenGL 4.6), picked at startup.
class Renderer {
public:
    bool init(const Window& window, bool validation, RenderApi api);
    void shutdown();
    const char* apiName() const { return backend_ ? backend_->name() : "none"; }
    GpuInfo gpuInfo() const { return backend_ ? backend_->gpuInfo() : GpuInfo{}; }
    // Starts a Dear ImGui frame; UI code runs between this and drawFrame().
    void beginUi();

    // Loaded through the virtual filesystem and cached by path.
    TextureId loadTexture(std::string_view path); // .yapf
    // A picture made in memory (8-bit RGBA, sRGB), cached under `name` like a loaded file, so
    // models whose material names it use it; made again in place if the name's taken. (Fry3DX:
    // reference images.)
    TextureId createTexture(std::string_view name, uint32_t w, uint32_t h, const uint8_t* rgba);
    // Hot reload: textures and models whose files changed since they were loaded are loaded
    // again in place (same ids). Returns their paths.
    std::vector<std::string> reloadChanged();
    ModelId loadModel(std::string_view path);     // .kmdl
    // A model built in memory (e.g. map geometry), cached under `name`.
    ModelId createModel(std::string_view name, const KmdlModel& model);
    // Replaces (or creates) the model cached under `name`; the old GPU buffers are freed
    // once no frame in flight can still use them. For meshes that change (breaking glass).
    ModelId updateModel(std::string_view name, const KmdlModel& model);
    glm::vec3 modelMins(ModelId m) const { return m.valid() ? models_[m.index].mins : glm::vec3(0); }
    glm::vec3 modelMaxs(ModelId m) const { return m.valid() ? models_[m.index].maxs : glm::vec3(0); }

    // A loaded texture for ImGui::Image (ImTextureID); 0 if it isn't one.
    uint64_t uiTexture(TextureId texture) { return backend_ ? backend_->uiTexture(texture) : 0; }
    UiRawRead uiRawRead(TextureId texture) { return backend_ ? backend_->uiRawRead(texture) : UiRawRead{}; }
    // Pixel size of a loaded texture.
    glm::ivec2 textureSize(TextureId texture) const {
        const auto it = textureSizes_.find(texture);
        return it == textureSizes_.end() ? glm::ivec2(0) : it->second;
    }

    void setCamera(const Camera& cam) { camera_ = cam; }
    const Camera& camera() const { return camera_; }
    // Volumetric smoke this frame (SmokeField::bake over kSmokeDims cells from `corner`); `clock`
    // turns its billows. Nothing set: none.
    void setSmoke(const glm::vec3& corner, std::vector<uint16_t> cells, float clock) {
        packet_.smoke = std::move(cells);
        packet_.smokeOrigin = glm::vec4(corner, clock);
    }
    // The camera's view-projection (clip space: Vulkan's, Y down), for placing 2D overlays on the world.
    glm::mat4 cameraViewProjection(float aspect) const;
    void drawBox(const glm::vec3& mins, const glm::vec3& maxs, const glm::vec4& color, TextureId texture = kWhiteTexture);
    // A particle: a camera-facing square `size` across, unlit (color can be HDR-bright).
    // additive: light added on (fire, sparks, flashes); else blended (smoke, dust).
    void drawSprite(const glm::vec3& at, float size, const glm::vec4& color, TextureId texture, float rotation = 0.0f, bool additive = true);
    // A sprite stretched from `from` to `to`, `width` across (`widthTo` at `to`: < 0, the same),
    // each end turned to face the eye as much as it can (rain; lines drawn in pieces meet exactly).
    void drawStreak(const glm::vec3& from, const glm::vec3& to, float width, const glm::vec4& color, TextureId texture, bool additive = false, float widthTo = -1.0f);
    // A grid of lines on the plane through `centre` along u and v (unit), drawn for each pixel:
    // every 10^n metres (`metre`: units in one), each set showing by how far apart its lines are
    // on the screen, the axes coloured; look: the lines' shade (0 dark, 1 light), how faint, how
    // strong; fading out `reach` from the centre.
    void drawGrid(const glm::vec3& centre, const glm::vec3& u, const glm::vec3& v, float metre, const glm::vec3& look, float reach);
    // skin: one matrix per joint (anim::skinMatrices) for skinned models; ignored otherwise.
    // lighting: probe light for moving things (nullptr = lightmap or simple sun shading).
    void drawModel(ModelId model, const glm::mat4& transform, const glm::vec4& tint = glm::vec4(1.0f),
                   const std::vector<glm::mat4>* skin = nullptr, const ModelLighting* lighting = nullptr, bool castShadow = true,
                   const glm::vec4* liquidPlane = nullptr, LiquidStyle liquid = LiquidStyle::None, const Surface* surface = nullptr);
    // liquidPlane (world space: xyz normal, w distance): draws a closed mesh as liquid filling it
    // up to the plane — above it is cut away, and its back faces show as the flat surface.
    // Alpha below 1 (tint × material) makes a submesh translucent: sorted, blended, after opaque.
    // The map's sun; dir points towards it. realtime: the lightmap has no direct sun, so it is
    // drawn here for every surface with cascaded shadows (the world casts too). Otherwise
    // it only lights probe-lit models and their shadows darken the baked light.
    void setSun(bool enabled, const glm::vec3& dir, const glm::vec3& color, bool realtime = false);
    // The sky (a .ksky; "" = none, the flat r_skycolor) and the air: its brightness and height
    // fog (density per unit at baseZ, thinning out by `falloff` per unit up).
    struct Atmosphere {
        std::string sky;
        float skyIntensity = 0.35f;
        float fogDensity = 0.00005f, fogFalloff = 0.0005f, fogMax = 0.9f, fogBaseZ = 0.0f;
    };
    void setAtmosphere(const Atmosphere& a);
    // This frame's weather (FrameData::weather): x wet, y snow cover, z night, w lightning flash.
    // Per frame, like draws: not set, it's clear.
    void setWeather(const glm::vec4& w, float cloud = 0.0f) { weather_ = w, cloud_ = cloud; }
    // The game's clock (seconds): water's swells move with it, so floating things match them.
    void setClock(double t) { clock_ = t, hasClock_ = true; }
    // Reflection probes (env_cubemap, baked by assetc as .ksky): glossy surfaces reflect the
    // nearest one. Call with the map's list every frame (it's only reloaded when it changes).
    void setProbes(const std::vector<std::pair<glm::vec3, std::string>>& probes);
    // Draws made while this is on only cast shadows (sun, dynamic lights) and aren't seen:
    // the first-person player's own body.
    void setShadowOnly(bool on) { shadowOnly_ = on; }
    // What's drawn next is the first-person view model (mirrors show your body, not these).
    void setViewModel(bool on) { viewModel_ = on; }
    // Models drawn next wear this picture instead of their own (0: their own): one
    // model, many pictures.
    void setTextureOverride(TextureId texture) { textureOverride_ = texture; }
    // A real-time light for this frame only (call every frame, like drawModel).
    void addLight(const DynamicLight& light) { lights_.push_back(light); }
    // Stats from the last frame, for r_lightinfo / tools.
    struct LightStats { uint32_t lights = 0, shadowed = 0, shadowViews = 0, indices = 0; };
    const LightStats& lightStats() const { return lightStats_; }
    // What the last frame drew (r_speeds): submesh draws in the view, culled away, into the sun's
    // cascades and lights' shadows.
    // Where the last frame's time went (r_profile): CPU (ms) and each GPU pass.
    struct Timings {
        float wait = 0, build = 0, record = 0; // waiting for a free frame (the GPU), building the draw list, recording + submitting
        std::vector<std::pair<const char*, float>> gpu;
    };
    const Timings& timings() const { return timings_; }
    struct DrawStats { uint32_t drawn = 0, culled = 0, occluded = 0, translucent = 0, sunShadow = 0, lightShadow = 0, triangles = 0; };
    const DrawStats& drawStats() const { return drawStats_; }
    void drawFrame();
    // Saves the next presented frame (UI included) as a PNG.
    void requestScreenshot(std::string path) { screenshotPath_ = std::move(path); }
    void onResize() {
        if (backend_) backend_->onResize();
    }
    void onSurfaceLost() {
        if (backend_) backend_->onSurfaceLost();
    }

private:
    struct Submesh {
        uint32_t firstIndex, indexCount;
        TextureId texture, lightmap;
        glm::vec4 color;
        TextureId material = 0; // "<texture>_n": relief (tangent-space normal) + gloss in alpha; 0 = none
        TextureId skyVisibility = 0; // "<lightmap>_sky": how much sky each luxel sees; 0 = none
        glm::vec3 lo{0.0f}, hi{0.0f}; // model-space bounds, for culling
        bool mirror = false;          // its texture's a mirror ("mirror" in its path): shows what's in front
        glm::vec4 plane{0.0f};        // a mirror's plane (model space): normal out of it, distance
    };
    struct Model {
        std::vector<Submesh> submeshes;
        glm::vec3 mins{0.0f}, maxs{0.0f};
        bool skinned = false;
    };
    struct ModelDraw {
        uint32_t model;
        glm::mat4 transform;
        glm::vec4 tint;
        uint32_t firstJoint = UINT32_MAX; // into the packet's joints, skinned draws only
        uint32_t drawIndex = 0;           // into the packet's draws
        bool castShadow = true;           // sun shadow map (the world is baked, so it doesn't)
        bool shadowOnly = false;          // not drawn in the view
        bool liquid = false;              // sorted just behind what holds it (liquid in a glass bottle)
        bool viewModel = false;           // your hands and tool in first person (not seen in mirrors)
        TextureId texture = 0;            // worn instead of the model's own pictures (0: its own)
        glm::vec3 boundsMin, boundsMax;   // world space, for culling shadow casters per light
    };

    Model describeModel(const KmdlModel& kmdl);
    TextureId newTexture(uint32_t w, uint32_t h, bool srgb, const std::vector<std::vector<uint8_t>>& mips, TexFormat format = TexFormat::RGBA8);
    void buildPacket(uint32_t width, uint32_t height);
    void clearFrame();

    std::unique_ptr<RenderBackend> backend_;
    TextureId nextTexture_ = kFirstTextureSlot;
    std::unordered_map<std::string, TextureId> textureCache_;
    std::unordered_map<TextureId, glm::ivec2> textureSizes_;
    std::vector<Model> models_;
    std::unordered_map<std::string, ModelId> modelCache_;
    std::unordered_map<std::string, int64_t> fileTimes_; // loaded files' times (hot reload)
    bool decodeTexture(const std::string& path, uint32_t& w, uint32_t& h, bool& srgb, std::vector<std::vector<uint8_t>>& mips, TexFormat& format);

    DrawStats drawStats_;
    // TAA: last frame's camera.
    glm::mat4 taaPrevViewProj_{1.0f};
    glm::vec3 taaPrevOrigin_{0.0f};
    uint32_t taaPrevW_ = 0, taaPrevH_ = 0, taaFrame_ = 0;
    bool taaPrevValid_ = false;
    // Occlusion (hierarchical Z): the newest grid back from the GPU as a max-depth pyramid,
    // with the camera it was drawn from. Frames are tagged; their cameras are remembered until
    // their grid returns.
    struct HzbFrame { glm::mat4 viewProj; glm::vec3 origin, forward; };
    std::vector<std::vector<float>> hzbLevels_; // level 0 = kHzbWidth × kHzbHeight
    HzbFrame hzbFrame_{};
    struct { bool valid = false; glm::mat4 viewProj{1.0f}; glm::vec3 origin{0.0f}, forward{1, 0, 0}; } lastCull_; // GPU culling: last frame's camera
    bool hzbValid_ = false;
    uint64_t hzbTag_ = 0;
    std::vector<std::pair<uint64_t, HzbFrame>> hzbPending_;
    void takeOcclusion();
    bool occluded(const glm::mat4& transform, const glm::vec3& lo, const glm::vec3& hi) const;
    Camera camera_;
    bool sunEnabled_ = false, sunRealtime_ = false;
    struct Sky {
        TextureId texture = 0;
        uint32_t levels = 0;
        glm::vec3 sunDir{0, 0, 1};
        glm::vec3 sh[9]{};
    };
    std::unordered_map<std::string, Sky> skies_;
    const Sky* loadSky(const std::string& path);
    struct Probe {
        glm::vec3 at;
        TextureId texture;
        float top;
    };
    std::vector<Probe> probes_;
    bool adaptOn_ = false;
    float dynScale_ = 1.0f;  // dynamic resolution's current scale
    double dynFrame_ = 0.016; // smoothed frame time
    std::vector<std::pair<glm::vec3, std::string>> probeList_;
    Sky sky_;
    Atmosphere atmosphere_;
    glm::vec3 sunDir_{0, 0, 1}, sunColor_{1.0f};
    std::vector<ModelDraw> modelDraws_;
    std::vector<DynamicLight> lights_;
    ClusterGrid clusters_;
    LightStats lightStats_;
    Timings timings_;
    std::string screenshotPath_;
    bool shadowOnly_ = false, viewModel_ = false;
    TextureId textureOverride_ = 0;
    glm::mat4 mirrorReflect_{1.0f}; // this frame's mirror: its reflection, and its pass's cut plane
    glm::vec4 mirrorClip_{0.0f};
    glm::vec4 weather_{0.0f};
    float cloud_ = 0.0f;
    double clock_ = 0.0;
    bool hasClock_ = false;

    FramePacket packet_;                             // reused every frame
    std::vector<MeshDraw> sunCasters_[4]; // per cascade, culled to it
    std::vector<std::vector<MeshDraw>> atlasCasters_;
};

} // namespace keel
