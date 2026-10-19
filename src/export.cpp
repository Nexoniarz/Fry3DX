// Exporting for other programs: glTF / GLB (scene.cpp), OBJ, STL and PLY here.
#include "scene.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

namespace fry {

bool exportGltf(const std::string& path, const Scene& scene, const std::vector<std::vector<uint8_t>>& pngs, bool binary, std::string* error);

namespace {

// An object's triangles where it stands (Fry3DX's axes, metres), shared corners where they agree.
struct Tris {
    std::vector<glm::vec3> pos, nrm;
    std::vector<glm::vec2> uv;
    std::vector<uint32_t> idx;
};
Tris trianglesOf(const Object& shown, bool painted) {
    Object o = shown;
    if (o.hasFinish()) o.mesh = finishedMesh(shown);
    const glm::mat4 m = objectMatrix(o);
    const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m)));
    const bool mirrored = glm::determinant(glm::mat3(m)) < 0.0f;
    const auto normals = cornerNormals(o.mesh, o.smooth, o.smoothAngle);
    Tris t;
    std::map<std::array<float, 8>, uint32_t> seen;
    for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
        const auto& face = o.mesh.faces[f];
        for (const auto& tri : triangulate(o.mesh, int(f))) {
            std::array<uint32_t, 3> c{};
            for (int k = 0; k < 3; ++k) {
                const size_t corner = size_t(std::find(face.begin(), face.end(), tri[size_t(k)]) - face.begin());
                const glm::vec3 p(m * glm::vec4(o.mesh.verts[size_t(tri[size_t(k)])], 1.0f)), n = glm::normalize(nm * normals[f][corner]);
                const glm::vec2 tc = painted ? o.uvs[f][corner] : glm::vec2(0.0f);
                auto [it, fresh] = seen.try_emplace(std::array<float, 8>{p.x, p.y, p.z, n.x, n.y, n.z, tc.x, tc.y}, uint32_t(t.pos.size()));
                if (fresh) t.pos.push_back(p), t.nrm.push_back(n), t.uv.push_back(tc);
                c[size_t(k)] = it->second;
            }
            if (mirrored) std::swap(c[1], c[2]);
            t.idx.insert(t.idx.end(), c.begin(), c.end());
        }
    }
    return t;
}

bool painted(const Scene& s, const std::vector<std::vector<uint8_t>>& pngs, size_t i) {
    return i < pngs.size() && !pngs[i].empty() && s.objects[i].uvsFit() && !s.objects[i].hasFinish();
}

bool writeBytes(const std::filesystem::path& p, const void* data, size_t n, std::string* error) {
    std::ofstream f(p, std::ios::binary);
    if (f.write(static_cast<const char*>(data), std::streamsize(n))) return true;
    if (error) *error = "can't write " + p.filename().string();
    return false;
}

std::string safeName(std::string s) {
    for (char& ch : s)
        if (ch == ' ' || uint8_t(ch) < 0x20) ch = '_';
    return s;
}

