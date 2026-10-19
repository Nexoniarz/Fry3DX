#include "scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fry {

glm::quat fromEuler(const glm::vec3& degrees) {
    const glm::vec3 r = glm::radians(degrees);
    return glm::angleAxis(r.z, glm::vec3(0, 0, 1)) * glm::angleAxis(r.y, glm::vec3(0, 1, 0)) * glm::angleAxis(r.x, glm::vec3(1, 0, 0));
}

glm::vec3 toEuler(const glm::quat& q) {
    const glm::mat3 m = glm::mat3_cast(glm::normalize(q)); // = Rz Ry Rx; m[column][row]
    const float y = std::asin(std::clamp(-m[0][2], -1.0f, 1.0f));
    float x, z;
    if (std::abs(m[0][2]) < 0.9999f) x = std::atan2(m[1][2], m[2][2]), z = std::atan2(m[0][1], m[0][0]);
    else x = std::atan2(-m[2][1], m[1][1]), z = 0.0f; // (straight up or down: X and Z turn the same way)
    return glm::degrees(glm::vec3(x, y, z));
}

glm::mat4 objectMatrix(const Object& o) {
    return glm::translate(glm::mat4(1.0f), o.position) * glm::mat4_cast(o.rotation) * glm::scale(glm::mat4(1.0f), o.scale);
}

Mesh finishedMesh(const Object& o, bool mirrorOnly) {
    Mesh m = o.mesh;
    for (int axis = 0; axis < 3; ++axis) {
        if (!(o.mirror & (1 << axis))) continue;
        glm::vec3 s(1.0f);
        s[axis] = -1.0f;
        const size_t before = m.verts.size();
        append(m, Mesh(m), glm::scale(glm::mat4(1.0f), s));
        // Points on the mirror line join their copies (so the halves are one piece there).
        const float eps = 1e-4f;
        std::vector<int> to(m.verts.size());
        for (size_t i = 0; i < to.size(); ++i) to[i] = int(i);
        for (size_t i = before; i < m.verts.size(); ++i)
            if (std::abs(m.verts[i][axis]) < eps) to[i] = int(i - before);
        for (auto& f : m.faces)
            for (int& v : f) v = to[size_t(v)];
        std::erase_if(m.faces, [](const std::vector<int>& f) { return std::set<int>(f.begin(), f.end()).size() < 3; });
        // (a face lying on the mirror line would now be there twice, back to back: both go)
        std::map<std::vector<int>, int> seen;
        for (size_t f = 0; f < m.faces.size(); ++f) {
            std::vector<int> k = m.faces[f];
            std::sort(k.begin(), k.end());
            seen[k]++;
        }
        std::erase_if(m.faces, [&](const std::vector<int>& f) {
            std::vector<int> k = f;
            std::sort(k.begin(), k.end());
            return seen[k] > 1;
        });
        removeUnused(m);
    }
    if (mirrorOnly) return m;
    if (o.round > 0.0f) bevelEdges(m, sharpEdges(m, o.roundAngle), o.round, o.roundSteps);
    for (int i = 0; i < std::min(o.smoothLevels, 4); ++i) subdivide(m, true, o.smoothKeepSharp ? 30.0f : 180.0f);
    return m;
}

std::array<glm::vec3, 4> referenceCorners(const Reference& r, float aspect) {
    // Each view's right, up and towards-you directions (see the cameras in app.cpp).
    glm::vec3 right, up;
    switch (r.axis) {
    case Axis::Front: right = {0, 1, 0}, up = {0, 0, 1}; break;
    case Axis::Side: right = {-1, 0, 0}, up = {0, 0, 1}; break;
    default: right = {0, -1, 0}, up = {1, 0, 0}; break;
    }
    const glm::vec3 c = right * r.offset.x + up * r.offset.y;
    const glm::vec3 x = right * (r.width * 0.5f), y = up * (std::abs(r.width) * aspect * 0.5f);
    return {c - x - y, c + x - y, c + x + y, c - x + y};
}

