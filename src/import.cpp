#include "import.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace fry {

// ───────────────────────────── DEFLATE (as zlib's puff: small, not fast) ─────────────────────────────

namespace {

struct Bits {
    const uint8_t* in;
    size_t size, at = 0;
    uint32_t buf = 0;
    int count = 0;
    bool bad = false;
    int need(int n) {
        while (count < n) {
            if (at >= size) return bad = true, 0;
            buf |= uint32_t(in[at++]) << count, count += 8;
        }
        const int v = int(buf & ((1u << n) - 1));
        buf >>= n, count -= n;
        return v;
    }
};

struct Huffman {
    short count[16] = {}, symbol[288] = {};
    // From each symbol's code length; false if they don't make a code.
    bool make(const short* lengths, int n) {
        std::fill(std::begin(count), std::end(count), 0);
        for (int s = 0; s < n; ++s) ++count[lengths[s]];
        if (count[0] == n) return true;
        int left = 1;
        for (int len = 1; len < 16; ++len)
            if ((left = left * 2 - count[len]) < 0) return false;
        short offs[16] = {};
        for (int len = 1; len < 15; ++len) offs[len + 1] = short(offs[len] + count[len]);
        for (int s = 0; s < n; ++s)
            if (lengths[s]) symbol[offs[lengths[s]]++] = short(s);
        return true;
    }
    int decode(Bits& b) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= b.need(1);
            if (b.bad) return -1;
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c, first += c, first <<= 1, code <<= 1;
        }
        return -1;
    }
};

const short kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& b, std::vector<uint8_t>& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int s = lit.decode(b);
        if (s < 0) return false;
        if (s < 256) out.push_back(uint8_t(s));
        else if (s == 256) return true;
        else {
            s -= 257;
            if (s >= 29) return false;
            const int len = kLenBase[s] + b.need(kLenExtra[s]);
            const int d = dist.decode(b);
            if (d < 0 || d >= 30) return false;
            const size_t back = size_t(kDistBase[d] + b.need(kDistExtra[d]));
            if (b.bad || back > out.size()) return false;
            for (int i = 0; i < len; ++i) out.push_back(out[out.size() - back]);
        }
    }
}

} // namespace

std::vector<uint8_t> inflate(const uint8_t* data, size_t size, size_t expect) {
    Bits b{data, size};
    std::vector<uint8_t> out;
    out.reserve(expect);
    for (bool last = false; !last;) {
        last = b.need(1);
        const int type = b.need(2);
        if (b.bad) return {};
        if (type == 0) { // stored
            b.buf = 0, b.count = 0;
            if (b.at + 4 > size) return {};
            const size_t len = size_t(data[b.at] | data[b.at + 1] << 8);
            b.at += 4;
            if (b.at + len > size) return {};
            out.insert(out.end(), data + b.at, data + b.at + len), b.at += len;
        } else if (type == 1) { // the fixed codes
            static Huffman lit, dist;
            static bool made = false;
            if (!made) {
                short l[288];
                for (int i = 0; i < 288; ++i) l[i] = short(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
                lit.make(l, 288);
                for (int i = 0; i < 30; ++i) l[i] = 5;
                dist.make(l, 30), made = true;
            }
            if (!codes(b, out, lit, dist)) return {};
        } else if (type == 2) { // codes of its own
            const int nlen = b.need(5) + 257, ndist = b.need(5) + 1, ncode = b.need(4) + 4;
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            short lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = short(b.need(3));
            Huffman lc;
            if (b.bad || !lc.make(lengths, 19)) return {};
            for (int i = 0; i < nlen + ndist;) {
                int s = lc.decode(b);
                if (s < 0) return {};
                if (s < 16) lengths[i++] = short(s);
                else {
                    short len = 0;
                    int rep;
                    if (s == 16) {
                        if (!i) return {};
                        len = lengths[i - 1], rep = 3 + b.need(2);
                    } else rep = s == 17 ? 3 + b.need(3) : 11 + b.need(7);
                    if (i + rep > nlen + ndist) return {};
                    while (rep--) lengths[i++] = len;
                }
            }
            Huffman lit, dist;
            if (!lit.make(lengths, nlen) || !dist.make(lengths + nlen, ndist) || !codes(b, out, lit, dist)) return {};
        } else
            return {};
    }
    return out;
}

// ───────────────────────────── the formats ─────────────────────────────

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
float toFloat(std::string_view s) { return std::strtof(std::string(s).c_str(), nullptr); }

void tidy(Mesh& m) {
    mergeByDistance(m, 1e-7f);
    removeUnused(m);
    std::erase_if(m.faces, [](const std::vector<int>& f) { return f.size() < 3 || std::set<int>(f.begin(), f.end()).size() < 3; });
}

std::vector<ImportedMesh> readStl(const std::vector<uint8_t>& d, const std::string& name, std::string* error) {
    Mesh m;
    const bool ascii = d.size() >= 5 && std::memcmp(d.data(), "solid", 5) == 0 && std::string_view(reinterpret_cast<const char*>(d.data()), std::min<size_t>(d.size(), 600)).find("facet") != std::string::npos;
    if (!ascii) {
        if (d.size() < 84) return *error = "That STL is too short", std::vector<ImportedMesh>{};
        uint32_t n;
        std::memcpy(&n, d.data() + 80, 4);
        if (d.size() < 84 + size_t(n) * 50) return *error = "That STL is cut short", std::vector<ImportedMesh>{};
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* t = d.data() + 84 + size_t(i) * 50 + 12;
            std::vector<int> f;
            for (int k = 0; k < 3; ++k) {
                float v[3];
                std::memcpy(v, t + k * 12, 12);
                f.push_back(int(m.verts.size())), m.verts.push_back(glm::vec3(v[0], v[1], v[2]) * 0.001f);
            }
            m.faces.push_back(f);
        }
    } else {
        std::istringstream in(std::string(d.begin(), d.end()));
        std::string word;
        std::vector<int> f;
        while (in >> word) {
            if (word == "vertex") {
                glm::vec3 v;
                in >> v.x >> v.y >> v.z;
                f.push_back(int(m.verts.size())), m.verts.push_back(v * 0.001f);
            } else if (word == "endfacet") {
                if (f.size() >= 3) m.faces.push_back(f);
                f.clear();
            }
        }
    }
    tidy(m);
    return {{name, std::move(m)}};
}

