// Textures and models: loaded through the VFS, described here, uploaded by the backend.
#include "asset/kmdl.h"
#include "core/log.h"
#include "fs/vfs.h"
#include "render/renderer.h"

#include <yapf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace keel {

TextureId Renderer::newTexture(uint32_t w, uint32_t h, bool srgb, const std::vector<std::vector<uint8_t>>& mips, TexFormat format) {
    if (nextTexture_ >= kMaxTextures) return kMissingTexture;
    const TextureId id = nextTexture_++;
    backend_->createTexture(id, w, h, srgb, mips, format);
    textureSizes_[id] = glm::ivec2(int(w), int(h));
    return id;
}

// Why YAPF couldn't read a file, in words.
static const char* yapfReason(int err) {
    switch (err) {
    case YAPF_ERR_INVALID: return "not a YAPF file";
    case YAPF_ERR_IO: return "couldn't be read";
    case YAPF_ERR_OOM: return "out of memory";
    case YAPF_ERR_CORRUPT: return "damaged";
    case YAPF_ERR_UNSUPPORTED: return "uses a feature this YAPF can't decode (a newer version?)";
    case YAPF_ERR_CHECKSUM: return "damaged (checksum mismatch)";
    case YAPF_ERR_TRUNCATED: return "truncated";
    default: return "unknown error";
    }
}

// A .yapf file (version 1 or 2) as mips for the GPU: BC7 blocks go up as they are when the GPU
// takes them; anything else (other block formats, 16-bit, half, float, gray) is decoded to RGBA8
// by YAPF (yapf_tex_to_rgba8).
bool Renderer::decodeTexture(const std::string& key, uint32_t& w, uint32_t& h, bool& srgb, std::vector<std::vector<uint8_t>>& mips,
                             TexFormat& format) {
    auto data = fs::read(key);
    if (!data) {
        log::warn("Texture not found: {}", key);
        return false;
    }
    int err = 0;
    yapf_texture_t* tex = yapf_tex_load_memory(data->data(), data->size(), 0, 0, &err);
    if (!tex) {
        log::warn("{}: can't load: {} (YAPF error {})", key, yapfReason(err), err);
        return false;
    }
    w = tex->width, h = tex->height, srgb = tex->flags & YAPF_FLAG_SRGB;
    // Float and half textures are linear: yapf_tex_to_rgba8 sRGB-encodes them.
    if (tex->payload != YAPF_PAYLOAD_BLOCKS && (tex->sample == YAPF_SAMPLE_F16 || tex->sample == YAPF_SAMPLE_F32)) srgb = true;
    const bool upload = tex->payload == YAPF_PAYLOAD_BLOCKS && tex->format == YAPF_GPU_BC7 && backend_->blockCompression();
    format = upload ? TexFormat::BC7 : TexFormat::RGBA8;
    mips.clear();
    for (uint32_t level = 0; level < tex->mip_levels; ++level) {
        uint32_t lw = 0, lh = 0, ld = 0;
        yapf_tex_mip_size(tex, level, &lw, &lh, &ld);
        const uint32_t surface = yapf_tex_surface(tex, 0, 0, 0, level);
        if (upload) {
            const uint8_t* src = static_cast<const uint8_t*>(tex->data[surface]);
            mips.emplace_back(src, src + yapf_tex_surface_bytes(tex, level));
            continue;
        }
        std::vector<uint8_t> rgba(size_t(lw) * lh * 4);
        if (const int e = yapf_tex_to_rgba8(tex, surface, 0, rgba.data()); e != YAPF_OK) {
            log::warn("{}: mip {} can't be decoded: {}", key, level, yapfReason(e));
            yapf_tex_free(tex);
            return false;
        }
        mips.push_back(std::move(rgba));
    }
    yapf_tex_free(tex);
    return true;
}