bool exportObj(const std::string& path, const Scene& scene, const std::vector<std::vector<uint8_t>>& pngs, std::string* error) {
    const std::filesystem::path p(path);
    const std::string mtlName = p.stem().string() + ".mtl";
    std::ostringstream obj, mtl;
    obj.precision(7), mtl.precision(5);
    obj << "# Fry3DX\nmtllib " << mtlName << "\n";
    mtl << "# Fry3DX\n";
    auto yUp = [](glm::vec3 v) { return glm::vec3(v.y, v.z, v.x); }; // (as glTF's)
    size_t base = 1, any = 0;
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const Object& o = scene.objects[i];
        if (!o.visible || o.mesh.faces.empty()) continue;
        const bool pic = painted(scene, pngs, i);
        const Tris t = trianglesOf(o, pic);
        const std::string name = safeName(o.name), mat = std::format("{}_{}", name, i + 1);
        obj << "o " << name << "\nusemtl " << mat << "\n";
        for (const glm::vec3& v : t.pos) obj << "v " << yUp(v).x << " " << yUp(v).y << " " << yUp(v).z << "\n";
        for (const glm::vec2& v : t.uv) obj << "vt " << v.x << " " << 1.0f - v.y << "\n";
        for (const glm::vec3& v : t.nrm) obj << "vn " << yUp(v).x << " " << yUp(v).y << " " << yUp(v).z << "\n";
        for (size_t k = 0; k < t.idx.size(); k += 3) {
            obj << "f";
            for (int j = 0; j < 3; ++j) obj << " " << base + t.idx[k + size_t(j)] << "/" << base + t.idx[k + size_t(j)] << "/" << base + t.idx[k + size_t(j)];
            obj << "\n";
        }
        base += t.pos.size(), ++any;
        const glm::vec3 c = pic ? glm::vec3(1.0f) : glm::pow(o.color, glm::vec3(2.2f));
        mtl << "newmtl " << mat << "\nKd " << c.r << " " << c.g << " " << c.b << "\nKs 0.04 0.04 0.04\nNs " << (1.0f - o.rough) * 200.0f << "\nPr " << o.rough
            << "\nPm " << o.metal << "\n";
        if (pic) {
            const std::string png = std::format("{}_{}.png", p.stem().string(), i + 1);
            if (!writeBytes(p.parent_path() / png, pngs[i].data(), pngs[i].size(), error)) return false;
            mtl << "map_Kd " << png << "\n";
        }
    }
    if (!any) {
        if (error) *error = "nothing to export (no visible objects)";
        return false;
    }
    const std::string a = obj.str(), b = mtl.str();
    return writeFileSafely(path, a.data(), a.size(), error) && writeFileSafely((p.parent_path() / mtlName).string(), b.data(), b.size(), error);
}

bool exportStl(const std::string& path, const Scene& scene, std::string* error) {
    std::vector<uint8_t> out(80, 0);
    const char head[] = "Fry3DX, millimetres, Z up";
    std::memcpy(out.data(), head, sizeof head);
    out.resize(84);
    uint32_t count = 0;
    auto put = [&](glm::vec3 v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 12); };
    for (const Object& o : scene.objects) {
        if (!o.visible) continue;
        const Tris t = trianglesOf(o, false);
        for (size_t k = 0; k < t.idx.size(); k += 3) {
            const glm::vec3 a = t.pos[t.idx[k]] * 1000.0f, b = t.pos[t.idx[k + 1]] * 1000.0f, c = t.pos[t.idx[k + 2]] * 1000.0f;
            const glm::vec3 n = glm::cross(b - a, c - a);
            put(glm::length(n) > 0 ? glm::normalize(n) : glm::vec3(0.0f)), put(a), put(b), put(c);
            out.push_back(0), out.push_back(0);
            ++count;
        }
    }
    if (!count) {
        if (error) *error = "nothing to export (no visible objects)";
        return false;
    }
    std::memcpy(out.data() + 80, &count, 4);
    return writeFileSafely(path, out.data(), out.size(), error);
}

