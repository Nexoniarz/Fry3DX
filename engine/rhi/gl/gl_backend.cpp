// The OpenGL 4.6 backend. Same shaders and passes as Vulkan:
//   - textures are ARB_bindless_texture handles in a storage-buffer table, indexed exactly like
//     Vulkan's descriptor array; shadow maps also get comparison handles in a second table
//   - per-draw / per-frame data are storage buffers at fixed bindings (model.glsl); the push
//     constants become a small uniform block, rewritten per draw
//   - glClipControl(LOWER_LEFT, ZERO_TO_ONE): with GL's own origin, clip-space y = -1 lands in
//     memory row 0 exactly as in Vulkan (whose projection we share, y flipped), so images,
//     shadow-map tiles and gl_FragCoord all match; depth is 0..1 like Vulkan. The y flip reverses
//     window-space winding, so front faces are clockwise. Only the final blit to the window
//     (which shows row 0 at the bottom) flips.
// Requires ARB_bindless_texture (NVIDIA, AMD); without it init fails and the engine uses Vulkan.
#include "ui/ui.h"
#include "render/backend.h"
#include "render/screenshot.h"
#include "rhi/gl/gl_loader.h"
#include "core/log.h"
#include "platform/window.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <cstring>
#include <map>
#include <string>
#include <string_view>

namespace keel {
namespace {
// Every shader's source and the shared headers, embedded by the build (cmake/EmbedText.cmake).
const std::map<std::string_view, std::string_view>& shaderSources() {
    static const std::map<std::string_view, std::string_view> sources = [] {
        struct Entry { const char* name; const char* text; };
        static const Entry entries[] = {
#include "gl_sources.inc"
        };
        std::map<std::string_view, std::string_view> m;
        for (const Entry& e : entries) m[e.name] = e.text;
        return m;
    }();
    return sources;
}

// The source with #includes expanded and the GL preamble in place of its #version line.
std::string expand(std::string_view name, int depth = 0) {
    const auto& sources = shaderSources();
    const auto it = sources.find(name);
    if (it == sources.end() || depth > 8) {
        log::error("OpenGL: shader source {} not found", name);
        return {};
    }
    std::string out;
    std::string_view text = it->second;
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        const std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.starts_with("#version")) {
            out += "#version 460 core\n#extension GL_ARB_bindless_texture : require\n#define KEEL_GL 1\n";
        } else if (line.starts_with("#include \"")) {
            const size_t a = line.find('"') + 1, b = line.find('"', a);
            out += expand(line.substr(a, b - a), depth + 1);
        } else {
            out += line;
            out += '\n';
        }
    }
    return out;
}

GLuint compile(GLenum type, std::string_view name) {
    const std::string src = expand(name);
    const char* text = src.c_str();
    const GLuint s = gl::CreateShader(type);
    gl::ShaderSource(s, 1, &text, nullptr);
    gl::CompileShader(s);
    GLint ok = 0;
    gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char logText[4096];
        gl::GetShaderInfoLog(s, sizeof logText, nullptr, logText);
        log::error("OpenGL: {} failed to compile:\n{}", name, logText);
        gl::DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint program(std::string_view vs, std::string_view fs) {
    const GLuint v = compile(GL_VERTEX_SHADER, vs), f = fs.empty() ? 0 : compile(GL_FRAGMENT_SHADER, fs);
    if (!v || (!fs.empty() && !f)) return 0;
    const GLuint p = gl::CreateProgram();
    gl::AttachShader(p, v);
    if (f) gl::AttachShader(p, f);
    gl::LinkProgram(p);
    gl::DeleteShader(v);
    if (f) gl::DeleteShader(f);
    GLint ok = 0;
    gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char logText[4096];
        gl::GetProgramInfoLog(p, sizeof logText, nullptr, logText);
        log::error("OpenGL: {} + {} failed to link:\n{}", vs, fs, logText);
        gl::DeleteProgram(p);
        return 0;
    }
    return p;
}

void APIENTRY debugMessage(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH) log::error("[gl] {}", message);
    else log::warn("[gl] {}", message);
}

// Uniform blocks (std140) standing in for the push constants.
struct ModelPush {
    glm::mat4 viewProj;
    uint32_t textures, tint, material, drawIndex, firstJoint, pad[3];
};
struct BoxPush {
    glm::mat4 viewProj;
    gpu::Box box;
};
struct HzbPush {
    uint32_t depth, srcW, srcH, dstW, dstH, pad[3];
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
    uint32_t depth, w, h, outW, outH;
    float pad[3];
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
    uint32_t pad[2];
};

// Storage buffer bindings (model.glsl, backend_gl.glsl).
enum Binding : GLuint {
    B_VERTICES = 0, B_SKIN = 1, B_JOINTS = 2, B_DRAWS = 3, B_FRAME = 4, B_LIGHTS = 5, B_CELLS = 6, B_INDICES = 7, B_VIEWS = 8,
    B_TEXTURES = 9, B_SHADOW_TEXTURES = 10,
};

class OpenGLBackend final : public RenderBackend {
public:
    const char* name() const override { return "OpenGL 4.6"; }
    GpuInfo gpuInfo() const override { return gpu_; }
    GpuInfo gpu_;