TextureId Renderer::createTexture(std::string_view name, uint32_t w, uint32_t h, const uint8_t* rgba) {
    // A full mip chain (box filtered), so a big picture seen small doesn't shimmer.
    std::vector<std::vector<uint8_t>> mips(1, std::vector<uint8_t>(rgba, rgba + size_t(w) * h * 4));
    for (uint32_t mw = w, mh = h; mw > 1 || mh > 1;) {
        const uint32_t nw = std::max(mw / 2, 1u), nh = std::max(mh / 2, 1u);
        const std::vector<uint8_t>& src = mips.back();
        std::vector<uint8_t> dst(size_t(nw) * nh * 4);
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const uint32_t x0 = std::min(x * 2, mw - 1), x1 = std::min(x * 2 + 1, mw - 1), y0 = std::min(y * 2, mh - 1), y1 = std::min(y * 2 + 1, mh - 1);
                    dst[(size_t(y) * nw + x) * 4 + c] = uint8_t((src[(size_t(y0) * mw + x0) * 4 + c] + src[(size_t(y0) * mw + x1) * 4 + c] +
                                                                 src[(size_t(y1) * mw + x0) * 4 + c] + src[(size_t(y1) * mw + x1) * 4 + c] + 2) / 4);
                }
        mips.push_back(std::move(dst));
        mw = nw, mh = nh;
    }
    const std::string key(name);
    if (auto it = textureCache_.find(key); it != textureCache_.end() && it->second >= kFirstTextureSlot) {
        backend_->createTexture(it->second, w, h, true, mips, TexFormat::RGBA8);
        textureSizes_[it->second] = glm::ivec2(int(w), int(h));
        return it->second;
    }
    return textureCache_[key] = newTexture(w, h, true, mips, TexFormat::RGBA8);
}

TextureId Renderer::loadTexture(std::string_view path) {
    if (path.empty()) return kWhiteTexture;
    const std::string key(path);
    if (auto it = textureCache_.find(key); it != textureCache_.end()) return it->second;
    TextureId id = kMissingTexture;
    uint32_t w = 0, h = 0;
    bool srgb = false;
    std::vector<std::vector<uint8_t>> mips;
    TexFormat format;
    if (decodeTexture(key, w, h, srgb, mips, format)) id = newTexture(w, h, srgb, mips, format);
    if (auto t = fs::mtime(key)) fileTimes_[key] = *t;
    textureCache_[key] = id;
    return id;
}

std::vector<std::string> Renderer::reloadChanged() {
    std::vector<std::string> changed;
    auto stale = [&](const std::string& path) {
        const auto t = fs::mtime(path);
        if (!t) return false;
        auto [it, isNew] = fileTimes_.try_emplace(path, *t);
        if (isNew || it->second == *t) return false;
        it->second = *t;
        return true;
    };
    for (auto& [path, id] : textureCache_) {
        if (id < kFirstTextureSlot || !stale(path)) continue;
        uint32_t w = 0, h = 0;
        bool srgb = false;
        std::vector<std::vector<uint8_t>> mips;
        TexFormat format;
        if (!decodeTexture(path, w, h, srgb, mips, format)) continue;
        backend_->createTexture(id, w, h, srgb, mips, format); // the same slot: everything using it updates
        textureSizes_[id] = glm::ivec2(int(w), int(h));
        changed.push_back(path);
    }
    std::vector<std::pair<std::string, ModelId>> models(modelCache_.begin(), modelCache_.end());
    for (auto& [path, id] : models) {
        if (!id.valid() || path.starts_with("map:") || !stale(path)) continue;
        auto data = fs::read(path);
        auto kmdl = data ? readKmdl(*data) : std::nullopt;
        if (!kmdl) continue;
        updateModel(path, *kmdl);
        changed.push_back(path);
    }
    return changed;
}