bool exportPly(const std::string& path, const Scene& scene, const std::vector<std::vector<uint8_t>>& pngs, std::string* error) {
    std::vector<uint8_t> verts, faces;
    size_t nv = 0, nf = 0;
    auto put = [](std::vector<uint8_t>& to, const void* p, size_t n) { to.insert(to.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const Object& o = scene.objects[i];
        if (!o.visible) continue;
        const bool pic = painted(scene, pngs, i);
        const Tris t = trianglesOf(o, pic);
        const uint8_t rgb[3] = {uint8_t(std::clamp(o.color.r, 0.0f, 1.0f) * 255.0f + 0.5f), uint8_t(std::clamp(o.color.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                                uint8_t(std::clamp(o.color.b, 0.0f, 1.0f) * 255.0f + 0.5f)};
        for (size_t v = 0; v < t.pos.size(); ++v) {
            const glm::vec2 st(t.uv[v].x, 1.0f - t.uv[v].y);
            put(verts, &t.pos[v], 12), put(verts, &t.nrm[v], 12), put(verts, &st, 8), put(verts, rgb, 3);
        }
        for (size_t k = 0; k < t.idx.size(); k += 3) {
            const uint8_t three = 3;
            const uint32_t f[3] = {uint32_t(nv + t.idx[k]), uint32_t(nv + t.idx[k + 1]), uint32_t(nv + t.idx[k + 2])};
            put(faces, &three, 1), put(faces, f, 12);
        }
        nv += t.pos.size(), nf += t.idx.size() / 3;
    }
    if (!nf) {
        if (error) *error = "nothing to export (no visible objects)";
        return false;
    }
    std::string head = std::format("ply\nformat binary_little_endian 1.0\ncomment Fry3DX, metres, Z up\nelement vertex {}\n"
                                   "property float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\nproperty float nz\n"
                                   "property float s\nproperty float t\nproperty uchar red\nproperty uchar green\nproperty uchar blue\n"
                                   "element face {}\nproperty list uchar uint vertex_indices\nend_header\n", nv, nf);
    std::vector<uint8_t> out(head.begin(), head.end());
    out.insert(out.end(), verts.begin(), verts.end()), out.insert(out.end(), faces.begin(), faces.end());
    return writeFileSafely(path, out.data(), out.size(), error);
}

// A zip with its files stored as they are (3MF is a zip).
std::vector<uint8_t> storedZip(const std::vector<std::pair<std::string, std::string>>& files) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    auto crc = [&](const std::string& d) {
        uint32_t c = 0xFFFFFFFFu;
        for (unsigned char ch : d) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    };
    std::vector<uint8_t> out, dir;
    auto u16 = [](std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)), v.push_back(uint8_t(x >> 8)); };
    auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) { u16(v, uint16_t(x)), u16(v, uint16_t(x >> 16)); };
    for (const auto& [name, data] : files) {
        const uint32_t at = uint32_t(out.size()), c = crc(data), n = uint32_t(data.size());
        u32(out, 0x04034b50), u16(out, 20), u16(out, 0), u16(out, 0), u16(out, 0), u16(out, 0x21), u32(out, c), u32(out, n), u32(out, n);
        u16(out, uint16_t(name.size())), u16(out, 0), out.insert(out.end(), name.begin(), name.end()), out.insert(out.end(), data.begin(), data.end());
        u32(dir, 0x02014b50), u16(dir, 20), u16(dir, 20), u16(dir, 0), u16(dir, 0), u16(dir, 0), u16(dir, 0x21), u32(dir, c), u32(dir, n), u32(dir, n);
        u16(dir, uint16_t(name.size())), u16(dir, 0), u16(dir, 0), u16(dir, 0), u16(dir, 0), u32(dir, 0), u32(dir, at);
        dir.insert(dir.end(), name.begin(), name.end());
    }
    const uint32_t dirAt = uint32_t(out.size()), dirSize = uint32_t(dir.size());
    out.insert(out.end(), dir.begin(), dir.end());
    u32(out, 0x06054b50), u16(out, 0), u16(out, 0), u16(out, uint16_t(files.size())), u16(out, uint16_t(files.size())), u32(out, dirSize), u32(out, dirAt), u16(out, 0);
    return out;
}