    bool init(const Window& window, bool validation) override {
        window_ = window.handle();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, validation ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0); // we render into our own targets
        context_ = SDL_GL_CreateContext(window_);
        if (!context_) {
            log::warn("OpenGL 4.6 context: {}", SDL_GetError());
            return false;
        }
        SDL_GL_MakeCurrent(window_, context_);
        if (!gl::load()) return false;
        bool bindless = false;
        GLint extensions = 0;
        gl::GetIntegerv(GL_NUM_EXTENSIONS, &extensions);
        for (GLint i = 0; i < extensions; ++i)
            bindless |= std::string_view(reinterpret_cast<const char*>(gl::GetStringi(GL_EXTENSIONS, GLuint(i)))) == "GL_ARB_bindless_texture";
        log::info("GPU: {} ({})", reinterpret_cast<const char*>(gl::GetString(GL_RENDERER)), reinterpret_cast<const char*>(gl::GetString(GL_VERSION)));
        gpu_.name = reinterpret_cast<const char*>(gl::GetString(GL_RENDERER)), gpu_.driver = reinterpret_cast<const char*>(gl::GetString(GL_VERSION));
        if (!bindless) {
            log::warn("OpenGL: GL_ARB_bindless_texture isn't supported by this driver");
            return false;
        }
        if (validation) {
            gl::Enable(GL_DEBUG_OUTPUT);
            gl::Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            gl::DebugMessageCallback(debugMessage, nullptr);
        }
        gl::ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        gl::FrontFace(GL_CW);
        gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl::PixelStorei(GL_PACK_ALIGNMENT, 1);