Renderer::Model Renderer::describeModel(const KmdlModel& kmdl) {
    Model m;
    if (kmdl.vertices.empty() || kmdl.indices.empty()) return m;
    m.mins = glm::vec3(kmdl.header.boundsMin[0], kmdl.header.boundsMin[1], kmdl.header.boundsMin[2]);
    m.maxs = glm::vec3(kmdl.header.boundsMax[0], kmdl.header.boundsMax[1], kmdl.header.boundsMax[2]);
    m.skinned = !kmdl.skin.empty();
    for (const KmdlSubmesh& s : kmdl.submeshes) {
        const KmdlMaterial& mat = kmdl.materials[s.material];
        const std::string tex(mat.texture, strnlen(mat.texture, sizeof mat.texture));
        const std::string lm(mat.lightmap, strnlen(mat.lightmap, sizeof mat.lightmap));
        // A material map next to the texture ("wall.yapf" → "wall_n.yapf"), when there is one.
        TextureId material = 0;
        if (const size_t dot = tex.rfind('.'); !tex.empty() && dot != std::string::npos)
            if (const std::string n = tex.substr(0, dot) + "_n" + tex.substr(dot); fs::exists(n)) material = loadTexture(n);
        TextureId skyVis = 0;
        if (const size_t dot = lm.rfind('.'); !lm.empty() && dot != std::string::npos)
            if (const std::string v = lm.substr(0, dot) + "_sky" + lm.substr(dot); fs::exists(v)) skyVis = loadTexture(v);
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (uint32_t i = s.firstIndex; i < s.firstIndex + s.indexCount && i < kmdl.indices.size(); ++i) {
            const float* p = kmdl.vertices[kmdl.indices[i]].position;
            lo = glm::min(lo, glm::vec3(p[0], p[1], p[2]));
            hi = glm::max(hi, glm::vec3(p[0], p[1], p[2]));
        }
        if (m.skinned) lo = m.mins - (m.maxs - m.mins) * 0.5f, hi = m.maxs + (m.maxs - m.mins) * 0.5f; // animated: room to move
        m.submeshes.push_back({s.firstIndex, s.indexCount, loadTexture(tex), lm.empty() ? 0u : loadTexture(lm),
                               glm::vec4(mat.baseColor[0], mat.baseColor[1], mat.baseColor[2], mat.baseColor[3]), material, skyVis, lo, hi});
        // A mirror: its plane from its first triangle, facing the way its vertices' normals do.
        if (tex.find("mirror") != std::string::npos && s.indexCount >= 3 && !m.skinned) {
            const KmdlVertex& va = kmdl.vertices[kmdl.indices[s.firstIndex]];
            const glm::vec3 a(va.position[0], va.position[1], va.position[2]);
            const float* pb = kmdl.vertices[kmdl.indices[s.firstIndex + 1]].position;
            const float* pc = kmdl.vertices[kmdl.indices[s.firstIndex + 2]].position;
            glm::vec3 n = glm::cross(glm::vec3(pb[0], pb[1], pb[2]) - a, glm::vec3(pc[0], pc[1], pc[2]) - a);
            if (glm::length(n) > 1e-6f) {
                n = glm::normalize(n);
                if (glm::dot(n, glm::vec3(va.normal[0], va.normal[1], va.normal[2])) < 0.0f) n = -n;
                m.submeshes.back().mirror = true;
                m.submeshes.back().plane = glm::vec4(n, glm::dot(n, a));
            }
        }
    }
    return m;
}

