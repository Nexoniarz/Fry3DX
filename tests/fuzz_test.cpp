// Shaking the modelling core: random shapes put through random edits, combined at random, and
// files made of nonsense or with bits knocked out of them read. Nothing may crash or hang, every
// face must point at points that are there, and shapes closed going into a combine come out
// closed. The same each run (a fixed seed); fuzz_test N for more rounds, fuzz_test N S another seed.
#include "import.h"
#include "mesh.h"
#include "scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>

using namespace fry;

namespace {

std::mt19937 rng;
int failures = 0;
std::string lastEdit; // (what the last edit was, exactly, for when it went wrong)

// A mesh as an OBJ (to look at what went wrong).
void saveObj(const Mesh& m, const std::string& path) {
    std::ofstream out(path);
    for (const glm::vec3& v : m.verts) out << "v " << v.x << " " << v.y << " " << v.z << "\n";
    for (const auto& f : m.faces) {
        out << "f";
        for (int v : f) out << " " << v + 1;
        out << "\n";
    }
}

float uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

void fail(int round, const char* what, const char* step) {
    ++failures;
    std::printf("FAIL round %d: %s (after %s)\n", round, what, step);
}

// Every face has 2 or more points and each is a point of the mesh; no point is NaN or infinite.
bool whole(const Mesh& m) {
    for (const glm::vec3& v : m.verts)
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
    for (const auto& f : m.faces) {
        if (f.size() < 2) return false;
        for (int v : f)
            if (v < 0 || size_t(v) >= m.verts.size()) return false;
    }
    return true;
}

Mesh randomShape() {
    Mesh m;
    switch (pick(0, 6)) {
    case 0: m = makeCube(uniform(0.1f, 2)); break;
    case 1: m = makeCylinder(pick(3, 40), uniform(0.05f, 1), uniform(0.05f, 2)); break;
    case 2: m = makeSphere(pick(3, 32), pick(2, 16), uniform(0.05f, 1)); break;
    case 3: m = makeCone(pick(3, 30), uniform(0.05f, 1), uniform(0.05f, 2)); break;
    case 4: m = makeTorus(pick(3, 32), pick(3, 16), uniform(0.2f, 1), uniform(0.02f, 0.19f)); break;
    case 5: m = makeFrustum(pick(3, 24), uniform(0.05f, 1), uniform(0.0f, 1), uniform(0.05f, 2)); break;
    default: m = makePlane(uniform(0.1f, 2)); break;
    }
    glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(uniform(-0.5f, 0.5f), uniform(-0.5f, 0.5f), uniform(-0.5f, 0.5f)));
    t = glm::rotate(t, uniform(0, 6.3f), glm::normalize(glm::vec3(uniform(-1, 1), uniform(-1, 1), uniform(0.1f, 1))));
    t = glm::scale(t, glm::vec3(uniform(0.3f, 2), uniform(0.3f, 2), uniform(0.3f, 2)));
    transform(m, t);
    return m;
}

std::set<int> someFaces(const Mesh& m) {
    std::set<int> s;
    if (m.faces.empty()) return s;
    for (int i = pick(1, 4); i > 0; --i) s.insert(pick(0, int(m.faces.size()) - 1));
    return s;
}
std::set<int> somePoints(const Mesh& m) {
    std::set<int> s;
    if (m.verts.empty()) return s;
    for (int i = pick(1, 6); i > 0; --i) s.insert(pick(0, int(m.verts.size()) - 1));
    return s;
}
std::pair<int, int> someEdge(const Mesh& m) {
    const auto e = edges(m);
    return e.empty() ? std::pair(0, 0) : e[size_t(pick(0, int(e.size()) - 1))];
}

