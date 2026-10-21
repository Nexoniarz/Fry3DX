// The modelling core without a window: shapes are closed and face out, faces with dents
// triangulate right, extrude / inset / merge keep the mesh whole, and files round-trip.
#include "bvh.h"
#include "import.h"
#include "text.h"
#include "mesh.h"
#include "image.h"
#include "scene.h"
#include "shapes.h"
#include "sculpt.h"

#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <fstream>
#include <map>

using namespace fry;

static int g_failures = 0;
#define CHECK(cond, ...)                                          \
    do {                                                          \
        if (!(cond)) ++g_failures, std::printf("FAIL: " __VA_ARGS__); \
        else std::printf("ok: " __VA_ARGS__);                     \
        std::printf("\n");                                        \
    } while (0)

// Closed: every edge has exactly two faces, running opposite ways along it.
static bool closed(const Mesh& m) {
    std::map<std::pair<int, int>, int> e;
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) ++e[{f[i], f[(i + 1) % f.size()]}];
    for (auto [k, n] : e)
        if (n != 1 || e[{k.second, k.first}] != 1) return false;
    return true;
}
// Faces out: the volume (divergence theorem) is positive.
static float volume(const Mesh& m) {
    float v = 0;
    for (size_t f = 0; f < m.faces.size(); ++f)
        for (const auto& t : triangulate(m, int(f))) v += glm::dot(m.verts[size_t(t[0])], glm::cross(m.verts[size_t(t[1])], m.verts[size_t(t[2])])) / 6.0f;
    return v;
}