namespace {
// Mirrors reflect in one plane each, but a model keeps all the faces of a texture in one
// submesh (several mirrors, or a mirror brush's faces). Each mirror submesh with more than one
// plane is split into one per plane (its triangles sorted so each runs on). False: nothing to do.
bool splitMirrors(const KmdlModel& in, KmdlModel& out) {
    auto isMirror = [&](const KmdlSubmesh& s) {
        const KmdlMaterial& m = in.materials[s.material];
        return std::string(m.texture, strnlen(m.texture, sizeof m.texture)).find("mirror") != std::string::npos;
    };
    auto planeOf = [&](uint32_t first) {
        const float* a = in.vertices[in.indices[first]].position;
        const float* b = in.vertices[in.indices[first + 1]].position;
        const float* c = in.vertices[in.indices[first + 2]].position;
        const glm::vec3 pa(a[0], a[1], a[2]);
        glm::vec3 n = glm::cross(glm::vec3(b[0], b[1], b[2]) - pa, glm::vec3(c[0], c[1], c[2]) - pa);
        if (glm::length(n) < 1e-6f) return glm::ivec4(0);
        n = glm::normalize(n);
        const float* vn = in.vertices[in.indices[first]].normal;
        if (glm::dot(n, glm::vec3(vn[0], vn[1], vn[2])) < 0.0f) n = -n;
        return glm::ivec4(glm::round(n * 1000.0f), int(std::round(glm::dot(n, pa) * 2.0f))); // within 0.001 / half a unit
    };
    bool any = false;
    for (const KmdlSubmesh& s : in.submeshes) {
        if (!isMirror(s) || s.indexCount <= 3) continue;
        const glm::ivec4 first = planeOf(s.firstIndex);
        for (uint32_t t = s.firstIndex + 3; t + 2 < s.firstIndex + s.indexCount && !any; t += 3) any = planeOf(t) != first;
        if (any) break;
    }
    if (!any) return false;
    out = in;
    out.submeshes.clear();
    for (const KmdlSubmesh& s : in.submeshes) {
        if (!isMirror(s)) {
            out.submeshes.push_back(s);
            continue;
        }
        std::vector<std::pair<glm::ivec4, std::vector<uint32_t>>> groups; // plane → its triangles' indices
        for (uint32_t t = s.firstIndex; t + 2 < s.firstIndex + s.indexCount; t += 3) {
            const glm::ivec4 pl = planeOf(t);
            auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == pl; });
            if (it == groups.end()) it = groups.insert(groups.end(), {pl, {}});
            it->second.insert(it->second.end(), {in.indices[t], in.indices[t + 1], in.indices[t + 2]});
        }
        uint32_t at = s.firstIndex;
        for (const auto& [pl, idx] : groups) {
            std::copy(idx.begin(), idx.end(), out.indices.begin() + at);
            out.submeshes.push_back({at, uint32_t(idx.size()), s.material});
            at += uint32_t(idx.size());
        }
    }
    return true;
}
} // namespace

ModelId Renderer::createModel(std::string_view name, const KmdlModel& source) {
    KmdlModel split;
    const KmdlModel& kmdl = splitMirrors(source, split) ? split : source;
    const std::string key(name);
    if (auto it = modelCache_.find(key); it != modelCache_.end()) return it->second;
    ModelId id;
    if (!kmdl.vertices.empty() && !kmdl.indices.empty()) {
        id.index = uint32_t(models_.size());
        models_.push_back(describeModel(kmdl));
        backend_->uploadModel(id.index, kmdl);
    }
    modelCache_[key] = id;
    return id;
}

ModelId Renderer::updateModel(std::string_view name, const KmdlModel& source) {
    KmdlModel split;
    const KmdlModel& kmdl = splitMirrors(source, split) ? split : source;
    const auto it = modelCache_.find(std::string(name));
    if (it == modelCache_.end() || !it->second.valid()) {
        if (it != modelCache_.end()) modelCache_.erase(it);
        return createModel(name, kmdl);
    }
    models_[it->second.index] = describeModel(kmdl); // may be empty: draws nothing
    backend_->uploadModel(it->second.index, kmdl);
    return it->second;
}

ModelId Renderer::loadModel(std::string_view path) {
    const std::string key(path);
    if (auto it = modelCache_.find(key); it != modelCache_.end()) return it->second;
    if (auto t = fs::mtime(key)) fileTimes_[key] = *t;
    auto data = fs::read(path);
    auto kmdl = data ? readKmdl(*data) : std::nullopt;
    if (!kmdl) {
        log::warn("Model {} {}", key, data ? "is not a valid KMDL file" : "not found");
        return modelCache_[key] = ModelId{};
    }
    return createModel(key, *kmdl);
}

} // namespace keel