std::vector<ImportedMesh> readObj(const std::vector<uint8_t>& d, const std::string& name) {
    std::vector<glm::vec3> all;
    std::vector<ImportedMesh> out{{name, {}}};
    std::map<int, int> local; // (the file's point number: the one in this object's mesh)
    std::istringstream in(std::string(d.begin(), d.end()));
    for (std::string line; std::getline(in, line);) {
        if (line.size() < 2) continue;
        std::istringstream l(line);
        std::string what;
        l >> what;
        if (what == "v") {
            glm::vec3 v;
            l >> v.x >> v.y >> v.z;
            all.push_back(glm::vec3(v.x, -v.z, v.y)); // (OBJ's up is Y; here it's Z)
        } else if ((what == "o" || what == "g") && !out.back().mesh.faces.empty()) {
            std::string n;
            std::getline(l >> std::ws, n);
            out.push_back({n.empty() ? name : n, {}}), local.clear();
        } else if (what == "o" || what == "g") {
            std::string n;
            std::getline(l >> std::ws, n);
            if (!n.empty()) out.back().name = n;
        } else if (what == "f") {
            std::vector<int> f;
            for (std::string c; l >> c;) {
                int i = std::atoi(c.c_str());
                i = i < 0 ? int(all.size()) + i : i - 1;
                if (i < 0 || i >= int(all.size())) continue;
                auto [it, fresh] = local.try_emplace(i, int(out.back().mesh.verts.size()));
                if (fresh) out.back().mesh.verts.push_back(all[size_t(i)]);
                f.push_back(it->second);
            }
            if (f.size() >= 3) out.back().mesh.faces.push_back(f);
        }
    }
    std::erase_if(out, [](const ImportedMesh& m) { return m.mesh.faces.empty(); });
    // (no units in an OBJ: one this big is millimetres, as most made for printing are)
    float big = 0;
    for (const ImportedMesh& m : out)
        if (!m.mesh.verts.empty()) big = std::max(big, glm::length(boundsMax(m.mesh) - boundsMin(m.mesh)));
    for (ImportedMesh& m : out) {
        if (big > 20) transform(m.mesh, glm::scale(glm::mat4(1.0f), glm::vec3(0.001f)));
        tidy(m.mesh);
    }
    return out;
}