bool export3mf(const std::string& path, const Scene& scene, std::string* error) {
    std::ostringstream model, items;
    model.precision(7);
    model << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<model unit=\"millimeter\" xml:lang=\"en-US\" "
             "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n<metadata name=\"Application\">Fry3DX</metadata>\n<resources>\n";
    auto xml = [](const std::string& s) {
        std::string o;
        for (char ch : s) o += ch == '&' ? "&amp;" : ch == '<' ? "&lt;" : ch == '>' ? "&gt;" : ch == '"' ? "&quot;" : std::string(1, ch);
        return o;
    };
    int id = 0;
    for (const Object& o : scene.objects) {
        if (!o.visible || o.mesh.faces.empty()) continue;
        const Tris t = trianglesOf(o, false);
        // (corners joined again: slicers want each point once, or they see the model as not closed)
        std::map<std::array<float, 3>, uint32_t> at;
        std::vector<uint32_t> remap(t.pos.size());
        std::ostringstream verts;
        verts.precision(7);
        for (size_t v = 0; v < t.pos.size(); ++v) {
            const glm::vec3 p = t.pos[v] * 1000.0f;
            auto [it, fresh] = at.try_emplace({p.x, p.y, p.z}, uint32_t(at.size()));
            if (fresh) verts << "<vertex x=\"" << p.x << "\" y=\"" << p.y << "\" z=\"" << p.z << "\"/>\n";
            remap[v] = it->second;
        }
        ++id;
        model << "<object id=\"" << id << "\" name=\"" << xml(o.name) << "\" type=\"model\">\n<mesh>\n<vertices>\n" << verts.str() << "</vertices>\n<triangles>\n";
        for (size_t k = 0; k < t.idx.size(); k += 3) {
            const uint32_t a = remap[t.idx[k]], b = remap[t.idx[k + 1]], c = remap[t.idx[k + 2]];
            if (a != b && b != c && a != c) model << "<triangle v1=\"" << a << "\" v2=\"" << b << "\" v3=\"" << c << "\"/>\n";
        }
        model << "</triangles>\n</mesh>\n</object>\n";
        items << "<item objectid=\"" << id << "\"/>\n";
    }
    if (!id) {
        if (error) *error = "nothing to export (no visible objects)";
        return false;
    }
    model << "</resources>\n<build>\n" << items.str() << "</build>\n</model>\n";
    const auto zip = storedZip({
        {"[Content_Types].xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                                "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                                "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/></Types>\n"},
        {"_rels/.rels", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                        "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>\n"},
        {"3D/3dmodel.model", model.str()},
    });
    return writeFileSafely(path, zip.data(), zip.size(), error);
}

// ── FBX (binary, 7.4: what Blender, Unity, Unreal and Maya read) ──
// A tree of nodes, each a name, a list of typed values, and nodes inside it.
struct FbxNode {
    std::string name;
    std::vector<uint8_t> props;
    uint32_t count = 0;
    std::vector<FbxNode> kids;

    template <class T>
    void raw(const T& v) { props.insert(props.end(), reinterpret_cast<const uint8_t*>(&v), reinterpret_cast<const uint8_t*>(&v) + sizeof v); }
    FbxNode& I(int32_t v) { return props.push_back('I'), raw(v), ++count, *this; }
    FbxNode& L(int64_t v) { return props.push_back('L'), raw(v), ++count, *this; }
    FbxNode& D(double v) { return props.push_back('D'), raw(v), ++count, *this; }
    FbxNode& C(bool v) { return props.push_back('C'), props.push_back(v ? 1 : 0), ++count, *this; }
    FbxNode& S(const std::string& v, char type = 'S') {
        props.push_back(uint8_t(type)), raw(uint32_t(v.size())), props.insert(props.end(), v.begin(), v.end());
        return ++count, *this;
    }
    FbxNode& R(const std::vector<uint8_t>& v) {
        props.push_back('R'), raw(uint32_t(v.size())), props.insert(props.end(), v.begin(), v.end());
        return ++count, *this;
    }
    template <class T>
    FbxNode& array(char type, const std::vector<T>& v) {
        props.push_back(uint8_t(type)), raw(uint32_t(v.size())), raw(uint32_t(0)), raw(uint32_t(v.size() * sizeof(T)));
        props.insert(props.end(), reinterpret_cast<const uint8_t*>(v.data()), reinterpret_cast<const uint8_t*>(v.data()) + v.size() * sizeof(T));
        return ++count, *this;
    }
    FbxNode& add(const std::string& n) { return kids.push_back({n, {}, 0, {}}), kids.back(); }
    // Properties70's P: name, type, label, flags, then its values.
    FbxNode& P(const std::string& n, const std::string& type, const std::string& label, const std::string& flags = "") {
        return add("P").S(n).S(type).S(label).S(flags);
    }

    void write(std::vector<uint8_t>& out) const {
        const size_t at = out.size();
        out.resize(at + 12); // (where it ends, how many values, their size: filled in below)
        out.push_back(uint8_t(name.size())), out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), props.begin(), props.end());
        if (!kids.empty() || props.empty()) {
            for (const FbxNode& k : kids) k.write(out);
            out.resize(out.size() + 13); // (the end of the list inside it)
        }
        const uint32_t head[3] = {uint32_t(out.size()), count, uint32_t(props.size())};
        std::memcpy(out.data() + at, head, 12);
    }
};

