// The modelling core without a window: shapes are closed and face out, faces with dents
// triangulate right, extrude / inset / merge keep the mesh whole, and files round-trip.
#include "mesh.h"
#include "image.h"
#include "scene.h"
#include "shapes.h"
#include "sculpt.h"

#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
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

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