// ── .fry ──

namespace {
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out + "\"";
}

std::string readQuoted(std::istream& in) {
    std::string out;
    char ch;
    in >> std::ws;
    if (in.peek() != '"') {
        in >> out;
        return out;
    }
    in.get();
    while (in.get(ch) && ch != '"') {
        if (ch == '\\') in.get(ch);
        out += ch;
    }
    return out;
}

const char* axisName(Axis a) { return a == Axis::Front ? "front" : a == Axis::Side ? "side" : "top"; }
} // namespace

std::string sceneText(const Scene& scene) {
    std::ostringstream out;
    out.precision(9); // (floats come back exactly)
    out << "fry3dx 1\n";
    const Look& k = scene.look;
    out << "look " << k.sunTurn << " " << k.sunHeight << " " << k.sunStrength << " " << k.sunColor.r << " " << k.sunColor.g << " " << k.sunColor.b << " " << k.sky
        << " " << k.ground << " " << k.background.r << " " << k.background.g << " " << k.background.b << " " << k.groundColor.r << " " << k.groundColor.g << " "
        << k.groundColor.b << " " << k.exposure << " " << k.ao << " " << k.bloom << " " << k.saturation << " " << k.vignette << " " << k.sharpen << " "
        << k.shadowSoft << "\n";
    for (const Reference& r : scene.references)
        out << "reference " << quoted(r.path) << " " << axisName(r.axis) << " " << r.offset.x << " " << r.offset.y << " " << r.width << " " << r.opacity
            << " " << r.visible << "\n";
    // Folders by their place in the file (1 up; 0 the top), as ids change on loading.
    std::map<uint32_t, size_t> folderAt;
    for (size_t i = 0; i < scene.folders.size(); ++i) folderAt[scene.folders[i].id] = i + 1;
    for (const Folder& f : scene.folders)
        out << "folder " << quoted(f.name) << " " << (folderAt.count(f.parent) ? folderAt[f.parent] : 0) << " " << f.open << " " << f.visible << " " << f.locked << "\n";
    for (const Object& o : scene.objects) {
        out << "object " << quoted(o.name) << "\n";
        if (folderAt.count(o.folder)) out << "  folder " << folderAt[o.folder] << "\n";
        out << "  position " << o.position.x << " " << o.position.y << " " << o.position.z << "\n";
        out << "  rotation " << o.rotation.w << " " << o.rotation.x << " " << o.rotation.y << " " << o.rotation.z << "\n"; // (a quaternion: w x y z)
        out << "  scale " << o.scale.x << " " << o.scale.y << " " << o.scale.z << "\n";
        out << "  color " << o.color.r << " " << o.color.g << " " << o.color.b << "\n";
        out << "  metal " << o.metal << "\n  rough " << o.rough << "\n";
        out << "  smooth " << o.smooth << " " << o.smoothAngle << "\n";
        out << "  visible " << o.visible << "\n  locked " << o.locked << "\n";
        if (!o.recipe.empty()) {
            out << "  recipe " << o.recipe;
            for (float v : o.settings) out << " " << v;
            out << "\n";
        }
        if (o.mirror) out << "  mirror " << o.mirror << "\n";
        if (o.round > 0.0f) out << "  round " << o.round << " " << o.roundSteps << " " << o.roundAngle << "\n";
        if (o.smoothLevels) out << "  subdivide " << o.smoothLevels << " " << o.smoothKeepSharp << "\n";
        if (!o.pictureFile.empty()) out << "  picture " << quoted(o.pictureFile) << "\n";
        for (const PictureLayer& l : o.layers) {
            if (l.file.empty()) continue;
            out << "  layer " << quoted(l.name) << " " << quoted(l.file) << " " << l.sticker << " " << l.visible << " " << l.opacity;
            for (const glm::vec2& c : l.corners) out << " " << c.x << " " << c.y;
            out << " " << l.face << " " << l.centre.x << " " << l.centre.y << " " << l.centre.z << " " << l.up.x << " " << l.up.y << " " << l.up.z << " " << l.width << " "
                << l.height << "\n";
        }
        for (const glm::vec3& v : o.mesh.verts) out << "  v " << v.x << " " << v.y << " " << v.z << "\n";
        for (const auto& f : o.mesh.faces) {
            out << "  f";
            for (int i : f) out << " " << i;
            out << "\n";
        }
        if (o.picture && o.uvsFit())
            for (const auto& t : o.uvs) {
                out << "  t";
                for (const glm::vec2& q : t) out << " " << q.x << " " << q.y;
                out << "\n";
            }
        out << "end\n";
    }
    return out.str();
}

