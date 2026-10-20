// The Vulkan 1.3 backend: bindless textures (one descriptor array), buffer device addresses
// for every per-draw and per-frame buffer (passed in push constants), dynamic rendering.
#include "ui/ui.h"
#include "render/backend.h"
#include "render/screenshot.h"
#include "rhi/vulkan/vk_device.h"
#include "core/log.h"
#include "platform/window.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <map>
#include <cstring>

namespace keel {
namespace {
#define SPV(name) const uint32_t name[] =
SPV(kBoxVert)
#include "box.vert.spv.inc"
;
SPV(kBoxFrag)
#include "box.frag.spv.inc"
;
SPV(kMeshVert)
#include "mesh.vert.spv.inc"
;
SPV(kMeshFrag)
#include "mesh.frag.spv.inc"
;
SPV(kDepthFrag)
#include "depth.frag.spv.inc"
;
SPV(kCullComp)
#include "cull.comp.spv.inc"
;
SPV(kFroxelComp)
#include "froxel.comp.spv.inc"
;
SPV(kFroxelRtComp)
#include "froxel_rt.comp.spv.inc"
;
SPV(kGiComp)
#include "gi.comp.spv.inc"
;
SPV(kSkinnedVert)
#include "skinned.vert.spv.inc"
;
SPV(kFullscreenVert)
#include "fullscreen.vert.spv.inc"
;
SPV(kTonemapFrag)
#include "tonemap.frag.spv.inc"
;
SPV(kSkyVert)
#include "sky.vert.spv.inc"
;
SPV(kSkyFrag)
#include "sky.frag.spv.inc"
;
SPV(kSmokeSkyFrag)
#include "smoke_sky.frag.spv.inc"
;
SPV(kHzbFrag)
#include "hzb.frag.spv.inc"
;
SPV(kSsaoFrag)
#include "ssao.frag.spv.inc"
;
SPV(kSsaoBlurFrag)
#include "ssao_blur.frag.spv.inc"
;
SPV(kBloomDownFrag)
#include "bloom_down.frag.spv.inc"
;
SPV(kBloomUpFrag)
#include "bloom_up.frag.spv.inc"
;
SPV(kTaaFrag)
#include "taa.frag.spv.inc"
;
SPV(kSsrFrag)
#include "ssr.frag.spv.inc"
;
SPV(kVolumetricFrag)
#include "volumetric.frag.spv.inc"
;
SPV(kSpriteVert)
#include "sprite.vert.spv.inc"
;
SPV(kSpriteFrag)
#include "sprite.frag.spv.inc"
;
SPV(kAdaptFrag)
#include "adapt.frag.spv.inc"
;
#undef SPV

struct BoxPush {
    glm::mat4 viewProj;
    gpu::Box box;
};
struct ModelPush { // matches model.glsl (Vulkan)
    glm::mat4 viewProj;
    VkDeviceAddress vertices, skin, joints, draw, frame;
    uint32_t textures; // albedo | lightmap << 16
    uint32_t tint;
    uint32_t material; // relief + gloss map, 0 = none
};
struct TonemapPush {
    uint32_t scene;
    float exposure;
    uint32_t ao, bloom;
    float bloomStrength, saturation, vignette, sharpen;
    uint32_t depth, pad[3];
    glm::vec4 shafts, sunColor;
};
struct SsaoPush {
    uint32_t depth, w, h, outW, outH;
    float radius, strength, pad;
    glm::vec4 proj;
};
struct SsaoBlurPush {
    uint32_t ao, depth, w, h, outW, outH;
    float pad0, pad1;
    glm::vec4 proj;
};
struct TaaPush {
    glm::mat4 reproject;
    uint32_t scene, history, depth, w, h, reset, ssr;
    float pad1;
};
struct AdaptPush {
    uint32_t src, prev;
    float dt, speed, key, minE, maxE, pad;
};
struct SpritePush {
    glm::mat4 viewProj;
    gpu::Sprite sprite;
};
struct VolumetricPush {
    uint64_t frame;
    uint32_t depth, w, h, outW, outH;
    float pad[5]; // proj sits at 48 (vec4 alignment)
    glm::vec4 proj;
};
struct SsrPush {
    uint32_t depth, gbuf, scene, w, h, outW, outH;
    float strength;
    glm::vec4 proj;
};
struct BloomPush {
    uint32_t src, srcW, srcH, outW, outH;
    uint32_t extra; // down: first level; up: radius (float bits)
};
struct HzbPush {
    uint32_t depth, srcW, srcH, dstW, dstH;
};
static_assert(sizeof(BoxPush) <= 128 && sizeof(ModelPush) <= 128 && sizeof(gpu::SkyPush) <= 128 && sizeof(SsaoPush) <= 128, "push constants must fit the guaranteed 128 bytes");
constexpr VkShaderStageFlags kAllStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
constexpr VkFormat kSceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kGbufFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // view normal (oct), roughness, reflection weight
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// One per-frame buffer holds the lights, shadow views, cluster cells and light indices.
constexpr VkDeviceSize kLightsOffset = 0, kViewsOffset = kMaxLights * sizeof(gpu::Light),
                       kCellsOffset = kViewsOffset + kMaxShadowViews * sizeof(gpu::ShadowView),
                       kIndicesOffset = kCellsOffset + kClusterX * kClusterY * kClusterZ * 8, kLightBufferSize = kIndicesOffset + kMaxLightIndices * 4;

struct PipelineDesc {
    const uint32_t* vs;
    size_t vsSize;
    const uint32_t* fs;
    size_t fsSize;
    VkFormat color; // UNDEFINED = depth only (shadow maps)
    bool depth;
    bool depthBias = false;
    bool blend = false; // alpha blended, depth test without writes (translucent pass)
    bool sky = false;   // on the far plane: depth less-or-equal, no writes
    bool additive = false; // adds onto what's there (bloom coming up)
    bool over = false;     // premultiplied, onto what's there: src + dst × src alpha (fog, smoke)
    int gbuf = 0;          // the scene pass's second attachment (G-buffer): 0 none, 1 there but not written, 2 written
    bool glow = false;     // light added on by its alpha, depth tested but not written (particles)
    bool prepassed = false; // the depth is already there (pre-pass): test less-or-equal, don't write
};

VkPipeline makePipeline(VkDevice dev, VkPipelineLayout layout, const PipelineDesc& d) {
    auto shader = [&](const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
        return m;
    };
    VkShaderModule vs = shader(d.vs, d.vsSize), fs = d.fs ? shader(d.fs, d.fsSize) : VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    if (d.depthBias) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 2.0f;
        rs.depthBiasSlopeFactor = 2.5f;
    }
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth;
    ds.depthWriteEnable = d.depth;
    ds.depthCompareOp = d.sky ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
    if (d.sky) ds.depthWriteEnable = VK_FALSE;
    if (d.prepassed) ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL, ds.depthWriteEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = 0xF;
    if (d.additive) {
        blendAtt.blendEnable = VK_TRUE;
        blendAtt.srcColorBlendFactor = blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAtt.srcAlphaBlendFactor = blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAtt.colorBlendOp = blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (d.over) {
        blendAtt.blendEnable = VK_TRUE;
        blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAtt.colorBlendOp = blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (d.glow) {
        blendAtt.blendEnable = VK_TRUE;
        blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAtt.srcAlphaBlendFactor = blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAtt.colorBlendOp = blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
        ds.depthWriteEnable = VK_FALSE;
    }
    if (d.blend) {
        blendAtt.blendEnable = VK_TRUE;
        blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAtt.colorBlendOp = VK_BLEND_OP_ADD;
        blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
        ds.depthWriteEnable = VK_FALSE;
    }
    const bool hasColor = d.color != VK_FORMAT_UNDEFINED;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState atts[2] = {blendAtt, {}};
    atts[1].colorWriteMask = d.gbuf == 2 ? 0xF : 0;
    blend.attachmentCount = hasColor ? (d.gbuf ? 2 : 1) : 0;
    blend.pAttachments = atts;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const VkFormat formats[2] = {d.color, kGbufFormat};
    rendering.colorAttachmentCount = hasColor ? (d.gbuf ? 2 : 1) : 0;
    rendering.pColorAttachmentFormats = hasColor ? formats : nullptr;
    rendering.depthAttachmentFormat = d.depth ? kDepthFormat : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rendering;
    pci.stageCount = fs ? 2 : 1;
    pci.pStages = stages;
    pci.pVertexInputState = &vertexInput;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &blend;
    pci.pDynamicState = &dyn;
    pci.layout = layout;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &p));
    vkDestroyShaderModule(dev, vs, nullptr);
    if (fs) vkDestroyShaderModule(dev, fs, nullptr);
    return p;
}

class VulkanBackend final : public RenderBackend {
public:
    const char* name() const override { return "Vulkan 1.3"; }
    GpuInfo gpuInfo() const override {
        GpuInfo g{device_.gpuName(), device_.gpuDriver()};
        device_.vram(g.vramTotal, g.vramUsed, g.vramOurs);
        return g;
    }

    bool init(const Window& window, bool validation) override {
        if (!device_.init(window, validation)) return false;
        swapFormat_ = device_.swapchainFormat();
        createBindless();
        createPipelines();
        // GPU timing (r_profile): timestamps between passes, one set per frame in flight.
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = vk::Device::kFramesInFlight * kMaxMarks;
        vkCreateQueryPool(device_.device(), &qi, nullptr, &queries_);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device_.physicalDevice(), &props);
        tickNs_ = props.limits.timestampPeriod;
        return initImGui(window);
    }