bool exportFbx(const std::string& path, const Scene& scene, const std::vector<std::vector<uint8_t>>& pngs, std::string* error) {
    const std::filesystem::path file(path);
    auto yUp = [](glm::vec3 v) { return glm::dvec3(v.y, v.z, v.x) * 100.0; }; // (glTF's axes; FBX counts in centimetres)
    std::vector<FbxNode> top;
    {
        FbxNode& h = top.emplace_back(FbxNode{"FBXHeaderExtension"});
        h.add("FBXHeaderVersion").I(1003), h.add("FBXVersion").I(7400);
        FbxNode& t = h.add("CreationTimeStamp");
        t.add("Version").I(1000);
        for (const char* k : {"Year", "Month", "Day", "Hour", "Minute", "Second", "Millisecond"}) t.add(k).I(std::string(k) == "Year" ? 1970 : 1);
        h.add("Creator").S("Fry3DX");
    }
    // (the same file id and time Blender writes, which go with the footer below)
    top.push_back(FbxNode{"FileId"}), top.back().R({0x28, 0xb3, 0x2a, 0xeb, 0xb6, 0x24, 0xcc, 0xc2, 0xbf, 0xc8, 0xb0, 0x2a, 0xa9, 0x2b, 0xfc, 0xf1});
    top.push_back(FbxNode{"CreationTime"}), top.back().S("1970-01-01 10:00:00:000");
    top.push_back(FbxNode{"Creator"}), top.back().S("Fry3DX");
    {
        FbxNode& g = top.emplace_back(FbxNode{"GlobalSettings"});
        g.add("Version").I(1000);
        FbxNode& p = g.add("Properties70");
        const std::pair<const char*, int> axes[] = {{"UpAxis", 1}, {"UpAxisSign", 1}, {"FrontAxis", 2}, {"FrontAxisSign", 1}, {"CoordAxis", 0}, {"CoordAxisSign", 1},
                                                    {"OriginalUpAxis", 1}, {"OriginalUpAxisSign", 1}};
        for (auto [k, v] : axes) p.P(k, "int", "Integer").I(v);
        p.P("UnitScaleFactor", "double", "Number").D(1.0), p.P("OriginalUnitScaleFactor", "double", "Number").D(1.0);
    }
    {
        FbxNode& d = top.emplace_back(FbxNode{"Documents"});
        d.add("Count").I(1);
        FbxNode& doc = d.add("Document").L(1000000).S("").S("Scene");
        doc.add("Properties70");
        doc.add("RootNode").L(0);
    }
    top.push_back(FbxNode{"References"});
    FbxNode objects{"Objects"}, links{"Connections"};
    int64_t next = 2000000;
    int models = 0, textures = 0;
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const Object& shown = scene.objects[i];
        if (!shown.visible || shown.mesh.faces.empty()) continue;
        const bool pic = painted(scene, pngs, i);
        Object o = shown;
        if (o.hasFinish()) o.mesh = finishedMesh(shown);
        const glm::mat4 rs = glm::mat4_cast(o.rotation) * glm::scale(glm::mat4(1.0f), o.scale);
        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(rs)));
        const bool mirrored = glm::determinant(glm::mat3(rs)) < 0.0f;
        const auto normals = cornerNormals(o.mesh, o.smooth, o.smoothAngle);
        std::vector<double> verts, nrm, uv;
        std::vector<int32_t> index, uvIndex;
        for (const glm::vec3& v : o.mesh.verts) {
            const glm::dvec3 p = yUp(glm::vec3(rs * glm::vec4(v, 1.0f)));
            verts.insert(verts.end(), {p.x, p.y, p.z});
        }
        for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
            const auto& fv = o.mesh.faces[f];
            for (size_t k = 0; k < fv.size(); ++k) {
                const size_t c = mirrored ? fv.size() - 1 - k : k;
                index.push_back(k + 1 == fv.size() ? -fv[c] - 1 : fv[c]); // (the last of a face: negative, less one)
                const glm::dvec3 n = yUp(glm::normalize(nm * normals[f][c])) / 100.0;
                nrm.insert(nrm.end(), {n.x, n.y, n.z});
                if (pic) {
                    uvIndex.push_back(int32_t(uv.size() / 2));
                    uv.insert(uv.end(), {double(o.uvs[f][c].x), 1.0 - double(o.uvs[f][c].y)});
                }
            }
        }
        const std::string name = o.name.empty() ? std::format("Object {}", i + 1) : o.name;
        const int64_t geo = next++, model = next++, mat = next++;
        FbxNode& g = objects.add("Geometry").L(geo).S(name + std::string("\x00\x01", 2) + "Geometry", 'S').S("Mesh");
        g.add("Properties70");
        g.add("GeometryVersion").I(124);
        g.add("Vertices").array('d', verts);
        g.add("PolygonVertexIndex").array('i', index);
        FbxNode& ln = g.add("LayerElementNormal").I(0);
        ln.add("Version").I(101), ln.add("Name").S(""), ln.add("MappingInformationType").S("ByPolygonVertex"), ln.add("ReferenceInformationType").S("Direct");
        ln.add("Normals").array('d', nrm);
        if (pic) {
            FbxNode& lu = g.add("LayerElementUV").I(0);
            lu.add("Version").I(101), lu.add("Name").S("UVMap"), lu.add("MappingInformationType").S("ByPolygonVertex"), lu.add("ReferenceInformationType").S("IndexToDirect");
            lu.add("UV").array('d', uv), lu.add("UVIndex").array('i', uvIndex);
        }
        FbxNode& lm = g.add("LayerElementMaterial").I(0);
        lm.add("Version").I(101), lm.add("Name").S(""), lm.add("MappingInformationType").S("AllSame"), lm.add("ReferenceInformationType").S("IndexToDirect");
        lm.add("Materials").array('i', std::vector<int32_t>{0});
        FbxNode& layer = g.add("Layer").I(0);
        layer.add("Version").I(100);
        for (const char* t : {"LayerElementNormal", "LayerElementMaterial", "LayerElementUV"}) {
            if (!pic && std::string(t) == "LayerElementUV") continue;
            FbxNode& e = layer.add("LayerElement");
            e.add("Type").S(t), e.add("TypedIndex").I(0);
        }

        FbxNode& m = objects.add("Model").L(model).S(name + std::string("\x00\x01", 2) + "Model").S("Mesh");
        m.add("Version").I(232);
        FbxNode& mp = m.add("Properties70");
        const glm::dvec3 t = yUp(o.position);
        mp.P("Lcl Translation", "Lcl Translation", "", "A").D(t.x).D(t.y).D(t.z);
        m.add("Shading").C(true), m.add("Culling").S("CullingOff");

        const glm::vec3 col = pic ? glm::vec3(1.0f) : glm::pow(o.color, glm::vec3(2.2f));
        FbxNode& ma = objects.add("Material").L(mat).S(name + std::string("\x00\x01", 2) + "Material").S("");
        ma.add("Version").I(102), ma.add("ShadingModel").S("phong"), ma.add("MultiLayer").I(0);
        FbxNode& pp = ma.add("Properties70");
        pp.P("DiffuseColor", "Color", "", "A").D(col.r).D(col.g).D(col.b);
        pp.P("DiffuseFactor", "Number", "", "A").D(1.0);
        pp.P("SpecularFactor", "Number", "", "A").D(0.5 * (1.0 - o.rough));
        pp.P("Shininess", "Number", "", "A").D(2.0 + (1.0 - o.rough) * 98.0);
        pp.P("ReflectionFactor", "Number", "", "A").D(o.metal);
        links.add("C").S("OO").L(model).L(0);
        links.add("C").S("OO").L(geo).L(model);
        links.add("C").S("OO").L(mat).L(model);
        ++models;
        if (pic) { // its picture: inside the file, and beside it as a PNG
            const std::string png = std::format("{}_{}.png", file.stem().string(), i + 1);
            if (!writeBytes(file.parent_path() / png, pngs[i].data(), pngs[i].size(), error)) return false;
            const int64_t tex = next++, video = next++;
            FbxNode& v = objects.add("Video").L(video).S(png + std::string("\x00\x01", 2) + "Video").S("Clip");
            v.add("Type").S("Clip");
            v.add("Properties70").P("Path", "KString", "XRefUrl", "").S(png);
            v.add("UseMipMap").I(0), v.add("Filename").S(png), v.add("RelativeFilename").S(png), v.add("Content").R(pngs[i]);
            FbxNode& tx = objects.add("Texture").L(tex).S(png + std::string("\x00\x01", 2) + "Texture").S("");
            tx.add("Type").S("TextureVideoClip"), tx.add("Version").I(202), tx.add("TextureName").S(png + std::string("\x00\x01", 2) + "Texture");
            tx.add("Properties70").P("UseMaterial", "bool", "", "").I(1);
            tx.add("Media").S(png + std::string("\x00\x01", 2) + "Video"), tx.add("FileName").S(png), tx.add("RelativeFilename").S(png);
            tx.add("ModelUVTranslation").D(0).D(0), tx.add("ModelUVScaling").D(1).D(1), tx.add("Texture_Alpha_Source").S("None");
            tx.add("Cropping").I(0).I(0).I(0).I(0);
            links.add("C").S("OP").L(tex).L(mat).S("DiffuseColor");
            links.add("C").S("OO").L(video).L(tex);
            ++textures;
        }
    }
    if (!models) {
        if (error) *error = "nothing to export (no visible objects)";
        return false;
    }
    {
        FbxNode& d = top.emplace_back(FbxNode{"Definitions"});
        d.add("Version").I(100);
        const std::pair<const char*, int> kinds[] = {{"GlobalSettings", 1}, {"Model", models}, {"Geometry", models}, {"Material", models}, {"Texture", textures}, {"Video", textures}};
        int all = 0;
        for (auto [k, n] : kinds) all += n;
        d.add("Count").I(all);
        for (auto [k, n] : kinds)
            if (n) d.add("ObjectType").S(k).add("Count").I(n);
    }
    top.push_back(std::move(objects));
    top.push_back(std::move(links));

    std::vector<uint8_t> out;
    const char head[] = "Kaydara FBX Binary  ";
    out.insert(out.end(), head, head + sizeof head); // (with its 0)
    out.push_back(0x1a), out.push_back(0);
    const uint32_t version = 7400;
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&version), reinterpret_cast<const uint8_t*>(&version) + 4);
    for (const FbxNode& n : top) n.write(out);
    out.resize(out.size() + 13);
    const uint8_t foot[] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
    out.insert(out.end(), foot, foot + 16), out.resize(out.size() + 4);
    size_t pad = ((out.size() + 15) & ~size_t(15)) - out.size();
    out.resize(out.size() + (pad ? pad : 16));
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&version), reinterpret_cast<const uint8_t*>(&version) + 4);
    out.resize(out.size() + 120);
    const uint8_t end[] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
    out.insert(out.end(), end, end + 16);
    return writeFileSafely(path, out.data(), out.size(), error);
}

} // namespace