bool writeFileSafely(const std::string& path, const void* data, size_t size, std::string* error) {
    // Written beside it, then moved over it: a crash mid-save never leaves half a file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f.write(static_cast<const char*>(data), std::streamsize(size)) || !(f.flush())) {
            if (error) *error = "can't write " + tmp;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec && error) *error = ec.message();
    return !ec;
}

bool saveScene(const std::string& path, const Scene& scene, std::string* error) {
    const std::string text = sceneText(scene);
    return writeFileSafely(path, text.data(), text.size(), error);
}

std::optional<Scene> loadScene(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "can't open " + path;
        return std::nullopt;
    }
    return parseScene(in, path, error);
}

std::optional<Scene> parseScene(std::istream& in, const std::string& path, std::string* error) {
    std::string word;
    int version = 0;
    if (!(in >> word >> version) || word != "fry3dx") {
        if (error) *error = path + " isn't a Fry3DX file";
        return std::nullopt;
    }
    Scene s;
    Object* o = nullptr;
    for (std::string line; std::getline(in, line);) {
        std::istringstream l(line);
        if (!(l >> word)) continue;
        if (word == "reference") {
            Reference r;
            r.id = s.nextId++;
            r.path = readQuoted(l);
            std::string axis;
            l >> axis >> r.offset.x >> r.offset.y >> r.width >> r.opacity >> r.visible;
            r.axis = axis == "front" ? Axis::Front : axis == "top" ? Axis::Top : Axis::Side;
            s.references.push_back(r);
        } else if (word == "look") {
            Look& k = s.look;
            l >> k.sunTurn >> k.sunHeight >> k.sunStrength >> k.sunColor.r >> k.sunColor.g >> k.sunColor.b >> k.sky >> k.ground >> k.background.r >> k.background.g >>
                k.background.b >> k.groundColor.r >> k.groundColor.g >> k.groundColor.b >> k.exposure >> k.ao >> k.bloom >> k.saturation >> k.vignette >> k.sharpen >>
                k.shadowSoft;
        } else if (word == "folder" && !o) {
            Folder f;
            f.id = s.nextId++;
            f.name = readQuoted(l);
            size_t parent = 0;
            l >> parent >> f.open >> f.visible >> f.locked;
            f.parent = uint32_t(parent); // (a place in the file for now)
            s.folders.push_back(f);
        } else if (word == "object") {
            s.objects.emplace_back();
            o = &s.objects.back();
            o->id = s.nextId++;
            o->name = readQuoted(l);
        } else if (!o) {
            continue;
        } else if (word == "position") l >> o->position.x >> o->position.y >> o->position.z;
        else if (word == "rotation") l >> o->rotation.w >> o->rotation.x >> o->rotation.y >> o->rotation.z;
        else if (word == "scale") l >> o->scale.x >> o->scale.y >> o->scale.z;
        else if (word == "color") l >> o->color.r >> o->color.g >> o->color.b;
        else if (word == "metal") l >> o->metal;
        else if (word == "rough") l >> o->rough;
        else if (word == "smooth") l >> o->smooth >> o->smoothAngle;
        else if (word == "folder") {
            size_t at = 0;
            l >> at;
            o->folder = uint32_t(at); // (a place in the file for now)
        } else if (word == "visible") l >> o->visible;
        else if (word == "locked") l >> o->locked;
        else if (word == "recipe") {
            l >> o->recipe;
            for (float v; l >> v;) o->settings.push_back(v);
        } else if (word == "mirror") l >> o->mirror;
        else if (word == "round") l >> o->round >> o->roundSteps >> o->roundAngle;
        else if (word == "subdivide") l >> o->smoothLevels >> o->smoothKeepSharp;
        else if (word == "picture") o->pictureFile = readQuoted(l);
        else if (word == "layer") {
            PictureLayer p;
            p.name = readQuoted(l);
            l >> std::ws;
            p.file = readQuoted(l);
            l >> p.sticker >> p.visible >> p.opacity;
            for (glm::vec2& c : p.corners) l >> c.x >> c.y;
            l >> p.face >> p.centre.x >> p.centre.y >> p.centre.z >> p.up.x >> p.up.y >> p.up.z >> p.width >> p.height;
            o->layers.push_back(p);
        }
        else if (word == "t") {
            std::vector<glm::vec2> t;
            for (glm::vec2 q; l >> q.x >> q.y;) t.push_back(q);
            o->uvs.push_back(t);
        }
        else if (word == "v") {
            glm::vec3 v;
            l >> v.x >> v.y >> v.z;
            o->mesh.verts.push_back(v);
        } else if (word == "f") {
            std::vector<int> f;
            for (int i; l >> i;)
                if (i >= 0 && i < int(o->mesh.verts.size())) f.push_back(i);
            if (f.size() >= 3) o->mesh.faces.push_back(f);
        } else if (word == "end") o = nullptr;
    }
    // Places in the file to ids (a parent or folder that isn't there: the top).
    auto idAt = [&](uint32_t at) { return at >= 1 && at <= s.folders.size() ? s.folders[at - 1].id : 0u; };
    for (Folder& f : s.folders) f.parent = idAt(f.parent);
    for (Folder& f : s.folders) // (a folder inside itself, round a loop: put at the top)
        for (uint32_t p = f.parent, n = 0; p; ++n) {
            if (p == f.id || n > s.folders.size()) {
                f.parent = 0;
                break;
            }
            p = std::find_if(s.folders.begin(), s.folders.end(), [&](const Folder& g) { return g.id == p; })->parent;
        }
    for (Object& o : s.objects) o.folder = idAt(o.folder);
    return s;
}

