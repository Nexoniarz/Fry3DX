#pragma once
// The line between the renderer and a graphics API. The Renderer (render/renderer.cpp) does
// everything that isn't API work — culling, lights and clusters, shadow cascades, sorting —
// and hands the result over each frame as a FramePacket; a RenderBackend (Vulkan or OpenGL)
// only owns GPU objects and records the passes. Both backends run the same shaders
// (engine/shaders), so the structures below match their layouts exactly.
#include "asset/kmdl.h"
#include "render/clusters.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace keel {

class Window;
using TextureId = uint32_t;

// OpenGL40/OpenGL31/GLES30: the cut-down fallback for old GPUs and phones (rhi/gl/gl_legacy.cpp).
enum class RenderApi { Vulkan, OpenGL, OpenGL40, OpenGL31, GLES30 };

// Texture slots. 0 and 1 are created by the renderer; 2-4 are the backend's render targets
// (sampled through the same table); loaded textures start at kFirstTextureSlot.
inline constexpr TextureId kWhiteTexture = 0, kMissingTexture = 1, kSceneSlot = 2, kSunShadowSlot = 3, kAtlasSlot = 4, kDepthSlot = 5;
// Post-processing targets: occlusion (raw, blurred), bloom levels (half size down to 1/64).
inline constexpr TextureId kAoSlot = 6, kAoBlurSlot = 7, kBloomSlot = 8;
inline constexpr uint32_t kBloomLevels = 6;
inline constexpr TextureId kTaaSlot = kBloomSlot + kBloomLevels; // two: this frame's and last frame's
// The mesh pass's G-buffer (view normal, roughness, reflection weight) and the reflections from it.
inline constexpr TextureId kGbufSlot = kTaaSlot + 2, kSsrSlot = kGbufSlot + 1;
inline constexpr TextureId kAdaptSlot = kSsrSlot + 1; // two (1×1): eye adaptation, this frame's and last frame's
// The occlusion grid and its pyramid (each level half the last, the farthest depth under each
// texel), for GPU culling: kHzbLevels slots from kHzbSlot.
inline constexpr uint32_t kHzbLevels = 8; // 256×128 down to 2×1
inline constexpr TextureId kHzbSlot = kAdaptSlot + 2;
inline constexpr TextureId kMirrorSlot = kHzbSlot + kHzbLevels; // what the nearest mirror shows
inline constexpr TextureId kFirstTextureSlot = kMirrorSlot + 1;
inline constexpr uint32_t kHzbWidth = 256, kHzbHeight = 128; // the occlusion grid read back to the CPU
inline constexpr uint32_t kMaxTextures = 4096;

inline constexpr uint32_t kShadowSize = 4096; // sun: 2×2 cascades (the default size; r_shadowres)
inline constexpr uint32_t kAtlasSize = 4096;  // dynamic lights' shadow tiles
inline constexpr size_t kMaxDraws = 8192;
inline constexpr size_t kMaxJointsPerFrame = 16384;
inline constexpr uint32_t kMaxLights = 1024, kMaxShadowViews = 96, kMaxLightIndices = 1 << 16;

namespace gpu {
struct DrawData { // DRAW_FIELDS in model.glsl
    glm::mat4 model;
    glm::vec4 cube[6];
    glm::vec4 params; // x = sun visibility, y = lit by probes, z = liquid cut, w = LiquidStyle
    glm::vec4 plane;  // liquid surface
    glm::vec4 probe;  // the nearest reflection probe: x = its texture (0 = none), y = its top level; z = time (s);
                      // w = its own surface without a material map: 0 none, else 1 + metal (0..255) + rough (0..255) * 256
};
struct FrameData { // FRAME_FIELDS in model.glsl, then (Vulkan) four buffer addresses
    glm::mat4 cascades[4];
    glm::vec4 splits, cascadeTexel;
    glm::vec4 sunDir, sunColor, shadow;
    glm::vec4 volumetric; // x strength (0 = off), y max distance, z air density
    glm::vec4 camPos, camFwd, screen;
    glm::vec4 sky, fog; // sky texture / intensity / top level / yaw; fog density / falloff / max / base z
    glm::vec4 skySH[9];
    glm::vec4 froxel{0.0f}; // the fog grid: x 1 = on, y near, z far (froxel.glsl)
    glm::vec4 gi{0.0f}, giOrigin{0.0f}; // GI probes: x 1 = on, y spacing; the grid's first cell (gi.glsl)
    glm::vec4 mirror{0.0f}, clip{0.0f}; // mirrors: the picture's texture / its own pass; that pass's cut plane
    glm::vec4 weather{0.0f};            // x wet, y snow cover, z night, w lightning flash (Renderer::setWeather)
    uint64_t lights = 0, cells = 0, lightIndices = 0, shadowViews = 0, froxels = 0, giProbes = 0; // filled by the Vulkan backend
};
struct Light { // Light in model.glsl
    glm::vec4 posRadius, color, spot, extra;
};
struct ShadowView { // ShadowView in model.glsl
    glm::mat4 viewProj;
    glm::vec4 rect;
};
struct SkyPush { // sky.frag's push block
    glm::mat4 invViewProj;
    glm::vec4 sunDir, sunColor, sky;
    glm::vec4 weather; // x cloud cover, y the photo's sun height (sky.frag), z night (stars), w lightning flash
};
struct Sprite { // sprite.vert's push block (after viewProj)
    glm::vec4 posSize, color, right, up; // right.w rotation, up.w texture index (as bits)
};
struct Box { // box.vert's push block (after viewProj)
    glm::vec4 center, extents, color;
};
} // namespace gpu