    // ── GPU timing ──
    static constexpr uint32_t kMaxMarks = 16;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    float tickNs_ = 1.0f;
    const char* markNames_[vk::Device::kFramesInFlight][kMaxMarks] = {};
    uint32_t markCount_[vk::Device::kFramesInFlight] = {};
    std::vector<std::pair<const char*, float>> gpuTimes_;
    // This slot's last results (its fence has passed), then start it again.
    void beginMarks(VkCommandBuffer cmd) {
        const uint32_t fi = device_.frameIndex(), n = markCount_[fi];
        if (n > 1) {
            uint64_t t[kMaxMarks];
            if (vkGetQueryPoolResults(device_.device(), queries_, fi * kMaxMarks, n, sizeof t, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                gpuTimes_.clear();
                for (uint32_t i = 1; i < n; ++i) gpuTimes_.push_back({markNames_[fi][i], float(double(t[i] - t[i - 1]) * tickNs_ * 1e-6)});
            }
        }
        vkCmdResetQueryPool(cmd, queries_, fi * kMaxMarks, kMaxMarks);
        markCount_[fi] = 0;
        mark(cmd, "start");
    }
    // The time now on the GPU; the span since the last mark is called `name`.
    void mark(VkCommandBuffer cmd, const char* name) {
        const uint32_t fi = device_.frameIndex();
        if (!queries_ || markCount_[fi] >= kMaxMarks) return;
        markNames_[fi][markCount_[fi]] = name;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries_, fi * kMaxMarks + markCount_[fi]++);
    }
    void gpuTimes(std::vector<std::pair<const char*, float>>& out) const override { out = gpuTimes_; }