const std::vector<std::pair<std::string, std::string>>& exportFormats() {
    static const std::vector<std::pair<std::string, std::string>> formats = {
        {".glb", "glTF binary, all in one file (games: Godot, Unity, Unreal; Blender)"},
        {".gltf", "glTF with its .bin and pictures beside it"},
        {".fbx", "FBX (Blender, Unity, Unreal, Maya, 3ds Max)"},
        {".obj", "Wavefront OBJ with an .mtl and pictures (nearly every 3D program)"},
        {".3mf", "3MF for 3D printing slicers (PrusaSlicer, Bambu Studio, Cura, Orca)"},
        {".stl", "STL for 3D printing (millimetres, shape only)"},
        {".ply", "PLY (shape, colours; scanners, MeshLab)"},
    };
    return formats;
}

bool exportModel(const std::string& path, const Scene& scene, const std::vector<std::vector<uint8_t>>& pngs, std::string* error) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".gltf" || ext == ".glb") return exportGltf(path, scene, pngs, ext == ".glb", error);
    if (ext == ".fbx") return exportFbx(path, scene, pngs, error);
    if (ext == ".obj") return exportObj(path, scene, pngs, error);
    if (ext == ".3mf") return export3mf(path, scene, error);
    if (ext == ".stl") return exportStl(path, scene, error);
    if (ext == ".ply") return exportPly(path, scene, pngs, error);
    if (error) *error = "can't export to " + ext + " files";
    return false;
}

} // namespace fry