// One indexed draw of a model's submesh.
struct MeshDraw {
    uint32_t model;             // backend model slot
    uint32_t firstIndex, indexCount;
    uint32_t draw;              // into FramePacket::draws
    uint32_t firstJoint;        // into FramePacket::joints; UINT32_MAX = not skinned
    uint32_t textures, tint, material;
    bool liquid = false;        // cut at its surface in the shader: not in the depth pre-pass
    glm::vec3 lo{0.0f}, hi{0.0f}; // world box (opaque scene draws: GPU occlusion culling)
};

// Something rays can hit (real-time GI): a model's submesh where it stands, and its look.
struct RayInstance {
    glm::mat4 transform;
    uint32_t model, firstIndex, textures, tint;
};

// A shadow map render: a view and a rectangle of the target, in pixels from the top-left.
struct ShadowPass {
    glm::mat4 viewProj;
    glm::ivec4 rect;
    const std::vector<MeshDraw>* draws;
};

// Volumetric smoke's grid (the fog grid reads it, Vulkan): kSmokeDims cells of kSmokeCell units.
inline constexpr glm::ivec3 kSmokeDims{160, 160, 48};
inline constexpr float kSmokeCell = 16.0f;

struct FramePacket {
    uint32_t width = 0, height = 0;       // the 3D scene's size (render scale applied)
    uint32_t outWidth = 0, outHeight = 0; // the window's: the tonemap scales the scene up to it, the UI is drawn at it
    bool prepass = true;                  // r_prepass: depth first, then each pixel shaded once
    // GPU occlusion culling (r_gpuculling): the opaque draws are tested on the GPU against the
    // occlusion grid, made from last frame (cullViewProj); cullOcclusion: the camera's close
    // enough to that frame's for the test to hold.
    bool gpuCull = false, cullOcclusion = false;
    std::vector<RayInstance> rayInstances; // real-time GI: the opaque, still-shaped world (not skinned)
    // Smoke: kSmokeDims cells from smokeOrigin (its world corner; w a clock that turns its
    // billows), each density | shade << 8 (0..255); empty: none.
    std::vector<uint16_t> smoke;
    glm::vec4 smokeOrigin{0.0f};
    // A mirror: the scene again from the eye reflected in its plane (into kMirrorSlot, the
    // scene's size), before the scene pass, with its own frame data (mirrorFrame).
    float skyCloud = 0.0f; // overcast: the sky greys and the sun's disc goes (sky.frag)
    float skyPhotoSunZ = 2.0f; // how high the sky photo's own sun is (2: none), to hide it when the sun has moved
    bool mirror = false;
    glm::mat4 mirrorViewProj{1.0f}, mirrorInvViewProj{1.0f};
    glm::ivec4 mirrorRect{0}; // where the mirror is on screen (x, y, w, h in pixels): only that's drawn
    gpu::FrameData mirrorFrame{};
    std::vector<MeshDraw> mirrorOpaque, mirrorTranslucent;
    glm::mat4 cullViewProj{1.0f};
    glm::mat4 viewProj{1.0f};
    gpu::FrameData frame{};
    std::vector<gpu::DrawData> draws;
    std::vector<glm::mat4> joints;
    std::vector<gpu::Light> lights;
    std::vector<gpu::ShadowView> views;
    const ClusterGrid* clusters = nullptr;
    std::vector<ShadowPass> sunPasses;   // into the sun's shadow map (kSunShadowSlot)
    std::vector<ShadowPass> atlasPasses; // into the dynamic lights' atlas (kAtlasSlot)
    std::vector<gpu::Box> boxes;
    std::vector<gpu::Sprite> sprites, glowSprites; // particles: alpha-blended (back to front), additive
    std::vector<MeshDraw> opaque;
    std::vector<MeshDraw> translucent; // already sorted back to front
    glm::vec3 clearColor{0.0f};        // linear, before exposure (when there's no sky)
    glm::mat4 invViewProj{1.0f};       // for the sky pass (drawn when frame.sky.x != 0)
    float exposure = 1.0f;
    std::string screenshot;            // save this frame as PNG here, if set
    uint32_t shadowSize = kShadowSize; // the sun's shadow map (2×2 cascades); the backend resizes to it
    uint64_t hzbTag = 0;               // nonzero: build the occlusion grid and read it back with this tag
    // Post-processing: ambient occlusion (0 = off), bloom (0 = off), grade.
    struct Post {
        float aoStrength = 1.0f, aoRadius = 24.0f;
        float bloomStrength = 0.035f;
        float saturation = 1.05f, vignette = 0.25f, sharpen = 0.3f;
        glm::vec4 proj{1.0f, 1.0f, 2.0f, 16384.0f}; // tan(half fov x), tan(half fov y), near, far
        bool taa = false, taaReset = true;
        // Sun shafts (screen-space light scattering): the sun's screen position (uv) and
        // strength (z, 0 = off), and its colour × intensity.
        glm::vec4 shafts{0.0f}, sunColor{0.0f};
        float ssr = 0.0f; // screen-space reflections' strength (0 = off)
        // Eye adaptation: on, the frame's seconds, the key (mid grey's target), reset (no history).
        bool adapt = false, adaptReset = true;
        float adaptDt = 0.016f, adaptKey = 0.18f;
        glm::mat4 reproject{1.0f}; // this frame's clip space (unjittered) → last frame's
    } post;
};