// A zip's file (stored or deflated), or empty.
std::vector<uint8_t> zipFile(const std::vector<uint8_t>& z, const std::function<bool(const std::string&)>& wanted, std::string* found = nullptr) {
    auto u16 = [&](size_t at) { return at + 2 <= z.size() ? uint32_t(z[at] | z[at + 1] << 8) : 0u; };
    auto u32 = [&](size_t at) { return at + 4 <= z.size() ? u16(at) | u16(at + 2) << 16 : 0u; };
    // The central directory, from its end record (sizes are right there even when the local headers leave them out).
    size_t end = z.size() >= 22 ? z.size() - 22 : 0;
    while (end > 0 && u32(end) != 0x06054b50) --end;
    if (u32(end) != 0x06054b50) return {};
    size_t at = u32(end + 16);
    for (uint32_t n = u16(end + 10); n-- && u32(at) == 0x02014b50;) {
        const uint32_t method = u16(at + 10), packed = u32(at + 20), size = u32(at + 24), nameLen = u16(at + 28), extra = u16(at + 30), comment = u16(at + 32), local = u32(at + 42);
        const std::string name(z.begin() + long(at + 46), z.begin() + long(std::min(z.size(), at + 46 + nameLen)));
        at += 46 + nameLen + extra + comment;
        if (!wanted(name)) continue;
        const size_t data = local + 30 + u16(local + 26) + u16(local + 28);
        if (data + packed > z.size()) return {};
        if (found) *found = name;
        if (method == 0) return std::vector<uint8_t>(z.begin() + long(data), z.begin() + long(data + packed));
        if (method == 8) return inflate(z.data() + data, packed, size);
        return {};
    }
    return {};
}

// The value of attribute `a` in a tag (as text).
std::string_view attr(std::string_view tag, std::string_view a) {
    for (size_t at = 0; (at = tag.find(a, at)) != std::string_view::npos; at += a.size()) {
        if (at > 0 && !std::isspace(static_cast<unsigned char>(tag[at - 1]))) continue;
        size_t q = at + a.size();
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size() || tag[q] != '=') continue;
        ++q;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size()) break;
        const char quote = tag[q];
        const size_t close = tag.find(quote, q + 1);
        if (close == std::string_view::npos) break;
        return tag.substr(q + 1, close - q - 1);
    }
    return {};
}
// XML's &amp; &lt; &gt; &quot; &apos; and &#...; made the characters again.
std::string unescape(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const size_t semi = s[i] == '&' ? s.find(';', i) : std::string_view::npos;
        if (semi == std::string_view::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string_view e = s.substr(i + 1, semi - i - 1);
        uint32_t c = e == "amp" ? '&' : e == "lt" ? '<' : e == "gt" ? '>' : e == "quot" ? '"' : e == "apos" ? '\'' : 0;
        if (!c && e.starts_with("#x")) c = uint32_t(std::strtoul(std::string(e.substr(2)).c_str(), nullptr, 16));
        else if (!c && e.starts_with("#")) c = uint32_t(std::strtoul(std::string(e.substr(1)).c_str(), nullptr, 10));
        if (!c) {
            out += s[i];
            continue;
        }
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
        else out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 0x3F)), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
        i = semi;
    }
    return out;
}
glm::mat4 transform3mf(std::string_view t) {
    if (t.empty()) return glm::mat4(1.0f);
    float v[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::istringstream in{std::string(t)};
    for (float& f : v) in >> f;
    glm::mat4 m(1.0f);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) m[r][c] = v[r * 3 + c];
    return m;
}

std::vector<ImportedMesh> read3mf(const std::vector<uint8_t>& d, const std::string& name, std::string* error) {
    const std::vector<uint8_t> xml = zipFile(d, [](const std::string& n) { return lower(n).ends_with(".model"); });
    if (xml.empty()) return *error = "No model in that 3MF (or it's packed a way that can't be read)", std::vector<ImportedMesh>{};
    const std::string_view s(reinterpret_cast<const char*>(xml.data()), xml.size());
    float unit = 0.001f;
    if (const size_t m = s.find("<model"); m != std::string_view::npos) {
        const std::string u(attr(s.substr(m, s.find('>', m) - m), "unit"));
        unit = u == "micron" ? 1e-6f : u == "centimeter" ? 0.01f : u == "meter" ? 1.0f : u == "inch" ? 0.0254f : u == "foot" ? 0.3048f : 0.001f;
    }
    struct Obj {
        std::string name;
        Mesh mesh;
        std::vector<std::pair<int, glm::mat4>> parts;
    };
    std::map<int, Obj> objs;
    std::vector<std::pair<int, glm::mat4>> items;
    Obj* cur = nullptr;
    for (size_t at = 0; (at = s.find('<', at)) != std::string_view::npos;) {
        const size_t close = s.find('>', at);
        if (close == std::string_view::npos) break;
        const std::string_view tag = s.substr(at + 1, close - at - 1);
        at = close + 1;
        auto is = [&](std::string_view t) { return tag.starts_with(t) && tag.size() > t.size() && (std::isspace(static_cast<unsigned char>(tag[t.size()])) || tag[t.size()] == '/'); };
        if (is("object")) {
            cur = &objs[std::atoi(std::string(attr(tag, "id")).c_str())];
            cur->name = unescape(attr(tag, "name"));
        } else if (is("vertex") && cur)
            cur->mesh.verts.push_back(glm::vec3(toFloat(attr(tag, "x")), toFloat(attr(tag, "y")), toFloat(attr(tag, "z"))));
        else if (is("triangle") && cur)
            cur->mesh.faces.push_back({std::atoi(std::string(attr(tag, "v1")).c_str()), std::atoi(std::string(attr(tag, "v2")).c_str()), std::atoi(std::string(attr(tag, "v3")).c_str())});
        else if (is("component") && cur)
            cur->parts.push_back({std::atoi(std::string(attr(tag, "objectid")).c_str()), transform3mf(attr(tag, "transform"))});
        else if (is("item"))
            items.push_back({std::atoi(std::string(attr(tag, "objectid")).c_str()), transform3mf(attr(tag, "transform"))});
    }
    std::function<void(int, const glm::mat4&, Mesh&, int)> place = [&](int id, const glm::mat4& m, Mesh& into, int depth) {
        auto it = objs.find(id);
        if (it == objs.end() || depth > 8) return;
        for (const auto& f : it->second.mesh.faces)
            for (int v : f)
                if (v < 0 || v >= int(it->second.mesh.verts.size())) return;
        append(into, it->second.mesh, m);
        for (const auto& [part, pm] : it->second.parts) place(part, m * pm, into, depth + 1);
    };
    std::vector<ImportedMesh> out;
    if (items.empty())
        for (const auto& [id, o] : objs) items.push_back({id, glm::mat4(1.0f)});
    for (const auto& [id, m] : items) {
        ImportedMesh im{objs.count(id) && !objs[id].name.empty() ? objs[id].name : name, {}};
        place(id, glm::scale(glm::mat4(1.0f), glm::vec3(unit)) * m, im.mesh, 0);
        tidy(im.mesh);
        if (!im.mesh.faces.empty()) out.push_back(std::move(im));
    }
    if (out.empty()) *error = "No shapes in that 3MF";
    return out;
}