    void shutdown() override {
        if (!device_.device()) return;
        device_.waitIdle();
        if (queries_) vkDestroyQueryPool(device_.device(), queries_, nullptr), queries_ = VK_NULL_HANDLE;
        if (imgui_) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            imgui_ = false;
        }
        VkDevice dev = device_.device();
        destroyTexture(scene_);
        destroyTexture(depth_);
        for (Texture* t : {&mirror_, &mirrorGbuf_, &mirrorDepth_}) destroyTexture(*t);
        for (auto& b : mirrorFrames_) device_.destroyBuffer(b);
        for (auto& b : smoke_) device_.destroyBuffer(b);
        destroyTexture(hzb_);
        for (Texture& t : hzbMips_) destroyTexture(t);
        destroyTexture(ao_);
        destroyTexture(gbuf_);
        destroyTexture(ssr_);
        for (Texture& t : adapt_) destroyTexture(t);
        destroyTexture(aoBlur_);
        for (Texture& t : bloom_) destroyTexture(t);
        for (Texture& t : taa_) destroyTexture(t);
        for (auto& b : hzbReadback_) device_.destroyBuffer(b);
        for (Texture& t : textures_) destroyTexture(t);
        for (auto& [frame, m] : retired_) destroyModel(m);
        for (auto& [frame, t] : retiredTextures_) destroyTexture(t);
        for (Model& m : models_) destroyModel(m);
        for (vk::Buffer* b : {&froxelCells_[0], &froxelCells_[1], &froxelOut_, &smokeSky_, &giProbes_[0], &giProbes_[1]}) device_.destroyBuffer(*b);
        for (Tlas& t : tlas_) destroyTlas(t);
        for (vk::Buffer* list : {jointBuffers_, drawBuffers_, frameBuffers_, lightBuffers_, cullBoxes_, cullCmds_, cullCounter_})
            for (uint32_t i = 0; i < vk::Device::kFramesInFlight; ++i) device_.destroyBuffer(list[i]);
        destroyTexture(shadowMap_);
        destroyTexture(shadowAtlas_);
        for (VkPipeline p : {meshBlendPipeline_, skinnedBlendPipeline_, boxPipeline_, meshPipeline_, skinnedPipeline_, shadowMeshPipeline_, shadowSkinnedPipeline_, cullPipeline_, froxelPipeline_, froxelRtPipeline_, giPipeline_, depthMeshPipeline_, depthSkinnedPipeline_, meshAfterDepthPipeline_, skinnedAfterDepthPipeline_,
                             tonemapPipeline_, skyPipeline_, hzbPipeline_, ssaoPipeline_, ssaoBlurPipeline_, bloomDownPipeline_, bloomUpPipeline_, taaPipeline_, ssrPipeline_, adaptPipeline_, volumetricPipeline_, volumetricAddPipeline_, spritePipeline_, glowPipeline_, smokeSkyPipeline_})
            vkDestroyPipeline(dev, p, nullptr);
        vkDestroyPipelineLayout(dev, layout_, nullptr);
        vkDestroyDescriptorPool(dev, pool_, nullptr);
        vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
        vkDestroySampler(dev, sampler_, nullptr);
        vkDestroySampler(dev, shadowSampler_, nullptr);
        vkDestroySampler(dev, clampSampler_, nullptr);
        device_.shutdown();
    }

    void beginUi() override {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ui::applyPixelScale();
        ImGui::NewFrame();
    }
    void setVsync(bool on) override { device_.setVsync(on); }
    void onResize() override { device_.markResized(); }
    void onSurfaceLost() override { device_.markSurfaceLost(); }

    bool renderScale() const override { return true; }
    bool blockCompression() const override {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(device_.physicalDevice(), VK_FORMAT_BC7_SRGB_BLOCK, &p);
        return p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    }

    void createTexture(TextureId slot, uint32_t w, uint32_t h, bool srgb, const std::vector<std::vector<uint8_t>>& mips,
                       TexFormat fmt = TexFormat::RGBA8) override {
        const VkFormat format = fmt == TexFormat::BC7 ? (srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK)
                                                      : (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
        Texture t;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {w, h, 1};
        ici.mipLevels = uint32_t(mips.size());
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (srgb) ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; // (the UI reads it as plain numbers: uiView)
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(device_.allocator(), &ici, &aci, &t.image, &t.alloc, nullptr));
        size_t total = 0;
        for (auto& m : mips) total += m.size();
        vk::Buffer staging = device_.createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::vector<VkBufferImageCopy> copies;
        size_t offset = 0;
        for (uint32_t level = 0; level < mips.size(); ++level) {
            std::memcpy((uint8_t*)staging.mapped + offset, mips[level].data(), mips[level].size());
            VkBufferImageCopy c{};
            c.bufferOffset = offset;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            c.imageExtent = {std::max(1u, w >> level), std::max(1u, h >> level), 1};
            copies.push_back(c);
            offset += mips[level].size();
        }
        device_.immediateSubmit([&](VkCommandBuffer cmd) {
            vk::transitionImage(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(cmd, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
            vk::transitionImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        device_.destroyBuffer(staging);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = t.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(mips.size()), 0, 1};
        VK_CHECK(vkCreateImageView(device_.device(), &vci, nullptr, &t.view));
        // ImGui's colours are sRGB numbers drawn as they are, so the UI must read the picture without
        // turning it linear first (else it shows too dark): the same image, seen as UNORM.
        if (srgb) {
            vci.format = fmt == TexFormat::BC7 ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM;
            VK_CHECK(vkCreateImageView(device_.device(), &vci, nullptr, &t.uiView));
        }
        if (textures_.size() <= slot) textures_.resize(slot + 1);
        // Replaced (hot reload): frames in flight may still sample the old one.
        if (textures_[slot].image) retiredTextures_.emplace_back(frameNumber_, textures_[slot]);
        textures_[slot] = t;
        bind(slot, t.view);
    }

    void uploadModel(uint32_t index, const KmdlModel& kmdl) override {
        if (models_.size() <= index) models_.resize(index + 1);
        if (models_[index].vertices.buffer) retired_.emplace_back(frameNumber_, models_[index]);
        Model m;
        if (!kmdl.vertices.empty() && !kmdl.indices.empty()) {
            const VkBufferUsageFlags rt = device_.rayQuery() ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
            m.vertices = device_.uploadBuffer(kmdl.vertices.data(), kmdl.vertices.size() * sizeof(KmdlVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | rt);
            m.indices = device_.uploadBuffer(kmdl.indices.data(), kmdl.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rt);
            if (device_.rayQuery() && kmdl.skin.empty()) buildBlas(m, kmdl);
            if (!kmdl.skin.empty())
                m.skin = device_.uploadBuffer(kmdl.skin.data(), kmdl.skin.size() * sizeof(KmdlSkin), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        models_[index] = m;
    }

    uint64_t uiTexture(TextureId slot) override {
        if (slot >= textures_.size() || !textures_[slot].view) return 0;
        auto it = uiTextures_.find(slot);
        if (it != uiTextures_.end() && it->second.view != textures_[slot].view) { // (the slot's picture was replaced: a new set, the old one let go later)
            retiredUiSets_.push_back({frameNumber_, it->second.set});
            uiTextures_.erase(it), it = uiTextures_.end();
        }
        if (it == uiTextures_.end())
            it = uiTextures_.emplace(slot, UiTexture{uint64_t(ImGui_ImplVulkan_AddTexture(sampler_, textures_[slot].uiView ? textures_[slot].uiView : textures_[slot].view,
                                                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)),
                                                     textures_[slot].view})
                     .first;
        return it->second.set;
    }

    bool beginFrame(uint32_t& width, uint32_t& height) override {
        cmd_ = device_.beginFrame();
        ++frameNumber_;
        // Models replaced a few frames ago: no frame in flight uses their buffers any more.
        std::erase_if(retiredTextures_, [&](auto& r) {
            if (frameNumber_ - r.first <= vk::Device::kFramesInFlight + 1) return false;
            destroyTexture(r.second);
            return true;
        });
        std::erase_if(retiredUiSets_, [&](auto& r) {
            if (frameNumber_ - r.first <= vk::Device::kFramesInFlight + 1) return false;
            ImGui_ImplVulkan_RemoveTexture(VkDescriptorSet(r.second));
            return true;
        });
        std::erase_if(retired_, [&](auto& r) {
            if (frameNumber_ - r.first <= vk::Device::kFramesInFlight + 1) return false;
            destroyModel(r.second);
            return true;
        });
        if (!cmd_) return false;
        // This slot's last frame has finished (its fence passed): its occlusion grid is ready.
        if (const uint32_t fi = device_.frameIndex(); hzbTags_[fi]) {
            vmaInvalidateAllocation(device_.allocator(), hzbReadback_[fi].alloc, 0, VK_WHOLE_SIZE);
            const float* d = static_cast<const float*>(hzbReadback_[fi].mapped);
            if (hzbTags_[fi] > hzbLatest_.tag) hzbLatest_ = {hzbTags_[fi], std::vector<float>(d, d + kHzbWidth * kHzbHeight)};
            hzbTags_[fi] = 0;
        }
        const VkExtent2D e = device_.extent(); // (targets are sized to the scene in drawFrame)
        width = e.width;
        height = e.height;
        return true;
    }

    void render(FramePacket& p) override;

    bool takeHzb(HzbReadback& out) override {
        if (hzbLatest_.tag <= hzbTaken_) return false;
        out = hzbLatest_;
        hzbTaken_ = hzbLatest_.tag;
        return true;
    }

private:
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageView uiView = VK_NULL_HANDLE; // (an sRGB picture seen as UNORM, for the UI)
    };
    // A ray-tracing shape (bottom-level acceleration structure): one per submesh, by first index.
    struct Blas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        vk::Buffer buffer;
        VkDeviceAddress address = 0;
    };
    struct Model {
        vk::Buffer vertices, indices, skin; // skin: KmdlSkin per vertex, skinned models only
        std::vector<std::pair<uint32_t, Blas>> blas; // with ray queries
    };

    void destroyTexture(Texture& t) {
        if (t.view) vkDestroyImageView(device_.device(), t.view, nullptr);
        if (t.uiView) vkDestroyImageView(device_.device(), t.uiView, nullptr);
        if (t.image) vmaDestroyImage(device_.allocator(), t.image, t.alloc);
        t = {};
    }
    void destroyModel(Model& m) {
        for (auto& [first, b] : m.blas) {
            vkDestroyAccelerationStructureKHR(device_.device(), b.as, nullptr);
            device_.destroyBuffer(b.buffer);
        }
        m.blas.clear();
        device_.destroyBuffer(m.vertices);
        device_.destroyBuffer(m.indices);
        device_.destroyBuffer(m.skin);
    }
    // Each submesh's ray-tracing shape, built once (now, waiting for it).
    void buildBlas(Model& m, const KmdlModel& kmdl) {
        VkDevice dev = device_.device();
        std::vector<VkAccelerationStructureGeometryKHR> geoms;
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<vk::Buffer> scratch;
        geoms.reserve(kmdl.submeshes.size());
        for (const KmdlSubmesh& sm : kmdl.submeshes) {
            if (sm.indexCount < 3) continue;
            VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            auto& t = g.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress = m.vertices.address;
            t.vertexStride = sizeof(KmdlVertex);
            t.maxVertex = uint32_t(kmdl.vertices.size() - 1);
            t.indexType = VK_INDEX_TYPE_UINT32;
            t.indexData.deviceAddress = m.indices.address;
            geoms.push_back(g);
            VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            bi.geometryCount = 1;
            bi.pGeometries = &geoms.back();
            const uint32_t prims = sm.indexCount / 3;
            VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &sz);
            Blas b;
            b.buffer = device_.createBuffer(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
            VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ci.buffer = b.buffer.buffer;
            ci.size = sz.accelerationStructureSize;
            ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            VK_CHECK(vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &b.as));
            VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            ai.accelerationStructure = b.as;
            b.address = vkGetAccelerationStructureDeviceAddressKHR(dev, &ai);
            scratch.push_back(device_.createBuffer(sz.buildScratchSize + scratchAlign_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false));
            bi.dstAccelerationStructure = b.as;
            bi.scratchData.deviceAddress = alignUp(scratch.back().address, scratchAlign_);
            infos.push_back(bi);
            ranges.push_back({prims, sm.firstIndex * 4u, 0, 0});
            m.blas.emplace_back(sm.firstIndex, b);
        }
        if (infos.empty()) return;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePtrs;
        for (auto& r : ranges) rangePtrs.push_back(&r);
        device_.immediateSubmit([&](VkCommandBuffer cmd) { vkCmdBuildAccelerationStructuresKHR(cmd, uint32_t(infos.size()), infos.data(), rangePtrs.data()); });
        for (auto& b : scratch) device_.destroyBuffer(b);
    }
    static VkDeviceAddress alignUp(VkDeviceAddress a, VkDeviceAddress align) { return (a + align - 1) / align * align; }

    void bind(TextureId slot, VkImageView view) {
        VkDescriptorImageInfo info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set_;
        w.dstBinding = 0;
        w.dstArrayElement = slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(device_.device(), 1, &w, 0, nullptr);
    }
    Texture makeTarget(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        Texture t;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {extent.width, extent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VK_CHECK(vmaCreateImage(device_.allocator(), &ici, &aci, &t.image, &t.alloc, nullptr));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = t.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {aspect, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_.device(), &vci, nullptr, &t.view));
        return t;
    }
    void createBindless();
    void createPipelines();
    bool initImGui(const Window& window);
    void ensureTargets(VkExtent2D extent);
    void drawMeshes(VkCommandBuffer cmd, const std::vector<MeshDraw>& draws, const glm::mat4& viewProj, bool shadow, VkPipeline mesh,
                    VkPipeline skinned, const vk::Buffer* indirect = nullptr, bool skipLiquid = false);
    void shadowPasses(VkCommandBuffer cmd, Texture& target, uint32_t size, const std::vector<ShadowPass>& passes);

    vk::Device device_;
    // GPU occlusion culling (cull.comp): per frame in flight, the opaque draws' boxes, their
    // indirect commands (written by the GPU) and how many it hid.
    struct CullBox { glm::vec4 lo, hi; glm::uvec4 draw; };
    struct CullPush { glm::mat4 viewProj; VkDeviceAddress boxes, cmds, counter; uint32_t count, hzb, occlusion, pad; };
    static constexpr uint32_t kMaxCulled = 32768;
    vk::Buffer cullBoxes_[vk::Device::kFramesInFlight], cullCmds_[vk::Device::kFramesInFlight], cullCounter_[vk::Device::kFramesInFlight];
    VkPipeline cullPipeline_ = VK_NULL_HANDLE;
    bool hzbReady_ = false, hzbNext_ = false; // the grid holds last frame's depth (and is readable)
    Texture hzbMips_[kHzbLevels]; // [0] unused (that's hzb_)
    uint32_t gpuCulled_ = 0;
    const vk::Buffer* cullFor_ = nullptr; // this frame's commands, while the opaque lists are drawn
    bool gpuCulling() const override { return cullPipeline_ != VK_NULL_HANDLE; }
    // The fog grid (froxel.comp): two cell buffers (this frame's, last frame's) and the integrated one.
    struct FroxelPush { VkDeviceAddress frame, cur, prev, integrated; glm::vec4 proj, prevPos, prevFwd; uint32_t mode, frameIndex; VkDeviceAddress tlas, smoke, smokeSky; glm::vec4 smokeOrigin; };
    struct SmokeSkyPush { VkDeviceAddress frame, sky; };
    vk::Buffer smokeSky_; // the fog grid's columns, smoke alone (smoke_sky.frag)
    VkPipeline smokeSkyPipeline_ = VK_NULL_HANDLE;
    bool smokeSkyOn_ = false; // this frame
    static_assert(sizeof(FroxelPush) == 128); // (the whole push range)
    vk::Buffer smoke_[vk::Device::kFramesInFlight]; // the smoke grid, written each frame there is smoke
    static constexpr uint32_t kFroxelCount = 160 * 90 * 64; // kFroxels in froxel.glsl
    vk::Buffer froxelCells_[2], froxelOut_;
    VkPipeline froxelPipeline_ = VK_NULL_HANDLE, froxelRtPipeline_ = VK_NULL_HANDLE; // (rays: sky seen from each cell)
    uint32_t froxelFrame_ = 0;
    glm::vec4 froxelPrevPos_{0.0f}, froxelPrevFwd_{0.0f};
    bool froxelFog() const override { return froxelPipeline_ != VK_NULL_HANDLE; }
    // Real-time GI (gi.comp): the scene for rays (per frame in flight), and the probes (this
    // frame's, last frame's).
    struct Tlas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        vk::Buffer buffer, scratch, instances, info;
        VkDeviceAddress address = 0;
        uint32_t capacity = 0;
    };
    struct RayInst { VkDeviceAddress vertices, indices; uint32_t firstIndex, textures, tint, pad; }; // RayInst in gi.comp
    struct GiPush { VkDeviceAddress frame, cur, prev, insts; VkDeviceAddress tlas; uint32_t fresh; float hysteresis; glm::vec4 rotation; };
    static constexpr uint32_t kGiProbes = 24 * 24 * 12; // kGiDims in gi.glsl
    static constexpr uint32_t kGiProbeSize = 128;       // GiProbe
    Tlas tlas_[vk::Device::kFramesInFlight];
    vk::Buffer giProbes_[2];
    VkPipeline giPipeline_ = VK_NULL_HANDLE;
    uint32_t giFrame_ = 0, giIdle_ = 0;
    bool giFresh_ = true, giOn_ = false;
    VkDeviceAddress scratchAlign_ = 256;
    bool rayTracing() const override { return giPipeline_ != VK_NULL_HANDLE; }
    void destroyTlas(Tlas& t) {
        if (t.as) vkDestroyAccelerationStructureKHR(device_.device(), t.as, nullptr);
        for (vk::Buffer* b : {&t.buffer, &t.scratch, &t.instances, &t.info}) device_.destroyBuffer(*b);
        t = {};
    }
    // Room for n instances (this slot's last frame is done with it).
    void ensureTlas(Tlas& t, uint32_t n) {
        if (t.capacity >= n) return;
        destroyTlas(t);
        uint32_t cap = 1024;
        while (cap < n) cap *= 2;
        VkDevice dev = device_.device();
        t.instances = device_.createBuffer(cap * sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
        t.info = device_.createBuffer(cap * sizeof(RayInst), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &cap, &sz);
        t.buffer = device_.createBuffer(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        t.scratch = device_.createBuffer(sz.buildScratchSize + scratchAlign_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = t.buffer.buffer;
        ci.size = sz.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VK_CHECK(vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &t.as));
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = t.as;
        t.address = vkGetAccelerationStructureDeviceAddressKHR(dev, &ai);
        t.capacity = cap;
    }
    uint32_t gpuCulled() const override { return gpuCulled_; }
    VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool imgui_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE, shadowSampler_ = VK_NULL_HANDLE, clampSampler_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE; // shared: bindless set + 128 bytes of push constants
    VkPipeline boxPipeline_ = VK_NULL_HANDLE, meshPipeline_ = VK_NULL_HANDLE, meshBlendPipeline_ = VK_NULL_HANDLE,
               skinnedPipeline_ = VK_NULL_HANDLE, skinnedBlendPipeline_ = VK_NULL_HANDLE, shadowMeshPipeline_ = VK_NULL_HANDLE, shadowSkinnedPipeline_ = VK_NULL_HANDLE,
               depthMeshPipeline_ = VK_NULL_HANDLE, depthSkinnedPipeline_ = VK_NULL_HANDLE, meshAfterDepthPipeline_ = VK_NULL_HANDLE,
               skinnedAfterDepthPipeline_ = VK_NULL_HANDLE,
               tonemapPipeline_ = VK_NULL_HANDLE, skyPipeline_ = VK_NULL_HANDLE, hzbPipeline_ = VK_NULL_HANDLE, ssaoPipeline_ = VK_NULL_HANDLE,
               ssaoBlurPipeline_ = VK_NULL_HANDLE, bloomDownPipeline_ = VK_NULL_HANDLE, bloomUpPipeline_ = VK_NULL_HANDLE,
               taaPipeline_ = VK_NULL_HANDLE, ssrPipeline_ = VK_NULL_HANDLE, adaptPipeline_ = VK_NULL_HANDLE,
               volumetricPipeline_ = VK_NULL_HANDLE, volumetricAddPipeline_ = VK_NULL_HANDLE,
               spritePipeline_ = VK_NULL_HANDLE, glowPipeline_ = VK_NULL_HANDLE;
    Texture adapt_[2];
    uint32_t adaptCur_ = 0;
    bool adaptValid_ = false; // the targets were (re)made: no history yet
    Texture taa_[2];
    uint32_t shadowMapSize_ = kShadowSize; // TAA history: written this frame / read from last frame, alternating
    uint32_t taaCur_ = 0;
    // Post-processing targets (sized with the screen).
    Texture ao_, aoBlur_, bloom_[kBloomLevels], gbuf_, ssr_;
    VkExtent2D aoExtent_{}, bloomExtent_[kBloomLevels]{};
    // Occlusion grid: rendered, copied to a readback buffer per frame in flight, read when that
    // frame's fence has passed.
    Texture hzb_;
    vk::Buffer hzbReadback_[vk::Device::kFramesInFlight];
    uint64_t hzbTags_[vk::Device::kFramesInFlight] = {};
    HzbReadback hzbLatest_;
    uint64_t hzbTaken_ = 0;
    Texture shadowMap_, shadowAtlas_, scene_, depth_;
    // Mirrors: the reflected scene (and the G-buffer and depth its pass needs), the scene's size.
    Texture mirror_, mirrorGbuf_, mirrorDepth_;
    vk::Buffer mirrorFrames_[vk::Device::kFramesInFlight];
    VkDeviceAddress drawFrame_ = 0;
    bool mirrorValid_ = false; // the mirror target's been drawn (else its layout's undefined) // the frame data drawMeshes gives the shaders (0: this frame's)
    bool mirrors() const override { return true; }
    VkExtent2D targetExtent_{};
    vk::Buffer drawBuffers_[vk::Device::kFramesInFlight], frameBuffers_[vk::Device::kFramesInFlight],
        jointBuffers_[vk::Device::kFramesInFlight], lightBuffers_[vk::Device::kFramesInFlight];
    std::vector<Texture> textures_; // index = slot
    std::vector<Model> models_;
    std::vector<std::pair<uint64_t, Model>> retired_; // freed after kFramesInFlight more frames
    std::vector<std::pair<uint64_t, Texture>> retiredTextures_;
    uint64_t frameNumber_ = 0;
    struct UiTexture {
        uint64_t set;
        VkImageView view;
    };
    std::map<TextureId, UiTexture> uiTextures_; // ImGui descriptor sets, made on first use (again if the picture's replaced)
    std::vector<std::pair<uint64_t, uint64_t>> retiredUiSets_;
};

void VulkanBackend::createBindless() {
    VkDevice dev = device_.device();
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = VK_TRUE;
    sci.maxAnisotropy = 16.0f;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &sampler_));
    VkSamplerCreateInfo shci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    shci.magFilter = shci.minFilter = VK_FILTER_LINEAR;
    shci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    shci.addressModeU = shci.addressModeV = shci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    shci.compareEnable = VK_TRUE;
    shci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(dev, &shci, nullptr, &shadowSampler_));
    // Screen-space passes read render targets: clamped, so their edges never wrap round to the
    // opposite side (dark bands along the top and bottom of the screen).
    VkSamplerCreateInfo cci = sci;
    cci.addressModeU = cci.addressModeV = cci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cci.anisotropyEnable = VK_FALSE;
    VK_CHECK(vkCreateSampler(dev, &cci, nullptr, &clampSampler_));

    VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures, kAllStages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kAllStages, &sampler_},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kAllStages, &shadowSampler_},
        {3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kAllStages, &clampSampler_},
    };
    VkDescriptorBindingFlags flags[4] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo fci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    fci.bindingCount = 4;
    fci.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.pNext = &fci;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 4;
    lci.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &setLayout_));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures}, {VK_DESCRIPTOR_TYPE_SAMPLER, 3}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &pci, nullptr, &pool_));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &set_));
    VkPushConstantRange range{kAllStages, 0, 128};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &layout_));

    // Shadow maps for the sun and for dynamic lights, sampled through the table.
    constexpr VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    shadowMap_ = makeTarget(kDepthFormat, {kShadowSize, kShadowSize}, depthUsage, VK_IMAGE_ASPECT_DEPTH_BIT);
    shadowAtlas_ = makeTarget(kDepthFormat, {kAtlasSize, kAtlasSize}, depthUsage, VK_IMAGE_ASPECT_DEPTH_BIT);
    bind(kSunShadowSlot, shadowMap_.view);
    bind(kAtlasSlot, shadowAtlas_.view);
    hzb_ = makeTarget(VK_FORMAT_R32_SFLOAT, {kHzbWidth, kHzbHeight}, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT);
    bind(kHzbSlot, hzb_.view);
    for (uint32_t l = 1; l < kHzbLevels; ++l) { // the pyramid above it
        hzbMips_[l] = makeTarget(VK_FORMAT_R32_SFLOAT, {std::max(1u, kHzbWidth >> l), std::max(1u, kHzbHeight >> l)},
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        bind(kHzbSlot + l, hzbMips_[l].view);
    }
    for (auto& b : hzbReadback_) b = device_.createBuffer(kHzbWidth * kHzbHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
    for (uint32_t i = 0; i < vk::Device::kFramesInFlight; ++i) {
        drawBuffers_[i] = device_.createBuffer(kMaxDraws * sizeof(gpu::DrawData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        frameBuffers_[i] = device_.createBuffer(sizeof(gpu::FrameData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        jointBuffers_[i] = device_.createBuffer(kMaxJointsPerFrame * sizeof(glm::mat4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        lightBuffers_[i] = device_.createBuffer(kLightBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        cullBoxes_[i] = device_.createBuffer(kMaxCulled * sizeof(CullBox), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        cullCmds_[i] = device_.createBuffer(kMaxCulled * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
        cullCounter_[i] = device_.createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true);
        mirrorFrames_[i] = device_.createBuffer(sizeof(gpu::FrameData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        smoke_[i] = device_.createBuffer(size_t(kSmokeDims.x) * kSmokeDims.y * kSmokeDims.z * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
    for (auto& b : froxelCells_) b = device_.createBuffer(kFroxelCount * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    froxelOut_ = device_.createBuffer(kFroxelCount * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    smokeSky_ = device_.createBuffer(160 * 90 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    if (device_.rayQuery()) {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &asp;
        vkGetPhysicalDeviceProperties2(device_.physicalDevice(), &props);
        scratchAlign_ = std::max<VkDeviceAddress>(asp.minAccelerationStructureScratchOffsetAlignment, 1);
        for (auto& b : giProbes_) {
            b = device_.createBuffer(kGiProbes * kGiProbeSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
            device_.immediateSubmit([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, b.buffer, 0, VK_WHOLE_SIZE, 0); });
        }
    }
}

void VulkanBackend::createPipelines() {
    VkDevice dev = device_.device();
    // The scene pass has two attachments (colour, G-buffer); only opaque meshes write the second.
    auto scene = [](PipelineDesc d, int gbuf) { d.gbuf = gbuf; return d; };
    boxPipeline_ = makePipeline(dev, layout_, scene({kBoxVert, sizeof kBoxVert, kBoxFrag, sizeof kBoxFrag, kSceneFormat, true}, 1));
    meshPipeline_ = makePipeline(dev, layout_, scene({kMeshVert, sizeof kMeshVert, kMeshFrag, sizeof kMeshFrag, kSceneFormat, true}, 2));
    meshBlendPipeline_ = makePipeline(dev, layout_, scene({kMeshVert, sizeof kMeshVert, kMeshFrag, sizeof kMeshFrag, kSceneFormat, true, false, true}, 1));
    skinnedPipeline_ = makePipeline(dev, layout_, scene({kSkinnedVert, sizeof kSkinnedVert, kMeshFrag, sizeof kMeshFrag, kSceneFormat, true}, 2));
    skinnedBlendPipeline_ = makePipeline(dev, layout_, scene({kSkinnedVert, sizeof kSkinnedVert, kMeshFrag, sizeof kMeshFrag, kSceneFormat, true, false, true}, 1));
    {
        PipelineDesc add{kFullscreenVert, sizeof kFullscreenVert, kVolumetricFrag, sizeof kVolumetricFrag, kSceneFormat, false};
        volumetricAddPipeline_ = makePipeline(dev, layout_, [&] { auto d = add; d.additive = true; return d; }());
        volumetricPipeline_ = makePipeline(dev, layout_, add);
    }
    { // particles: in the scene pass, depth-tested, not written; blended or added on
        PipelineDesc sp{kSpriteVert, sizeof kSpriteVert, kSpriteFrag, sizeof kSpriteFrag, kSceneFormat, true, false, true};
        sp.gbuf = 1;
        spritePipeline_ = makePipeline(dev, layout_, sp);
        sp.blend = false, sp.glow = true;
        glowPipeline_ = makePipeline(dev, layout_, sp);
    }
    adaptPipeline_ = makePipeline(dev, layout_, {kFullscreenVert, sizeof kFullscreenVert, kAdaptFrag, sizeof kAdaptFrag, kSceneFormat, false});
    ssrPipeline_ = makePipeline(dev, layout_, {kFullscreenVert, sizeof kFullscreenVert, kSsrFrag, sizeof kSsrFrag, kSceneFormat, false});
    shadowMeshPipeline_ = makePipeline(dev, layout_, {kMeshVert, sizeof kMeshVert, nullptr, 0, VK_FORMAT_UNDEFINED, true, true});
    // Compute pipelines (GPU culling, the fog grid, ray-traced light), on the same layout. All of
    // them are extras: one a driver can't build is left out (and what it does is off). (The
    // Galaxy S22's driver says it has ray queries but can't build gi.comp or froxel_rt.comp.)
    auto compute = [&](const char* name, const uint32_t* code, size_t size, VkPipeline& out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        pi.layout = layout_;
        if (const VkResult res = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &out); res != VK_SUCCESS) {
            log::warn("The graphics driver couldn't build the {} shader (VkResult {}): it's off", name, int(res));
            out = VK_NULL_HANDLE;
        }
        vkDestroyShaderModule(dev, m, nullptr);
    };
    compute("GPU culling", kCullComp, sizeof kCullComp, cullPipeline_);
    compute("fog grid", kFroxelComp, sizeof kFroxelComp, froxelPipeline_);
    if (device_.rayQuery()) compute("ray-traced light", kGiComp, sizeof kGiComp, giPipeline_), compute("ray-traced fog", kFroxelRtComp, sizeof kFroxelRtComp, froxelRtPipeline_);
    // Depth pre-pass, and the scene's opaque meshes after it.
    depthMeshPipeline_ = makePipeline(dev, layout_, {kMeshVert, sizeof kMeshVert, kDepthFrag, sizeof kDepthFrag, VK_FORMAT_UNDEFINED, true});
    depthSkinnedPipeline_ = makePipeline(dev, layout_, {kSkinnedVert, sizeof kSkinnedVert, kDepthFrag, sizeof kDepthFrag, VK_FORMAT_UNDEFINED, true});
    {
        PipelineDesc m = scene({kMeshVert, sizeof kMeshVert, kMeshFrag, sizeof kMeshFrag, kSceneFormat, true}, 2);
        m.prepassed = true;
        meshAfterDepthPipeline_ = makePipeline(dev, layout_, m);
        m.vs = kSkinnedVert, m.vsSize = sizeof kSkinnedVert;
        skinnedAfterDepthPipeline_ = makePipeline(dev, layout_, m);
    }
    shadowSkinnedPipeline_ = makePipeline(dev, layout_, {kSkinnedVert, sizeof kSkinnedVert, nullptr, 0, VK_FORMAT_UNDEFINED, true, true});
    {
        PipelineDesc sky{kSkyVert, sizeof kSkyVert, kSkyFrag, sizeof kSkyFrag, kSceneFormat, true};
        sky.sky = true;
        sky.gbuf = 1;
        skyPipeline_ = makePipeline(dev, layout_, sky);
        sky.fs = kSmokeSkyFrag, sky.fsSize = sizeof kSmokeSkyFrag, sky.over = true;
        smokeSkyPipeline_ = makePipeline(dev, layout_, sky);
    }
    ssaoPipeline_ = makePipeline(dev, layout_, {kFullscreenVert, sizeof kFullscreenVert, kSsaoFrag, sizeof kSsaoFrag, VK_FORMAT_R8_UNORM, false});
    ssaoBlurPipeline_ = makePipeline(dev, layout_, {kFullscreenVert, sizeof kFullscreenVert, kSsaoBlurFrag, sizeof kSsaoBlurFrag, VK_FORMAT_R8_UNORM, false});
    taaPipeline_ = makePipeline(dev, layout_, {kFullscreenVert, sizeof kFullscreenVert, kTaaFrag, sizeof kTaaFrag, kSceneFormat, false});
    bloomDownPipeline_ = makePipeline(dev, layout_, {kFullscreenVert, sizeof kFullscreenVert, kBloomDownFrag, sizeof kBloomDownFrag, kSceneFormat, false});
    {
        PipelineDesc up{kFullscreenVert, sizeof kFullscreenVert, kBloomUpFrag, sizeof kBloomUpFrag, kSceneFormat, false};
        up.additive = true;
        bloomUpPipeline_ = makePipeline(dev, layout_, up);
    }
    hzbPipeline_ = makePipeline(dev, layout_, {kFullscreenVert, sizeof kFullscreenVert, kHzbFrag, sizeof kHzbFrag, VK_FORMAT_R32_SFLOAT, false});
    tonemapPipeline_ = makePipeline(dev, layout_, {kFullscreenVert, sizeof kFullscreenVert, kTonemapFrag, sizeof kTonemapFrag, swapFormat_, false});
}

bool VulkanBackend::initImGui(const Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = device_.instance();
    ii.PhysicalDevice = device_.physicalDevice();
    ii.Device = device_.device();
    ii.QueueFamily = device_.queueFamily();
    ii.Queue = device_.queue();
    ii.DescriptorPoolSize = 2048; // (one per texture the UI shows; out of them, the font gets none: a crash)
    ii.MinImageCount = 2;
    ii.ImageCount = device_.imageCount();
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapFormat_;
    imgui_ = ImGui_ImplSDL3_InitForVulkan(window.handle()) && ImGui_ImplVulkan_Init(&ii);
    return imgui_;
}

void VulkanBackend::ensureTargets(VkExtent2D extent) {
    if (scene_.image && targetExtent_.width == extent.width && targetExtent_.height == extent.height) return;
    device_.waitIdle();
    destroyTexture(scene_);
    destroyTexture(depth_);
    scene_ = makeTarget(kSceneFormat, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    depth_ = makeTarget(kDepthFormat, extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    targetExtent_ = extent;
    bind(kSceneSlot, scene_.view);
    bind(kDepthSlot, depth_.view);
    for (Texture* t : {&mirror_, &mirrorGbuf_, &mirrorDepth_}) destroyTexture(*t);
    mirror_ = makeTarget(kSceneFormat, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    mirrorGbuf_ = makeTarget(kGbufFormat, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    mirrorDepth_ = makeTarget(kDepthFormat, extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    bind(kMirrorSlot, mirror_.view);
    mirrorValid_ = false;
    // Occlusion at half size; bloom from half down to 1/64.
    destroyTexture(ao_);
    destroyTexture(aoBlur_);
    for (Texture& t : bloom_) destroyTexture(t);
    for (Texture& t : taa_) destroyTexture(t);
    constexpr VkImageUsageFlags target = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    aoExtent_ = {std::max(1u, extent.width / 2), std::max(1u, extent.height / 2)};
    ao_ = makeTarget(VK_FORMAT_R8_UNORM, aoExtent_, target, VK_IMAGE_ASPECT_COLOR_BIT);
    aoBlur_ = makeTarget(VK_FORMAT_R8_UNORM, aoExtent_, target, VK_IMAGE_ASPECT_COLOR_BIT);
    bind(kAoSlot, ao_.view);
    bind(kAoBlurSlot, aoBlur_.view);
    destroyTexture(gbuf_);
    destroyTexture(ssr_);
    gbuf_ = makeTarget(kGbufFormat, extent, target, VK_IMAGE_ASPECT_COLOR_BIT);
    ssr_ = makeTarget(kSceneFormat, aoExtent_, target, VK_IMAGE_ASPECT_COLOR_BIT);
    bind(kGbufSlot, gbuf_.view);
    bind(kSsrSlot, ssr_.view);
    for (uint32_t i = 0; i < 2; ++i) {
        destroyTexture(adapt_[i]);
        adapt_[i] = makeTarget(kSceneFormat, {1, 1}, target, VK_IMAGE_ASPECT_COLOR_BIT);
        adaptValid_ = false;
        bind(kAdaptSlot + i, adapt_[i].view);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        taa_[i] = makeTarget(kSceneFormat, extent, target, VK_IMAGE_ASPECT_COLOR_BIT);
        bind(kTaaSlot + i, taa_[i].view);
    }
    for (uint32_t i = 0; i < kBloomLevels; ++i) {
        bloomExtent_[i] = {std::max(1u, extent.width >> (i + 1)), std::max(1u, extent.height >> (i + 1))};
        bloom_[i] = makeTarget(kSceneFormat, bloomExtent_[i], target, VK_IMAGE_ASPECT_COLOR_BIT);
        bind(kBloomSlot + i, bloom_[i].view);
    }
}

void VulkanBackend::drawMeshes(VkCommandBuffer cmd, const std::vector<MeshDraw>& draws, const glm::mat4& viewProj, bool shadow, VkPipeline mesh,
                               VkPipeline skinned, const vk::Buffer* indirect, bool skipLiquid) {
    const uint32_t fi = device_.frameIndex();
    VkPipeline bound = VK_NULL_HANDLE;
    uint32_t boundModel = UINT32_MAX;
    ModelPush mp{};
    mp.viewProj = viewProj;
    mp.frame = drawFrame_ ? drawFrame_ : frameBuffers_[fi].address;
    for (const MeshDraw& d : draws) {
        const Model& m = models_[d.model];
        if (!m.indices.buffer || (skipLiquid && d.liquid)) continue;
        const bool isSkinned = d.firstJoint != UINT32_MAX;
        VkPipeline want = isSkinned ? skinned : mesh;
        if (want != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            bound = want;
        }
        if (d.model != boundModel) {
            vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            boundModel = d.model;
        }
        mp.vertices = m.vertices.address;
        mp.skin = isSkinned ? m.skin.address : 0;
        mp.joints = isSkinned ? jointBuffers_[fi].address + d.firstJoint * sizeof(glm::mat4) : 0;
        mp.draw = drawBuffers_[fi].address + d.draw * sizeof(gpu::DrawData);
        mp.textures = d.textures;
        mp.tint = d.tint;
        mp.material = shadow ? 0 : d.material;
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof mp, &mp);
        if (indirect) // GPU-culled: its command says whether it's drawn
            vkCmdDrawIndexedIndirect(cmd, indirect->buffer, (&d - draws.data()) * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
        else
            vkCmdDrawIndexed(cmd, d.indexCount, 1, d.firstIndex, 0, 0);
    }
}

void VulkanBackend::shadowPasses(VkCommandBuffer cmd, Texture& target, uint32_t size, const std::vector<ShadowPass>& passes) {
    vk::transitionImage(cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target.view;
    att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {size, size}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &att;
    vkCmdBeginRendering(cmd, &ri);
    for (const ShadowPass& pass : passes) {
        const VkViewport v{float(pass.rect.x), float(pass.rect.y), float(pass.rect.z), float(pass.rect.w), 0, 1};
        const VkRect2D s{{pass.rect.x, pass.rect.y}, {uint32_t(pass.rect.z), uint32_t(pass.rect.w)}};
        vkCmdSetViewport(cmd, 0, 1, &v);
        vkCmdSetScissor(cmd, 0, 1, &s);
        drawMeshes(cmd, *pass.draws, pass.viewProj, true, shadowMeshPipeline_, shadowSkinnedPipeline_);
    }
    vkCmdEndRendering(cmd);
    vk::transitionImage(cmd, target.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void VulkanBackend::render(FramePacket& p) {
    VkCommandBuffer cmd = cmd_;
    ensureTargets({p.width, p.height}); // the scene's size (render scale), not the window's
    const uint32_t fi = device_.frameIndex(); // the frame fence guarantees the GPU is done with these
    std::memcpy(drawBuffers_[fi].mapped, p.draws.data(), p.draws.size() * sizeof(gpu::DrawData));
    std::memcpy(jointBuffers_[fi].mapped, p.joints.data(), p.joints.size() * sizeof(glm::mat4));
    auto* lights = static_cast<uint8_t*>(lightBuffers_[fi].mapped);
    std::memcpy(lights + kLightsOffset, p.lights.data(), p.lights.size() * sizeof(gpu::Light));
    std::memcpy(lights + kViewsOffset, p.views.data(), p.views.size() * sizeof(gpu::ShadowView));
    std::memcpy(lights + kCellsOffset, p.clusters->cells.data(), p.clusters->cells.size() * sizeof(glm::uvec2));
    std::memcpy(lights + kIndicesOffset, p.clusters->indices.data(), p.clusters->indices.size() * sizeof(uint32_t));
    const VkDeviceAddress lb = lightBuffers_[fi].address;
    p.frame.lights = lb + kLightsOffset;
    p.frame.cells = lb + kCellsOffset;
    p.frame.lightIndices = lb + kIndicesOffset;
    p.frame.shadowViews = lb + kViewsOffset;
    p.frame.froxels = froxelOut_.address;
    // Real-time GI: on while there are dynamic lights (and a little after, while their light fades).
    giIdle_ = p.lights.empty() ? giIdle_ + 1 : 0;
    giOn_ = p.frame.gi.x > 0.5f && giPipeline_ && !p.rayInstances.empty() && giIdle_ < 120;
    if (giOn_) {
        const float spacing = p.frame.gi.y;
        const glm::vec3 cell = glm::floor(glm::vec3(p.frame.camPos) / spacing) - glm::vec3(12, 12, 6);
        p.frame.giOrigin = glm::vec4(cell, 0.0f);
        p.frame.giProbes = giProbes_[giFrame_ & 1u].address; // this frame's (written before the scene reads it)
    } else {
        p.frame.gi.x = 0.0f;
        giFresh_ = true;
    }
    std::memcpy(frameBuffers_[fi].mapped, &p.frame, sizeof p.frame);

    beginMarks(cmd);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    // ── GPU occlusion culling: the opaque draws' commands, before anything draws them ──
    cullFor_ = nullptr;
    hzbReady_ = hzbNext_, hzbNext_ = false; // last frame made the grid: it's there to test against
    if (p.gpuCull && !p.opaque.empty() && p.opaque.size() <= kMaxCulled) {
        gpuCulled_ = *static_cast<uint32_t*>(cullCounter_[fi].mapped); // this slot's last count (its fence has passed)
        *static_cast<uint32_t*>(cullCounter_[fi].mapped) = 0;
        auto* boxes = static_cast<CullBox*>(cullBoxes_[fi].mapped);
        for (size_t i = 0; i < p.opaque.size(); ++i) {
            const MeshDraw& d = p.opaque[i];
            boxes[i] = {glm::vec4(d.lo, 0), glm::vec4(d.hi, 0), glm::uvec4(d.indexCount, d.firstIndex, d.lo != d.hi ? 1u : 0u, 0)};
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
        const CullPush cp{p.cullViewProj, cullBoxes_[fi].address, cullCmds_[fi].address, cullCounter_[fi].address, uint32_t(p.opaque.size()), kHzbSlot,
                          p.cullOcclusion && hzbReady_ ? 1u : 0u, 0};
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof cp, &cp);
        vkCmdDispatch(cmd, (uint32_t(p.opaque.size()) + 63) / 64, 1, 1);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        cullFor_ = &cullCmds_[fi];
        mark(cmd, "gpu culling");
    }
    if (p.shadowSize != shadowMapSize_) { // r_shadowres changed
        device_.waitIdle();
        destroyTexture(shadowMap_);
        shadowMap_ = makeTarget(kDepthFormat, {p.shadowSize, p.shadowSize}, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
        bind(kSunShadowSlot, shadowMap_.view);
        shadowMapSize_ = p.shadowSize;
    }
    shadowPasses(cmd, shadowMap_, p.shadowSize, p.sunPasses); // always: clears it when there's no sun
    if (!p.atlasPasses.empty()) shadowPasses(cmd, shadowAtlas_, kAtlasSize, p.atlasPasses);
    mark(cmd, "shadows");

    // ── Real-time GI: this frame's scene for rays, then every probe traces it (the fog grid
    // traces it too: what sky each cell sees) ──
    const bool fogRays = p.frame.froxel.x > 0.5f && froxelRtPipeline_ && !p.rayInstances.empty();
    VkDeviceAddress fogTlas = 0;
    if (giOn_ || fogRays) {
        Tlas& t = tlas_[fi];
        const uint32_t n = uint32_t(std::min<size_t>(p.rayInstances.size(), 1u << 20));
        ensureTlas(t, n);
        auto* inst = static_cast<VkAccelerationStructureInstanceKHR*>(t.instances.mapped);
        auto* info = static_cast<RayInst*>(t.info.mapped);
        uint32_t used = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const RayInstance& r = p.rayInstances[i];
            if (r.model >= models_.size()) continue;
            const Model& m = models_[r.model];
            const Blas* b = nullptr;
            for (auto& [first, blas] : m.blas)
                if (first == r.firstIndex) b = &blas;
            if (!b) continue;
            VkAccelerationStructureInstanceKHR& o = inst[used];
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 4; ++col) o.transform.matrix[row][col] = r.transform[col][row];
            o.instanceCustomIndex = used;
            o.mask = 0xFF;
            o.instanceShaderBindingTableRecordOffset = 0;
            o.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
            o.accelerationStructureReference = b->address;
            info[used] = {m.vertices.address, m.indices.address, r.firstIndex, r.textures, r.tint, 0};
            ++used;
        }
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        g.geometry.instances.data.deviceAddress = t.instances.address;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        bi.dstAccelerationStructure = t.as;
        bi.scratchData.deviceAddress = alignUp(t.scratch.address, scratchAlign_);
        const VkAccelerationStructureBuildRangeInfoKHR range{used, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, &ranges);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        if (fogRays) fogTlas = t.address;
      if (giOn_) {
        // A different turn of the ray pattern each frame (random unit quaternion), so the probes
        // see all round them over a few frames.
        uint32_t h = giFrame_ * 747796405u + 2891336453u;
        auto rnd = [&] { h = h * 747796405u + 2891336453u; uint32_t w = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u; return float((w >> 22u) ^ w) / 4294967296.0f; };
        const float u1 = rnd(), u2 = rnd() * 6.2831853f, u3 = rnd() * 6.2831853f;
        const glm::vec4 rot(std::sqrt(1 - u1) * std::sin(u2), std::sqrt(1 - u1) * std::cos(u2), std::sqrt(u1) * std::sin(u3), std::sqrt(u1) * std::cos(u3));
        const uint32_t cur = giFrame_ & 1u;
        const GiPush gp{frameBuffers_[fi].address, giProbes_[cur].address, giProbes_[cur ^ 1u].address, t.info.address, t.address, giFresh_ ? 1u : 0u, 0.9f, rot};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, giPipeline_);
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof gp, &gp);
        vkCmdDispatch(cmd, kGiProbes, 1, 1);
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &dep);
        giFresh_ = false;
        ++giFrame_;
        mark(cmd, "gi probes");
      }
    }

    // ── The fog grid: lit and shadowed cells, then each column added up (the scene reads it) ──
    if (p.frame.froxel.x > 0.5f) {
        auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess) {
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = src, mb.srcAccessMask = srcAccess, mb.dstStageMask = dst, mb.dstAccessMask = dstAccess;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        // (Last frame's reads of the grid, and of the cells, are done before they're written.)
        barrier(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        const uint32_t cur = froxelFrame_ & 1u;
        // A jitter along the slice that walks the whole of it over a few frames (golden ratio).
        const float jitter = std::fmod(0.5f + float(froxelFrame_) * 0.618034f, 1.0f);
        FroxelPush fp{frameBuffers_[fi].address, froxelCells_[cur].address, froxelCells_[cur ^ 1u].address, froxelOut_.address,
                      p.post.proj, froxelPrevPos_, glm::vec4(glm::vec3(froxelPrevFwd_), jitter), 0, froxelFrame_, fogTlas, 0, 0, p.smokeOrigin};
        if (!p.smoke.empty()) {
            std::memcpy(smoke_[fi].mapped, p.smoke.data(), std::min(p.smoke.size(), size_t(kSmokeDims.x) * kSmokeDims.y * kSmokeDims.z) * 2);
            fp.smoke = smoke_[fi].address;
            fp.smokeSky = smokeSky_.address;
        } else fp.smokeOrigin.w = -1.0f;
        smokeSkyOn_ = !p.smoke.empty();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fogTlas ? froxelRtPipeline_ : froxelPipeline_);
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof fp, &fp);
        vkCmdDispatch(cmd, 160 / 8, (90 + 7) / 8, 64);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        fp.mode = 1;
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof fp, &fp);
        vkCmdDispatch(cmd, 160 / 8, (90 + 7) / 8, 1);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        froxelPrevPos_ = glm::vec4(glm::vec3(p.frame.camPos), 1.0f);
        froxelPrevFwd_ = glm::vec4(glm::vec3(p.frame.camFwd), 0.0f);
        ++froxelFrame_;
        mark(cmd, "fog grid");
    } else {
        froxelPrevPos_.w = 0.0f; // (nothing to blend with when it comes back on)
    }


    // ── Mirror: the scene from the reflected eye, cut at the mirror (no pre-pass, no post) ──
    if (p.mirror) {
        gpu::FrameData mf = p.mirrorFrame;
        mf.lights = p.frame.lights, mf.cells = p.frame.cells, mf.lightIndices = p.frame.lightIndices, mf.shadowViews = p.frame.shadowViews;
        mf.froxels = p.frame.froxels, mf.giProbes = p.frame.giProbes;
        mf.gi = p.frame.gi, mf.giOrigin = p.frame.giOrigin; // as decided here (GI may be off this frame: no probes to read)
        std::memcpy(mirrorFrames_[fi].mapped, &mf, sizeof mf);
        const VkExtent2D me{p.width, p.height};
        vk::transitionImage(cmd, mirror_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vk::transitionImage(cmd, mirrorGbuf_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vk::transitionImage(cmd, mirrorDepth_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo mc[2] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        mc[0].imageView = mirror_.view;
        mc[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        mc[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        mc[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        mc[0].clearValue.color = {{p.clearColor.r, p.clearColor.g, p.clearColor.b, 1.0f}};
        mc[1] = mc[0];
        mc[1].imageView = mirrorGbuf_.view;
        mc[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkRenderingAttachmentInfo md{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        md.imageView = mirrorDepth_.view;
        md.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        md.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        md.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        md.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo mri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        const VkRect2D ms{{p.mirrorRect.x, p.mirrorRect.y}, {uint32_t(p.mirrorRect.z), uint32_t(p.mirrorRect.w)}}; // only where the mirror is
        // The whole target (a smaller render area left it unwritten on NVIDIA); the scissor
        // keeps the drawing to the mirror's rectangle.
        mri.renderArea = {{0, 0}, me};
        mri.layerCount = 1;
        mri.colorAttachmentCount = 2;
        mri.pColorAttachments = mc;
        mri.pDepthAttachment = &md;
        vkCmdBeginRendering(cmd, &mri);
        const VkViewport mv{0, 0, float(me.width), float(me.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &mv);
        vkCmdSetScissor(cmd, 0, 1, &ms);
        drawFrame_ = mirrorFrames_[fi].address;
        drawMeshes(cmd, p.mirrorOpaque, p.mirrorViewProj, false, meshPipeline_, skinnedPipeline_);
        if (p.frame.sky.x > 0.5f) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
            const gpu::SkyPush sp{p.mirrorInvViewProj, p.frame.sunDir, p.frame.sunColor, p.frame.sky, glm::vec4(p.skyCloud, p.skyPhotoSunZ, p.frame.weather.z, p.frame.weather.w)};
            vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof sp, &sp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        drawMeshes(cmd, p.mirrorTranslucent, p.mirrorViewProj, false, meshBlendPipeline_, skinnedBlendPipeline_);
        drawFrame_ = 0;
        vkCmdEndRendering(cmd);
        vk::transitionImage(cmd, mirror_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        mirrorValid_ = true;
        mark(cmd, "mirror");
    } else if (!mirrorValid_ && mirror_.image) { // (bound in the table: keep it in a readable layout)
        vk::transitionImage(cmd, mirror_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        mirrorValid_ = true;
    }

    // ── Scene pass (HDR) ──
    const VkExtent2D extent{p.width, p.height}, outExtent{p.outWidth, p.outHeight};
    const VkViewport outViewport{0, 0, float(p.outWidth), float(p.outHeight), 0, 1};
    const VkRect2D outScissor{{0, 0}, outExtent};
    const VkViewport viewport{0, 0, float(p.width), float(p.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vk::transitionImage(cmd, scene_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk::transitionImage(cmd, gbuf_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk::transitionImage(cmd, depth_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo colors[2] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
    VkRenderingAttachmentInfo& color = colors[0];
    color.imageView = scene_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{p.clearColor.r, p.clearColor.g, p.clearColor.b, 1.0f}};
    colors[1] = color;
    colors[1].imageView = gbuf_.view;
    colors[1].clearValue.color = {{0.5f, 0.5f, 1.0f, 0.0f}}; // nothing to reflect
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depth_.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};
    if (p.prepass) { // depth first (no liquids: they're cut at their surface), then the scene on top of it
        VkRenderingAttachmentInfo pd = depth;
        pd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo pri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        pri.renderArea = scissor;
        pri.layerCount = 1;
        pri.pDepthAttachment = &pd;
        vkCmdBeginRendering(cmd, &pri);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        drawMeshes(cmd, p.opaque, p.viewProj, false, depthMeshPipeline_, depthSkinnedPipeline_, cullFor_, true);
        vkCmdEndRendering(cmd);
        mark(cmd, "depth pre-pass");
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = scissor;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 2;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, boxPipeline_);
    for (const gpu::Box& b : p.boxes) {
        const BoxPush bp{p.viewProj, b};
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof bp, &bp);
        vkCmdDraw(cmd, 36, 1, 0, 0);
    }
    drawMeshes(cmd, p.opaque, p.viewProj, false, p.prepass ? meshAfterDepthPipeline_ : meshPipeline_, p.prepass ? skinnedAfterDepthPipeline_ : skinnedPipeline_,
               cullFor_);
    if (p.frame.sky.x > 0.5f) { // where nothing was drawn
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
        const gpu::SkyPush sp{p.invViewProj, p.frame.sunDir, p.frame.sunColor, p.frame.sky, glm::vec4(p.skyCloud, p.skyPhotoSunZ, p.frame.weather.z, p.frame.weather.w)};
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof sp, &sp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    if (smokeSkyOn_ && p.frame.froxel.x > 0.5f) { // smoke in front of the sky
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, smokeSkyPipeline_);
        const SmokeSkyPush sp{frameBuffers_[device_.frameIndex()].address, smokeSky_.address};
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof sp, &sp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    drawMeshes(cmd, p.translucent, p.viewProj, false, meshBlendPipeline_, skinnedBlendPipeline_); // back to front, blended
    for (auto [list, pipe] : {std::pair{&p.sprites, spritePipeline_}, std::pair{&p.glowSprites, glowPipeline_}}) {
        if (list->empty()) continue;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        for (const gpu::Sprite& sp : *list) {
            const SpritePush push{p.viewProj, sp};
            vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof push, &push);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);
    vk::transitionImage(cmd, gbuf_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk::transitionImage(cmd, depth_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    vk::transitionImage(cmd, scene_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    mark(cmd, "scene");
    // ── Occlusion grid: the depth (only opaque things write it) shrunk and copied for the CPU ──
    if (p.hzbTag) {
        const uint32_t fi = device_.frameIndex();
        vk::transitionImage(cmd, hzb_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo grid{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        grid.imageView = hzb_.view;
        grid.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        grid.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        grid.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const VkRect2D area{{0, 0}, {kHzbWidth, kHzbHeight}};
        VkRenderingInfo gi{VK_STRUCTURE_TYPE_RENDERING_INFO};
        gi.renderArea = area;
        gi.layerCount = 1;
        gi.colorAttachmentCount = 1;
        gi.pColorAttachments = &grid;
        vkCmdBeginRendering(cmd, &gi);
        const VkViewport gv{0, 0, float(kHzbWidth), float(kHzbHeight), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &gv);
        vkCmdSetScissor(cmd, 0, 1, &area);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hzbPipeline_);
        const HzbPush hp{kDepthSlot, extent.width, extent.height, kHzbWidth, kHzbHeight};
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof hp, &hp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        vk::transitionImage(cmd, hzb_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kHzbWidth, kHzbHeight, 1};
        vkCmdCopyImageToBuffer(cmd, hzb_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hzbReadback_[fi].buffer, 1, &region);
        vk::transitionImage(cmd, hzb_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); // next frame's culling reads it
        // The pyramid: each level the farthest depth of the four (or so) under it.
        for (uint32_t l = 1; l < kHzbLevels; ++l) {
            const uint32_t sw = std::max(1u, kHzbWidth >> (l - 1)), sh = std::max(1u, kHzbHeight >> (l - 1));
            const uint32_t dw = std::max(1u, kHzbWidth >> l), dh = std::max(1u, kHzbHeight >> l);
            vk::transitionImage(cmd, hzbMips_[l].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo lv{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            lv.imageView = hzbMips_[l].view;
            lv.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            lv.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            lv.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            const VkRect2D la{{0, 0}, {dw, dh}};
            VkRenderingInfo li{VK_STRUCTURE_TYPE_RENDERING_INFO};
            li.renderArea = la;
            li.layerCount = 1;
            li.colorAttachmentCount = 1;
            li.pColorAttachments = &lv;
            vkCmdBeginRendering(cmd, &li);
            const VkViewport lvp{0, 0, float(dw), float(dh), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &lvp);
            vkCmdSetScissor(cmd, 0, 1, &la);
            const HzbPush lp{kHzbSlot + l - 1, sw, sh, dw, dh};
            vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof lp, &lp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            vk::transitionImage(cmd, hzbMips_[l].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        hzbNext_ = true;
        VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &toHost;
        vkCmdPipelineBarrier2(cmd, &dep);
        hzbTags_[fi] = p.hzbTag;
        vkCmdSetViewport(cmd, 0, 1, &viewport); // back to the full screen for the tonemap
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    mark(cmd, "occlusion grid");
    // ── Post: ambient occlusion, bloom ──
    // One full-screen pass into `target` (left ready to sample); load: blend onto what's there.
    auto pass = [&](Texture& t, VkExtent2D ext, VkFormat, VkPipeline pipe, const void* push, uint32_t size, bool load) {
        vk::transitionImage(cmd, t.image, load ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = t.view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const VkRect2D area{{0, 0}, ext};
        VkRenderingInfo ri3{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri3.renderArea = area;
        ri3.layerCount = 1;
        ri3.colorAttachmentCount = 1;
        ri3.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri3);
        const VkViewport vp{0, 0, float(ext.width), float(ext.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &area);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, size, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        vk::transitionImage(cmd, t.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };
    const bool ao = p.post.aoStrength > 0.0f, bloom = p.post.bloomStrength > 0.0f, reflections = p.post.ssr > 0.0f,
               volumetric = p.frame.volumetric.x > 0.0f && p.frame.froxel.x < 0.5f, // the sun and the dynamic lights (else in the fog grid)
               ssr = reflections || volumetric; // (the half-size target both go to)
    if (reflections) { // reflections from this frame's scene, added by TAA (or the tonemap)
        const SsrPush sp{kDepthSlot, kGbufSlot, kSceneSlot, extent.width, extent.height, aoExtent_.width, aoExtent_.height, p.post.ssr, p.post.proj};
        pass(ssr_, aoExtent_, kSceneFormat, ssrPipeline_, &sp, sizeof sp, false);
    }
    if (volumetric) { // sunlight in the air, onto the same target
        const VolumetricPush vp{frameBuffers_[fi].address, kDepthSlot, extent.width, extent.height, aoExtent_.width, aoExtent_.height, {}, p.post.proj};
        pass(ssr_, aoExtent_, kSceneFormat, reflections ? volumetricAddPipeline_ : volumetricPipeline_, &vp, sizeof vp, reflections);
    }
    if (ssr) mark(cmd, "reflections + volumetric");
    // TAA first: what follows (bloom, tonemap) uses the resolved image.
    TextureId resolved = kSceneSlot;
    if (p.post.taa) {
        taaCur_ ^= 1u;
        const TaaPush tp{p.post.reproject, kSceneSlot, kTaaSlot + (taaCur_ ^ 1u), kDepthSlot, extent.width, extent.height, p.post.taaReset ? 1u : 0u, ssr ? kSsrSlot : 0u, 0};
        pass(taa_[taaCur_], extent, kSceneFormat, taaPipeline_, &tp, sizeof tp, false);
        resolved = kTaaSlot + taaCur_;
        mark(cmd, "taa");
    }
    if (ao) {
        const SsaoPush sp{kDepthSlot, extent.width, extent.height, aoExtent_.width, aoExtent_.height, p.post.aoRadius, p.post.aoStrength, 0, p.post.proj};
        pass(ao_, aoExtent_, VK_FORMAT_R8_UNORM, ssaoPipeline_, &sp, sizeof sp, false);
        const SsaoBlurPush bp{kAoSlot, kDepthSlot, extent.width, extent.height, aoExtent_.width, aoExtent_.height, 0, 0, p.post.proj};
        pass(aoBlur_, aoExtent_, VK_FORMAT_R8_UNORM, ssaoBlurPipeline_, &bp, sizeof bp, false);
        mark(cmd, "ssao");
    }
    if (bloom) {
        for (uint32_t i = 0; i < kBloomLevels; ++i) {
            const VkExtent2D src = i == 0 ? extent : bloomExtent_[i - 1];
            const BloomPush bp{i == 0 ? resolved : kBloomSlot + i - 1, src.width, src.height, bloomExtent_[i].width, bloomExtent_[i].height, i == 0 ? 1u : 0u};
            pass(bloom_[i], bloomExtent_[i], kSceneFormat, bloomDownPipeline_, &bp, sizeof bp, false);
        }
        const float radius = 1.0f;
        uint32_t radiusBits;
        std::memcpy(&radiusBits, &radius, 4);
        for (uint32_t i = kBloomLevels - 1; i > 0; --i) {
            const BloomPush bp{kBloomSlot + i, bloomExtent_[i].width, bloomExtent_[i].height, bloomExtent_[i - 1].width, bloomExtent_[i - 1].height, radiusBits};
            pass(bloom_[i - 1], bloomExtent_[i - 1], kSceneFormat, bloomUpPipeline_, &bp, sizeof bp, true);
        }
    }

    if (bloom) mark(cmd, "bloom");
    if (p.post.adapt) { // eye adaptation from the smallest bloom level
        adaptCur_ ^= 1u;
        const AdaptPush ap{kBloomSlot + kBloomLevels - 1, p.post.adaptReset || !adaptValid_ ? 0u : kAdaptSlot + (adaptCur_ ^ 1u), p.post.adaptDt, 1.5f,
                           p.post.adaptKey, 0.6f, 1.8f, 0};
        pass(adapt_[adaptCur_], {1, 1}, kSceneFormat, adaptPipeline_, &ap, sizeof ap, false);
        adaptValid_ = true;
    }

    // ── Tonemap to the swapchain, then UI ──
    VkRenderingAttachmentInfo swap{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    swap.imageView = device_.currentImageView();
    swap.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swap.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    swap.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri2{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri2.renderArea = outScissor;
    ri2.layerCount = 1;
    ri2.colorAttachmentCount = 1;
    ri2.pColorAttachments = &swap;
    vkCmdBeginRendering(cmd, &ri2);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);
    vkCmdSetViewport(cmd, 0, 1, &outViewport);
    vkCmdSetScissor(cmd, 0, 1, &outScissor);
    const TonemapPush tp{resolved, p.exposure, ao ? kAoBlurSlot : 0u, bloom ? kBloomSlot : 0u, p.post.bloomStrength, p.post.saturation, p.post.vignette, p.post.taa ? p.post.sharpen : 0.0f, kDepthSlot, {ssr && !p.post.taa ? kSsrSlot : 0u, p.post.adapt ? kAdaptSlot + adaptCur_ : 0u, 0}, p.post.shafts, p.post.sunColor};
    vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof tp, &tp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
    mark(cmd, "tonemap + ui");

    vk::Buffer shot;
    if (!p.screenshot.empty()) {
        shot = device_.createBuffer(VkDeviceSize(outExtent.width) * outExtent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
        vk::transitionImage(cmd, device_.currentImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {outExtent.width, outExtent.height, 1};
        vkCmdCopyImageToBuffer(cmd, device_.currentImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buffer, 1, &region);
        vk::transitionImage(cmd, device_.currentImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    device_.endFrame();
    if (shot.buffer) {
        device_.waitIdle();
        vmaInvalidateAllocation(device_.allocator(), shot.alloc, 0, VK_WHOLE_SIZE);
        const size_t count = size_t(outExtent.width) * outExtent.height;
        std::vector<uint8_t> px(static_cast<uint8_t*>(shot.mapped), static_cast<uint8_t*>(shot.mapped) + count * 4);
        device_.destroyBuffer(shot);
        if (swapFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapFormat_ == VK_FORMAT_B8G8R8A8_SRGB)
            for (size_t i = 0; i < count; ++i) std::swap(px[i * 4], px[i * 4 + 2]);
        saveScreenshot(p.screenshot, std::move(px), outExtent.width, outExtent.height);
    }
}
} // namespace

std::unique_ptr<RenderBackend> createVulkanBackend() { return std::make_unique<VulkanBackend>(); }

} // namespace keel
