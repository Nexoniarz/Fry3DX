#pragma once
// Meshes as people think of them: points, and faces round them (any number of corners, flat or
// nearly). Fry3DX's space: metres, Z up, X forward (the model's front looks along +X), Y left —
// the same as glTF's once turned, so what you make faces the same way everywhere.
#include <glm/glm.hpp>

#include <array>
#include <set>
#include <utility>
#include <vector>

namespace fry {

struct Mesh {
    std::vector<glm::vec3> verts;
    std::vector<std::vector<int>> faces; // corners counter-clockwise, seen from outside
};

// ── Shapes to start from (centred on the origin) ──
Mesh makeCube(float size = 1.0f);
Mesh makePlane(float size = 1.0f);
Mesh makeCylinder(int sides = 24, float radius = 0.5f, float height = 1.0f);
Mesh makeCone(int sides = 24, float radius = 0.5f, float height = 1.0f);
// A cylinder narrower at the top (a cone if `top` is 0).
Mesh makeFrustum(int sides, float bottom, float top, float height);
Mesh makeSphere(int segments = 24, int rings = 12, float radius = 0.5f);
Mesh makeTorus(int segments = 32, int sides = 12, float radius = 0.5f, float tube = 0.15f);
// An outline (points round it, on one plane, either way round) made solid: a front and a back
// face `thickness` apart, centred on the outline's plane, and the sides between them.
Mesh extrudeOutline(const std::vector<glm::vec3>& outline, const glm::vec3& planeNormal, float thickness);

// ── Looking at it ──
glm::vec3 faceNormal(const Mesh& m, int face); // unit (Newell's: fine for faces a little bent)
glm::vec3 faceCenter(const Mesh& m, int face);
float faceArea(const Mesh& m, int face);
// Triangles covering a face (ear clipping: works for L shapes and other dents), as corners of
// the mesh, counter-clockwise like the face.
std::vector<std::array<int, 3>> triangulate(const Mesh& m, int face);
// Each edge once, lower index first.
std::vector<std::pair<int, int>> edges(const Mesh& m);
// Every face corner's normal: flat (its face's), or smooth — averaged with the faces round the
// point that meet it at less than `angle` degrees (sharp creases stay sharp).
std::vector<std::vector<glm::vec3>> cornerNormals(const Mesh& m, bool smooth, float angle);
glm::vec3 boundsMin(const Mesh& m);
glm::vec3 boundsMax(const Mesh& m);

// ── Changing it ──
// Pulls the faces out `distance` along their normals, as one piece, with walls round the edge
// of the piece (the faces keep their numbers; returns the new points).
std::vector<int> extrudeFaces(Mesh& m, const std::set<int>& faces, float distance);
// Shrinks each face towards its middle by `amount` (0..1 of the way), leaving a ring of faces.
void insetFaces(Mesh& m, const std::set<int>& faces, float amount);
void flipFaces(Mesh& m, const std::set<int>& faces);
void deleteFaces(Mesh& m, const std::set<int>& faces);
// Deletes points and every face that uses one.
void deleteVerts(Mesh& m, const std::set<int>& verts);
// A new face through the points, in the order given (made to face the camera's way if `towards`
// isn't zero). False if fewer than 3.
bool makeFace(Mesh& m, const std::vector<int>& verts, const glm::vec3& towards = glm::vec3(0.0f));
// Points closer than `distance` become one (the first). Returns how many went.
int mergeByDistance(Mesh& m, float distance);
// Every point to their middle (merging them).
void mergeToCenter(Mesh& m, const std::set<int>& verts);
// Drops points no face uses, renumbering the rest.
void removeUnused(Mesh& m);
void transform(Mesh& m, const glm::mat4& matrix);
// Adds another mesh's points and faces (moved by `matrix`).
void append(Mesh& m, const Mesh& other, const glm::mat4& matrix = glm::mat4(1.0f));

// ── More tools (tools.cpp) ──
// Rounds (or with 1 segment, cuts off) edges: each becomes a strip `width` wide across, curved
// in `segments` steps, with rounded corners where several meet. Edges lower index first; edges
// on a hole's rim are left alone.
void bevelEdges(Mesh& m, const std::set<std::pair<int, int>>& edges, float width, int segments);
// Edges where the faces meet sharper than `angle` degrees.
std::set<std::pair<int, int>> sharpEdges(const Mesh& m, float angle);
// Every face cut into four-sided pieces; `smooth` rounds it (Catmull–Clark) — edges sharper
// than `keepAngle` degrees stay sharp (180: none do).
void subdivide(Mesh& m, bool smooth, float keepAngle = 180.0f);
// The ring of four-sided faces across an edge, cut round the middle `cuts` times. Returns the new
// points (empty if the edge has no four-sided face).
std::vector<int> loopCut(Mesh& m, std::pair<int, int> edge, int cuts = 1);
// The ring of faces a loop cut through `edge` would cut (for showing it before you click).
std::vector<int> faceRing(const Mesh& m, std::pair<int, int> edge);
// The same faces' corners, each starting with the edge the ring comes in through: the cut runs
// from corners 0–1 across to corners 3–2.
std::vector<std::array<int, 4>> ringQuads(const Mesh& m, std::pair<int, int> edge);
// The edges across that ring (each quad's way in and way out, `edge` too): a ring of edges.
std::set<std::pair<int, int>> edgeRing(const Mesh& m, std::pair<int, int> edge);
// The line of edges an edge continues into, through points with four edges (edges lower first).
std::set<std::pair<int, int>> edgeLoop(const Mesh& m, std::pair<int, int> edge);
// ── Cutting (cut.cpp) ──
// The knife: a path drawn on the screen, seen from `eye` (all in the mesh's own space; only the
// way from the eye to each point matters). Each step of the path sweeps a wedge out from the
// eye, and every face it passes over is split along it, edge to edge. `through`: every face in
// its way, front and back (a cut all the way through); else only faces the eye can see.
// Returns the new edges.
std::set<std::pair<int, int>> knifeCut(Mesh& m, const glm::vec3& eye, const std::vector<glm::vec3>& path, bool through);
// Pulls a mesh apart along edges that go all the way round it: each piece gets its own points
// there, and with `fill` a face closing each opening. Returns each piece's faces (one piece if
// the edges don't part it).
std::vector<std::set<int>> splitAlong(Mesh& m, const std::set<std::pair<int, int>>& edges, bool fill);
// How much a closed piece holds (cubic metres).
float volumeOf(const Mesh& m, const std::set<int>& faces);
// A hole's outline on a face: `sides` points round `centre` (on the face's plane), `radius` out;
// 4 sides make a square lined up with the face's longest edge. Counter-clockwise, seen from outside.
std::vector<glm::vec3> holeOutline(const Mesh& m, int face, const glm::vec3& centre, float radius, int sides);
// The hole tool's shapes, lined up with the face's longest edge, turned `turn` degrees more:
//   Round: `across` wide (`sides` points), Rectangle: `across` x `tall`, Triangle: sides `across`
//   long (a point up), Hexagon: `across` its flats (a nut's spanner size; flats top and bottom).
enum class HoleShape { Round, Rectangle, Triangle, Hexagon };
std::vector<glm::vec3> holeOutline(const Mesh& m, int face, const glm::vec3& centre, HoleShape shape, float across, float tall = 0.0f, int sides = 32,
                                   float turn = 0.0f);
// Only the outline cut into the face (nothing taken away): the face becomes a ring round a new
// face inside the outline, to extrude, bevel or paint on its own. The new face, or -1 if the
// outline doesn't fit inside the face (nothing changed).
int imprintOutline(Mesh& m, int face, const std::vector<glm::vec3>& outline);
// Cuts a hole in a face along `outline` (inside the face). depth > 0: a pocket that deep with a
// floor; depth <= 0: all the way through to the face behind (the outline must fit in that face
// too). False if it doesn't fit (nothing changed).
bool cutHole(Mesh& m, int face, const std::vector<glm::vec3>& outline, float depth);

// ── Pictures on it (uv.cpp) ──
// Where each face corner sits on a picture (0..1 across and down): the shape is cut into
// pieces that face about the same way, each laid flat, all packed in with `margin` round them.
std::vector<std::vector<glm::vec2>> autoUnwrap(const Mesh& m, float margin = 0.015f);

// Every face joined to these by edges.
std::set<int> linkedFaces(const Mesh& m, const std::set<int>& faces);
std::set<int> linkedVerts(const Mesh& m, const std::set<int>& verts);
// The points next to these (by an edge), and these.
std::set<int> growVerts(const Mesh& m, const std::set<int>& verts);

// Points moved towards their neighbours' middle (`amount` 0..1, a few times for more): bumps
// evened out (Blender's Smooth Vertices, Maya's Average Vertices).
void smoothPoints(Mesh& m, const std::set<int>& verts, float amount = 0.5f, int times = 1);
// Points pushed out along the surface's normal there (negative: pulled in): fatter or thinner,
// keeping the shape (Blender's Shrink/Fatten).
void pushPoints(Mesh& m, const std::set<int>& verts, float distance);
// Two faces with as many corners each taken out and joined by a tunnel of faces between their
// edges, each corner to the nearest one round (Bridge, as in Blender and Maya). The new faces;
// empty (and nothing changed) if they aren't two faces with as many corners.
std::vector<int> bridgeFaces(Mesh& m, int a, int b);

// ── Whole shapes ──

// Faces next to each other lying flat together (normals within `angle` degrees) joined into
// one where they make a single outline; how many faces went.
int mergeFlatFaces(Mesh& m, float angle = 0.5f);
// Made `thickness` thick: a surface gets a back and walls round its open edges; a closed shape
// gets an inside (hollow, walls that thick). Negative: the new side outwards instead.
void thicken(Mesh& m, float thickness);
// Turned round an axis (through `origin`, along `axis`) by `degrees`, in `steps` pieces, rising
// `rise` along the axis as it goes (a screw; 0: a plain spin): what it sweeps through. Its open
// edges (or a flat shape's outline) sweep into the walls; a flat shape is its two ends, when the
// turn doesn't come back round on itself.
Mesh spin(const Mesh& m, const glm::vec3& origin, const glm::vec3& axis, float degrees, int steps, float rise = 0.0f);
// Two shapes (in the same space) made one: `b` added to `a`, taken out of it, or only where they
// overlap kept. The result goes in `a`; false if nothing's left.
enum class Combine { Add, Subtract, Overlap };
bool combine(Mesh& a, const Mesh& b, Combine op);

// ── For 3D printing ──

// What a slicer would mind: edges with a face on one side only (holes: not closed), edges with
// more than two faces, neighbours facing opposite ways (one turned inside out), faces with no
// area; and how much it holds, its surface, and how much of it hangs over the air (faces facing
// down more steeply than `overhang` degrees from upright, not lying on the bed at bedZ).
struct PrintCheck {
    int openEdges = 0, crowdedEdges = 0, flippedEdges = 0, tinyFaces = 0, pieces = 0;
    bool insideOut = false;
    float volume = 0, area = 0, overhangArea = 0; // (signed volume: < 0 when inside out)
    bool closed() const { return openEdges == 0 && crowdedEdges == 0 && flippedEdges == 0; }
};
PrintCheck checkForPrint(const Mesh& m, float overhang = 45.0f, float bedZ = 0.0f);
bool overhangs(const Mesh& m, int face, float overhang, float bedZ);
// Closes each hole with a face across it; how many.
int closeHoles(Mesh& m);
// Turns faces so each piece's all face the same way, outwards; how many were turned.
int faceOutward(Mesh& m);
// Where a level plane at height z cuts the shape: the pieces of its outline there (a slicer's
// layer), each from one face.
std::vector<std::pair<glm::vec3, glm::vec3>> sliceAt(const Mesh& m, float z);
// Made ready to print, as far as it can be without guessing: points in the same place joined (an
// STL's corners come apart), faces with no area gone, faces there twice once (back to back: both
// gone, an inside wall), faces turned the same way, holes closed. What it did.
struct Repair {
    int joinedPoints = 0, emptyFaces = 0, doubledFaces = 0, turnedFaces = 0, closedHoles = 0;
    int total() const { return joinedPoints + emptyFaces + doubledFaces + turnedFaces + closedHoles; }
};
Repair repairForPrint(Mesh& m);

// ── Its picture's layout ──

// The frames of the layout: faces joined where they share an edge laid out the same on both.
std::vector<std::vector<int>> uvIslands(const Mesh& m, const std::vector<std::vector<glm::vec2>>& uvs);

} // namespace fry