// One random edit; its name, for when something's wrong after it.
const char* randomEdit(Mesh& m) {
    if (m.faces.empty()) return "nothing";
    switch (pick(0, 17)) {
    case 0: {
        const auto fs = someFaces(m);
        const float d = uniform(-0.3f, 0.5f);
        lastEdit = "extrude " + std::to_string(d) + " faces";
        for (int f : fs) lastEdit += " " + std::to_string(f);
        extrudeFaces(m, fs, d);
        return "extrude";
    }
    case 1: insetFaces(m, someFaces(m), uniform(0, 0.95f)); return "inset";
    case 2: flipFaces(m, someFaces(m)); return "flip";
    case 3: deleteFaces(m, someFaces(m)); return "delete faces";
    case 4: deleteVerts(m, somePoints(m)); return "delete points";
    case 5: mergeByDistance(m, uniform(0, 0.2f)); return "merge by distance";
    case 6: mergeToCenter(m, somePoints(m)); return "merge to centre";
    case 7: {
        std::set<std::pair<int, int>> e;
        for (int i = pick(1, 3); i > 0; --i) e.insert(someEdge(m));
        bevelEdges(m, e, uniform(0.001f, 0.2f), pick(1, 4));
        return "bevel";
    }
    case 8:
        if (m.faces.size() < 3000) subdivide(m, pick(0, 1), uniform(0, 180));
        return "subdivide";
    case 9: loopCut(m, someEdge(m), pick(1, 3)); return "loop cut";
    case 10: smoothPoints(m, somePoints(m), uniform(0, 1), pick(1, 3)); return "smooth points";
    case 11: pushPoints(m, somePoints(m), uniform(-0.2f, 0.2f)); return "push points";
    case 12: closeHoles(m); return "close holes";
    case 13: faceOutward(m); return "face outward";
    case 14: repairForPrint(m); return "repair";
    case 15: mergeFlatFaces(m); return "merge flat faces";
    case 16: {
        const auto fs = someFaces(m);
        if (fs.size() >= 2) bridgeFaces(m, *fs.begin(), *fs.rbegin());
        return "bridge";
    }
    default: {
        const int f = pick(0, int(m.faces.size()) - 1);
        if (m.faces[size_t(f)].size() >= 3) cutHole(m, f, holeOutline(m, f, faceCenter(m, f), uniform(0.01f, 0.4f), pick(3, 24)), uniform(-1, 0.5f));
        return "hole";
    }
    }
}

// Whatever comes out, looked at the ways the app will (it mustn't crash those either).
void lookAt(const Mesh& m) {
    for (size_t f = 0; f < m.faces.size(); ++f)
        if (m.faces[f].size() >= 3) (void)triangulate(m, int(f));
    (void)checkForPrint(m);
    (void)cornerNormals(m, true, 30);
    (void)sliceAt(m, (boundsMin(m).z + boundsMax(m).z) * 0.5f);
    bool loop = false;
    (void)alongPath(pathOf(m, loop), loop, 7);
}

