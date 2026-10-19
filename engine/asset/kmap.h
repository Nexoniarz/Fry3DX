#pragma once
// KMAP: a compiled map, written by assetc from a TrenchBroom .map file.
//   KmapHeader
//   render model: a complete KMDL blob (renderModelSize bytes)
//   KmapBrush × brushCount, then glm-compatible float[3] × pointCount
//   entity text (entitySize bytes), Quake style: { "key" "value" ... } ...
//   light probes: float × 19 per probe (ambient cube: 6 × rgb for +X -X +Y -Y +Z -Z, then sun visibility)
//   material names, '\n'-separated (materialTableSize bytes): what KmapBrush::material indexes
#include "asset/kmdl.h"

#include <glm/glm.hpp>

#include <map>
#include <string>

namespace keel {

inline constexpr char kKmapMagic[4] = {'K', 'M', 'A', 'P'};
inline constexpr uint32_t kKmapVersion = 5;

enum BrushContents : uint32_t {
    CONTENTS_SOLID = 1 << 0,
    CONTENTS_PLAYERCLIP = 1 << 1, // blocks players only (tools/clip)
    CONTENTS_TRIGGER = 1 << 2,    // not solid; reports players inside (trigger_* entities)
};

inline constexpr uint32_t kWorldEntity = 0xFFFFFFFF;

struct KmapHeader {
    char magic[4];
    uint32_t version;
    uint32_t renderModelSize;
    uint32_t brushCount, pointCount;
    uint32_t entitySize;
    float probeOrigin[3];
    float probeSpacing;
    uint32_t probeDims[3];
    uint32_t materialTableSize;
};

inline constexpr int kProbeFloats = 19;

// Baked lighting for moving things: a grid of ambient cubes (like Source).
struct KmapProbes {
    glm::vec3 origin{0.0f};
    float spacing = 0.0f;
    glm::uvec3 dims{0u};
    std::vector<float> data; // kProbeFloats per probe, x fastest
};

struct KmapBrush {
    uint32_t firstPoint, pointCount;
    uint32_t contents;
    uint32_t entity; // index into entities, or kWorldEntity
    // Texture of its top, sides and bottom (into KmapData::materials): what you walk on,
    // run into, bump your head on. Sounds and effects look up the surface from it.
    uint16_t material[3];
    uint16_t pad;
};
enum BrushFace { FACE_TOP = 0, FACE_SIDE = 1, FACE_BOTTOM = 2 };

using EntityKeys = std::map<std::string, std::string>;

struct KmapData {
    KmdlModel render;
    KmapProbes probes;
    std::vector<KmapBrush> brushes;
    std::vector<std::string> materials;
    std::vector<float> points; // xyz triples
    std::vector<EntityKeys> entities;
};

// Parses `{ "k" "v" }` blocks. Shared by the compiler (for .map entity keys) and loader.
inline std::vector<EntityKeys> parseEntityText(const std::string& text) {
    std::vector<EntityKeys> out;
    size_t i = 0;
    auto quoted = [&](std::string& s) {
        while (i < text.size() && text[i] != '"' && text[i] != '}') ++i;
        if (i >= text.size() || text[i] == '}') return false;
        const size_t end = text.find('"', i + 1);
        if (end == std::string::npos) return false;
        s = text.substr(i + 1, end - i - 1);
        i = end + 1;
        return true;
    };
    while ((i = text.find('{', i)) != std::string::npos) {
        ++i;
        EntityKeys e;
        std::string k, v;
        while (quoted(k) && quoted(v)) e[k] = v;
        out.push_back(std::move(e));
        i = text.find('}', i);
        if (i == std::string::npos) break;
    }
    return out;
}

inline std::string entityText(const std::vector<EntityKeys>& entities) {
    std::string out;
    for (const auto& e : entities) {
        out += "{\n";
        for (const auto& [k, v] : e) out += "\"" + k + "\" \"" + v + "\"\n";
        out += "}\n";
    }
    return out;
}

inline std::vector<uint8_t> writeKmap(const KmapData& m) {
    const std::vector<uint8_t> render = writeKmdl(m.render);
    const std::string ents = entityText(m.entities);
    KmapHeader h{};
    std::memcpy(h.magic, kKmapMagic, 4);
    h.version = kKmapVersion;
    h.renderModelSize = uint32_t(render.size());
    h.brushCount = uint32_t(m.brushes.size());
    h.pointCount = uint32_t(m.points.size() / 3);
    h.entitySize = uint32_t(ents.size());
    std::memcpy(h.probeOrigin, &m.probes.origin, 12);
    h.probeSpacing = m.probes.spacing;
    std::memcpy(h.probeDims, &m.probes.dims, 12);
    std::string table;
    for (const std::string& name : m.materials) table += name + "\n";
    h.materialTableSize = uint32_t(table.size());
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    put(&h, sizeof h);
    put(render.data(), render.size());
    put(m.brushes.data(), m.brushes.size() * sizeof(KmapBrush));
    put(m.points.data(), m.points.size() * sizeof(float));
    put(ents.data(), ents.size());
    put(m.probes.data.data(), m.probes.data.size() * sizeof(float));
    put(table.data(), table.size());
    return out;
}

inline std::optional<KmapData> readKmap(const std::vector<uint8_t>& data) {
    KmapHeader h;
    if (data.size() < sizeof h) return std::nullopt;
    std::memcpy(&h, data.data(), sizeof h);
    if (std::memcmp(h.magic, kKmapMagic, 4) != 0 || h.version != kKmapVersion) return std::nullopt;
    const uint64_t need = sizeof h + uint64_t(h.renderModelSize) + uint64_t(h.brushCount) * sizeof(KmapBrush) +
                          uint64_t(h.pointCount) * 12 + h.entitySize;
    if (data.size() < need) return std::nullopt;
    KmapData m;
    size_t pos = sizeof h;
    auto render = readKmdl(std::vector<uint8_t>(data.begin() + pos, data.begin() + pos + h.renderModelSize));
    if (h.renderModelSize && !render) return std::nullopt;
    if (render) m.render = std::move(*render);
    pos += h.renderModelSize;
    m.brushes.resize(h.brushCount);
    std::memcpy(m.brushes.data(), data.data() + pos, h.brushCount * sizeof(KmapBrush));
    pos += h.brushCount * sizeof(KmapBrush);
    m.points.resize(size_t(h.pointCount) * 3);
    std::memcpy(m.points.data(), data.data() + pos, m.points.size() * sizeof(float));
    pos += m.points.size() * sizeof(float);
    for (const KmapBrush& b : m.brushes)
        if (uint64_t(b.firstPoint) + b.pointCount > h.pointCount) return std::nullopt;
    m.entities = parseEntityText(std::string((const char*)data.data() + pos, h.entitySize));
    pos += h.entitySize;
    const uint64_t probes = uint64_t(h.probeDims[0]) * h.probeDims[1] * h.probeDims[2];
    if (probes > 4'000'000 || data.size() < pos + probes * kProbeFloats * 4) return std::nullopt;
    std::memcpy(&m.probes.origin, h.probeOrigin, 12);
    m.probes.spacing = h.probeSpacing;
    std::memcpy(&m.probes.dims, h.probeDims, 12);
    m.probes.data.resize(size_t(probes) * kProbeFloats);
    std::memcpy(m.probes.data.data(), data.data() + pos, m.probes.data.size() * 4);
    pos += m.probes.data.size() * 4;
    if (data.size() < pos + h.materialTableSize) return std::nullopt;
    const std::string table((const char*)data.data() + pos, h.materialTableSize);
    for (size_t a = 0, b; (b = table.find('\n', a)) != std::string::npos; a = b + 1) m.materials.push_back(table.substr(a, b - a));
    for (const KmapBrush& b : m.brushes)
        for (uint16_t mat : b.material)
            if (mat >= m.materials.size() && !m.materials.empty()) return std::nullopt;
    return m;
}

} // namespace keel