// The occlusion grid of an earlier frame (depth, kHzbWidth × kHzbHeight, row 0 = clip y −1, as
// the depth buffer's rows), and the tag that frame was given.
struct HzbReadback {
    uint64_t tag = 0;
    std::vector<float> depth;
};

// How a texture's mips are laid out.
enum class TexFormat : uint8_t { RGBA8, BC7 };

// The GPU and its memory (the debug overlay's corner). VRAM: the device-local heaps; used is all
// processes' use where the driver says (VK_EXT_memory_budget), else just ours.
struct GpuInfo {
    std::string name, driver;
    uint64_t vramTotal = 0, vramUsed = 0, vramOurs = 0;
};

class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    virtual const char* name() const = 0;
    virtual GpuInfo gpuInfo() const { return {}; }
    virtual bool init(const Window& window, bool validation) = 0;
    virtual void shutdown() = 0;
    // Dear ImGui: the backend's NewFrame (renderer + platform).
    virtual void beginUi() = 0;
    virtual void setVsync(bool on) = 0;
    virtual void onResize() = 0;
    // A texture in table slot `slot`, mips largest first: RGBA8 pixels, or BC7 blocks (YAPF 2's
    // GPU payload) when blockCompression() says the GPU takes them.
    virtual void createTexture(TextureId slot, uint32_t width, uint32_t height, bool srgb, const std::vector<std::vector<uint8_t>>& mips,
                               TexFormat format = TexFormat::RGBA8) = 0;
    virtual bool blockCompression() const { return false; }
    // Can draw the scene smaller than the window and scale it up (r_renderscale, r_dynres).
    virtual bool renderScale() const { return false; }
    // Model GPU data in slot `model`: created, or replaced (the old data lives until no frame uses it).
    virtual void uploadModel(uint32_t model, const KmdlModel& kmdl) = 0;
    // A texture as Dear ImGui's ImTextureID, for drawing it in the UI (0 = none).
    virtual uint64_t uiTexture(TextureId slot) = 0;
    // Starts a frame; false when nothing can be drawn (minimised). Sets the drawable size.
    virtual bool beginFrame(uint32_t& width, uint32_t& height) = 0;
    // Renders the packet, then ImGui's draw data, and presents.
    virtual void render(FramePacket& packet) = 0;
    // The newest occlusion grid that has come back from the GPU (a frame or two old); false if
    // none newer than the last one taken.
    virtual bool takeHzb(HzbReadback& out) = 0;
    // How long each pass took on the GPU, a frame or two ago (r_profile); empty if not measured.
    virtual void gpuTimes(std::vector<std::pair<const char*, float>>& out) const { out.clear(); }
    // GPU occlusion culling: supported, and how many draws it hid (a frame or two ago).
    virtual bool gpuCulling() const { return false; }
    virtual bool froxelFog() const { return false; } // the fog grid (FrameData.froxel)
    virtual bool rayTracing() const { return false; } // hardware ray queries: real-time GI
    virtual bool mirrors() const { return false; }    // a second scene pass for a mirror (FramePacket::mirror)
    virtual uint32_t gpuCulled() const { return 0; }
};

std::unique_ptr<RenderBackend> createVulkanBackend();
std::unique_ptr<RenderBackend> createOpenGLBackend();
std::unique_ptr<RenderBackend> createLegacyOpenGLBackend(int major, int minor, bool es = false);

} // namespace keel