// ── glTF ──

bool exportGltf(const std::string& path, const Scene& scene, const std::vector<std::vector<uint8_t>>& pngs, bool binary, std::string* error) {
    // Fry3DX: X forward, Y left, Z up. glTF: Z forward, X left, Y up.
    auto toGltf = [](glm::vec3 v) { return glm::vec3(v.y, v.z, v.x); };
    std::vector<uint8_t> bin;
    auto put = [&](const void* p, size_t n) {
        while (bin.size() % 4) bin.push_back(0);
        const size_t at = bin.size();
        bin.insert(bin.end(), (const uint8_t*)p, (const uint8_t*)p + n);
        return at;
    };
    std::ostringstream views, accessors, meshes, nodes, materials, images;
    int nImage = 0;
    int nView = 0, nAcc = 0, nMesh = 0;
    std::vector<int> roots;
    auto view = [&](size_t offset, size_t length, int target) {
        views << (nView ? "," : "") << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << length;
        if (target) views << ",\"target\":" << target;
        views << "}";
        return nView++;
    };
    auto escape = [](const std::string& s) {
        std::string out;
        for (char ch : s) {
            if (ch == '"' || ch == '\\') out += '\\';
            if (uint8_t(ch) >= 0x20) out += ch;
        }
        return out;
    };
    const std::filesystem::path p(path);
    for (size_t oi = 0; oi < scene.objects.size(); ++oi) {
        const Object& shown = scene.objects[oi];
        if (!shown.visible || shown.mesh.faces.empty()) continue;
        Object o = shown;
        if (o.hasFinish()) o.mesh = finishedMesh(shown);
        // Painted: its picture, laid on as painted (not with rounding or smoothing added since).
        const bool painted = oi < pngs.size() && !pngs[oi].empty() && shown.uvsFit() && !shown.hasFinish();
        // Its turn and size go into its points; its place stays the node's (its origin: a hinge).
        const glm::mat4 rs = glm::mat4_cast(o.rotation) * glm::scale(glm::mat4(1.0f), o.scale);
        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(rs)));
        const bool mirrored = glm::determinant(glm::mat3(rs)) < 0.0f;
        const auto normals = cornerNormals(o.mesh, o.smooth, o.smoothAngle);
        std::vector<glm::vec3> pos, nrm;
        std::vector<glm::vec2> uv;
        std::vector<uint32_t> idx;
        std::map<std::array<float, 8>, uint32_t> seen; // (shared corners where the normal and picture agree)
        for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
            const auto& face = o.mesh.faces[f];
            for (const auto& tri : triangulate(o.mesh, int(f))) {
                std::array<uint32_t, 3> t{};
                for (int k = 0; k < 3; ++k) {
                    const size_t corner = size_t(std::find(face.begin(), face.end(), tri[size_t(k)]) - face.begin());
                    const glm::vec3 p = toGltf(glm::vec3(rs * glm::vec4(o.mesh.verts[size_t(tri[size_t(k)])], 1.0f)));
                    const glm::vec3 n = toGltf(glm::normalize(nm * normals[f][corner]));
                    // Pictures wrap round it from the side it faces most (1 m = once), or as painted.
                    const glm::vec3 a = glm::abs(n);
                    const glm::vec2 tc = painted ? o.uvs[f][corner]
                                                : a.x >= a.y && a.x >= a.z ? glm::vec2(p.z, -p.y) : a.y >= a.z ? glm::vec2(p.x, -p.z) : glm::vec2(p.x, -p.y);
                    const std::array<float, 8> key{p.x, p.y, p.z, n.x, n.y, n.z, tc.x, tc.y};
                    auto [it, fresh] = seen.try_emplace(key, uint32_t(pos.size()));
                    if (fresh) {
                        uv.push_back(tc);
                        pos.push_back(p), nrm.push_back(n);
                    }
                    t[size_t(k)] = it->second;
                }
                if (mirrored) std::swap(t[1], t[2]);
                idx.insert(idx.end(), t.begin(), t.end());
            }
        }
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const glm::vec3& p : pos) lo = glm::min(lo, p), hi = glm::max(hi, p);
        const int vp = view(put(pos.data(), pos.size() * 12), pos.size() * 12, 34962);
        const int vn = view(put(nrm.data(), nrm.size() * 12), nrm.size() * 12, 34962);
        const int vu = view(put(uv.data(), uv.size() * 8), uv.size() * 8, 34962);
        const int vi = view(put(idx.data(), idx.size() * 4), idx.size() * 4, 34963);
        accessors << (nAcc ? "," : "") << "{\"bufferView\":" << vp << ",\"componentType\":5126,\"count\":" << pos.size() << ",\"type\":\"VEC3\",\"min\":["
                  << lo.x << "," << lo.y << "," << lo.z << "],\"max\":[" << hi.x << "," << hi.y << "," << hi.z << "]}"
                  << ",{\"bufferView\":" << vn << ",\"componentType\":5126,\"count\":" << nrm.size() << ",\"type\":\"VEC3\"}"
                  << ",{\"bufferView\":" << vu << ",\"componentType\":5126,\"count\":" << uv.size() << ",\"type\":\"VEC2\"}"
                  << ",{\"bufferView\":" << vi << ",\"componentType\":5125,\"count\":" << idx.size() << ",\"type\":\"SCALAR\"}";
        meshes << (nMesh ? "," : "") << "{\"name\":\"" << escape(o.name) << "\",\"primitives\":[{\"attributes\":{\"POSITION\":" << nAcc
               << ",\"NORMAL\":" << nAcc + 1 << ",\"TEXCOORD_0\":" << nAcc + 2 << "},\"indices\":" << nAcc + 3 << ",\"material\":" << nMesh << "}]}";
        nAcc += 4;
        const glm::vec3 lin = painted ? glm::vec3(1.0f) : glm::pow(o.color, glm::vec3(2.2f)); // (picked as seen, sRGB; glTF's factor is linear)
        materials << (nMesh ? "," : "") << "{\"name\":\"" << escape(o.name) << "\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" << lin.r << ","
                  << lin.g << "," << lin.b << ",1],";
        if (painted) {
            materials << "\"baseColorTexture\":{\"index\":" << nImage << "},";
            if (binary) images << (nImage ? "," : "") << "{\"mimeType\":\"image/png\",\"bufferView\":" << view(put(pngs[oi].data(), pngs[oi].size()), pngs[oi].size(), 0) << "}";
            else {
                const std::string name = std::format("{}_{}.png", p.stem().string(), oi + 1);
                std::ofstream f(p.parent_path() / name, std::ios::binary);
                if (!f.write(reinterpret_cast<const char*>(pngs[oi].data()), std::streamsize(pngs[oi].size()))) {
                    if (error) *error = "can't write " + name;
                    return false;
                }
                images << (nImage ? "," : "") << "{\"uri\":\"" << escape(name) << "\"}";
            }
            ++nImage;
        }
        materials << "\"metallicFactor\":" << o.metal << ",\"roughnessFactor\":" << o.rough << "}}";
        const glm::vec3 t = toGltf(o.position);
        nodes << (nMesh ? "," : "") << "{\"name\":\"" << escape(o.name) << "\",\"mesh\":" << nMesh << ",\"translation\":[" << t.x << "," << t.y << ","
              << t.z << "]}";
        roots.push_back(nMesh);
        ++nMesh;
    }
    if (!nMesh) {
        if (error) *error = "nothing to export (no visible objects)";
        return false;
    }
    const std::string binName = p.stem().string() + ".bin";
    std::ostringstream j;
    j << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Fry3DX\"},\"scene\":0,\"scenes\":[{\"nodes\":[";
    for (size_t i = 0; i < roots.size(); ++i) j << (i ? "," : "") << roots[i];
    j << "]}],\"nodes\":[" << nodes.str() << "],\"meshes\":[" << meshes.str() << "],\"materials\":[" << materials.str() << "],\"accessors\":["
      << accessors.str() << "]";
    if (nImage) {
        j << ",\"samplers\":[{}],\"images\":[" << images.str() << "],\"textures\":[";
        for (int i = 0; i < nImage; ++i) j << (i ? "," : "") << "{\"sampler\":0,\"source\":" << i << "}";
        j << "]";
    }
    j << ",\"bufferViews\":[" << views.str() << "],\"buffers\":[{";
    if (!binary) j << "\"uri\":\"" << escape(binName) << "\",";
    j << "\"byteLength\":" << bin.size() << "}]}";
    if (binary) { // .glb: a header, the JSON, then the binary, each padded to 4 bytes
        std::string json = j.str();
        while (json.size() % 4) json += ' ';
        while (bin.size() % 4) bin.push_back(0);
        std::vector<uint8_t> out;
        auto u32 = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
        u32(0x46546C67), u32(2), u32(uint32_t(12 + 8 + json.size() + 8 + bin.size()));
        u32(uint32_t(json.size())), u32(0x4E4F534A), out.insert(out.end(), json.begin(), json.end());
        u32(uint32_t(bin.size())), u32(0x004E4942), out.insert(out.end(), bin.begin(), bin.end());
        return writeFileSafely(path, out.data(), out.size(), error);
    }
    j << "\n";
    std::ofstream jf(path, std::ios::binary), bf((p.parent_path() / binName).string(), std::ios::binary);
    if (!(jf << j.str()) || !bf.write((const char*)bin.data(), std::streamsize(bin.size()))) {
        if (error) *error = "can't write " + path;
        return false;
    }
    return true;
}

} // namespace fry