std::vector<ImportedMesh> readGltf(const std::string& path, const std::string& name, std::string* error) {
    cgltf_options opt{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success || cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) {
        if (data) cgltf_free(data);
        *error = "That glTF can't be read";
        return {};
    }
    std::vector<ImportedMesh> out;
    for (size_t n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;
        float w[16];
        cgltf_node_transform_world(&node, w);
        const glm::mat4 m = glm::mat4(1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1) * glm::make_mat4(w); // (glTF's up is Y; here it's Z)
        ImportedMesh im{node.name ? node.name : node.mesh->name ? node.mesh->name : name, {}};
        for (size_t p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor* pos = nullptr;
            for (size_t a = 0; a < prim.attributes_count; ++a)
                if (prim.attributes[a].type == cgltf_attribute_type_position) pos = prim.attributes[a].data;
            if (!pos) continue;
            const int base = int(im.mesh.verts.size());
            for (size_t i = 0; i < pos->count; ++i) {
                float v[3] = {};
                cgltf_accessor_read_float(pos, i, v, 3);
                im.mesh.verts.push_back(glm::vec3(m * glm::vec4(v[0], v[1], v[2], 1)));
            }
            const size_t count = prim.indices ? prim.indices->count : pos->count;
            for (size_t i = 0; i + 2 < count; i += 3) {
                std::vector<int> f(3);
                for (int k = 0; k < 3; ++k) f[size_t(k)] = base + int(prim.indices ? cgltf_accessor_read_index(prim.indices, i + size_t(k)) : i + size_t(k));
                im.mesh.faces.push_back(f);
            }
        }
        tidy(im.mesh);
        if (!im.mesh.faces.empty()) out.push_back(std::move(im));
    }
    cgltf_free(data);
    if (out.empty()) *error = "No shapes in that glTF";
    return out;
}

} // namespace

const std::vector<std::string>& importFormats() {
    static const std::vector<std::string> f = {".stl", ".obj", ".3mf", ".glb", ".gltf"};
    return f;
}

std::vector<ImportedMesh> importModel(const std::string& path, std::string* error) {
    std::string dummy;
    if (!error) error = &dummy;
    const std::filesystem::path p(path);
    const std::string ext = lower(p.extension().string()), name = p.stem().string();
    if (ext == ".glb" || ext == ".gltf") return readGltf(path, name, error);
    std::ifstream in(path, std::ios::binary);
    if (!in) return *error = "Couldn't read " + p.filename().string(), std::vector<ImportedMesh>{};
    const std::vector<uint8_t> d{std::istreambuf_iterator<char>(in), {}};
    if (ext == ".stl") return readStl(d, name, error);
    if (ext == ".obj") return readObj(d, name);
    if (ext == ".3mf") return read3mf(d, name, error);
    *error = "Fry3DX can't read " + ext + " files (STL, OBJ, 3MF, glTF and GLB it can)";
    return {};
}

} // namespace fry