// Bytes knocked about: some changed, some cut out, some put in twice.
std::string mangle(std::string s) {
    for (int i = pick(1, 12); i > 0 && !s.empty(); --i) {
        const size_t at = size_t(pick(0, int(s.size()) - 1));
        switch (pick(0, 3)) {
        case 0: s[at] = char(pick(0, 255)); break;
        case 1: s.erase(at, size_t(pick(1, 40))); break;
        case 2: s.insert(at, s.substr(at, size_t(pick(1, 40)))); break;
        default: s.resize(at); break;
        }
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 150;
    rng.seed(argc > 2 ? unsigned(std::atoi(argv[2])) : 20261021u);
    const auto start = std::chrono::steady_clock::now();
    int combined = 0, closedIn = 0;
    for (int r = 0; r < rounds; ++r) {
        // Edits, one after another, on one shape.
        Mesh m = randomShape();
        for (int k = pick(1, 6); k > 0; --k) {
            const Mesh before = m;
            const char* step = randomEdit(m);
            if (!whole(m)) {
                if (std::getenv("FRY_FUZZ_KEEP")) {
                    saveObj(before, std::getenv("FRY_FUZZ_KEEP")), std::printf("  %s\n", lastEdit.c_str());
                    for (const glm::vec3& v : m.verts) std::printf("  (%g %g %g)", v.x, v.y, v.z);
                    std::printf("\n");
                }
                fail(r, "a face points at a point that isn't there, or a point isn't a number", step);
                break;
            }
            lookAt(m);
        }
        // Two shapes combined: closed going in, closed coming out.
        Mesh a = randomShape();
        const Mesh b = randomShape();
        const bool bothClosed = checkForPrint(a).closed() && checkForPrint(b).closed();
        const Combine op = Combine(pick(0, 2));
        const bool any = combine(a, b, op);
        ++combined;
        if (!whole(a)) fail(r, "combined: broken", "combine");
        else if (bothClosed && any) {
            ++closedIn;
            const PrintCheck c = checkForPrint(a);
            if (!c.closed()) {
                std::printf("  (op %d: %d open edges, %zu faces)\n", int(op), c.openEdges, a.faces.size());
                fail(r, "two closed shapes combined into one that isn't closed", "combine");
            }
        }
        lookAt(a);
        // Spun, as the Spin window does (an open shape: a plane).
        Mesh s = spin(makePlane(uniform(0.1f, 1)), glm::vec3(uniform(-1, 1), 0, 0), glm::vec3(0, 0, 1), uniform(-720, 720), pick(1, 64), uniform(-1, 1));
        if (!whole(s)) fail(r, "spun: broken", "spin");
    }
    // Files: nonsense, and real ones with bits knocked out, read as each kind.
    const auto dir = std::filesystem::temp_directory_path() / "fry3dx_fuzz";
    std::filesystem::create_directories(dir);
    Scene sc;
    for (int i = 0; i < 3; ++i) {
        Object o;
        o.id = sc.nextId++, o.name = "o" + std::to_string(i), o.mesh = randomShape(), o.mirror = i, o.cuts = i == 2 ? 1 : 0;
        sc.objects.push_back(o);
    }
    const std::string sceneFile = sceneText(sc);
    std::string objFile = "o thing\n";
    for (const glm::vec3& v : sc.objects[0].mesh.verts) objFile += "v " + std::to_string(v.x) + " " + std::to_string(v.y) + " " + std::to_string(v.z) + "\n";
    for (const auto& f : sc.objects[0].mesh.faces) {
        objFile += "f";
        for (int v : f) objFile += " " + std::to_string(v + 1);
        objFile += "\n";
    }
    std::string stl = "solid x\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1 0 0\n   vertex 0 1 0\n  endloop\n endfacet\nendsolid x\n";
    const int files = std::max(30, rounds);
    for (int i = 0; i < files; ++i) {
        const std::string text = mangle(sceneFile);
        std::ofstream(dir / "last.fry", std::ios::binary) << text; // (left there if it crashes: the file that did it)
        std::istringstream in(text);
        if (auto s = parseScene(in, "fuzz.fry")) {
            for (const Object& o : s->objects)
                if (!whole(o.mesh)) fail(i, "a scene read with a broken object", "parseScene");
            (void)withCutsMade(*s);
        }
        const char* ext[] = {".obj", ".stl", ".3mf", ".glb", ".gltf"};
        const int kind = pick(0, 4);
        std::string bytes = kind == 0 ? mangle(objFile) : kind == 1 ? mangle(stl) : std::string();
        if (bytes.empty())
            for (int n = pick(0, 600); n > 0; --n) bytes += char(pick(0, 255));
        if (kind == 2 && pick(0, 1)) bytes = "PK\x03\x04" + bytes; // (looks like a zip at first)
        if (kind == 3 && pick(0, 1)) bytes = std::string("glTF\x02\x00\x00\x00", 8) + bytes;
        const auto path = (dir / ("f" + std::string(ext[kind]))).string();
        std::ofstream(path, std::ios::binary) << bytes;
        for (const ImportedMesh& im : importModel(path))
            if (!whole(im.mesh)) fail(i, "a file read into a broken mesh", ext[kind]);
        std::vector<uint8_t> raw(bytes.begin(), bytes.end());
        (void)inflate(raw.data(), raw.size(), size_t(pick(0, 4096)));
    }
    std::filesystem::remove_all(dir);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%d rounds, %d combines (%d of closed shapes), %d files: %s in %.1f s\n", rounds, combined, closedIn, files * 2, failures ? "FAILED" : "passed", s);
    return failures ? 1 : 0;
}