int main() {
    const Mesh cube = makeCube(2.0f);
    CHECK(cube.verts.size() == 8 && cube.faces.size() == 6 && closed(cube), "a cube: 8 points, 6 faces, closed");
    CHECK(std::abs(volume(cube) - 8.0f) < 1e-4f, "its faces face out (volume %.3f)", volume(cube));
    for (const auto& [name, m] : {std::pair{"cylinder", makeCylinder()}, {"cone", makeCone()}, {"sphere", makeSphere()}, {"torus", makeTorus()}})
        CHECK(closed(m) && volume(m) > 0.0f, "a %s is closed and faces out (volume %.3f)", name, volume(m));

    // An L: six corners, one dent. Ear clipping gives 4 triangles covering its 3 m².
    const Mesh l{{{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}, {1, 2, 0}, {0, 2, 0}}, {{0, 1, 2, 3, 4, 5}}};
    const auto tris = triangulate(l, 0);
    float area = 0;
    for (const auto& t : tris) area += glm::length(glm::cross(l.verts[size_t(t[1])] - l.verts[size_t(t[0])], l.verts[size_t(t[2])] - l.verts[size_t(t[0])])) / 2;
    CHECK(tris.size() == 4 && std::abs(area - 3.0f) < 1e-4f, "an L shape triangulates (%zu triangles, %.3f m2)", tris.size(), area);

    // A drawn outline made solid: closed, the right thickness.
    const Mesh shape = extrudeOutline({{0, 0, 0}, {0, 2, 0}, {0, 2, 1}, {0, 1, 1}, {0, 1, 2}, {0, 0, 2}}, glm::vec3(1, 0, 0), 0.05f);
    CHECK(closed(shape) && std::abs(volume(shape) - 3.0f * 0.05f) < 1e-4f, "an outline made solid (volume %.4f)", volume(shape));

    // Extrude the cube's top up 1 m: 12 points, 10 faces, still closed, 4 m³ more.
    Mesh m = cube;
    extrudeFaces(m, {0}, 1.0f);
    CHECK(m.verts.size() == 12 && m.faces.size() == 10 && closed(m) && std::abs(volume(m) - 12.0f) < 1e-4f, "extrude pulls a face out (volume %.2f)", volume(m));
    insetFaces(m, {0}, 0.5f);
    CHECK(m.faces.size() == 14 && closed(m) && std::abs(volume(m) - 12.0f) < 1e-4f, "inset keeps it closed");
    Mesh d = m;
    mergeByDistance(d, 0.01f);
    CHECK(d.verts.size() == m.verts.size(), "merge by distance leaves separate points alone");
    Mesh two = cube;
    two.verts.push_back(two.verts[0] + glm::vec3(0.0001f));
    two.faces[1][0] = 8;
    CHECK(mergeByDistance(two, 0.001f) == 1 && closed(two), "and joins points on top of each other");
    deleteFaces(m, {0});
    CHECK(m.faces.size() == 13 && !closed(m), "deleting a face opens it");

    // Rounding: every edge of a cube, one edge, a rounded box; all stay closed, a bit smaller.
    for (int segs : {1, 3, 4}) {
        Mesh b = makeCube(1.0f);
        bevelEdges(b, sharpEdges(b, 30.0f), 0.1f, segs);
        CHECK(closed(b) && volume(b) < 1.0f && volume(b) > 0.93f, "rounding all a cube's edges, %d steps: closed (volume %.4f)", segs, volume(b));
    }
    {
        Mesh b = makeCube(1.0f);
        bevelEdges(b, {{4, 5}}, 0.2f, 1);
        CHECK(closed(b) && std::abs(volume(b) - (1.0f - 0.02f)) < 1e-3f, "cutting off one edge: closed, 0.02 m3 gone (volume %.4f)", volume(b));
        Mesh c = makeCube(1.0f);
        bevelEdges(c, {{4, 5}, {5, 7}}, 0.1f, 3);
        CHECK(closed(c), "rounding two edges that meet: closed");
    }
    const Mesh rb = makeRoundedBox({1, 1, 1}, 0.25f, 4);
    CHECK(closed(rb) && std::abs(volume(rb) - 0.86f) < 0.02f, "a rounded box is closed (volume %.4f)", volume(rb));
    Mesh sub = makeCube(1.0f);
    subdivide(sub, true);
    CHECK(sub.faces.size() == 24 && closed(sub) && volume(sub) < 1.0f, "smoothing a cube: 24 faces, closed, rounder (volume %.3f)", volume(sub));
    Mesh sharp = makeCube(1.0f);
    subdivide(sharp, true, 30.0f);
    CHECK(std::abs(volume(sharp) - 1.0f) < 1e-4f, "smoothing that keeps sharp edges keeps a cube a cube");
    Mesh cut = makeCube(1.0f);
    const auto made = loopCut(cut, {4, 5}, 2);
    CHECK(made.size() == 8 && cut.faces.size() == 14 && closed(cut) && std::abs(volume(cut) - 1.0f) < 1e-4f, "two loop cuts round a cube (%zu points, %zu faces)",
          made.size(), cut.faces.size());
    Mesh grid = makeCube(1.0f);
    subdivide(grid, false);
    std::pair<int, int> mid{-1, -1}; // an edge between a side's middle and an edge's middle: its loop goes all round
    for (auto e : edges(grid))
        if (e.first >= 8 && e.second >= 8) mid = e;
    CHECK(edgeLoop(grid, mid).size() == 8, "an edge loop goes all round (%zu edges)", edgeLoop(grid, mid).size());
    for (const ShapeRecipe& r : shapeRecipes()) {
        const Mesh m = makeFromRecipe(r, defaultSettings(r));
        const bool flat = std::string(r.id) == "plane" || std::string(r.id) == "circle";
        CHECK(!m.faces.empty() && std::abs(boundsMin(m).z) < 1e-4f && (flat || (closed(m) && volume(m) > 0)), "the %s stands on the ground%s", r.name,
              flat ? "" : ", closed, facing out");
    }
    Object fin;
    fin.mesh = makeCube(1.0f), fin.mirror = 1, fin.round = 0.05f;
    fin.mesh.verts[0].x = fin.mesh.verts[2].x = fin.mesh.verts[4].x = fin.mesh.verts[6].x = 0.0f; // (a half cube on the mirror line)
    const Mesh fm = finishedMesh(fin);
    CHECK(closed(fm) && std::abs(volume(fm) - 1.0f) < 0.05f, "a half mirrored, then rounded: one closed piece (volume %.3f)", volume(fm));

    // The knife: a line seen from the front (+X). Only what's seen: the front face is cut in two.
    {
        Mesh k = makeCube(1.0f);
        const glm::vec3 eye(5, 0, 0);
        const auto cutEdges = knifeCut(k, eye, {{0, 0.1f, 2}, {0, 0.1f, -2}}, false);
        CHECK(cutEdges.size() == 1 && k.faces.size() == 7 && closed(k), "the knife cuts the face it's drawn over (%zu edges, %zu faces)", cutEdges.size(), k.faces.size());
        Mesh t = makeCube(1.0f);
        const auto all = knifeCut(t, eye, {{0, 0.1f, 2}, {0, 0.1f, -2}}, true);
        CHECK(all.size() == 4 && closed(t), "through: all the way round (%zu edges)", all.size());
        const auto pieces = splitAlong(t, all, true);
        float v = 0;
        for (const auto& p : pieces) v += volumeOf(t, p);
        CHECK(pieces.size() == 2 && closed(t) && std::abs(v - 1.0f) < 1e-3f && std::abs(volume(t) - 1.0f) < 1e-3f, "pulled apart and filled: 2 closed pieces (%zu, volume %.4f)",
              pieces.size(), v);
        Mesh o = makeCube(1.0f);
        const auto e2 = knifeCut(o, eye, {{0, 0.1f, 2}, {0, 0.1f, -2}}, true);
        splitAlong(o, e2, false);
        CHECK(!closed(o) && o.faces.size() == 10, "pulled apart, left open: hollow (%zu faces)", o.faces.size());
        Mesh l = makeCube(1.0f); // a bent line: an L across the front
        const auto le = knifeCut(l, eye, {{0, 0.6f, 0.2f}, {0, -0.2f, 0.2f}, {0, -0.2f, -0.6f}}, false);
        CHECK(le.size() >= 2 && closed(l), "a bent line cuts along both its steps (%zu edges)", le.size());
    }
    // Holes: round, through a cube and as a pocket.
    {
        auto frontFace = [](const Mesh& m) {
            for (size_t f = 0; f < m.faces.size(); ++f)
                if (faceNormal(m, int(f)).x > 0.99f) return int(f);
            return -1;
        };
        Mesh h = makeCube(1.0f);
        int f = frontFace(h);
        const auto ring = holeOutline(h, f, faceCenter(h, f), 0.2f, 16);
        const float hole = 8 * 0.04f * std::sin(2 * 3.14159265f / 16);
        CHECK(cutHole(h, f, ring, 0) && closed(h) && std::abs(volume(h) - (1 - hole)) < 1e-3f, "a round hole all the way through (volume %.4f)", volume(h));
        Mesh p = makeCube(1.0f);
        f = frontFace(p);
        CHECK(cutHole(p, f, holeOutline(p, f, faceCenter(p, f) + glm::vec3(0, 0.1f, 0), 0.15f, 4), 0.3f) && closed(p) &&
                  std::abs(volume(p) - (1 - 0.09f * 0.3f)) < 1e-3f,
              "a square pocket 0.3 deep (volume %.4f)", volume(p));
        Mesh big = makeCube(1.0f);
        f = frontFace(big);
        CHECK(!cutHole(big, f, holeOutline(big, f, faceCenter(big, f), 0.7f, 16), 0) && big.faces.size() == 6, "a hole bigger than the face: refused, nothing changed");
    }
    // Laid out flat for a picture: everything inside 0..1, nothing on top of anything else.
    {
        Mesh u = makeCylinder(16, 0.5f, 1.0f);
        const auto uv = autoUnwrap(u);
        bool inside = uv.size() == u.faces.size();
        float area = 0;
        for (size_t f = 0; f < uv.size(); ++f) {
            inside &= uv[f].size() == u.faces[f].size();
            for (size_t i = 0; i < uv[f].size(); ++i) {
                inside &= uv[f][i].x >= 0 && uv[f][i].x <= 1 && uv[f][i].y >= 0 && uv[f][i].y <= 1;
                const glm::vec2 a = uv[f][i], b = uv[f][(i + 1) % uv[f].size()];
                area += (a.x * b.y - b.x * a.y) * 0.5f;
            }
        }
        CHECK(inside && area < -0.2f && area >= -1.0f, "laid flat inside the picture, not mirrored (area %.3f: y runs down)", area);
    }

    // Pictures: a stamped square bent into a corner pin lands where its corners say.
    {
        Image dst = makeImage(64, 64, {0, 0, 0, 1}), src = makeImage(8, 8, {1, 0, 0, 1});
        stampQuad(dst, src, {glm::vec2(10, 10), glm::vec2(50, 14), glm::vec2(40, 50), glm::vec2(12, 40)});
        const glm::vec2 mid = mapSquare(squareToQuad({glm::vec2(10, 10), glm::vec2(50, 14), glm::vec2(40, 50), glm::vec2(12, 40)}), {0.5f, 0.5f});
        CHECK(pixel(dst, int(mid.x), int(mid.y)).r > 0.9f && pixel(dst, 2, 2).r < 0.1f && pixel(dst, 60, 60).r < 0.1f, "a picture pinned by its corners");
        Image f = makeImage(16, 16, {1, 1, 1, 1});
        for (int y = 0; y < 16; ++y) f.at(8, y)[0] = 0; // a line down the middle
        floodFill(f, {2, 2}, {0, 0, 1, 1}, 0.1f);
        CHECK(pixel(f, 0, 15).b > 0.9f && pixel(f, 0, 15).r < 0.1f && pixel(f, 12, 3).r > 0.9f, "fill stops at a line");
        Stroke st;
        Image s2 = makeImage(16, 16, {1, 1, 1, 1});
        st.begin(s2, {0, 0, 0, 0.5f});
        st.dab(s2, {8, 8}, 4, 1), st.dab(s2, {8, 8}, 4, 1);
        CHECK(std::abs(pixel(s2, 8, 8).r - 0.5f) < 0.01f, "a stroke over itself doesn't build up");
    }

    // A sticker hanging over the top edge of a cube's front folds onto the top, and no further.
    {
        Object o;
        o.mesh = makeCube(1.0f), o.uvs = autoUnwrap(o.mesh), o.picture = std::make_shared<Image>(makeImage(256, 256, {1, 1, 1, 1}));
        int front = 0;
        for (size_t f = 0; f < o.mesh.faces.size(); ++f)
            if (faceNormal(o.mesh, int(f)).x > 0.99f) front = int(f);
        PictureLayer l;
        l.image = std::make_shared<Image>(makeImage(8, 8, {1, 0, 0, 1}));
        l.face = front, l.centre = {0.5f, 0, 0.4f}, l.up = {0, 0, 1}, l.width = l.height = 0.4f;
        o.layers.push_back(l);
        std::vector<uint8_t> owner;
        composePicture(o, &owner);
        auto covered = [&](int f) {
            glm::vec2 lo(1), hi(0);
            for (glm::vec2 q : o.uvs[size_t(f)]) lo = glm::min(lo, q), hi = glm::max(hi, q);
            int n = 0;
            for (int y = int(lo.y * 256) + 2; y < int(hi.y * 256) - 2; ++y)
                for (int x = int(lo.x * 256) + 2; x < int(hi.x * 256) - 2; ++x) n += owner[size_t(y) * 256 + size_t(x)] == 1;
            return n;
        };
        int top = 0, bottom = 0, back = 0;
        for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
            const glm::vec3 n = faceNormal(o.mesh, int(f));
            if (n.z > 0.99f) top = covered(int(f));
            if (n.z < -0.99f) bottom = covered(int(f));
            if (n.x < -0.99f) back = covered(int(f));
        }
        CHECK(covered(front) > 200 && top > 50 && bottom == 0 && back == 0, "a sticker over an edge folds onto the next face (front %d, top %d, bottom %d)", covered(front), top, bottom);
    }

    // At a corner each face gets the part of the sticker that reaches it straight: a sticker of four
    // colours (red top-left, green top-right, blue bottom-left, white bottom-right) at the front's
    // top-right corner puts red and green on the top, green and white on the right, no blue there.
    {
        Object o;
        o.mesh = makeCube(1.0f), o.uvs = autoUnwrap(o.mesh), o.picture = std::make_shared<Image>(makeImage(256, 256, {0, 0, 0, 1}));
        Image four = makeImage(64, 64, {1, 1, 1, 1});
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                uint8_t* p = four.at(x, y);
                if (y < 32) p[0] = x < 32 ? 255 : 0, p[1] = x < 32 ? 0 : 255, p[2] = 0;
                else if (x < 32) p[0] = 0, p[1] = 0, p[2] = 255;
            }
        int front = 0;
        for (size_t f = 0; f < o.mesh.faces.size(); ++f)
            if (faceNormal(o.mesh, int(f)).x > 0.99f) front = int(f);
        PictureLayer l;
        l.image = std::make_shared<Image>(four);
        l.face = front, l.centre = {0.5f, 0.42f, 0.42f}, l.up = {0, 0, 1}, l.width = l.height = 0.4f;
        o.layers.push_back(l);
        const Image shown = composePicture(o);
        auto count = [&](glm::vec3 n, int r, int g, int b) {
            int c = 0;
            for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
                if (glm::dot(faceNormal(o.mesh, int(f)), n) < 0.99f) continue;
                glm::vec2 lo(1), hi(0);
                for (glm::vec2 q : o.uvs[f]) lo = glm::min(lo, q), hi = glm::max(hi, q);
                for (int y = int(lo.y * 256) + 2; y < int(hi.y * 256) - 2; ++y)
                    for (int x = int(lo.x * 256) + 2; x < int(hi.x * 256) - 2; ++x) {
                        const uint8_t* p = shown.px.data() + (size_t(y) * 256 + size_t(x)) * 4;
                        c += std::abs(p[0] - r) < 60 && std::abs(p[1] - g) < 60 && std::abs(p[2] - b) < 60;
                    }
            }
            return c;
        };
        const glm::vec3 top(0, 0, 1), right(0, 1, 0);
        // Moved so its middle is past the top edge: it goes onto the top, bent down over the front.
        PictureLayer moved = l;
        moved.centre = {0.5f, 0.0f, 0.6f};
        settleSticker(o.mesh, moved);
        CHECK(faceNormal(o.mesh, moved.face).z > 0.99f && glm::distance(moved.centre, glm::vec3(0.4f, 0.0f, 0.5f)) < 1e-3f && glm::dot(moved.up, glm::vec3(-1, 0, 0)) > 0.99f,
              "a sticker's middle past an edge: it moves onto that face (%g %g %g)", moved.centre.x, moved.centre.y, moved.centre.z);
        CHECK(count(top, 255, 0, 0) > 20 && count(top, 0, 255, 0) > 20 && count(top, 0, 0, 255) == 0 && count(right, 0, 255, 0) > 20 &&
                  count(right, 255, 255, 255) > 20 && count(right, 255, 0, 0) == 0 && count(right, 0, 0, 255) == 0,
              "a sticker at a corner: each face gets its own part (top red %d green %d blue %d; right green %d white %d red %d blue %d)", count(top, 255, 0, 0),
              count(top, 0, 255, 0), count(top, 0, 0, 255), count(right, 0, 255, 0), count(right, 255, 255, 255), count(right, 255, 0, 0), count(right, 0, 0, 255));
    }

    // For printing: a cube is closed and right way out; with a face gone it has a hole, closed
    // again by Close holes; turned inside out, Face outward turns it back.
    {
        Mesh m = makeCube(1.0f);
        PrintCheck c = checkForPrint(m, 45, -0.5f);
        CHECK(c.closed() && !c.insideOut && std::abs(c.volume - 1) < 1e-4f && c.pieces == 1 && c.overhangArea == 0, "a cube is ready to print");
        m.faces.pop_back();
        c = checkForPrint(m, 45, -0.5f);
        CHECK(c.openEdges == 4 && closeHoles(m) == 1 && checkForPrint(m).closed() && std::abs(checkForPrint(m).volume - 1) < 1e-4f, "a hole found and closed");
        std::reverse(m.faces[2].begin(), m.faces[2].end());
        CHECK(checkForPrint(m).flippedEdges == 4 && faceOutward(m) == 1 && checkForPrint(m).closed() && checkForPrint(m).volume > 0, "a face turned the wrong way, turned back");
        for (auto& f : m.faces) std::reverse(f.begin(), f.end());
        CHECK(checkForPrint(m).insideOut && faceOutward(m) == 6 && !checkForPrint(m).insideOut, "inside out, turned right way out");
        Mesh t = makeCube(1.0f);
        transform(t, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 2)));
        CHECK(std::abs(checkForPrint(t, 45, -0.5f).overhangArea - 1) < 1e-4f, "a cube held up in the air overhangs by its bottom");
    }
    // The layout's frames: a cube laid out in six; one moved with what's painted on it.
    {
        Mesh m = makeCube(1.0f);
        auto uv = autoUnwrap(m);
        const auto islands = uvIslands(m, uv);
        CHECK(!islands.empty() && islands.size() <= 6, "a cube's layout has frames (%zu)", islands.size());
        Image im = makeImage(64, 64, {0, 0, 0, 1});
        int f = islands[0][0];
        glm::vec2 c(0);
        for (glm::vec2 q : uv[size_t(f)]) c += q / float(uv[size_t(f)].size());
        im.at(int(c.x * 64), int(c.y * 64))[0] = 255;
        im.at(int(c.x * 64) + 1, int(c.y * 64))[0] = 255, im.at(int(c.x * 64), int(c.y * 64) + 1)[0] = 255, im.at(int(c.x * 64) + 1, int(c.y * 64) + 1)[0] = 255;
        auto moved = uv;
        for (glm::vec2& q : moved[size_t(f)]) q += glm::vec2(0.0f, 0.0f);
        remapPicture(im, m, uv, moved, {f});
        CHECK(im.at(int(c.x * 64), int(c.y * 64))[0] > 100, "moving a frame nowhere keeps what's painted");
    }
    std::filesystem::create_directories(std::filesystem::temp_directory_path() / "fry3dx_test");
    CHECK(exportModel((std::filesystem::temp_directory_path() / "fry3dx_test" / "c.3mf").string(), [] { Scene s; Object o; o.mesh = makeCube(0.02f); s.objects.push_back(o); return s; }(), {}),
          "exports 3MF");

    // Hole shapes: a hexagon for an M3 nut is 5.5 across its flats; a 20 x 10 slot; a triangle.
    // Outline only: the face gets a new face inside, nothing taken away (the volume stays).
    {
        Mesh m = makeCube(0.04f);
        int top = 0;
        for (size_t f = 0; f < m.faces.size(); ++f)
            if (faceNormal(m, int(f)).z > 0.99f) top = int(f);
        const glm::vec3 c = faceCenter(m, top);
        const auto hex = holeOutline(m, top, c, HoleShape::Hexagon, 0.0055f);
        float flats = 1e9f;
        for (size_t i = 0; i < 6; ++i) flats = std::min(flats, glm::distance((hex[i] + hex[(i + 1) % 6]) * 0.5f, c) * 2.0f);
        CHECK(hex.size() == 6 && std::abs(flats - 0.0055f) < 1e-6f, "a hexagon 5.5 mm across its flats (%g)", flats * 1000);
        const auto slot = holeOutline(m, top, c, HoleShape::Rectangle, 0.02f, 0.01f);
        CHECK(slot.size() == 4 && std::abs(glm::distance(slot[0], slot[1]) - 0.02f) < 1e-6f && std::abs(glm::distance(slot[1], slot[2]) - 0.01f) < 1e-6f, "a 20 x 10 mm slot");
        const auto tri = holeOutline(m, top, c, HoleShape::Triangle, 0.012f);
        CHECK(tri.size() == 3 && std::abs(glm::distance(tri[0], tri[1]) - 0.012f) < 1e-6f, "a triangle with 12 mm sides");
        Mesh p = m;
        CHECK(cutHole(p, top, hex, 0.0024f) && std::abs(checkForPrint(p).volume - (0.04f * 0.04f * 0.04f - 0.0024f * 0.0055f * 0.0055f * std::sqrt(3.0f) / 2.0f)) < 1e-10f && checkForPrint(p).closed(),
              "a pocket for an M3 nut, 2.4 mm deep, closed");
        const size_t before = m.faces.size();
        const int f = imprintOutline(m, top, slot);
        CHECK(f >= 0 && m.faces.size() == before + 4 && std::abs(faceArea(m, f) - 0.0002f) < 1e-9f && checkForPrint(m).closed() && std::abs(checkForPrint(m).volume - 0.000064f) < 1e-10f,
              "outline only: a new 20 x 10 mm face, nothing taken away");
        CHECK(imprintOutline(m, top, holeOutline(m, top, c, HoleShape::Round, 0.08f)) < 0, "an outline bigger than the face isn't cut");
    }

    // Combining: two 1 m cubes half overlapping make 1.5 m3 added, 0.5 taken away, 0.5 overlapping;
    // each closed all round (ready to print), its flat sides joined back into whole faces.
    {
        Mesh b = makeCube(1.0f);
        transform(b, glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0, 0)));
        const char* names[] = {"added", "taken away", "overlapping"};
        const float want[] = {1.5f, 0.5f, 0.5f};
        for (int op = 0; op < 3; ++op) {
            Mesh a = makeCube(1.0f);
            const bool ok = combine(a, b, Combine(op));
            const PrintCheck c = checkForPrint(a);
            CHECK(ok && std::abs(c.volume - want[op]) < 1e-4f && c.closed() && a.faces.size() <= 14, "cubes %s: %g m3, %d open edges, %d flipped, %zu faces", names[op], c.volume,
                  c.openEdges, c.flippedEdges, a.faces.size());
        }
        Mesh a = makeCube(1.0f);
        const Mesh cyl = makeCylinder(24, 0.2f, 3.0f);
        Mesh drill = cyl;
        transform(drill, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -1.5f)));
        CHECK(combine(a, drill, Combine::Subtract) && checkForPrint(a).closed() && checkForPrint(a).volume < 1.0f && checkForPrint(a).volume > 0.85f,
              "a cylinder drilled through a cube: closed, %g m3", checkForPrint(a).volume);
    }
    // Joining points is quick on a big mesh: a sphere in loose triangles, as an STL brings it.
    {
        const Mesh sphere = makeSphere(200, 100, 1.0f);
        Mesh soup;
        for (size_t f = 0; f < sphere.faces.size(); ++f)
            for (const auto& t : triangulate(sphere, int(f))) {
                soup.faces.push_back({int(soup.verts.size()), int(soup.verts.size()) + 1, int(soup.verts.size()) + 2});
                for (int v : t) soup.verts.push_back(sphere.verts[size_t(v)]);
            }
        const size_t loose = soup.verts.size();
        const auto t0 = std::chrono::steady_clock::now();
        mergeByDistance(soup, 1e-6f);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(soup.verts.size() == sphere.verts.size() && ms < 3000, "%zu loose points joined to %zu in %.0f ms", loose, soup.verts.size(), ms);
    }
    // Repair: a cube as an STL brings it (each face its own corners), one face missing, one
    // turned in, one there twice, an inside wall: closed and the right way out after.
    {
        const Mesh good = makeCube(1.0f);
        Mesh bad;
        for (size_t f = 0; f < good.faces.size(); ++f) {
            if (f == 2) continue; // (missing)
            std::vector<int> face;
            for (int v : good.faces[f]) face.push_back(int(bad.verts.size())), bad.verts.push_back(good.verts[size_t(v)] + glm::vec3(f == 4 ? 1e-8f : 0.0f));
            if (f == 3) std::reverse(face.begin(), face.end()); // (turned in)
            bad.faces.push_back(face);
            if (f == 5) bad.faces.push_back(face); // (twice)
        }
        bad.faces.push_back({0, 1, 1}); // (no area)
        const int a = int(bad.verts.size());
        bad.verts.insert(bad.verts.end(), {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}});
        bad.faces.push_back({a, a + 1, a + 2, a + 3}), bad.faces.push_back({a + 3, a + 2, a + 1, a}); // (an inside wall, back to back)
        CHECK(!checkForPrint(bad).closed(), "the broken cube is broken");
        const Repair r = repairForPrint(bad);
        const PrintCheck c = checkForPrint(bad);
        CHECK(c.closed() && !c.insideOut && std::abs(c.volume - 1.0f) < 1e-4f && r.closedHoles == 1 && r.doubledFaces == 3 && r.emptyFaces == 1 && r.joinedPoints > 0,
              "repair: closed, outwards, 1 m3 (joined %d, empty %d, doubled %d, turned %d, holes %d)", r.joinedPoints, r.emptyFaces, r.doubledFaces, r.turnedFaces, r.closedHoles);
        Mesh fine = makeCube(1.0f);
        CHECK(repairForPrint(fine).total() == 0 && fine.faces == good.faces, "a good cube: nothing to repair");
    }
    // Spin: a square beside the axis turned all the way round is a ring (Pappus: its area times
    // how far its middle goes); a quarter turn has ends; a screw rises.
    {
        const float kPi = 3.14159265f;
        Mesh sq{{{0.5f, 0, 0}, {1.0f, 0, 0}, {1.0f, 0, 0.5f}, {0.5f, 0, 0.5f}}, {{0, 1, 2, 3}}};
        const Mesh ring = spin(sq, glm::vec3(0), glm::vec3(0, 0, 1), 360, 96);
        const float want = 0.25f * 2 * kPi * 0.75f * std::sin(kPi / 96) / (kPi / 96);
        CHECK(checkForPrint(ring).closed() && std::abs(checkForPrint(ring).volume - want) < want * 2e-3f, "a square spun round: a ring (%.4f m3, want %.4f)", checkForPrint(ring).volume, want);
        const Mesh quarter = spin(sq, glm::vec3(0), glm::vec3(0, 0, 1), 90, 24);
        CHECK(checkForPrint(quarter).closed() && checkForPrint(quarter).volume > 0 && std::abs(checkForPrint(quarter).volume - want / 4) < want * 0.01f, "a quarter turn: closed, its ends on (%.4f)", checkForPrint(quarter).volume);
        const Mesh screw = spin(sq, glm::vec3(0), glm::vec3(0, 0, 1), 720, 96, 2.0f);
        CHECK(checkForPrint(screw).closed() && std::abs(boundsMax(screw).z - 2.5f) < 1e-4f, "a screw: two turns up 2 m, closed (top %.3f)", boundsMax(screw).z);
        Mesh tri{{{0, 0, 0}, {0.5f, 0, 0}, {0, 0, 1}}, {{0, 1, 2}}}; // (a side on the axis: a cone)
        const Mesh cone = spin(tri, glm::vec3(0), glm::vec3(0, 0, 1), 360, 64);
        const float coneWant = kPi * 0.25f / 3 * std::sin(2 * kPi / 64) / (2 * kPi / 64); // (a 64-sided cone's)
        CHECK(checkForPrint(cone).closed() && std::abs(checkForPrint(cone).volume - coneWant) < coneWant * 1e-3f, "a triangle on the axis spun: a closed cone (%.4f, want %.4f)", checkForPrint(cone).volume, coneWant);
    }
    // Text as a shape: letters with holes (O, A, B) come out closed, as tall as asked.
    {
        std::ifstream in(FRY_FONTS_DIR "/InstrumentSans-Bold.ttf", std::ios::binary);
        const std::vector<uint8_t> font{std::istreambuf_iterator<char>(in), {}};
        TextStyle st;
        st.height = 0.05f, st.depth = 0.01f;
        std::string err;
        const Mesh t = makeText(font, "Fry 3D", st, &err);
        const PrintCheck c = checkForPrint(t);
        CHECK(!t.faces.empty() && c.closed() && std::abs(boundsMax(t).z - 0.01f) < 1e-4f && boundsMax(t).y > 0.045f && boundsMax(t).y < 0.06f,
              "text \"Fry 3D\": %zu faces, closed %d, %.3f m tall %s", t.faces.size(), int(c.closed()), boundsMax(t).y, err.c_str());
        // (every face's triangles cover just the face: letters are dented, with points in straight lines)
        int covered = 0;
        for (size_t f = 0; f < t.faces.size(); ++f) {
            float a = 0;
            for (const auto& tr : triangulate(t, int(f))) a += 0.5f * glm::length(glm::cross(t.verts[size_t(tr[1])] - t.verts[size_t(tr[0])], t.verts[size_t(tr[2])] - t.verts[size_t(tr[0])]));
            covered += std::abs(a - faceArea(t, int(f))) <= faceArea(t, int(f)) * 1e-2f + 1e-12f; // (a sliver far out: float rounding)
        }
        CHECK(covered == int(t.faces.size()), "letters' faces made into triangles that cover them (%d of %zu)", covered, t.faces.size());
        // (letters drawn as overlapping strokes are whole: an F is as wide as an E, not just its stem)
        const float fw = boundsMax(makeText(font, "F", st)).x - boundsMin(makeText(font, "F", st)).x, ew = boundsMax(makeText(font, "E", st)).x - boundsMin(makeText(font, "E", st)).x;
        CHECK(fw > ew * 0.8f && checkForPrint(makeText(font, "F", st)).closed(), "an F is whole (%.1f mm wide, an E %.1f)", fw * 1000, ew * 1000);
        const Mesh o = makeText(font, "O", st);
        CHECK(checkForPrint(o).closed() && checkForPrint(o).volume < 0.05f * 0.04f * 0.01f, "an O has its hole (%g m3)", checkForPrint(o).volume);
        CHECK(makeText({}, "x", st, &err).faces.empty() && !err.empty(), "no font: nothing, and why");
    }
    // Creases: a cube smoothed with all its edges creased stays a cube; with none it rounds off;
    // mirrored, the copy's creases hold too; and they're saved.
    {
        Object o;
        o.id = 1, o.mesh = makeCube(1.0f), o.smoothLevels = 2;
        const float round = checkForPrint(finishedMesh(o)).volume;
        for (auto [a, b] : edges(o.mesh)) o.creases.insert({a, b});
        const float kept = checkForPrint(finishedMesh(o)).volume;
        CHECK(round < 0.8f && std::abs(kept - 1.0f) < 1e-4f, "creased all round: still a cube (%.3f; smooth %.3f)", kept, round);
        Object half;
        half.id = 2, half.mesh = makeCube(1.0f), half.smoothLevels = 2, half.mirror = 1;
        transform(half.mesh, glm::translate(glm::mat4(1), glm::vec3(0.5f, 0, 0)));
        for (auto [a, b] : edges(half.mesh)) half.creases.insert({a, b});
        const float both = checkForPrint(finishedMesh(half)).volume;
        CHECK(std::abs(both - 2.0f) < 1e-3f, "mirrored, creased: two cubes' worth, square (%.3f)", both);
        Scene sc;
        sc.objects.push_back(o);
        const auto file = (std::filesystem::temp_directory_path() / "fry3dx_test" / "crease.fry").string();
        std::filesystem::create_directories(std::filesystem::path(file).parent_path());
        auto back = saveScene(file, sc) ? loadScene(file) : std::nullopt;
        CHECK(back && back->objects.size() == 1 && back->objects[0].creases == o.creases, "creases saved and back");
    }
    // A slicer's layer: a 1 m cube cut halfway is a 4 m outline, a cylinder's a circle's.
    {
        float len = 0;
        for (const auto& [a, b] : sliceAt(makeCube(1.0f), 0.1f)) len += glm::distance(a, b);
        CHECK(std::abs(len - 4.0f) < 1e-4f, "a cube's slice: 4 m round (%.4f)", len);
        CHECK(sliceAt(makeCube(1.0f), 2.0f).empty(), "above it: nothing");
    }
    // Wall thickness: a box 2 mm thin one way is 2 mm thick on its big faces, 1 m the other ways.
    {
        Mesh slab = makeCube(1.0f);
        transform(slab, glm::scale(glm::mat4(1), glm::vec3(1.0f, 1.0f, 0.002f)));
        const std::vector<float> t = wallThickness(slab);
        int thin = 0, wide = 0;
        for (size_t f = 0; f < t.size(); ++f) thin += std::abs(t[f] - 0.002f) < 1e-5f, wide += std::abs(t[f] - 1.0f) < 1e-4f;
        CHECK(thin == 2 && wide == 4, "wall thickness: 2 faces 2 mm, 4 faces 1 m (%d, %d)", thin, wide);
    }
    // The tree of boxes finds the same nearest hit as trying every triangle (and the same "in the way").
    {
        Mesh m = makeSphere(32, 16, 1.0f);
        const glm::mat4 M = glm::translate(glm::mat4(1), glm::vec3(0.3f, -0.2f, 0.5f)) * glm::rotate(glm::mat4(1), 0.7f, glm::vec3(0, 1, 0));
        const TriangleTree tree(m, M);
        uint32_t seed = 7;
        auto rnd = [&] { return (seed = seed * 1664525u + 1013904223u) / 4294967296.0f * 2 - 1; };
        int same = 0, tries = 300;
        for (int i = 0; i < tries; ++i) {
            const glm::vec3 o(rnd() * 3, rnd() * 3, rnd() * 3 + 3), d = glm::normalize(glm::vec3(rnd(), rnd(), rnd() - 1.5f));
            float best = 1e30f;
            for (size_t f = 0; f < m.faces.size(); ++f)
                for (const auto& t : triangulate(m, int(f)))
                    if (auto h = rayTriangle(o, d, glm::vec3(M * glm::vec4(m.verts[size_t(t[0])], 1)), glm::vec3(M * glm::vec4(m.verts[size_t(t[1])], 1)), glm::vec3(M * glm::vec4(m.verts[size_t(t[2])], 1)))) best = std::min(best, *h);
            const auto h = tree.nearest(o, d);
            if ((best > 1e29f && !h) || (h && std::abs(h->t - best) < 1e-4f && tree.blocked(o, d, best + 1e-3f) && !tree.blocked(o, d, best - 1e-3f))) ++same;
        }
        CHECK(same == tries, "the tree of boxes finds what trying every triangle does (%d of %d)", same, tries);
    }
    // Shapes drawn and cut in, again and again inside the last cut, on a turned object (the shape
    // carried into its own space, as the Draw tool does). This once built its tree forever and
    // crashed; and the cutting's rounding left small gaps.
    {
        uint32_t seed = 12345;
        auto rnd = [&] { return (seed = seed * 1664525u + 1013904223u) / 4294967296.0f * 2 - 1; }; // (the same everywhere: -1..1)
        int cuts = 0, sealedCuts = 0;
        for (int run = 0; run < 12; ++run) {
            Mesh m = makeCube(1.0f);
            const glm::vec3 axis = glm::normalize(glm::vec3(rnd(), rnd(), rnd() + 2));
            const glm::mat4 M = glm::translate(glm::mat4(1), glm::vec3(rnd(), rnd(), 0.5f)) * glm::rotate(glm::mat4(1), rnd() * 3, axis);
            float top = 0.5f; // (the floor of the last cut, the object's own space)
            for (int k = 0; k < 5; ++k) {
                const glm::vec3 P = glm::vec3(M * glm::vec4(rnd() * 0.03f, rnd() * 0.03f, top, 1)), N = glm::normalize(glm::mat3(M) * glm::vec3(0, 0, 1));
                const glm::vec3 X = glm::normalize(glm::cross(N, glm::vec3(0.3f, 0.2f, 0.9f))), Y = glm::cross(N, X);
                const int corners = 3 + k % 4;
                const float r = 0.25f - 0.04f * float(k);
                std::vector<glm::vec3> pts;
                glm::vec3 c(0.0f);
                for (int i = 0; i < corners; ++i) {
                    const float t = 6.2831f * float(i) / float(corners) + rnd() * 0.3f;
                    pts.push_back(P + X * std::cos(t) * r + Y * std::sin(t) * r), c += pts.back();
                }
                c /= float(corners);
                for (glm::vec3& q : pts) q -= c;
                const float thick = 0.05f + 0.0002f;
                Mesh shape = extrudeOutline(pts, N, thick);
                transform(shape, glm::inverse(M) * glm::translate(glm::mat4(1), c + N * (0.0001f - thick / 2)));
                Mesh was = m;
                if (!combine(m, shape, Combine::Subtract)) m = was;
                ++cuts, sealedCuts += checkForPrint(m).closed();
                top -= 0.05f;
            }
        }
        CHECK(sealedCuts >= cuts - 2, "cuts drawn inside cuts: no crash, %d of %d closed after", sealedCuts, cuts);
    }
    // An edge's ring: across a cube's side, the four edges going round it.
    {
        const Mesh c = makeCube(1.0f);
        const auto& f = c.faces[0];
        CHECK(edgeRing(c, {std::min(f[0], f[1]), std::max(f[0], f[1])}).size() == 4, "a cube edge's ring is 4 edges (%zu)", edgeRing(c, {std::min(f[0], f[1]), std::max(f[0], f[1])}).size());
    }
    // Thicken: a plane 1 m square made 0.1 thick is a closed slab of 0.1 m3; a cube made hollow.
    {
        Mesh p = makePlane(1.0f);
        thicken(p, 0.1f);
        CHECK(checkForPrint(p).closed() && std::abs(checkForPrint(p).volume - 0.1f) < 1e-4f, "a plane thickened into a slab (%g m3)", checkForPrint(p).volume);
        Mesh c = makeCube(1.0f);
        thicken(c, 0.1f);
        CHECK(checkForPrint(c).closed() && std::abs(checkForPrint(c).volume - (1.0f - 0.8f * 0.8f * 0.8f)) < 1e-3f, "a cube hollowed, walls 0.1 thick (%g m3)", checkForPrint(c).volume);
    }

    // Save, load: the same; export writes a glTF and its .bin.
    Scene s;
    Object o;
    o.id = 1, o.name = "Box \"one\"", o.mesh = cube, o.position = {1, 2, 3}, o.rotation = fromEuler({10, 20, 30}), o.color = {0.5f, 0.25f, 1};
    s.objects.push_back(o);
    const auto dir = std::filesystem::temp_directory_path() / "fry3dx_test";
    std::filesystem::create_directories(dir);
    CHECK(saveScene((dir / "a.fry").string(), s), "saves");
    auto back = loadScene((dir / "a.fry").string());
    CHECK(back && back->objects.size() == 1 && back->objects[0].name == o.name && back->objects[0].mesh.faces == cube.faces &&
              glm::length(toEuler(back->objects[0].rotation) - glm::vec3(10, 20, 30)) < 1e-3f,
          "loads back the same");
    CHECK(exportModel((dir / "a.gltf").string(), s, {}) && std::filesystem::file_size(dir / "a.bin") > 0, "exports glTF");
    for (const auto& [ext, what] : exportFormats())
        CHECK(exportModel((dir / ("b" + ext)).string(), s, {}) && std::filesystem::file_size(dir / ("b" + ext)) > 100, "exports %s", ext.c_str());
    // Read back in: the same shape, the same size (STL and 3MF go by millimetres, OBJ and glTF by Y up).
    for (const std::string ext : {".stl", ".obj", ".3mf", ".glb", ".gltf"}) {
        std::string err;
        const auto in = importModel((dir / ((ext == ".gltf" ? "a" : "b") + ext)).string(), &err);
        const float want = checkForPrint(cube).volume, got = in.empty() ? 0 : checkForPrint(in[0].mesh).volume;
        if (ext == ".3mf") CHECK(!in.empty() && in[0].name == o.name, "a 3MF's name, as it was (%s)", in.empty() ? "" : in[0].name.c_str());
        CHECK(in.size() == 1 && checkForPrint(in[0].mesh).closed() && std::abs(got - want) < want * 1e-3f, "imports %s back (%g m3, %s)", ext.c_str(), got, err.c_str());
    }
    {
        const uint8_t packed[] = {115,43,170,52,118,137,80,40,74,77,76,41,86,48,246,117,83,72,203,204,73,45,214,83,112,27,21,31,21,31,21,31,244,226,12,140,76,204,44,172,108,236,28,156,92,220,60,188,124,252,2,130,66,194,34,162,98,226,18,146,82,210,50,178,114,242,10,138,74,202,42,170,106,234,26,154,90,218,58,186,122,250,6,134,70,198,38,166,102,230,22,150,86,214,54,182,118,246,14,142,78,206,46,174,110,238,30,158,94,222,62,190,126,254,1,129,65,193,33,161,97,225,17,145,81,209,49,177,113,241,9,137,73,201,41,169,105,233,25,153,89,217,57,185,121,249,5,133,69,197,37,165,101,229,21,149,85,213,53,181,117,245,13,141,77,205,45,173,109,237,29,157,93,221,61,189,125,253,19,38,78,154,60,101,234,180,233,51,102,206,154,61,103,238,188,249,11,22,46,90,188,100,233,178,229,43,86,174,90,189,102,237,186,245,27,54,110,218,188,101,235,182,237,59,118,238,218,189,103,239,190,253,7,14,30,58,124,228,232,177,227,39,78,158,58,125,230,236,185,243,23,46,94,186,124,229,234,181,235,55,110,222,186,125,231,238,189,251,15,30,62,122,252,228,233,179,231,47,94,190,122,253,230,237,187,247,31,62,126,250,252,229,235,183,239,63,126,254,250,253,231,239,191,255,0};
        std::string want;
        for (int i = 0; i < 40; ++i) want += "Fry3DX reads 3MF files. ";
        for (int i = 0; i < 256; ++i) want += char(i);
        const std::vector<uint8_t> out = inflate(packed, sizeof packed);
        CHECK(std::string(out.begin(), out.end()) == want && out.size() == 1216, "unpacks DEFLATE (%zu bytes)", out.size());
        CHECK(inflate(packed, sizeof packed / 2).empty(), "a cut-short DEFLATE: nothing");
    }

    // Its own file: one packed file with its pictures and layers, back the same; old text ones open too.
    {
        Scene p = s;
        Image im = makeImage(256, 256, {1, 1, 1, 1});
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) im.at(x, y)[0] = uint8_t(x ^ y), im.at(x, y)[1] = uint8_t(y);
        p.objects[0].picture = std::make_shared<Image>(im);
        p.objects[0].uvs = autoUnwrap(p.objects[0].mesh);
        PictureLayer l;
        l.name = "Logo", l.image = std::make_shared<Image>(makeImage(3, 5, {1, 0, 0, 0.5f})), l.width = 0.25f;
        p.objects[0].layers.push_back(l);
        CHECK(savePackage((dir / "p.fry").string(), p) && isPackage((dir / "p.fry").string()), "saves its own file");
        auto q = loadPackage((dir / "p.fry").string());
        CHECK(q && q->objects.size() == 1 && q->objects[0].picture && q->objects[0].picture->px == im.px && q->objects[0].layers.size() == 1 &&
                  q->objects[0].layers[0].image->px == l.image->px && q->objects[0].layers[0].name == "Logo" && q->objects[0].layers[0].width == 0.25f &&
                  q->objects[0].uvs == p.objects[0].uvs,
              "loads its own file back the same, pictures exactly");
        CHECK(std::filesystem::file_size(dir / "p.fry") < 256 * 256 * 4 / 8, "packed small (%zu bytes)", size_t(std::filesystem::file_size(dir / "p.fry")));
        CHECK(loadPackage((dir / "a.fry").string()).has_value(), "opens the old text .fry too");
        Scene sc = s; // scripts kept in it, any characters and lines (an "end" or "object" line in one too)
        sc.scripts = {{"Town", "-- a town\nobject \"x\"\nend\nlocal s = \"\\n\"\n"}, {"Empty", ""}, {"Last", "print(1)"}};
        CHECK(savePackage((dir / "s.fry").string(), sc), "saves scripts in it");
        auto sb = loadPackage((dir / "s.fry").string());
        CHECK(sb && sb->scripts.size() == 3 && sb->scripts[0].name == "Town" && sb->scripts[0].code == sc.scripts[0].code && sb->scripts[1].code.empty() &&
                  sb->scripts[2].code == "print(1)" && sb->objects.size() == 1 && sb->objects[0].name == s.objects[0].name,
              "scripts come back the same, and the model with them");
        Scene g = s;
        g.folders = {{50, "Parts", 0, true, true, false}, {51, "Screws", 50, false, true, true}};
        g.objects[0].folder = 51;
        CHECK(savePackage((dir / "g.fry").string(), g), "saves folders");
        auto h = loadPackage((dir / "g.fry").string());
        CHECK(h && h->folders.size() == 2 && h->folders[1].name == "Screws" && h->folders[1].parent == h->folders[0].id && !h->folders[1].open && h->folders[1].locked &&
                  h->objects[0].folder == h->folders[1].id,
              "folders inside folders come back, the object in its folder");
    }
    std::ifstream j(dir / "a.gltf");
    const std::string text((std::istreambuf_iterator<char>(j)), {});
    CHECK(text.find("\"translation\":[2,3,1]") != std::string::npos, "in glTF's axes (Y up): the position is [2,3,1]");

    { // Bridge: two boxes' facing sides joined by a tunnel; one closed shape, every face outwards.
        Mesh two = makeCube();
        append(two, makeCube(), glm::translate(glm::mat4(1.0f), glm::vec3(3, 0.2f, 0)));
        int fa = -1, fb = -1;
        for (size_t f = 0; f < two.faces.size(); ++f) {
            const glm::vec3 n = faceNormal(two, int(f)), c = faceCenter(two, int(f));
            if (n.x > 0.9f && c.x < 1.0f) fa = int(f);
            if (n.x < -0.9f && c.x > 2.0f) fb = int(f);
        }
        const std::vector<int> made = bridgeFaces(two, fa, fb);
        const PrintCheck c = checkForPrint(two);
        CHECK(made.size() == 4 && c.closed() && c.volume > 2.0f + 2.0f * 0.95f && c.pieces == 1, "bridge: one closed shape (volume %.2f, pieces %d, open %d crowded %d flipped %d)", c.volume, c.pieces, c.openEdges, c.crowdedEdges, c.flippedEdges);
        Mesh tri = makeCube();
        append(tri, makeCylinder(3, 0.5f, 1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(3, 0, 0)));
        CHECK(bridgeFaces(tri, 0, int(tri.faces.size()) - 1).empty() && tri.faces.size() == 6 + 5, "bridge: not between a 4 and a 3 cornered face");
    }
    { // Smoothing points evens a bump; pushing a cube's points out makes it bigger all round.
        Mesh p = makePlane(2.0f);
        for (int i = 0; i < 3; ++i) subdivide(p, false);
        int mid = 0;
        for (size_t v = 0; v < p.verts.size(); ++v)
            if (glm::length(glm::vec2(p.verts[v])) < glm::length(glm::vec2(p.verts[size_t(mid)]))) mid = int(v);
        p.verts[size_t(mid)].z = 1.0f;
        smoothPoints(p, {mid}, 1.0f);
        CHECK(std::abs(p.verts[size_t(mid)].z) < 1e-5f, "smooth: the bump's point back to its neighbours' middle");
        Mesh c = makeCube();
        std::set<int> all;
        for (size_t v = 0; v < c.verts.size(); ++v) all.insert(int(v));
        pushPoints(c, all, 0.1f);
        CHECK(std::abs(boundsMax(c).x - (0.5f + 0.1f / std::sqrt(3.0f))) < 1e-4f && checkForPrint(c).volume > 1.0f, "push: corners out along their normals");
    }
    { // A gear with a screw hole (scripts/gear.lua): cutting leaves points a hair apart and
      // needles along edges; what comes out must still print (closed, nothing without size).
        for (int teeth : {10, 16})
            for (int sides : {24, 32}) {
                std::vector<glm::vec3> o;
                const float radius = teeth == 16 ? 0.03f : 0.018f;
                for (int i = 0; i < teeth * 4; ++i) {
                    const float a = float(i) / float(teeth * 4) * 6.2831853f, r = i % 4 < 2 ? radius + 0.004f : radius;
                    o.push_back({std::cos(a) * r, std::sin(a) * r, 0});
                }
                Mesh g = extrudeOutline(o, {0, 0, 1}, 0.008f);
                transform(g, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0.004f)));
                combine(g, makeCylinder(sides, 0.0027f, 0.03f), Combine::Subtract);
                const PrintCheck c = checkForPrint(g);
                CHECK(c.closed() && c.tinyFaces == 0 && closed(g), "a %d-tooth gear minus a %d-sided hole prints (open %d, crowded %d, flipped %d, tiny %d)", teeth, sides,
                      c.openEdges, c.crowdedEdges, c.flippedEdges, c.tinyFaces);
            }
    }
    { // Sculpting: a cube made finer keeps its shape; brushes move only what's under them.
        Mesh m = makeCube(1.0f);
        const int times = finerForSculpting(m, 2000);
        CHECK(times == 5 && m.verts.size() >= 2000 && closed(m) && boundsMax(m) == glm::vec3(0.5f), "a cube made finer for sculpting: %d times, %zu points, same box", times, m.verts.size());
        const SculptMesh sm(m);
        const Mesh before = m;
        const glm::vec3 top(0, 0, 0.5f);
        for (int i = 0; i < 20; ++i) sculptDab(m, sm, SculptBrush::Draw, {top, 0.25f, 1.0f});
        CHECK(boundsMax(m).z > 0.55f && boundsMin(m) == boundsMin(before), "draw raises the top (%.3f) and leaves the rest", boundsMax(m).z);
        CHECK(closed(m), "still closed after sculpting");
        Mesh dug = before;
        for (int i = 0; i < 20; ++i) sculptDab(dug, sm, SculptBrush::Draw, {top, 0.25f, 1.0f, true});
        CHECK(boundsMax(dug).z == 0.5f, "inverted, it digs in: nothing above the top");
        float rough = 0.0f, smooth = 0.0f; // how far the top's points are from flat
        Mesh bumpy = m;
        for (int i = 0; i < 10; ++i) sculptDab(bumpy, sm, SculptBrush::Smooth, {top, 0.3f, 1.0f});
        for (size_t i = 0; i < m.verts.size(); ++i)
            if (glm::length(glm::vec2(m.verts[i])) < 0.15f && m.verts[i].z > 0.4f) rough += m.verts[i].z - 0.5f, smooth += bumpy.verts[i].z - 0.5f;
        CHECK(smooth < rough, "smoothing lowers the bump (%.3f to %.3f)", rough, smooth);
        Mesh flat = m;
        for (int i = 0; i < 30; ++i) sculptDab(flat, sm, SculptBrush::Flatten, {top, 0.3f, 1.0f});
        CHECK(boundsMax(flat).z < boundsMax(m).z, "flatten presses it down");
        Mesh g = before;
        const auto held = grabbed(g, top, 0.2f);
        std::vector<glm::vec3> from;
        for (const auto& [i, w] : held) from.push_back(g.verts[size_t(i)]);
        moveGrabbed(g, held, from, glm::vec3(0, 0, 0.3f));
        CHECK(!held.empty() && std::abs(boundsMax(g).z - 0.8f) < 0.01f && closed(g), "grab pulls the middle up with it (%.3f)", boundsMax(g).z);
    }

    { // editable cuts: a box with a bar through it, the bar still an object
        Scene sc;
        Object box, bar;
        box.id = 1, box.name = "Box", box.mesh = makeCube(2.0f);
        bar.id = 2, bar.name = "Bar", bar.mesh = makeCube(1.0f), bar.scale = {3, 1, 1}, bar.cuts = 1;
        sc.objects = {box, bar};
        sc.nextId = 3;
        CHECK(cutTarget(sc, sc.objects[1]) == &sc.objects[0] && hasCuts(sc, sc.objects[0]) && !hasCuts(sc, sc.objects[1]), "the bar cuts the box");
        const float v = checkForPrint(cutMesh(sc, sc.objects[0])).volume;
        CHECK(std::abs(v - 6.0f) < 1e-3f, "a 1x1 tunnel through a 2-box: 6 left (%.3f)", v);
        sc.objects[1].position = {0, 0, 0.5f};
        const float moved = checkForPrint(cutMesh(sc, sc.objects[0])).volume;
        CHECK(std::abs(moved - 6.0f) < 1e-3f && boundsMax(cutMesh(sc, sc.objects[0])).z == 1.0f, "moved up, a slot at the top instead (%.3f)", moved);
        const Scene made = withCutsMade(sc);
        CHECK(!made.objects[1].visible && std::abs(checkForPrint(made.objects[0].mesh).volume - 6.0f) < 1e-3f, "exporting: the cut made, the bar hidden");
        sc.objects[1].cuts = 2;
        CHECK(!cutTarget(sc, sc.objects[1]) && !hasCuts(sc, sc.objects[0]), "cutting itself: no cut");
        sc.objects[1].cuts = 1;
        const auto file = (std::filesystem::temp_directory_path() / "fry3dx_test" / "cuts.fry").string();
        std::filesystem::create_directories(std::filesystem::path(file).parent_path());
        auto back = saveScene(file, sc) ? loadScene(file) : std::nullopt;
        CHECK(back && back->objects.size() == 2 && back->objects[1].cuts == back->objects[0].id && back->objects[0].cuts == 0, "cuts saved and back (to the new ids)");
    }

    { // copies along a path: a square's outline, a line, a cube's biggest face
        Mesh sq;
        sq.verts = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
        sq.faces = {{0, 1, 2, 3}};
        bool loop = false;
        const auto p = pathOf(sq, loop);
        const auto pts = alongPath(p, loop, 8);
        bool even = pts.size() == 8;
        for (size_t i = 0; i < pts.size() && even; ++i) even = glm::distance(pts[i].at, pts[(i + 1) % pts.size()].at) > 0.99f && glm::distance(pts[i].at, pts[(i + 1) % pts.size()].at) < 1.42f;
        CHECK(loop && p.size() == 4 && even, "a square's outline is a closed path, 8 points 1 apart round it");
        Mesh line;
        line.verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {1, 1, 0}, {3, 1, 0}};
        line.faces = {{0, 1}, {1, 2}, {3, 4}, {2, 3}};
        const auto lp = pathOf(line, loop);
        const auto lpts = alongPath(lp, loop, 5);
        CHECK(!loop && lp.size() == 5 && lpts.size() == 5 && glm::distance(lpts.front().at, glm::vec3(0)) < 1e-5f && glm::distance(lpts.back().at, glm::vec3(3, 1, 0)) < 1e-5f &&
                  std::abs(lpts[2].at.x - 1) < 1e-5f && std::abs(lpts[2].at.y - 1) < 1e-5f && lpts[4].along == glm::vec3(1, 0, 0),
              "a line (a piece with no length in it): open, both ends, the middle at its corner, going its way");
        const auto cp = pathOf(makeCube(1.0f), loop);
        CHECK(loop && cp.size() == 4, "a closed box: its biggest face's outline");
        CHECK(alongPath({}, false, 4).empty() && alongPath({glm::vec3(1), glm::vec3(1)}, false, 3).empty(), "no path, or one with no length: nothing");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