        gl::CreateSamplers(1, &repeatSampler_);
        gl::SamplerParameteri(repeatSampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        gl::SamplerParameteri(repeatSampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl::SamplerParameteri(repeatSampler_, GL_TEXTURE_WRAP_S, GL_REPEAT);
        gl::SamplerParameteri(repeatSampler_, GL_TEXTURE_WRAP_T, GL_REPEAT);
        gl::SamplerParameterf(repeatSampler_, GL_TEXTURE_MAX_ANISOTROPY, 16.0f);
        gl::CreateSamplers(1, &clampSampler_);
        gl::SamplerParameteri(clampSampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl::SamplerParameteri(clampSampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl::SamplerParameteri(clampSampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::SamplerParameteri(clampSampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl::CreateSamplers(1, &compareSampler_);
        gl::SamplerParameteri(compareSampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl::SamplerParameteri(compareSampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl::SamplerParameteri(compareSampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::SamplerParameteri(compareSampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl::SamplerParameteri(compareSampler_, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        gl::SamplerParameteri(compareSampler_, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

        auto buffer = [](GLsizeiptr size) {
            GLuint b;
            gl::CreateBuffers(1, &b);
            gl::NamedBufferStorage(b, size, nullptr, GL_DYNAMIC_STORAGE_BIT);
            return b;
        };
        textureTable_ = buffer(kMaxTextures * sizeof(GLuint64));
        shadowTable_ = buffer(kMaxTextures * sizeof(GLuint64));
        draws_ = buffer(kMaxDraws * sizeof(gpu::DrawData));
        joints_ = buffer(kMaxJointsPerFrame * sizeof(glm::mat4));
        frame_ = buffer(sizeof(gpu::FrameData));
        mirrorFrame_ = buffer(sizeof(gpu::FrameData));
        lights_ = buffer(kMaxLights * sizeof(gpu::Light));
        views_ = buffer(kMaxShadowViews * sizeof(gpu::ShadowView));
        cells_ = buffer(kClusterX * kClusterY * kClusterZ * sizeof(glm::uvec2));
        indices_ = buffer(kMaxLightIndices * sizeof(uint32_t));
        push_ = buffer(256);
        empty_ = buffer(16); // bound where a shader declares a buffer the draw doesn't use
        gl::CreateVertexArrays(1, &vao_);

        meshProgram_ = program("mesh.vert", "mesh.frag");
        skinnedProgram_ = program("skinned.vert", "mesh.frag");
        shadowMeshProgram_ = program("mesh.vert", "");
        shadowSkinnedProgram_ = program("skinned.vert", "");
        boxProgram_ = program("box.vert", "box.frag");
        tonemapProgram_ = program("fullscreen.vert", "tonemap.frag");
        skyProgram_ = program("sky.vert", "sky.frag");
        hzbProgram_ = program("fullscreen.vert", "hzb.frag");
        ssaoProgram_ = program("fullscreen.vert", "ssao.frag");
        ssaoBlurProgram_ = program("fullscreen.vert", "ssao_blur.frag");
        bloomDownProgram_ = program("fullscreen.vert", "bloom_down.frag");
        bloomUpProgram_ = program("fullscreen.vert", "bloom_up.frag");
        taaProgram_ = program("fullscreen.vert", "taa.frag");
        ssrProgram_ = program("fullscreen.vert", "ssr.frag");
        adaptProgram_ = program("fullscreen.vert", "adapt.frag");
        volumetricProgram_ = program("fullscreen.vert", "volumetric.frag");
        spriteProgram_ = program("sprite.vert", "sprite.frag");
        gl::CreateTextures(GL_TEXTURE_2D, 1, &hzbTex_);
        gl::TextureStorage2D(hzbTex_, 1, GL_R32F, GLsizei(kHzbWidth), GLsizei(kHzbHeight));
        gl::CreateFramebuffers(1, &hzbFbo_);
        gl::NamedFramebufferTexture(hzbFbo_, GL_COLOR_ATTACHMENT0, hzbTex_, 0);
        gl::CreateBuffers(kHzbRing, hzbPbo_);
        for (GLuint b : hzbPbo_) gl::NamedBufferStorage(b, kHzbWidth * kHzbHeight * 4, nullptr, GL_MAP_READ_BIT | GL_CLIENT_STORAGE_BIT);
        if (!meshProgram_ || !skinnedProgram_ || !shadowMeshProgram_ || !shadowSkinnedProgram_ || !boxProgram_ || !tonemapProgram_ || !skyProgram_)
            return false;

        sunShadow_ = depthTarget(kShadowSize, sunShadowFbo_);
        atlas_ = depthTarget(kAtlasSize, atlasFbo_);
        setHandles(kSunShadowSlot, sunShadow_, clampSampler_, true);
        setHandles(kAtlasSlot, atlas_, clampSampler_, true);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        imgui_ = ImGui_ImplSDL3_InitForOpenGL(window_, context_) && ImGui_ImplOpenGL3_Init("#version 460");
        return imgui_;
    }

    void shutdown() override {
        if (!context_) return;
        if (imgui_) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            imgui_ = false;
        }
        if (gl::DeleteBuffers) {
            for (auto& [slot, t] : textures_) releaseTexture(t);
            for (Model& m : models_) releaseModel(m);
            releaseTargets();
            for (GLuint* t : {&sunShadow_.texture, &atlas_.texture}) gl::DeleteTextures(1, t);
            for (GLuint* f : {&sunShadowFbo_, &atlasFbo_, &hzbFbo_}) gl::DeleteFramebuffers(1, f);
            gl::DeleteTextures(1, &hzbTex_);
            for (int i = 0; i < kHzbRing; ++i) {
                if (hzbFence_[i]) gl::DeleteSync(hzbFence_[i]);
                gl::DeleteBuffers(1, &hzbPbo_[i]);
            }
            for (GLuint b : {textureTable_, shadowTable_, draws_, joints_, frame_, mirrorFrame_, lights_, views_, cells_, indices_, push_, empty_})
                gl::DeleteBuffers(1, &b);
            for (GLuint s : {repeatSampler_, clampSampler_, compareSampler_}) gl::DeleteSamplers(1, &s);
            for (GLuint p : {meshProgram_, skinnedProgram_, shadowMeshProgram_, shadowSkinnedProgram_, boxProgram_, tonemapProgram_, skyProgram_, hzbProgram_, ssaoProgram_, ssaoBlurProgram_, ssrProgram_, adaptProgram_, volumetricProgram_, spriteProgram_,
                            bloomDownProgram_, bloomUpProgram_, taaProgram_})
                gl::DeleteProgram(p);
            gl::DeleteVertexArrays(1, &vao_);
        }
        SDL_GL_DestroyContext(context_);
        context_ = nullptr;
    }

    void beginUi() override {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ui::applyPixelScale();
        ImGui::NewFrame();
    }
    void setVsync(bool on) override { SDL_GL_SetSwapInterval(on ? 1 : 0); }
    void onResize() override {}

    bool blockCompression() const override { return true; } // BPTC (BC7) is core since 4.2
    bool renderScale() const override { return true; }

    void createTexture(TextureId slot, uint32_t w, uint32_t h, bool srgb, const std::vector<std::vector<uint8_t>>& mips,
                       TexFormat format = TexFormat::RGBA8) override {
        Texture t;
        gl::CreateTextures(GL_TEXTURE_2D, 1, &t.texture);
        if (format == TexFormat::BC7) {
            const GLenum f = srgb ? 0x8E8D /* GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM */ : 0x8E8C /* GL_COMPRESSED_RGBA_BPTC_UNORM */;
            gl::TextureStorage2D(t.texture, GLsizei(mips.size()), f, GLsizei(w), GLsizei(h));
            for (size_t level = 0; level < mips.size(); ++level)
                gl::CompressedTextureSubImage2D(t.texture, GLint(level), 0, 0, GLsizei(std::max(1u, w >> level)), GLsizei(std::max(1u, h >> level)), f,
                                                GLsizei(mips[level].size()), mips[level].data());
        } else {
            gl::TextureStorage2D(t.texture, GLsizei(mips.size()), srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, GLsizei(w), GLsizei(h));
            for (size_t level = 0; level < mips.size(); ++level)
                gl::TextureSubImage2D(t.texture, GLint(level), 0, 0, GLsizei(std::max(1u, w >> level)), GLsizei(std::max(1u, h >> level)), GL_RGBA,
                                      GL_UNSIGNED_BYTE, mips[level].data());
        }
        if (auto it = textures_.find(slot); it != textures_.end()) retiredTextures_.emplace_back(frameNumber_, it->second); // queued draws may use it
        setHandles(slot, t, repeatSampler_, false); // like Vulkan: one repeating sampler for every texture
        textures_[slot] = t;
    }

    void uploadModel(uint32_t index, const KmdlModel& kmdl) override {
        if (models_.size() <= index) models_.resize(index + 1);
        releaseModel(models_[index]); // GL keeps it alive while queued draws still use it
        Model m;
        auto upload = [](const void* data, size_t size) {
            GLuint b;
            gl::CreateBuffers(1, &b);
            gl::NamedBufferStorage(b, GLsizeiptr(size), data, 0);
            return b;
        };
        if (!kmdl.vertices.empty() && !kmdl.indices.empty()) {
            m.vertices = upload(kmdl.vertices.data(), kmdl.vertices.size() * sizeof(KmdlVertex));
            m.indices = upload(kmdl.indices.data(), kmdl.indices.size() * sizeof(uint32_t));
            if (!kmdl.skin.empty()) m.skin = upload(kmdl.skin.data(), kmdl.skin.size() * sizeof(KmdlSkin));
        }
        models_[index] = m;
    }

    uint64_t uiTexture(TextureId slot) override {
        const auto it = textures_.find(slot);
        return it == textures_.end() ? 0 : uint64_t(it->second.texture);
    }

    bool beginFrame(uint32_t& width, uint32_t& height) override {
        int w = 0, h = 0;
        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) return false;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        if (w <= 0 || h <= 0) return false;
        width = uint32_t(w); // (targets are sized to the scene in render)
        height = uint32_t(h);
        return true;
    }

    void render(FramePacket& p) override;

    // Reads ring slot i's grid if the GPU is done with it (or, wait: before reusing the slot).
    void collectHzb(int i, bool wait) {
        if (!hzbFence_[i]) return;
        const GLenum r = gl::ClientWaitSync(hzbFence_[i], wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, wait ? 1'000'000'000 : 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) return;
        gl::DeleteSync(hzbFence_[i]);
        hzbFence_[i] = nullptr;
        if (hzbTags_[i] <= hzbLatest_.tag) return;
        if (const float* d = static_cast<const float*>(gl::MapNamedBufferRange(hzbPbo_[i], 0, kHzbWidth * kHzbHeight * 4, GL_MAP_READ_BIT))) {
            hzbLatest_ = {hzbTags_[i], std::vector<float>(d, d + kHzbWidth * kHzbHeight)};
            gl::UnmapNamedBuffer(hzbPbo_[i]);
        }
    }

    bool takeHzb(HzbReadback& out) override {
        for (int i = 0; i < kHzbRing; ++i) collectHzb(i, false);
        if (hzbLatest_.tag <= hzbTaken_) return false;
        out = hzbLatest_;
        hzbTaken_ = hzbLatest_.tag;
        return true;
    }

private:
    struct Texture {
        GLuint texture = 0;
        GLuint64 handle = 0, shadowHandle = 0;
    };
    struct Model {
        GLuint vertices = 0, indices = 0, skin = 0;
    };

    void setHandles(TextureId slot, Texture& t, GLuint sampler, bool depth) {
        t.handle = gl::GetTextureSamplerHandleARB(t.texture, sampler);
        gl::MakeTextureHandleResidentARB(t.handle);
        gl::NamedBufferSubData(textureTable_, GLintptr(slot) * 8, 8, &t.handle);
        if (depth) {
            t.shadowHandle = gl::GetTextureSamplerHandleARB(t.texture, compareSampler_);
            gl::MakeTextureHandleResidentARB(t.shadowHandle);
            gl::NamedBufferSubData(shadowTable_, GLintptr(slot) * 8, 8, &t.shadowHandle);
        }
    }
    void releaseTexture(Texture& t) {
        if (t.handle) gl::MakeTextureHandleNonResidentARB(t.handle);
        if (t.shadowHandle) gl::MakeTextureHandleNonResidentARB(t.shadowHandle);
        if (t.texture) gl::DeleteTextures(1, &t.texture);
        t = {};
    }
    void releaseModel(Model& m) {
        for (GLuint* b : {&m.vertices, &m.indices, &m.skin})
            if (*b) gl::DeleteBuffers(1, b);
        m = {};
    }
    Texture depthTarget(uint32_t size, GLuint& fbo) {
        Texture t;
        gl::CreateTextures(GL_TEXTURE_2D, 1, &t.texture);
        gl::TextureStorage2D(t.texture, 1, GL_DEPTH_COMPONENT32F, GLsizei(size), GLsizei(size));
        gl::CreateFramebuffers(1, &fbo);
        gl::NamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, t.texture, 0);
        gl::NamedFramebufferDrawBuffer(fbo, GL_NONE);
        return t;
    }
    void releaseTargets() {
        releaseTexture(scene_);
        releaseTexture(gbuf_);
        releaseTexture(mirror_);
        releaseTexture(mirrorGbuf_);
        if (mirrorDepth_) gl::DeleteTextures(1, &mirrorDepth_);
        if (mirrorFbo_) gl::DeleteFramebuffers(1, &mirrorFbo_);
        mirrorDepth_ = mirrorFbo_ = 0;
        releasePostTarget(ssr_);
        for (PostTarget& t : adapt_) releasePostTarget(t);
        releasePostTarget(ao_);
        releasePostTarget(aoBlur_);
        for (PostTarget& t : bloom_) releasePostTarget(t);
        for (PostTarget& t : taa_) releasePostTarget(t);
        if (depthTex_.handle) gl::MakeTextureHandleNonResidentARB(depthTex_.handle);
        depthTex_ = {};
        if (sceneDepth_) gl::DeleteTextures(1, &sceneDepth_);
        if (sceneFbo_) gl::DeleteFramebuffers(1, &sceneFbo_);
        sceneDepth_ = sceneFbo_ = 0;
    }
    void ensureTargets(uint32_t w, uint32_t h) {
        if (scene_.texture && w == targetW_ && h == targetH_) return;
        releaseTargets();
        gl::CreateTextures(GL_TEXTURE_2D, 1, &scene_.texture);
        gl::TextureStorage2D(scene_.texture, 1, GL_RGBA16F, GLsizei(w), GLsizei(h));
        gl::CreateTextures(GL_TEXTURE_2D, 1, &sceneDepth_);
        gl::TextureStorage2D(sceneDepth_, 1, GL_DEPTH_COMPONENT32F, GLsizei(w), GLsizei(h));
        gl::CreateFramebuffers(1, &sceneFbo_);
        gl::NamedFramebufferTexture(sceneFbo_, GL_COLOR_ATTACHMENT0, scene_.texture, 0);
        gl::NamedFramebufferTexture(sceneFbo_, GL_DEPTH_ATTACHMENT, sceneDepth_, 0);
        gl::CreateTextures(GL_TEXTURE_2D, 1, &gbuf_.texture); // the G-buffer: view normal, roughness, reflection weight
        gl::TextureStorage2D(gbuf_.texture, 1, GL_RGBA16F, GLsizei(w), GLsizei(h));
        gl::NamedFramebufferTexture(sceneFbo_, GL_COLOR_ATTACHMENT1, gbuf_.texture, 0);
        const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        gl::NamedFramebufferDrawBuffers(sceneFbo_, 2, bufs);
        setHandles(kGbufSlot, gbuf_, clampSampler_, false);
        if (gl::CheckNamedFramebufferStatus(sceneFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) log::error("OpenGL: scene framebuffer incomplete");
        setHandles(kSceneSlot, scene_, clampSampler_, false);
        gl::CreateTextures(GL_TEXTURE_2D, 1, &mirror_.texture);
        gl::TextureStorage2D(mirror_.texture, 1, GL_RGBA16F, GLsizei(w), GLsizei(h));
        gl::CreateTextures(GL_TEXTURE_2D, 1, &mirrorGbuf_.texture);
        gl::TextureStorage2D(mirrorGbuf_.texture, 1, GL_RGBA16F, GLsizei(w), GLsizei(h));
        gl::CreateTextures(GL_TEXTURE_2D, 1, &mirrorDepth_);
        gl::TextureStorage2D(mirrorDepth_, 1, GL_DEPTH_COMPONENT32F, GLsizei(w), GLsizei(h));
        gl::CreateFramebuffers(1, &mirrorFbo_);
        gl::NamedFramebufferTexture(mirrorFbo_, GL_COLOR_ATTACHMENT0, mirror_.texture, 0);
        gl::NamedFramebufferTexture(mirrorFbo_, GL_COLOR_ATTACHMENT1, mirrorGbuf_.texture, 0);
        gl::NamedFramebufferTexture(mirrorFbo_, GL_DEPTH_ATTACHMENT, mirrorDepth_, 0);
        gl::NamedFramebufferDrawBuffers(mirrorFbo_, 2, bufs);
        setHandles(kMirrorSlot, mirror_, clampSampler_, false);
        depthTex_.texture = sceneDepth_;
        setHandles(kDepthSlot, depthTex_, clampSampler_, false);
        ao_ = makePostTarget(kAoSlot, GL_R8, std::max(1u, w / 2), std::max(1u, h / 2));
        aoBlur_ = makePostTarget(kAoBlurSlot, GL_R8, std::max(1u, w / 2), std::max(1u, h / 2));
        ssr_ = makePostTarget(kSsrSlot, GL_RGBA16F, std::max(1u, w / 2), std::max(1u, h / 2));
        for (uint32_t i = 0; i < 2; ++i) adapt_[i] = makePostTarget(kAdaptSlot + i, GL_RGBA16F, 1, 1);
        adaptValid_ = false;
        for (uint32_t i = 0; i < 2; ++i) taa_[i] = makePostTarget(kTaaSlot + i, GL_RGBA16F, w, h);
        for (uint32_t i = 0; i < kBloomLevels; ++i)
            bloom_[i] = makePostTarget(kBloomSlot + i, GL_RGBA16F, std::max(1u, w >> (i + 1)), std::max(1u, h >> (i + 1)));
        targetW_ = w;
        targetH_ = h;
    }
    template <typename T> void push(const T& data) { gl::NamedBufferSubData(push_, 0, sizeof data, &data); }
    void drawMeshes(const std::vector<MeshDraw>& draws, const glm::mat4& viewProj, bool shadow, GLuint mesh, GLuint skinned);
    void shadowPasses(GLuint fbo, uint32_t size, const std::vector<ShadowPass>& passes);

    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    bool imgui_ = false;
    GLuint repeatSampler_ = 0, clampSampler_ = 0, compareSampler_ = 0;
    GLuint mirrorFrame_ = 0; // the mirror's pass's frame data
    GLuint textureTable_ = 0, shadowTable_ = 0, draws_ = 0, joints_ = 0, frame_ = 0, lights_ = 0, views_ = 0, cells_ = 0, indices_ = 0;
    GLuint push_ = 0, empty_ = 0, vao_ = 0;
    GLuint meshProgram_ = 0, skinnedProgram_ = 0, shadowMeshProgram_ = 0, shadowSkinnedProgram_ = 0, boxProgram_ = 0, tonemapProgram_ = 0, skyProgram_ = 0, hzbProgram_ = 0;
    GLuint ssaoProgram_ = 0, ssaoBlurProgram_ = 0, bloomDownProgram_ = 0, bloomUpProgram_ = 0, taaProgram_ = 0, ssrProgram_ = 0, adaptProgram_ = 0, volumetricProgram_ = 0, spriteProgram_ = 0;
    uint32_t adaptCur_ = 0;
    bool adaptValid_ = false;
    uint32_t taaCur_ = 0;
    uint64_t frameNumber_ = 0;
    std::vector<std::pair<uint64_t, Texture>> retiredTextures_;
    uint32_t shadowMapSize_ = kShadowSize;
    // Post-processing targets: a texture in the table and a framebuffer to draw into it.
    struct PostTarget {
        Texture tex;
        GLuint fbo = 0;
        uint32_t w = 0, h = 0;
    };
    PostTarget ao_, aoBlur_, bloom_[kBloomLevels], taa_[2], ssr_, adapt_[2];
    PostTarget makePostTarget(TextureId slot, GLenum format, uint32_t w, uint32_t h) {
        PostTarget t;
        t.w = w, t.h = h;
        gl::CreateTextures(GL_TEXTURE_2D, 1, &t.tex.texture);
        gl::TextureStorage2D(t.tex.texture, 1, format, GLsizei(w), GLsizei(h));
        gl::CreateFramebuffers(1, &t.fbo);
        gl::NamedFramebufferTexture(t.fbo, GL_COLOR_ATTACHMENT0, t.tex.texture, 0);
        setHandles(slot, t.tex, clampSampler_, false);
        return t;
    }
    void releasePostTarget(PostTarget& t) {
        releaseTexture(t.tex);
        if (t.fbo) gl::DeleteFramebuffers(1, &t.fbo);
        t = {};
    }
    // Occlusion grid: drawn, read into a pixel-pack buffer, picked up once its fence passes.
    static constexpr int kHzbRing = 3;
    GLuint hzbTex_ = 0, hzbFbo_ = 0, hzbPbo_[kHzbRing] = {};
    GLsync hzbFence_[kHzbRing] = {};
    uint64_t hzbTags_[kHzbRing] = {};
    int hzbNext_ = 0;
    uint64_t hzbTaken_ = 0;
    HzbReadback hzbLatest_;
    Texture depthTex_; // the scene depth's handle (texture owned by sceneDepth_)
    Texture sunShadow_, atlas_, scene_, gbuf_;
    // Mirrors: the reflected scene (and its G-buffer and depth), the scene's size.
    Texture mirror_, mirrorGbuf_;
    GLuint mirrorDepth_ = 0, mirrorFbo_ = 0;
    bool mirrors() const override { return true; }
    GLuint sunShadowFbo_ = 0, atlasFbo_ = 0, sceneFbo_ = 0, sceneDepth_ = 0;
    uint32_t targetW_ = 0, targetH_ = 0;
    std::map<TextureId, Texture> textures_;
    std::vector<Model> models_;
};

void OpenGLBackend::drawMeshes(const std::vector<MeshDraw>& draws, const glm::mat4& viewProj, bool shadow, GLuint mesh, GLuint skinned) {
    GLuint bound = 0;
    uint32_t boundModel = UINT32_MAX;
    ModelPush mp{};
    mp.viewProj = viewProj;
    for (const MeshDraw& d : draws) {
        const Model& m = models_[d.model];
        if (!m.indices) continue;
        const bool isSkinned = d.firstJoint != UINT32_MAX;
        const GLuint want = isSkinned ? skinned : mesh;
        if (want != bound) {
            gl::UseProgram(want);
            bound = want;
        }
        if (d.model != boundModel) {
            gl::VertexArrayElementBuffer(vao_, m.indices);
            gl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, B_VERTICES, m.vertices);
            gl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, B_SKIN, m.skin ? m.skin : empty_);
            boundModel = d.model;
        }
        mp.textures = d.textures;
        mp.tint = d.tint;
        mp.material = shadow ? 0 : d.material;
        mp.drawIndex = d.draw;
        mp.firstJoint = isSkinned ? d.firstJoint : 0;
        push(mp);
        gl::DrawElements(GL_TRIANGLES, GLsizei(d.indexCount), GL_UNSIGNED_INT, reinterpret_cast<const void*>(uintptr_t(d.firstIndex) * 4));
    }
}

void OpenGLBackend::shadowPasses(GLuint fbo, uint32_t size, const std::vector<ShadowPass>& passes) {
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl::DepthMask(GL_TRUE);
    gl::Disable(GL_SCISSOR_TEST);
    const GLfloat one = 1.0f;
    gl::ClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &one);
    gl::Enable(GL_SCISSOR_TEST);
    gl::Enable(GL_POLYGON_OFFSET_FILL);
    gl::PolygonOffset(2.5f, 2.0f); // same as Vulkan's depth bias (slope, constant)
    for (const ShadowPass& pass : passes) {
        gl::Viewport(pass.rect.x, pass.rect.y, pass.rect.z, pass.rect.w);
        gl::Scissor(pass.rect.x, pass.rect.y, pass.rect.z, pass.rect.w);
        drawMeshes(*pass.draws, pass.viewProj, true, shadowMeshProgram_, shadowSkinnedProgram_);
    }
    gl::Disable(GL_POLYGON_OFFSET_FILL);
    gl::Disable(GL_SCISSOR_TEST);
    (void)size;
}

void OpenGLBackend::render(FramePacket& p) {
    ++frameNumber_;
    ensureTargets(p.width, p.height); // the scene's size (render scale), not the window's
    std::erase_if(retiredTextures_, [&](auto& r) {
        if (frameNumber_ - r.first <= 3) return false;
        releaseTexture(r.second);
        return true;
    });
    gl::ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE); // ImGui may have touched state; ours is set fresh each frame
    gl::FrontFace(GL_CW);
    auto upload = [](GLuint buffer, const void* data, size_t size) {
        if (size) gl::NamedBufferSubData(buffer, 0, GLsizeiptr(size), data);
    };
    upload(draws_, p.draws.data(), p.draws.size() * sizeof(gpu::DrawData));
    upload(joints_, p.joints.data(), p.joints.size() * sizeof(glm::mat4));
    upload(frame_, &p.frame, sizeof p.frame);
    upload(lights_, p.lights.data(), p.lights.size() * sizeof(gpu::Light));
    upload(views_, p.views.data(), p.views.size() * sizeof(gpu::ShadowView));
    upload(cells_, p.clusters->cells.data(), p.clusters->cells.size() * sizeof(glm::uvec2));
    upload(indices_, p.clusters->indices.data(), p.clusters->indices.size() * sizeof(uint32_t));
    const std::pair<Binding, GLuint> bindings[] = {{B_DRAWS, draws_}, {B_JOINTS, joints_}, {B_FRAME, frame_}, {B_LIGHTS, lights_},
                                                   {B_CELLS, cells_}, {B_INDICES, indices_}, {B_VIEWS, views_},
                                                   {B_TEXTURES, textureTable_}, {B_SHADOW_TEXTURES, shadowTable_}};
    for (auto [binding, buffer] : bindings) gl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buffer);
    gl::BindBufferBase(GL_UNIFORM_BUFFER, 0, push_);
    gl::BindVertexArray(vao_);
    gl::Disable(GL_CULL_FACE);
    gl::Disable(GL_BLEND);
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthFunc(GL_LESS);

    if (p.shadowSize != shadowMapSize_) { // r_shadowres changed
        releaseTexture(sunShadow_);
        gl::DeleteFramebuffers(1, &sunShadowFbo_);
        sunShadow_ = depthTarget(p.shadowSize, sunShadowFbo_);
        setHandles(kSunShadowSlot, sunShadow_, clampSampler_, true);
        shadowMapSize_ = p.shadowSize;
    }
    shadowPasses(sunShadowFbo_, p.shadowSize, p.sunPasses); // always: clears it when there's no sun
    if (!p.atlasPasses.empty()) shadowPasses(atlasFbo_, kAtlasSize, p.atlasPasses);

    // ── Mirror: the scene from the reflected eye, cut at the mirror, with its own frame data ──
    if (p.mirror) {
        gpu::FrameData mf = p.mirrorFrame;
        mf.gi = mf.giOrigin = glm::vec4(0.0f); // (no GI probes here)
        upload(mirrorFrame_, &mf, sizeof mf);
        gl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, B_FRAME, mirrorFrame_);
        gl::BindFramebuffer(GL_FRAMEBUFFER, mirrorFbo_);
        gl::Viewport(0, 0, GLsizei(p.width), GLsizei(p.height));
        gl::DepthMask(GL_TRUE);
        const GLfloat mclear[4] = {p.clearColor.r, p.clearColor.g, p.clearColor.b, 1.0f}, mone = 1.0f;
        gl::ClearNamedFramebufferfv(mirrorFbo_, GL_COLOR, 0, mclear);
        gl::ClearNamedFramebufferfv(mirrorFbo_, GL_DEPTH, 0, &mone);
        gl::Enable(GL_SCISSOR_TEST); // only where the mirror is on screen
        gl::Scissor(p.mirrorRect.x, p.mirrorRect.y, p.mirrorRect.z, p.mirrorRect.w);
        drawMeshes(p.mirrorOpaque, p.mirrorViewProj, false, meshProgram_, skinnedProgram_);
        if (p.frame.sky.x > 0.5f) {
            gl::UseProgram(skyProgram_);
            push(gpu::SkyPush{p.mirrorInvViewProj, p.frame.sunDir, p.frame.sunColor, p.frame.sky, glm::vec4(p.skyCloud, p.skyPhotoSunZ, p.frame.weather.z, p.frame.weather.w)});
            gl::DepthFunc(GL_LEQUAL);
            gl::DepthMask(GL_FALSE);
            gl::DrawArrays(GL_TRIANGLES, 0, 3);
            gl::DepthMask(GL_TRUE);
            gl::DepthFunc(GL_LESS);
        }
        if (!p.mirrorTranslucent.empty()) {
            gl::Enable(GL_BLEND);
            gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            gl::DepthMask(GL_FALSE);
            drawMeshes(p.mirrorTranslucent, p.mirrorViewProj, false, meshProgram_, skinnedProgram_);
            gl::DepthMask(GL_TRUE);
            gl::Disable(GL_BLEND);
        }
        gl::Disable(GL_SCISSOR_TEST);
        gl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, B_FRAME, frame_);
    }

    // ── Scene pass (HDR) ──
    gl::BindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    gl::Viewport(0, 0, GLsizei(p.width), GLsizei(p.height));
    gl::DepthMask(GL_TRUE);
    const GLfloat clear[4] = {p.clearColor.r, p.clearColor.g, p.clearColor.b, 1.0f}, one = 1.0f;
    const GLfloat noReflection[4] = {0.5f, 0.5f, 1.0f, 0.0f};
    gl::ClearNamedFramebufferfv(sceneFbo_, GL_COLOR, 0, clear);
    gl::ClearNamedFramebufferfv(sceneFbo_, GL_COLOR, 1, noReflection);
    gl::ClearNamedFramebufferfv(sceneFbo_, GL_DEPTH, 0, &one);
    // Only opaque meshes write the G-buffer (attachment 1).
    auto gbufWrites = [](bool on) { gl::ColorMaski(1, on, on, on, on); };
    gbufWrites(false);
    gl::UseProgram(boxProgram_);
    for (const gpu::Box& b : p.boxes) {
        push(BoxPush{p.viewProj, b});
        gl::DrawArrays(GL_TRIANGLES, 0, 36);
    }
    gbufWrites(true);
    drawMeshes(p.opaque, p.viewProj, false, meshProgram_, skinnedProgram_);
    gbufWrites(false);
    if (p.frame.sky.x > 0.5f) { // where nothing was drawn
        gl::UseProgram(skyProgram_);
        push(gpu::SkyPush{p.invViewProj, p.frame.sunDir, p.frame.sunColor, p.frame.sky, glm::vec4(p.skyCloud, p.skyPhotoSunZ, p.frame.weather.z, p.frame.weather.w)});
        gl::DepthFunc(GL_LEQUAL);
        gl::DepthMask(GL_FALSE);
        gl::DrawArrays(GL_TRIANGLES, 0, 3);
        gl::DepthMask(GL_TRUE);
        gl::DepthFunc(GL_LESS);
    }
    if (!p.translucent.empty()) { // back to front, blended, no depth writes
        gl::Enable(GL_BLEND);
        gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        gl::DepthMask(GL_FALSE);
        drawMeshes(p.translucent, p.viewProj, false, meshProgram_, skinnedProgram_);
        gl::DepthMask(GL_TRUE);
        gl::Disable(GL_BLEND);
    }
    if (!p.sprites.empty() || !p.glowSprites.empty()) { // particles: depth-tested, not written; blended, then added on
        gbufWrites(false);
        gl::UseProgram(spriteProgram_);
        gl::Enable(GL_BLEND);
        gl::DepthMask(GL_FALSE);
        gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        for (const gpu::Sprite& sp : p.sprites) push(SpritePush{p.viewProj, sp}), gl::DrawArrays(GL_TRIANGLES, 0, 6);
        gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
        for (const gpu::Sprite& sp : p.glowSprites) push(SpritePush{p.viewProj, sp}), gl::DrawArrays(GL_TRIANGLES, 0, 6);
        gl::DepthMask(GL_TRUE);
        gl::Disable(GL_BLEND);
        gbufWrites(true);
    }
    gbufWrites(true);

    // ── Occlusion grid: the depth (only opaque things write it) shrunk and read back ──
    if (p.hzbTag) {
        gl::BindFramebuffer(GL_FRAMEBUFFER, hzbFbo_);
        gl::Viewport(0, 0, GLsizei(kHzbWidth), GLsizei(kHzbHeight));
        gl::Disable(GL_DEPTH_TEST);
        gl::UseProgram(hzbProgram_);
        push(HzbPush{kDepthSlot, p.width, p.height, kHzbWidth, kHzbHeight, {}});
        gl::DrawArrays(GL_TRIANGLES, 0, 3);
        const int i = hzbNext_;
        hzbNext_ = (hzbNext_ + 1) % kHzbRing;
        collectHzb(i, true); // the CPU ran ahead (no vsync): take that grid before the slot is reused
        gl::BindBuffer(GL_PIXEL_PACK_BUFFER, hzbPbo_[i]);
        gl::ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl::ReadPixels(0, 0, GLsizei(kHzbWidth), GLsizei(kHzbHeight), GL_RED, GL_FLOAT, nullptr);
        gl::BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        hzbFence_[i] = gl::FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        hzbTags_[i] = p.hzbTag;
        gl::Enable(GL_DEPTH_TEST);
    }

    // ── Post: ambient occlusion, bloom ──
    gl::Disable(GL_DEPTH_TEST);
    auto pass = [&](const PostTarget& t, GLuint prog, const auto& data, bool additive) {
        gl::BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        gl::Viewport(0, 0, GLsizei(t.w), GLsizei(t.h));
        gl::UseProgram(prog);
        push(data);
        if (additive) {
            gl::Enable(GL_BLEND);
            gl::BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        }
        gl::DrawArrays(GL_TRIANGLES, 0, 3);
        if (additive) gl::Disable(GL_BLEND);
    };
    const bool ao = p.post.aoStrength > 0.0f, bloom = p.post.bloomStrength > 0.0f, reflections = p.post.ssr > 0.0f,
               volumetric = p.frame.volumetric.x > 0.0f, // the sun and the dynamic lights
               ssr = reflections || volumetric; // (the half-size target both go to)
    if (reflections) pass(ssr_, ssrProgram_, SsrPush{kDepthSlot, kGbufSlot, kSceneSlot, p.width, p.height, ssr_.w, ssr_.h, p.post.ssr, p.post.proj}, false);
    if (volumetric) // sunlight in the air, onto the same target
        pass(ssr_, volumetricProgram_, VolumetricPush{kDepthSlot, p.width, p.height, ssr_.w, ssr_.h, {}, p.post.proj}, reflections);
    TextureId resolved = kSceneSlot;
    if (p.post.taa) {
        taaCur_ ^= 1u;
        pass(taa_[taaCur_], taaProgram_,
             TaaPush{p.post.reproject, kSceneSlot, kTaaSlot + (taaCur_ ^ 1u), kDepthSlot, p.width, p.height, p.post.taaReset ? 1u : 0u, ssr ? kSsrSlot : 0u, 0}, false);
        resolved = kTaaSlot + taaCur_;
    }
    if (ao) {
        pass(ao_, ssaoProgram_, SsaoPush{kDepthSlot, p.width, p.height, ao_.w, ao_.h, p.post.aoRadius, p.post.aoStrength, 0, p.post.proj}, false);
        pass(aoBlur_, ssaoBlurProgram_, SsaoBlurPush{kAoSlot, kDepthSlot, p.width, p.height, aoBlur_.w, aoBlur_.h, 0, 0, p.post.proj}, false);
    }
    if (bloom) {
        for (uint32_t i = 0; i < kBloomLevels; ++i) {
            const uint32_t sw = i == 0 ? p.width : bloom_[i - 1].w, sh = i == 0 ? p.height : bloom_[i - 1].h;
            pass(bloom_[i], bloomDownProgram_, BloomPush{i == 0 ? resolved : kBloomSlot + i - 1, sw, sh, bloom_[i].w, bloom_[i].h, i == 0 ? 1u : 0u, {}}, false);
        }
        const float radius = 1.0f;
        uint32_t radiusBits;
        std::memcpy(&radiusBits, &radius, 4);
        for (uint32_t i = kBloomLevels - 1; i > 0; --i)
            pass(bloom_[i - 1], bloomUpProgram_, BloomPush{kBloomSlot + i, bloom_[i].w, bloom_[i].h, bloom_[i - 1].w, bloom_[i - 1].h, radiusBits, {}}, true);
    }

    if (p.post.adapt) { // eye adaptation from the smallest bloom level
        adaptCur_ ^= 1u;
        pass(adapt_[adaptCur_], adaptProgram_,
             AdaptPush{kBloomSlot + kBloomLevels - 1, p.post.adaptReset || !adaptValid_ ? 0u : kAdaptSlot + (adaptCur_ ^ 1u), p.post.adaptDt, 1.5f,
                       p.post.adaptKey, 0.6f, 1.8f, 0},
             false);
        adaptValid_ = true;
    }

    // ── Tonemap to the window, then UI ──
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::Viewport(0, 0, GLsizei(p.outWidth), GLsizei(p.outHeight));
    gl::Disable(GL_DEPTH_TEST);
    gl::UseProgram(tonemapProgram_);
    push(TonemapPush{resolved, p.exposure, ao ? kAoBlurSlot : 0u, bloom ? kBloomSlot : 0u, p.post.bloomStrength, p.post.saturation, p.post.vignette, p.post.taa ? p.post.sharpen : 0.0f, kDepthSlot, {ssr && !p.post.taa ? kSsrSlot : 0u, p.post.adapt ? kAdaptSlot + adaptCur_ : 0u, 0}, p.post.shafts, p.post.sunColor});
    gl::DrawArrays(GL_TRIANGLES, 0, 3);
    gl::BindVertexArray(0);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (!p.screenshot.empty()) {
        std::vector<uint8_t> rows(size_t(p.outWidth) * p.outHeight * 4), px(rows.size());
        gl::ReadBuffer(GL_BACK);
        gl::ReadPixels(0, 0, GLsizei(p.outWidth), GLsizei(p.outHeight), GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        const size_t stride = size_t(p.outWidth) * 4; // the window's row 0 is its bottom
        for (uint32_t y = 0; y < p.outHeight; ++y) std::memcpy(&px[y * stride], &rows[(p.outHeight - 1 - y) * stride], stride);
        saveScreenshot(p.screenshot, std::move(px), p.outWidth, p.outHeight);
    }
    SDL_GL_SwapWindow(window_);
}
} // namespace

std::unique_ptr<RenderBackend> createOpenGLBackend() { return std::make_unique<OpenGLBackend>(); }

} // namespace keel
