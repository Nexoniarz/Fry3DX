#pragma once
// KMDL: the engine's runtime model format, written by assetc from glTF. Laid out so
// the loader is a few memcpys:
//   KmdlHeader
//   KmdlMaterial × materialCount
//   KmdlSubmesh  × submeshCount
//   KmdlVertex   × vertexCount
//   uint32_t     × indexCount
//   if jointCount > 0:
//     KmdlJoint × jointCount (parents before children)
//     KmdlSkin  × vertexCount
//     animationCount × { KmdlAnimation, then per channel: KmdlChannel,
//                        float times[keyCount], float values[keyCount × 3 or 4] }
//   hullCount × { uint32_t pointCount, float xyz[pointCount × 3] }
// Hulls are the collision model (like Source's VPhysics .phy): convex pieces, model space.
// None = collide as the convex hull of all vertices.
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace keel {

inline constexpr char kKmdlMagic[4] = {'K', 'M', 'D', 'L'};
inline constexpr uint32_t kKmdlVersion = 5;
// KmdlHeader::flags
inline constexpr uint32_t KMDL_PLAYER_SOLID = 1u; // players collide with it whatever its size (doors)

struct KmdlHeader {
    char magic[4];
    uint32_t version;
    uint32_t materialCount, submeshCount, vertexCount, indexCount;
    float boundsMin[3], boundsMax[3];
    uint32_t jointCount, animationCount;
    uint32_t hullCount;
    uint32_t flags;
};

struct KmdlJoint {
    char name[32];
    int32_t parent;          // -1 = root
    float inverseBind[16];   // column-major
    float translation[3], rotation[4], scale[3]; // rest pose, local; rotation xyzw
};

struct KmdlSkin {
    uint8_t joints[4];
    uint8_t weights[4]; // sum to 255
};

enum KmdlChannelPath : uint32_t { KMDL_TRANSLATION = 0, KMDL_ROTATION = 1, KMDL_SCALE = 2 };

struct KmdlAnimationHeader {
    char name[32];
    float duration;
    uint32_t channelCount;
};

struct KmdlChannelHeader {
    uint32_t joint, path, keyCount;
};

struct KmdlChannel {
    KmdlChannelHeader header;
    std::vector<float> times, values; // values: 3 per key (T, S) or 4 (R, xyzw)
};

struct KmdlAnimation {
    KmdlAnimationHeader header;
    std::vector<KmdlChannel> channels;
};

struct KmdlMaterial {
    char texture[120];  // game path to a .yapf, empty = white
    char lightmap[120]; // baked lighting (RGBM .yapf, uses KmdlVertex::lightmapUv); empty = lit at runtime
    float baseColor[4];
};

struct KmdlSubmesh {
    uint32_t firstIndex, indexCount, material;
};

struct KmdlVertex {
    float position[3];
    float normal[3];
    float uv[2];
    float lightmapUv[2];
};

struct KmdlModel {
    KmdlHeader header{};
    std::vector<KmdlMaterial> materials;
    std::vector<KmdlSubmesh> submeshes;
    std::vector<KmdlVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<KmdlJoint> joints;
    std::vector<KmdlSkin> skin; // one per vertex when skinned
    std::vector<KmdlAnimation> animations;
    std::vector<std::vector<float>> hulls; // xyz triples
};

inline std::vector<uint8_t> writeKmdl(KmdlModel m) {
    std::memcpy(m.header.magic, kKmdlMagic, 4);
    m.header.version = kKmdlVersion;
    m.header.materialCount = uint32_t(m.materials.size());
    m.header.submeshCount = uint32_t(m.submeshes.size());
    m.header.vertexCount = uint32_t(m.vertices.size());
    m.header.indexCount = uint32_t(m.indices.size());
    m.header.jointCount = uint32_t(m.joints.size());
    m.header.animationCount = uint32_t(m.animations.size());
    m.header.hullCount = uint32_t(m.hulls.size());
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    put(&m.header, sizeof m.header);
    put(m.materials.data(), m.materials.size() * sizeof(KmdlMaterial));
    put(m.submeshes.data(), m.submeshes.size() * sizeof(KmdlSubmesh));
    put(m.vertices.data(), m.vertices.size() * sizeof(KmdlVertex));
    put(m.indices.data(), m.indices.size() * sizeof(uint32_t));
    if (!m.joints.empty()) {
        put(m.joints.data(), m.joints.size() * sizeof(KmdlJoint));
        put(m.skin.data(), m.skin.size() * sizeof(KmdlSkin));
        for (auto& a : m.animations) {
            a.header.channelCount = uint32_t(a.channels.size());
            put(&a.header, sizeof a.header);
            for (auto& c : a.channels) {
                c.header.keyCount = uint32_t(c.times.size());
                put(&c.header, sizeof c.header);
                put(c.times.data(), c.times.size() * sizeof(float));
                put(c.values.data(), c.values.size() * sizeof(float));
            }
        }
    }
    for (const auto& h : m.hulls) {
        const uint32_t n = uint32_t(h.size() / 3);
        put(&n, 4);
        put(h.data(), size_t(n) * 3 * sizeof(float));
    }
    return out;
}

inline std::optional<KmdlModel> readKmdl(const std::vector<uint8_t>& data) {
    KmdlModel m;
    size_t pos = 0;
    auto take = [&](void* p, size_t n) {
        if (pos + n > data.size()) return false;
        std::memcpy(p, data.data() + pos, n);
        pos += n;
        return true;
    };
    if (!take(&m.header, sizeof m.header) || std::memcmp(m.header.magic, kKmdlMagic, 4) != 0 || m.header.version != kKmdlVersion)
        return std::nullopt;
    m.materials.resize(m.header.materialCount);
    m.submeshes.resize(m.header.submeshCount);
    m.vertices.resize(m.header.vertexCount);
    m.indices.resize(m.header.indexCount);
    if (!take(m.materials.data(), m.materials.size() * sizeof(KmdlMaterial)) ||
        !take(m.submeshes.data(), m.submeshes.size() * sizeof(KmdlSubmesh)) ||
        !take(m.vertices.data(), m.vertices.size() * sizeof(KmdlVertex)) ||
        !take(m.indices.data(), m.indices.size() * sizeof(uint32_t)))
        return std::nullopt;
    for (uint32_t i : m.indices)
        if (i >= m.vertices.size()) return std::nullopt;
    if (m.header.jointCount) {
        if (m.header.jointCount > 255) return std::nullopt;
        m.joints.resize(m.header.jointCount);
        m.skin.resize(m.header.vertexCount);
        if (!take(m.joints.data(), m.joints.size() * sizeof(KmdlJoint)) || !take(m.skin.data(), m.skin.size() * sizeof(KmdlSkin)))
            return std::nullopt;
        for (size_t j = 0; j < m.joints.size(); ++j)
            if (m.joints[j].parent >= int32_t(j)) return std::nullopt; // parents first
        for (const KmdlSkin& sk : m.skin)
            for (uint8_t j : sk.joints)
                if (j >= m.joints.size()) return std::nullopt;
        m.animations.resize(m.header.animationCount);
        for (auto& a : m.animations) {
            if (!take(&a.header, sizeof a.header) || a.header.channelCount > 4096) return std::nullopt;
            a.channels.resize(a.header.channelCount);
            for (auto& c : a.channels) {
                if (!take(&c.header, sizeof c.header) || c.header.joint >= m.joints.size() || c.header.path > 2 || c.header.keyCount > 1'000'000)
                    return std::nullopt;
                c.times.resize(c.header.keyCount);
                c.values.resize(size_t(c.header.keyCount) * (c.header.path == KMDL_ROTATION ? 4 : 3));
                if (!take(c.times.data(), c.times.size() * 4) || !take(c.values.data(), c.values.size() * 4)) return std::nullopt;
            }
        }
    }
    if (m.header.hullCount > 4096) return std::nullopt;
    m.hulls.resize(m.header.hullCount);
    for (auto& h : m.hulls) {
        uint32_t n = 0;
        if (!take(&n, 4) || n > 65536) return std::nullopt;
        h.resize(size_t(n) * 3);
        if (!take(h.data(), h.size() * sizeof(float))) return std::nullopt;
    }
    for (auto& s : m.submeshes)
        if (uint64_t(s.firstIndex) + s.indexCount > m.indices.size() || s.material >= m.materials.size()) return std::nullopt;
    return m;
}

} // namespace keel
