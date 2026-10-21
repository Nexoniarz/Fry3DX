// Fry3DX — Frying 3D Extreme (the app: its state and what it does, declared; defined in app*.cpp). A 3D modeller that works the way you'd expect: pick a tool from the
// toolbox, click and drag. The view in the middle is drawn by the engine's renderer: plain
// and clear while you model (Solid), or properly lit (Rendered). The panels are Dear ImGui.
//
//   fry3dx [file.fry]
#pragma once
#include "bvh.h"
#include "commands.h"
#include "icons.h"
#include "mesh.h"
#include "scene.h"
#include "script.h"
#include "sculpt.h"
#include "shapes.h"
#include "import.h"
#include "text.h"

#include <SDL3/SDL.h>
#include <core/cvar.h>
#include <core/sysinfo.h>
#include <core/log.h>
#include <engine.h>
#include <fs/vfs.h>
#include <render/renderer.h>
#include <render/screenshot.h>
#include <ui/ui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <chrono>
#include <ctime>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <span>

using namespace fry;
namespace stdfs = std::filesystem;

namespace fryapp {

inline double now() { return double(SDL_GetTicks()) / 1000.0; } // (seconds; works before the UI exists)

constexpr float kUnits = 39.37f; // the engine's units (inches) in a metre: the view renders in them
constexpr float kPi = 3.14159265358979f;

enum class Mode { Object, Edit };
enum class Tool { Select, Move, Rotate, Scale, Draw, Extrude, Inset, Bevel, LoopCut, Knife, Hole };
enum class Tab { Model, Paint, Layout, Render, Print };
enum class LayTool { Select, Move, Rotate, Scale };
enum class PrintTool { None, Measure, LayFlat };
enum class PaintTool { Brush, Eraser, Fill, Picker, Picture };
enum class Elem { Vertex, Edge, Face };
enum class View { Perspective, Front, Back, Left, Right, Top, Bottom };
enum class Orient { Local, World, Normal };
// which way the gizmo's arrows point
enum class Pivot { Middle, Origin, Opposite, Bottom }; // what stays put when turning / sizing
enum class Shading { Solid, Rendered, Wire };
enum class SelShape { Box, Brush, Lasso };

struct ToolInfo {
    Tool tool;
    Icon icon;
    const char* name;
    const char* key;
    const char* hint;
    bool objectMode, editMode;
};
const ToolInfo kTools[] = {
    {Tool::Select, Icon::Select, "Select", "Q", "Click to select; drag round things. Objects: Shift adds, Ctrl takes away. Shape: Ctrl adds/takes one, Shift+click a whole loop, Shift+Ctrl+click an edge ring.", true, true},
    {Tool::Move, Icon::Move, "Move", "W", "Drag an arrow to move along it, the square to move freely. Hold Ctrl to snap.", true, true},
    {Tool::Rotate, Icon::Rotate, "Rotate", "E", "Drag a ring to turn about it; the outer ring turns it as you see it. Ctrl: 15 degree steps.", true, true},
    {Tool::Scale, Icon::Scale, "Scale", "R", "Drag a side's handle to stretch it (the other side stays, or see Tool options); hold Shift to stretch both sides from the middle. Middle: all ways.", true, true},
    {Tool::Draw, Icon::Draw, "Draw shape", "D", "Click points round a shape (on the ground, or on any face of an object); click the first point (or press Enter) to finish. Esc cancels.", true, false},
    {Tool::Extrude, Icon::Extrude, "Extrude", "X", "Select faces, then drag the arrow to pull them out (along the way they face).", false, true},
    {Tool::Inset, Icon::Inset, "Inset", "I", "Select faces, then drag the handle to make a smaller face inside each.", false, true},
    {Tool::Bevel, Icon::Bevel, "Round edges", "B", "Select edges (or faces), then drag the round handle to round them off.", false, true},
    {Tool::LoopCut, Icon::LoopCut, "Loop cut", "C", "Point at an edge: the cut shows across. Click to cut there.", false, true},
    {Tool::Knife, Icon::Knife, "Knife", "K", "Click points along where to cut, or drag a straight line. Enter or double-click cuts. Cutting right through: Tool options.", false, true},
    {Tool::Hole, Icon::Hole, "Hole", "O", "Press on a face and drag out the size: round, rectangle, triangle or hexagon (nuts); all the way through, a pocket, or only its outline (a new face).", true, true},
};

struct PaintToolInfo {
    PaintTool tool;
    Icon icon;
    const char* name;
    const char* key;
    const char* hint;
};
struct LayToolInfo {
    LayTool tool;
    Icon icon;
    const char* name;
    const char* key;
    const char* hint;
};
const LayToolInfo kLayTools[] = {
    {LayTool::Select, Icon::Select, "Select", "Q", "Click frames (or corners) to pick them, Shift adds; drag round them to pick many. Clicking the model picks its frame too."},
    {LayTool::Move, Icon::Move, "Move", "W", "Drag frames (or corners) about the picture."},
    {LayTool::Rotate, Icon::Rotate, "Turn", "E", "Drag round to turn what's picked (Ctrl: 15 degree steps)."},
    {LayTool::Scale, Icon::Scale, "Size", "R", "Drag out or in to make what's picked bigger or smaller (Shift: only across or only up)."},
};
// Printers' build plates (mm, across x deep x high) and what's printed with.
struct PrinterInfo {
    const char* name;
    float x, y, z;
};
const PrinterInfo kPrinters[] = {
    {"Prusa MK4 / MK4S", 250, 210, 220}, {"Prusa Core One", 250, 220, 270},   {"Prusa MINI+", 180, 180, 180},       {"Bambu Lab X1 / P1 / P2", 256, 256, 256},
    {"Bambu Lab A1", 256, 256, 256},     {"Bambu Lab A1 mini", 180, 180, 180}, {"Creality Ender 3 (V2, V3)", 220, 220, 250}, {"Creality K1", 220, 220, 250},
    {"Elegoo Neptune 4", 225, 225, 265}, {"Anycubic Kobra 3", 250, 250, 260},  {"Your own size", 0, 0, 0},
};
struct MaterialInfo {
    const char* name;
    float density; // g/cm3
};
// What the Script panel starts with.
inline const char* kFirstScript = R"(-- A script builds or changes the scene. F5 or Ctrl+Enter runs it;
-- Ctrl+Z takes back everything it did. The ? button lists what there is.

local legs = 6
for i = 1, legs do
  local a = math.rad(i * 360 / legs)
  local leg = fry.add("cylinder", {radius = 0.03, height = 0.7, color = "#c47a3c"})
  leg.position = {math.cos(a) * 0.45, math.sin(a) * 0.45, 0}
end
local top = fry.add("cylinder", {radius = 0.6, height = 0.05, sides = 48, color = "#d9a066"})
top.position = {0, 0, 0.7}
fry.frame()
)";
const MaterialInfo kMaterials[] = {{"PLA", 1.24f}, {"PETG", 1.27f}, {"ABS", 1.04f}, {"ASA", 1.07f}, {"TPU", 1.21f}, {"Nylon", 1.14f}, {"Resin", 1.12f}};

struct SculptToolInfo {
    SculptBrush brush;
    Icon icon;
    const char* name;
    const char* key;
    const char* hint;
};
const SculptToolInfo kSculptTools[] = {
    {SculptBrush::Draw, Icon::Sculpt, "Raise", "1", "Builds the surface up where you brush. Ctrl digs in instead."},
    {SculptBrush::Inflate, Icon::Inflate, "Puff out", "2", "Swells it out all round, like blowing it up. Ctrl shrinks it."},
    {SculptBrush::Smooth, Icon::Smooth, "Smooth", "3", "Evens out bumps. Hold Shift with any brush to smooth."},
    {SculptBrush::Flatten, Icon::Flatten, "Flatten", "4", "Presses it flat, like a palette knife."},
    {SculptBrush::Pinch, Icon::Pinch, "Pinch", "5", "Pulls it in towards the middle of the brush: sharp creases and ridges. Ctrl pushes apart."},
    {SculptBrush::Grab, Icon::Grab, "Grab", "6", "Take hold of a patch and drag it where it should be."},
};

const PaintToolInfo kPaintTools[] = {
    {PaintTool::Brush, Icon::PaintBrush, "Brush", "B", "Paint on the model or on the flat picture. Alt+click picks a colour; [ and ] change the size."},
    {PaintTool::Eraser, Icon::Eraser, "Eraser", "E", "Paints its plain colour back."},
    {PaintTool::Fill, Icon::Fill, "Fill", "G", "Fills the patch of about the same colour where you click."},
    {PaintTool::Picker, Icon::Picker, "Pick colour", "I", "Click to take a colour from the model or the picture."},
    {PaintTool::Picture, Icon::Picture, "Place a picture", "Ctrl+V", "Paste a picture (copied in any app) or add one from a file, then drag its corners to fit. Enter places it."},
};

inline glm::vec3 forwardOf(float yaw, float pitch) {
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
}

// The engine's view-projection (render/renderer.cpp), in metres.
inline glm::mat4 viewProjection(const glm::vec3& eye, float yaw, float pitch, float fov, float aspect, glm::vec2 shift, glm::vec2 nearFar) {
    const glm::vec3 f = forwardOf(yaw, pitch);
    glm::mat4 view = glm::lookAt(eye * kUnits, eye * kUnits + f, glm::vec3(0, 0, 1));
    glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, nearFar.x, nearFar.y);
    proj[1][1] *= -1.0f;
    proj[2][0] -= shift.x, proj[2][1] -= shift.y;
    return proj * view * glm::scale(glm::mat4(1.0f), glm::vec3(kUnits));
}

struct Ray {
    glm::vec3 origin, dir;
};

// Möller–Trumbore; t along the ray, or nothing.
inline std::optional<glm::vec3> hitPlane(const Ray& r, const glm::vec3& point, const glm::vec3& normal) {
    const float d = glm::dot(r.dir, normal);
    if (std::abs(d) < 1e-8f) return std::nullopt;
    const float t = glm::dot(point - r.origin, normal) / d;
    return t > 0 ? std::optional<glm::vec3>(r.origin + r.dir * t) : std::nullopt;
}

// Where along the line (from `p`, along unit `axis`) the ray passes closest.
inline float closestOnAxis(const Ray& r, const glm::vec3& p, const glm::vec3& axis) {
    const glm::vec3 w = p - r.origin;
    const float b = glm::dot(axis, r.dir), d = glm::dot(axis, w), e = glm::dot(r.dir, w);
    const float den = 1.0f - b * b;
    return den < 1e-6f ? 0.0f : (b * e - d) / den;
}

inline float distToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float vx = b.x - a.x, vy = b.y - a.y, len = vx * vx + vy * vy;
    const float t = len > 0 ? std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / len, 0.0f, 1.0f) : 0.0f;
    const float dx = a.x + vx * t - p.x, dy = a.y + vy * t - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
inline bool insidePolygon(ImVec2 p, const std::vector<ImVec2>& poly) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > p.y) != (poly[j].y > p.y) && p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) in = !in;
    return in;
}

inline float snapTo(float v, float step) { return step > 0 ? std::round(v / step) * step : v; }

using Axes = std::array<glm::vec3, 3>;
inline glm::vec3 inFrame(const Axes& a, const glm::vec3& p) { return {glm::dot(p, a[0]), glm::dot(p, a[1]), glm::dot(p, a[2])}; }
inline glm::vec3 fromFrame(const Axes& a, const glm::vec3& c) { return a[0] * c.x + a[1] * c.y + a[2] * c.z; }

const ImU32 kAxisCol[3] = {IM_COL32(235, 70, 70, 255), IM_COL32(110, 210, 80, 255), IM_COL32(70, 130, 245, 255)};
const ImU32 kHot = IM_COL32(255, 225, 90, 255);
constexpr ImU32 kAccent = IM_COL32(255, 120, 40, 255);
// The UI's colours: Niveus's roles, in the appearance that's showing (keel/ui).
inline ImVec4 roleColor(keel::ui::Role r) { return ImGui::ColorConvertU32ToFloat4(keel::ui::color(r)); }
inline ImVec4 accentText() { return roleColor(keel::ui::Role::AccentText); }
// The one button that does it, filled with the accent (until popAccent).
inline void pushAccent() {
    using keel::ui::Role;
    ImGui::PushStyleColor(ImGuiCol_Button, roleColor(Role::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, roleColor(Role::AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, roleColor(Role::AccentActive));
    ImGui::PushStyleColor(ImGuiCol_Text, roleColor(Role::OnAccent));
}
inline void popAccent() { ImGui::PopStyleColor(4); }

// An object as it'll be printed: where it stands, with its finish, checked (kept until it changes).
struct PrintInfo {
    uint64_t key = ~0ull;
    Mesh world;
    PrintCheck check;
    glm::vec3 lo{0.0f}, hi{0.0f};
    keel::ModelId overhang{};
    bool hasOverhang = false;
    // Walls too thin to print: how thick at each face (worked out again only when its shape or
    // size changes: moving or turning it doesn't), how many under the least, the thinnest.
    uint64_t thickKey = ~0ull;
    std::vector<float> thick;
    int thinFaces = 0;
    float thinnest = 1e30f;
    keel::ModelId thin{};
    // The slice preview's layer: where it was cut, and for which shape (kept beside what it's of).
    mutable uint64_t sliceKey = ~0ull;
    mutable float sliceZ = -1;
    mutable std::vector<std::pair<glm::vec3, glm::vec3>> slice;
};

extern keel::Cvar fry_printer;
extern keel::Cvar fry_bed_x;
extern keel::Cvar fry_bed_y;
extern keel::Cvar fry_bed_z;
extern keel::Cvar fry_touch;
extern keel::Cvar fry_uiscale;
extern keel::Cvar fry_uisize;
extern keel::Cvar fry_theme;
extern keel::Cvar fry_pluginsoff;
extern keel::Cvar fry_autosave;

class App : public keel::Game {public:
    explicit App(std::string path) : startPath_(std::move(path)) {}
    const char* name() const override { return "Fry3DX"; } // (its settings folder, and bin/Fry3DX/)
    const char* title() const override { return "Fry3DX"; }
    bool wantsCursor() const override { return true; }
    bool dockingUi() const override { return true; }

    // "Spiral_stairs" is "Spiral stairs" (typed in the console).
    static bool sameCommand(const std::string& name, std::string typed);
    void init() override;

    // Going into the background: the work is saved now (a phone may close the app while it's
    // away, to free memory), and a phone's next start carries on with it, view and all.
    std::string resumeFile() const { return keel::prefPath() + "resume.txt"; }
    void onBackground() override;
    void resumeWork();

    void frame(double) override { autosave(); }
    bool allowQuit() override;
    void shutdown() override {
        if (autosaveJob_.valid()) autosaveJob_.wait();
    }
    void onEscape() override;

    void render(keel::Renderer& r) override;

private:
    // ── The document ──
    Scene scene_;
    std::string startPath_, path_;
    bool dirty_ = false;
    // An undo step. Its objects without their shapes (points, faces, painting's layout): those are
    // kept apart and shared with the steps before and after where they're the same, so a step
    // that moved one thing doesn't copy every shape in the scene again.
    struct Shape {
        std::shared_ptr<const Mesh> mesh;
        std::shared_ptr<const std::vector<std::vector<glm::vec2>>> uvs;
    };
    struct Snapshot {
        Scene scene;
        std::map<uint32_t, Shape> shapes;
        std::set<uint32_t> selected;
        uint32_t active;
        Mode mode;
        std::set<int> verts, faces;
        std::set<std::pair<int, int>> edges;
        std::string label; // (what was done after it: the message said then)
        double at = 0;
    };
    std::vector<Snapshot> undo_, redo_;
    std::map<uint32_t, Shape> lastShapes_; // (each object's shape as last kept: the next step shares it if it's the same)
    Snapshot snapshot(std::string label, double at);
    std::vector<Snapshot> redoKept_; // (what redo had before the last undo point: back if that step is dropped)

    Mode mode_ = Mode::Object;
    Tool tool_ = Tool::Move;
    Elem elem_ = Elem::Face;
    std::set<uint32_t> selected_;
    uint32_t active_ = 0;
    // the object last picked (edited in shape editing)
    uint32_t activeRef_ = 0; // a reference picture picked in the Layers panel
    std::set<int> selVerts_, selFaces_;
    std::set<std::pair<int, int>> selEdges_;

    // ── Tabs: modelling, painting pictures onto it, making a picture of it ──
    Tab tab_ = Tab::Model;

    // ── The view ──
    keel::Renderer* r_ = nullptr;
    glm::vec2 shift_{0.0f}; // (the view's middle on the screen: it sits between the panels)
    View view_ = View::Perspective;
    glm::vec3 target_{0, 0, 0.4f};
    float yaw_ = -135.0f, pitch_ = 25.0f, dist_ = 4.0f, fov_ = 50.0f;
    glm::vec2 nearFar_{2.0f, 16384.0f};
    std::map<uint32_t, std::pair<uint64_t, float>> radii_; // (each object's mesh: how far it reaches from its place)
    ImVec2 viewMin_{0, 0}, viewMax_{1, 1};
    glm::mat4 vp_{1.0f};
    glm::vec3 eye_{0.0f};
    bool orbiting_ = false, panning_ = false;
    bool xray_ = false, showGrid_ = true, renderGrid_ = false;
    std::string appliedLook_;
    keel::ModelId ground_{};
    bool haveGround_ = false;
    Shading shading_ = Shading::Solid;

    // ── Tools ──
    bool snap_ = false;
    int snapKind_ = 0; // (moving: 0 steps, 1 points, 2 edges' middles, 3 faces' middles)
    float snapMove_ = 0.05f, snapAngle_ = 15.0f, snapScale_ = 0.1f;
    float drawThickness_ = 0.05f, extrudeAmount_ = 0.1f, insetAmount_ = 0.2f, mergeDistance_ = 0.001f;
    float bevelWidth_ = 0.05f;
    int bevelSteps_ = 3, cuts_ = 1;
    Orient orientObject_ = Orient::Local, orientEdit_ = Orient::Normal;
    Pivot rotatePivot_ = Pivot::Middle, scalePivot_ = Pivot::Opposite;
    SelShape selShape_ = SelShape::Box;
    float brushRadius_ = 30.0f;
    std::vector<glm::vec3> drawPts_;
    // Drawing on an object: the plane of the face the first point went on, and what's made with it
    // (0 a new object standing on it, 1 added to it, 2 cut into it).
    glm::vec3 drawOnP_{0.0f}, drawOnN_{0.0f, 0.0f, 1.0f};
    uint32_t drawOnto_ = 0;
    int drawOnMode_ = 0;
    struct Drag {
        bool active = false;
        int handle = -1; // 0-2 an axis, 3 free / even / about the view, 4-6 an axis's other end
        Tool tool = Tool::Select;
        Axes axes{};
        glm::vec3 pivot{0.0f}, anchor{0.0f}, handlePoint{0.0f};
        ImVec2 startMouse{0, 0};
        float startT = 0.0f, startDist = 0.0f, startMid = 0.0f; // (the handle's way from what stays put; from the middle)
        glm::vec3 startHit{0.0f};
        Scene before;
        std::set<std::pair<int, int>> bevelEdges;
        std::vector<std::pair<int, float>> soft; // points near the selection that follow it, and how much
        std::string typed; // (a number typed while dragging: exactly that much)
    } drag_;
    // Soft selection (shape editing): points within `softReach_` of the selection follow it, less
    // the further they are (Blender's proportional editing, Maya's soft select).
    bool softSel_ = false;
    float softReach_ = 0.3f;
    // Push out / pull in (Alt+S): the points and the shape as it was when it started.
    bool showPush_ = false;
    float pushBy_ = 0.0f;
    std::set<int> pushPts_;
    Mesh pushFrom_;
    uint32_t pushObject_ = 0;
    bool boxing_ = false, painting_ = false, lassoing_ = false, tapDraw_ = false;
    ImVec2 boxStart_{0, 0};
    std::vector<ImVec2> lasso_;
    std::pair<int, int> cutHover_{-1, -1};
    // The knife: points clicked on the screen (or a straight drag).
    std::vector<ImVec2> knifePts_;
    bool knifeDown_ = false;
    ImVec2 knifePress_{0, 0};
    bool knifeThrough_ = false, knifeFill_ = true;
    int knifeAfter_ = 1; // cut right through, then: 0 just the lines, 1 pieces of the one object, 2 objects of their own, 3 the smaller ones gone
    // Holes: pressed on a face, dragged out to size.
    // (sizes in metres where it stands: across = a round's width, a rectangle's width, a triangle's
    // side, a hexagon's flats; tall: a rectangle's height)
    float holeSize_ = 0.2f, holeTall_ = 0.1f, holeDepth_ = 0.1f, holeTurn_ = 0.0f, holeRoom_ = 0.0003f;
    int holeShape_ = 0, holeSides_ = 32; // a HoleShape
    int holeMode_ = 0;
    // 0 all the way through, 1 a pocket, 2 the outline only (a new face)
    struct HoleDrag {
        bool active = false;
        uint32_t object = 0;
        int face = -1;
        glm::vec3 centre{0.0f}; // (the object's own space)
        float radius = 0.0f;
        ImVec2 press{0, 0};
    } hole_;

    // ── Painting ──
    PaintTool paintTool_ = PaintTool::Brush;
    glm::vec4 paintColor_{0.85f, 0.25f, 0.1f, 1.0f}; // a: how strong
    float brushPx_ = 16.0f, hardness_ = 0.5f, fillTolerance_ = 0.15f;
    int pictureSize_ = 1024;
    bool showUvLines_ = true;
    struct PaintStroke {
        bool active = false, in2D = false, haveLast = false, waiting = false; // waiting: pressed off the object, starts when the brush gets to it
        uint32_t object = 0;
        std::shared_ptr<Image> image; // (the object's picture: a copy made for this stroke)
        Stroke stroke;
        glm::vec3 last{0.0f};
        glm::vec2 last2D{0.0f};
    } paint_;
    // Sculpting (the Paint tab's Shape side): brushes that push the surface about like clay.
    bool sculpting_ = false;
    SculptBrush sculptBrush_ = SculptBrush::Draw;
    float sculptStrength_ = 0.5f, sculptPx_ = 60.0f; // (its own brush size: sculpting wants a bigger one than painting)
    bool paintMirror_ = false, stencilOn_ = false, drawingStencil_ = false;
    std::vector<ImVec2> stencil_; // (on the screen)
    bool sculptMirror_ = false; // the same on the other side (left and right: the object's Y)
    struct SculptStroke {
        bool active = false, haveLast = false, waiting = false;
        uint32_t object = 0;
        std::unique_ptr<SculptMesh> mesh;
        glm::vec3 last{0.0f};
        // Grab: where it was taken hold of (the world), and what (the object's own space).
        glm::vec3 start{0.0f};
        std::vector<std::pair<int, float>> held[2];
        std::vector<glm::vec3> from[2];
    } sculpt_;
    // A picture layer unlocked to be moved: which, and what's being dragged (a corner 0-3, 4 the
    // whole of it, 5 the turning handle) from where.
    struct LayerEdit {
        uint32_t object = 0;
        int index = -1, drag = -1;
        bool fresh = false; // (just pasted: Esc takes it away again)
        glm::vec2 grab{0.0f}, offset{0.0f};
        std::array<glm::vec2, 4> startCorners{};
        PictureLayer start;
    } ledit_;
    bool pasteOnSurface_ = true;
    uint32_t renamingFolder_ = 0;
    // Touch (set when it starts: fry_touch).
    bool touchMode_ = false, portrait_ = false, showToolbox_ = false, panelsOpen_ = false, viewsOpen_ = false, shiftLatch_ = false, ctrlLatch_ = false, touchOrbit_ = false;
    struct Gesture {
        bool two = false, longDone = false, rightUp = false;
        ImVec2 mid{}, downPos{};
        float spread = 1;
        double downAt = -1;
    } gest_;
    std::string captureUri_; // (a picture of the view going into a picked file)
    struct CopiesSetting {
        int count = 3, axis = 2;
        bool round = false, joined = false, path = false, follow = true; // (path: along the last picked object)
        glm::vec3 step{0.3f, 0.0f, 0.0f}, centre{0.0f};
        float angle = 360.0f;
    } copies_;
    bool showCopies_ = false, showThicken_ = false, thicknessOut_ = false;
    float thickness_ = 0.002f;
    // The Layout tab: what's picked (frames, or corners as face and corner) and what's dragged.
    struct LayoutState {
        uint32_t object = 0;
        bool corners = false;
        std::set<int> frames;
        std::set<std::pair<int, int>> points;
        int drag = 0; // 1 moving / turning / sizing, 2 a box
        glm::vec2 grab{0.0f}, centre{0.0f};
        ImVec2 boxFrom{};
        std::vector<std::vector<glm::vec2>> start;
        std::shared_ptr<const Image> startPicture;
        double remapAt = 0;
    } lay_;
    LayTool layTool_ = LayTool::Move;
    bool layKeepPaint_ = true, layChecker_ = false;
    // The Print tab.
    PrintTool printTool_ = PrintTool::None;
    std::vector<glm::vec3> measure_;
    bool measureSnap_ = true, showOverhangs_ = true, sizing_ = false, showThin_ = true, showLayer_ = false;
    int layer_ = 1;
    float layerHeight_ = 0.0002f; // (m)
    float minWall_ = 0.0008f; // (m: the thinnest wall that prints well, twice a 0.4 mm nozzle)
    float overhangDeg_ = 45.0f, infill_ = 15.0f;
    int material_ = 0;
    std::map<uint32_t, PrintInfo> printInfos_;
    keel::ModelId bed_{};
    glm::vec3 bedFor_{0.0f};
    // Closing, New or Open with changes not saved: asked first (askSave_), then done; afterSave_:
    // what to do once the Save it chose has finished (0 nothing, 1 quit, 2 new, 3 open).
    int askSave_ = 0, afterSave_ = 0; // (what to do then: 1 quit, 2 new, 3 open..., 4 open dropped_)
    std::string dropped_;
    bool dialogCancelled_ = false;
    // Autosave: a copy packed small in the background every fry_autosave minutes while there are
    // changes, kept until saved or closed properly; offered back after a crash.
    std::string autosavePath_;
    uint64_t edits_ = 0, autosavedEdits_ = 0;
    double lastAutosave_ = 0;
    std::future<bool> autosaveJob_;
    std::vector<std::string> recover_;
    bool showRecover_ = false;
    std::map<uint32_t, std::vector<uint8_t>> owner_; // which layer shows at each pixel
    struct Uploaded {
        uint64_t stamp = 0;
        double at = 0;
        bool dirty = false;
    };
    std::map<uint32_t, Uploaded> uploaded_;
    std::map<uint32_t, keel::TextureId> paintTex_;
    float picZoom_ = 0.0f; // the Picture panel: screen pixels a picture pixel, and where its corner is
    ImVec2 picPan_{0, 0};
    uint32_t picFor_ = 0;

    // ── Saving a picture of the view (cropped to it, without the gizmos) ──
    int captureFrames_ = 0;
    std::string capturePath_;

    // ── GPU side ──
    std::map<uint32_t, std::pair<keel::ModelId, uint64_t>> models_; // object id → its model, the state built
    std::map<uint32_t, uint64_t> versions_;
    uint64_t versionClock_ = 1;
    struct Shown {
        uint64_t version = 0; // (the shape key it was made for)
        bool mirrorOnly = false;
        Mesh mesh;
        std::shared_ptr<const fry::TriangleTree> tree; // (made with it, when it was made on another thread)
        std::future<std::pair<Mesh, std::shared_ptr<const fry::TriangleTree>>> coming; // (a big one being made on another thread)
        uint64_t comingVersion = 0;
        bool comingMirror = false;
    };
    std::map<uint32_t, Shown> shown_;
    struct Picture {
        bool ok = false;
        float aspect = 1.0f;
    };
    std::map<std::string, Picture> pictures_;
    keel::TextureId logo_{};
    bool logoTried_ = false, haveLogo_ = false;

    // ── UI ──
    int appliedTheme_ = -1;
    // The Script panel: the script being written, where it's from, what it printed.
    bool showScript_ = false, showScriptHelp_ = false;
    size_t script_ = 0; // the file's script shown in the Script panel
    std::string scriptPath_; // where it was last saved or opened from (Save as starts there)
    // Plug-ins (Script > Plug-ins): each file, whether it's on, and how loading it went.
    struct PluginEntry {
        std::string path, file, error;
        bool bundled = false, on = true;
        std::unique_ptr<fry::Plugin> p;
    };
    std::vector<PluginEntry> plugins_;
    bool showPlugins_ = false;
    std::pair<std::string, std::string> pluginTool_; // (the plug-in tool that's on: its file and name)
    std::string pluginFilter_;
    // ── Commands: found by typing, on keys of your own, round the mouse ──
    std::vector<fry::Command> commands_;
    fry::CommandSearch search_;
    std::vector<std::string> recentCommands_;
    std::map<std::string, fry::Chord> myKeys_; // (a command's id: the key you gave it)
    int pie_ = 0;
    // 1: the tools round the mouse (Space held), 2: the views (Shift+Space)
    ImVec2 pieAt_{0, 0};
    bool pieStays_ = false; // (opened from the menu or console: until a click)
    std::string settingKey_;
    bool showHistory_ = false;
    // ── Workspaces: the panels as they were, kept under a name ──
    bool showWorkspaces_ = false, layoutAgain_ = false;
    std::string pendingLayout_;
    char workspaceName_[64] = "My workspace";
    // (the command waiting for its new key, in Keys and mouse)
    // A plug-in's command waiting for its settings (the form), and a material being named.
    struct PendingCommand {
        size_t plugin = 0, command = 0;
        std::vector<double> values;
    };
    std::optional<PendingCommand> pendingCommand_;
    bool nameMaterial_ = false;
    std::string materialName_;
    bool materialKeepColor_ = false;
    std::vector<std::pair<std::string, bool>> scriptLog_; // (line, an error)
    bool scriptLogToBottom_ = false;
    bool styled_ = false, layoutDone_ = false, showHelp_ = false, showAbout_ = false;
    std::string iniPath_, message_;
    double messageAt_ = -100.0;
    char renameBuf_[128] = "";
    uint32_t renaming_ = 0;
    std::mutex dialogMutex_;
    std::string dialogResult_;
    int dialogKind_ = 0; // 1 open, 2 save, 3 export, 4 reference picture, 5 picture of the view, 6 a picture to paint on, 7 save the painted picture
    int pendingKind_ = 0, dialogFilter_ = -1;

    // ───────────────────────────── document ─────────────────────────────

    Object* find(uint32_t id);
    Reference* findRef(uint32_t id);
    Object* editing() { return mode_ == Mode::Edit ? find(active_) : nullptr; }
    void touched(uint32_t id) { versions_[id] = ++versionClock_, dirty_ = true, ++edits_; }
    // What decides the shape an object shows, as a number: its points and faces, its finishing
    // touches, creases, cuts, shading and painting's layout (not where it is). Worked out again
    // only when it's been touched: moving a smoothed object doesn't smooth it again each frame.
    struct ShapeKey {
        uint64_t version = 0, key = 0;
    };
    std::map<uint32_t, ShapeKey> shapeKeys_;
    uint64_t shapeKey(const Object& o);
    void touchAll() {
        for (const Object& o : scene_.objects) versions_[o.id] = ++versionClock_;
    }
    // Its shape has been changed by hand: the library settings no longer apply.
    void handMade(Object& o) { o.recipe.clear(), o.settings.clear(); }
    void say(std::string text);

    void pushUndo();
    // The undo point just made, dropped (what it was for didn't happen): redo as it was before.
    void dropUndo();
    // Back to the undo point just made, and it dropped.
    void takeBack();
    void restore(Snapshot& s);
    void undo();
    void redo();

    void newScene();
    Object& addObject(const std::string& base, Mesh mesh);
    std::string uniqueName(const std::string& base);

    // A shape from the library, standing on the ground under the middle of the view.
    void addShape(const std::string& id, bool undoable = true);

    void deleteSelected();

    void duplicateSelected();

    // The selected objects become one (the last picked keeps its place, look and name).
    void joinSelected();

    // The selected faces become an object of their own.
    void separateSelected();

    // ───────────────────────────── files ─────────────────────────────

    // ── Files a phone's file picker gives (content://...): read and written through SDL ──
    static bool isUri(const std::string& p) { return p.starts_with("content://"); }
    // A file's name as shown (a picked file's: from the end of its address, decoded).
    static std::string fileName(const std::string& p);
    // A picked file copied to a file of our own to read ("" if it couldn't be).
    static std::string fromUri(const std::string& uri, const std::string& name);
    // A file of ours written into the picked one.
    static bool toUri(const std::string& local, const std::string& uri);
    static std::string tempFile(const std::string& name);

    void openFile(const std::string& picked);
    bool saveFile(std::string path);
    static std::string sizeText(uintmax_t n) {
        return n < 10240 ? std::format("{} bytes", n) : n < 10 << 20 ? std::format("{} KB", n >> 10) : std::format("{:.1f} MB", double(n) / 1048576.0);
    }
    static std::vector<uint8_t> pngBytes(const Image& im);
    void exportFile(std::string path, int filter = -1, const std::string& asExt = "");

    // ── Closing with changes, autosave ──

    // New, Open, Quit: asks about saving first if there are changes.
    void whenSaved(int action);
    void doAfterAsking(int action);
    // A file dropped on the window (or opened with Fry3DX from the desktop): a .fry opens (after
    // asking about changes), a script goes in the Script panel, a picture goes on as a reference
    // (or, in the Paint tab, as a sticker on the model).
    void onFileDropped(const std::string& path) override;
    void saveQuestion();
    std::string autosaveDir() const { return keel::prefPath() + "autosave/"; }
    // Every fry_autosave minutes (when there are changes and nothing's being dragged): a copy
    // packed as small as it goes, in the background.
    void autosave(bool force = false);
    // Its autosave isn't needed (saved, or closed on purpose).
    void dropAutosave();
    // Autosaves left behind (it didn't close properly): offered back. Only the newest 15 are
    // kept; older ones are deleted.
    void findAutosaves();
    void recoverWindow();

    // A picture of the 3D view only (no panels, gizmos or outlines): the screen is saved a few
    // frames on, with those hidden, then cut down to the view.
    void savePicture(std::string path);
    bool capturing() const { return captureFrames_ > 0; }
    void captureStep();
    static bool loadImageFile(const std::string& path, Image& out);
    static bool writePng(const std::string& path, const Image& im) { return stbi_write_png(path.c_str(), im.w, im.h, 4, im.px.data(), im.w * 4) != 0; }
    void addPicture(const std::string& path);

    // SDL's own file dialogs (the desktop's): the answer comes back later, maybe on another thread.
    void dialog(int kind);
    void takeDialogResult();

    // ───────────────────────────── modes and tools ─────────────────────────────

    void setTool(Tool t);
    void enterEdit();
    void leaveEdit();
    void clearElements() { selVerts_.clear(), selFaces_.clear(), selEdges_.clear(); }
    void selectAllElements();
    void selectAll();
    void selectNone();
    // The points the selection covers, whichever kind is picked.
    std::set<int> selectedPoints();
    // Selects (in the current kind) what these points cover.
    void selectPoints(const std::set<int>& pts);
    void growSelection() {
        if (Object* o = editing()) selectPoints(growVerts(o->mesh, selectedPoints()));
    }
    void shrinkSelection();
    // Like the picked: faces that face the same way, edges as long, points with as many edges.
    void selectSimilar();
    // The picked edges' whole loops (or the rings across them).
    void loopsOfSelected(bool ring);
    void selectLinked() {
        if (Object* o = editing()) selectPoints(linkedVerts(o->mesh, selectedPoints()));
    }
    void invertSelection();
    // The edges a rounding would work on: those selected, or round the selected faces, or between
    // selected points.
    std::set<std::pair<int, int>> edgesToRound();

    void deleteElements();
    // After a change that renumbers things: nothing selected, the shape is hand-made now.
    void changedShape(Object& o, bool keepSelection = false);
    void extrudeNow(float amount);
    void insetNow(float amount);
    void bevelNow();
    void subdivideNow(bool smooth);
    void cutAt(Object& o, std::pair<int, int> edge);
    void makeFaceNow();
    void mergeNow();
    void smoothPointsNow();
    void startPush();
    void pushWindow();
    void bridgeNow();
    void flipNow();
    // Rotation and scale baked into the points (so the object's turn is none and its size is 1).
    void applyTransform(Object& o);
    void applyFinish(Object& o);
    void originToMiddle(Object& o);
    void originToBottom(Object& o);
    void mirror(Object& o, int axis);


    // ───────────────────────────── tabs ─────────────────────────────

    void setTab(Tab t);
    Shading shading() const { return tab_ == Tab::Render ? Shading::Rendered : shading_; }

    // ───────────────────────────── the knife and holes ─────────────────────────────

    // Snapped to steps of 45 degrees from the last point while Ctrl is held.
    ImVec2 knifeEnd(const ImGuiIO& io) const;
    static Mesh subMesh(const Mesh& m, const std::set<int>& faces);
    // A new object like `from` (its look, place and finish), with another shape.
    uint32_t objectLike(uint32_t from, Mesh mesh, const std::string& name);
    void knifeApply(uint32_t id, std::vector<ImVec2> pts);
    static float objectScale(const Object& o) { return std::max(std::cbrt(std::abs(glm::determinant(glm::mat3(objectMatrix(o))))), 1e-6f); }
    // How wide (across) a shape is when it reaches `reach` from its middle (dragging it out).
    float holeAcrossFor(float reach) const {
        return HoleShape(holeShape_) == HoleShape::Triangle || HoleShape(holeShape_) == HoleShape::Hexagon ? reach * std::sqrt(3.0f) : reach * 2.0f;
    }
    // The outline, `across` wide in the object's own size (a rectangle keeps its proportions).
    std::vector<glm::vec3> holeShapeOutline(const Object& o, int face, const glm::vec3& centre, float across) const;
    void applyHole(uint32_t id, int face, const glm::vec3& centre, float across);
    // The hole tool's outline under the mouse (or being dragged out), in the world.
    std::vector<glm::vec3> holePreview(const ImGuiIO& io);
    // Where the mouse is on a face's plane (the object's own space).
    std::optional<glm::vec3> onFacePlane(const Object& o, int face, ImVec2 mouse);

    // ───────────────────────────── painting ─────────────────────────────

    // The object ready to be painted: its rounding and smoothing made part of it, its surface laid
    // out on a picture (made the plain colour of it at first).
    Object* readyToPaint(uint32_t id);
    // The object's picture, copied so this change can be undone; the copy is now the object's.
    Image& changePicture(Object& o);
    void beginStroke(Object& o, bool in2D);
    void endStroke();
    // Where on its picture a ray hits an object (0..1), or nothing.
    std::optional<glm::vec2> uvUnder(const Object& o, const Ray& ray);
    // One dab of the brush on the model: on every face near it, into the picture where that face
    // lies (so it carries on across the seams between pieces of the layout).
    // One dab of paint at a point of the object (the world), on the faces towards `eye` (the
    // mirrored dab: towards the eye mirrored). The stencil: only where the surface shows inside
    // it on the screen (the mirrored dab: where its mirror image does, `back` taking it there).
    void dab3D(Object& o, const glm::vec3& at, float radius, const glm::vec3* eyeAt = nullptr, const glm::mat4* back = nullptr);
    // The brush at a point of the view: starts a stroke on what's there, or carries one on.
    void paintAt(ImVec2 mouse, bool start);
    void pickColor(const Object& o, glm::vec2 uv);
    void fillAt(Object& o, glm::vec2 uv);

    // ── Sculpting ──

    // The object ready to sculpt: its finishing touches made part of it, and made finer (the same
    // shape) if it has too few points to push about.
    Object* readyToSculpt(uint32_t id);
    void endSculpt();
    // The brush at a point of the view: starts a stroke on what's there, or carries one on.
    void sculptAt(ImVec2 mouse, bool start, bool ctrl, bool shift);
    void sculptViewport(const ImGuiIO& io);

    // ── Picture layers ──

    PictureLayer* editLayer();
    // A picture put on the selected object as a new layer, unlocked to be moved: a sticker on the
    // surface in the middle of the view, or pinned onto the flat picture.
    void startFloating(Image img);
    void floatFromFile(const std::string& path);
    void pasteFromClipboard();
    // Enter: it stays where it is, a layer (click it in the Picture panel to move it again).
    void lockLayer();
    void unlockLayer(uint32_t object, int index);
    // Esc: a picture just pasted goes again; one unlocked is locked again.
    void cancelFloating();
    void deleteLayer(Object& o, int i);
    // The layer painted into the painting for good (it can't be moved after).
    void mergeLayer(Object& o, int i);
    // The handles of a sticker being moved, in the world: its corners (top-left, top-right,
    // bottom-right, bottom-left), its turning handle, its middle — on its face's plane.
    bool stickerHandles(const Object& o, const PictureLayer& l, std::array<glm::vec3, 6>& out);
    int stickerHandle(const Object& o, const PictureLayer& l, ImVec2 mouse);
    // Moving a sticker on the model: it slides over the surface (onto other faces too), its
    // corners size it, its handle turns it.
    void stickerMouse(const ImGuiIO& io);
    // A cheap fingerprint of what the object's picture shows (its painting, its layers, its shape).
    uint64_t pictureStamp(const Object& o);
    // The object's picture on the graphics card (its painting with its layers on top): sent again
    // when it changes — while painting or dragging, at most 20 times a second.
    void uploadPicture(const Object& o, bool now_ = false);

    // ───────────────────────────── the Layout tab ─────────────────────────────
    // The frames an object's faces are laid out in on its picture, moved, turned and sized by
    // hand (whole frames, or their corners); what's painted goes with them unless you say not.

    // What's picked, as (face, corner) pairs of its layout.
    std::vector<std::pair<int, int>> layoutPicked(const Object& o, const std::vector<std::vector<int>>& islands) const;
    // A corner and every other on the same point of the model at the same place on the picture.
    std::set<std::pair<int, int>> layoutCorner(const Object& o, int f, int c) const;
    void layoutSelectAll();
    // The picked corners moved by `to` (picture pixels), painting carried along if wanted.
    void layoutChange(Object& o, const std::vector<std::vector<glm::vec2>>& from, const std::shared_ptr<const Image>& picture,
                      const std::vector<std::pair<int, int>>& picked, const std::function<glm::vec2(glm::vec2)>& to, bool final);
    // Buttons' changes (turn, flip, straighten): about the middle of what's picked.
    void layoutApply(const std::function<glm::vec2(glm::vec2, glm::vec2)>& to);
    void layoutStraighten();
    // Laid out again by itself, what's painted carried over onto the new layout.
    void layoutAgain(Object& o);

    template <class F>
    void layoutCanvas(Object& o, ImDrawList* dl, F toScreen, glm::vec2 mouse, bool hovered, int W, int H) {
        const ImGuiIO& io = ImGui::GetIO();
        if (lay_.object != o.id) lay_ = {}, lay_.object = o.id;
        const glm::vec2 size{float(W), float(H)};
        const auto islands = uvIslands(o.mesh, o.uvs);
        std::vector<int> islandOf(o.mesh.faces.size(), -1);
        for (size_t i = 0; i < islands.size(); ++i)
            for (int f : islands[i]) islandOf[size_t(f)] = int(i);
        auto screen = [&](int f, int c) { return toScreen(o.uvs[size_t(f)][size_t(c)] * size); };
        auto faceUnder = [&]() {
            for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
                std::vector<ImVec2> poly;
                for (size_t c = 0; c < o.uvs[f].size(); ++c) poly.push_back(screen(int(f), int(c)));
                if (insidePolygon(io.MousePos, poly)) return int(f);
            }
            return -1;
        };
        auto cornerUnder = [&]() {
            std::pair<int, int> best{-1, -1};
            float d = 9.0f;
            for (size_t f = 0; f < o.uvs.size(); ++f)
                for (size_t c = 0; c < o.uvs[f].size(); ++c) {
                    const ImVec2 q = screen(int(f), int(c));
                    if (const float e = std::hypot(q.x - io.MousePos.x, q.y - io.MousePos.y); e < d) d = e, best = {int(f), int(c)};
                }
            return best;
        };
        const auto picked = layoutPicked(o, islands);
        std::set<int> pickedFaces;
        for (auto [f, c] : picked) pickedFaces.insert(f);
        // Pressing: pick (what's under it, or a box), then move / turn / size what's picked.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !lay_.drag) {
            bool hit = false, wasPicked = false;
            if (lay_.corners) {
                const auto [f, c] = cornerUnder();
                if (f >= 0) {
                    const auto same = layoutCorner(o, f, c);
                    hit = true, wasPicked = lay_.points.count({f, c}) > 0;
                    if (io.KeyShift && wasPicked)
                        for (auto p : same) lay_.points.erase(p);
                    else {
                        if (!io.KeyShift && !wasPicked) lay_.points.clear();
                        lay_.points.insert(same.begin(), same.end());
                    }
                }
            } else if (const int f = faceUnder(); f >= 0) {
                const int i = islandOf[size_t(f)];
                hit = true, wasPicked = lay_.frames.count(i) > 0;
                if (io.KeyShift && wasPicked) lay_.frames.erase(i);
                else {
                    if (!io.KeyShift && !wasPicked) lay_.frames.clear();
                    lay_.frames.insert(i);
                }
            }
            const bool anything = !lay_.frames.empty() || !lay_.points.empty();
            const bool transform = layTool_ != LayTool::Select && anything && !(io.KeyShift && hit) && (hit || layTool_ != LayTool::Move);
            if (transform) {
                pushUndo();
                lay_.drag = 1, lay_.grab = mouse, lay_.start = o.uvs, lay_.startPicture = o.picture;
                glm::vec2 lo(1e30f), hi(-1e30f);
                for (auto [f, c] : layoutPicked(o, islands)) lo = glm::min(lo, o.uvs[size_t(f)][size_t(c)] * size), hi = glm::max(hi, o.uvs[size_t(f)][size_t(c)] * size);
                lay_.centre = (lo + hi) * 0.5f;
            } else if (!hit) {
                if (!io.KeyShift) lay_.frames.clear(), lay_.points.clear();
                lay_.drag = 2, lay_.boxFrom = io.MousePos;
            }
        }
        if (lay_.drag == 1) {
            const glm::vec2 g = lay_.grab - lay_.centre, m = mouse - lay_.centre;
            std::function<glm::vec2(glm::vec2)> to;
            if (layTool_ == LayTool::Move || layTool_ == LayTool::Select) {
                const glm::vec2 d = mouse - lay_.grab;
                to = [d](glm::vec2 p) { return p + d; };
                message_ = std::format("Moved {:.0f}, {:.0f} px", d.x, d.y);
            } else if (layTool_ == LayTool::Rotate) {
                float a = std::atan2(m.y, m.x) - std::atan2(g.y, g.x);
                if (io.KeyCtrl) a = glm::radians(snapTo(glm::degrees(a), 15.0f));
                const glm::vec2 c = lay_.centre;
                to = [a, c](glm::vec2 p) { return c + glm::vec2(std::cos(a) * (p - c).x - std::sin(a) * (p - c).y, std::sin(a) * (p - c).x + std::cos(a) * (p - c).y); };
                message_ = std::format("Turned {:.0f} degrees", glm::degrees(a));
            } else {
                glm::vec2 k(glm::length(m) / std::max(glm::length(g), 1.0f));
                if (io.KeyShift) // (only across or only up: whichever the drag goes along more)
                    k = std::abs(g.x) > std::abs(g.y) ? glm::vec2(m.x / (std::abs(g.x) > 1 ? g.x : 1.0f), 1.0f) : glm::vec2(1.0f, m.y / (std::abs(g.y) > 1 ? g.y : 1.0f));
                const glm::vec2 c = lay_.centre;
                to = [k, c](glm::vec2 p) { return c + (p - c) * k; };
                message_ = std::format("Size {:.0f}% x {:.0f}%", k.x * 100, k.y * 100);
            }
            messageAt_ = now();
            const bool done = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
            layoutChange(o, lay_.start, lay_.startPicture, layoutPicked(o, islands), to, done);
            if (done) lay_.drag = 0, lay_.startPicture.reset();
        }
        // Drawing: every face's outline, what's picked filled; corners as dots.
        const ImU32 fill = (kAccent & 0x00FFFFFF) | 0x38000000;
        for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
            std::vector<ImVec2> pts;
            for (size_t c = 0; c < o.uvs[f].size(); ++c) pts.push_back(screen(int(f), int(c)));
            const bool on = pickedFaces.count(int(f)) && !lay_.corners;
            if (on)
                for (const auto& t : triangulate(o.mesh, int(f))) {
                    ImVec2 q[3];
                    for (int k = 0; k < 3; ++k) q[k] = pts[size_t(std::find(o.mesh.faces[f].begin(), o.mesh.faces[f].end(), t[size_t(k)]) - o.mesh.faces[f].begin())];
                    dl->AddTriangleFilled(q[0], q[1], q[2], fill);
                }
            dl->AddPolyline(pts.data(), int(pts.size()), on ? kAccent : IM_COL32(255, 255, 255, 150), ImDrawFlags_Closed, on ? 2.0f : 1.2f);
        }
        if (lay_.corners) {
            const auto hot = cornerUnder();
            for (size_t f = 0; f < o.uvs.size(); ++f)
                for (size_t c = 0; c < o.uvs[f].size(); ++c) {
                    const ImVec2 q = screen(int(f), int(c));
                    const bool on = lay_.points.count({int(f), int(c)}) > 0;
                    dl->AddCircleFilled(q, on ? 4.5f : 3.0f, on ? kAccent : hot == std::pair{int(f), int(c)} ? kHot : IM_COL32(255, 255, 255, 200));
                }
        } else if (hovered && !lay_.drag)
            if (const int f = faceUnder(); f >= 0 && !lay_.frames.count(islandOf[size_t(f)]))
                for (int g : islands[size_t(islandOf[size_t(f)])]) {
                    std::vector<ImVec2> pts;
                    for (size_t c = 0; c < o.uvs[size_t(g)].size(); ++c) pts.push_back(screen(g, int(c)));
                    dl->AddPolyline(pts.data(), int(pts.size()), kHot, ImDrawFlags_Closed, 1.5f);
                }
        if (lay_.drag == 1 && layTool_ != LayTool::Move && layTool_ != LayTool::Select) { // (the middle it turns or sizes about)
            const ImVec2 c = toScreen(lay_.centre);
            dl->AddCircle(c, 6, kAccent, 16, 2.0f);
            dl->AddLine(c, io.MousePos, (kAccent & 0x00FFFFFF) | 0x90000000, 1.0f);
        }
        if (lay_.drag == 2) { // a box to pick with
            const ImVec2 a = lay_.boxFrom, b = io.MousePos;
            dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(255, 120, 40, 30));
            dl->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), kAccent);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                auto in = [&](ImVec2 q) { return q.x >= std::min(a.x, b.x) && q.x <= std::max(a.x, b.x) && q.y >= std::min(a.y, b.y) && q.y <= std::max(a.y, b.y); };
                for (size_t f = 0; f < o.uvs.size(); ++f)
                    for (size_t c = 0; c < o.uvs[f].size(); ++c)
                        if (in(screen(int(f), int(c)))) {
                            if (lay_.corners) lay_.points.insert({int(f), int(c)});
                            else lay_.frames.insert(islandOf[f]);
                        }
                lay_.drag = 0;
            }
        }
    }
    // In the 3D view: clicking the model picks the frame its face is in (Shift: add).
    void layoutViewport(const ImGuiIO& io);
    void layoutOverlay(ImDrawList* dl);
    void layoutToolbox(float w);
    void layoutOptions(float w);
    void layoutProperties(float w);

    // ───────────────────────────── the Print tab ─────────────────────────────

    glm::vec3 bedSize() const;
    const PrintInfo& printInfo(const Object& o);
    std::vector<Object*> printTargets();
    void drawPrintBed(keel::Renderer& r, const keel::ModelLighting& light);
    void putOnBed();
    void middleOfBed();
    // Everything picked made `mm` big along `axis`, keeping its shape, standing where it stood.
    void printResize(int axis, float mm);
    // Everything picked made `k` times as big, standing where it stood.
    void printScale(float k);
    // The scaler: by a share, to fit the plate, or from the wrong units (a model made in inches,
    // centimetres or metres comes in 25.4, 10 or 1000 times off).
    float scalePercent_ = 100.0f;
    void printScaler(float w);
    void printSizeWindow();
    void printToolbox(float w);
    void printOptions(float w);
    void printViewport(const ImGuiIO& io);
    // Where the mouse is on the model (a corner if near one), or on the bed.
    std::optional<glm::vec3> measurePoint(ImVec2 mouse);
    // The slice preview: where the printer goes round at the chosen layer, over the model.
    void layerOverlay(ImDrawList* dl);
    void printOverlay(ImDrawList* dl, const ImGuiIO& io);
    void printProperties(float w);

    // ───────────────────────────── layers: folders and going to things ─────────────────────────────

    Folder* findFolder(uint32_t id);
    bool inFolder(uint32_t folder, uint32_t within);
    std::set<uint32_t> objectsIn(uint32_t folder);
    // The camera brought round to these (as framing them, but keeping the way it looks).
    void goTo(const std::set<uint32_t>& ids);
    // The picked objects into a new folder (Ctrl+G): a group, picked and moved together.
    void groupSelected();
    // Ungrouped: what's inside goes up a level, the folder goes.
    void ungroup(uint32_t id);
    void deleteFolder(uint32_t id);
    // A folder's eye or lock: all in it the same.
    void showFolder(Folder& f, bool on);
    void lockFolder(Folder& f, bool on);
    void moveToFolder(const std::set<uint32_t>& ids, uint32_t folder);
    // Only these shown (again: everything shown).
    void showOnly(const std::set<uint32_t>& ids);
    void folderMenu(uint32_t into, const std::set<uint32_t>& ids, int depth = 0);
    // What a right-click on an object (or the picked ones) offers.
    void objectMenu(Object& o);
    // A row's show / hide or lock button: the picture of how it is now, dim when it's the usual way.
    bool rowToggle(const char* id, Icon icon, bool usual, const char* tip);
    // The ⌖ button at the end of a row: brings the camera to it.
    bool goButton(const char* tip);
    // Rows for what's in a folder (0: the top): its folders, then its objects.
    void layerRows(uint32_t parent, int depth);

    // ───────────────────────────── combining, copies, thickness ─────────────────────────────

    // The last picked object with the others added, taken out of it, or only where they overlap
    // kept; the others go.
    void combineSelected(Combine op);
    void makeCopies();
    void copiesWindow();
    void thickenActive();
    // A model made elsewhere: each of its objects, standing on the ground in the middle of the view.
    void importFile(const std::string& file);

    // ── Spin: an outline turned round an axis (a vase, a wheel, a ring), or a screw ──
    bool showSpin_ = false, spinWorld_ = false, spinKeep_ = false;
    int spinAxis_ = 2, spinSteps_ = 48;
    float spinDegrees_ = 360.0f, spinRise_ = 0.0f;
    void spinWindow();
    void spinSelected();

    // ── Text as a shape ──
    bool showText_ = false;
    char textBuf_[512] = "Fry3DX";
    int textFont_ = 2, textAlign_ = 0;
    float textHeight_ = 0.05f, textDepth_ = 0.01f, textSpacing_ = 0.0f;
    bool textStanding_ = false;
    std::string textFontFile_; // (a font of your own, picked from a file)
    static constexpr const char* kFonts[] = {"Instrument Sans", "Instrument Sans (semi-bold)", "Instrument Sans (bold)", "JetBrains Mono"};
    static constexpr const char* kFontFiles[] = {"fonts/InstrumentSans-Regular.ttf", "fonts/InstrumentSans-SemiBold.ttf", "fonts/InstrumentSans-Bold.ttf", "fonts/JetBrainsMono-Regular.ttf"};
    void textWindow();
    void addText();

    void thickenWindow();
    void cleanFlatFaces();

    // ───────────────────────────── touch: phones and tablets ─────────────────────────────
    // A phone: everything drawn bigger, the side panels only when asked for, a bar of big buttons
    // at the bottom (undo, Shift and Ctrl that stay down, editing the shape...), and fingers:
    // one drags the view round (or a tool's arrows), two move it and pinch to zoom, holding one
    // still is a right-click.

    bool touch() const { return touchMode_; }
    // How much bigger than its pixels the UI is drawn.
    float touchScale() const;
    void touchFrame(ImGuiIO& io);
    // The bar along the bottom of the view: big buttons, in rows if the view's narrow.
    void touchBar();

    // ───────────────────────────── the camera ─────────────────────────────

    glm::vec3 viewDir() const { return forwardOf(yaw_, pitch_); }
    float viewHeight() const { return 2.0f * dist_ * std::tan(glm::radians(fov_) * 0.5f); } // metres across the view's height at the target
    void setView(View v);
    void leaveFlatView();
    void frameBox(glm::vec3 lo, glm::vec3 hi);
    void frameAll();
    void frameSelected();

    bool mouseInView(const ImGuiIO& io) const;

    void orbitBy(ImVec2 d);
    void panBy(ImVec2 d);

    void updateCamera(ImGuiIO& io);
    // Where the eye is and how the view maps to the screen, from the camera as it is now: again
    // just before drawing, so the model and the lines over it agree (a touch can turn it between).
    void settleView();
    // How near and far the view draws (units), going by how far away it looks: close up to a
    // 5 mm part, or out to see a whole town, the same share of the depth's precision.
    glm::vec2 nearFar();

    // World point → screen (false behind the eye).
    // The window and its view are up (commands given at the start run before the first frame).
    bool viewReady() const { return ImGui::GetCurrentContext() && viewMax_.x - viewMin_.x > 1 && viewMax_.y - viewMin_.y > 1; }
    bool project(const glm::vec3& p, ImVec2& out) const;
    Ray mouseRay(ImVec2 m) const;
    // How many metres a pixel is, at a point.
    float pixelSize(const glm::vec3& at) const;

    // ───────────────────────────── what's shown ─────────────────────────────

    // The mesh as seen: with its rounding, smoothing and mirror — but while its shape is being
    // edited, only the mirror (the rest would hide the points you're moving).
    // Editable cuts show (but not while its own shape's being edited: the points you move).
    // A big one (smoothed into many faces, cut) is made on another thread: what was shown stays
    // till it's ready, so the window doesn't stop.
    const Mesh& shownMesh(const Object& o);
    // The key of the mesh actually shown (the one before, while a new one's being made).
    uint64_t meshKey(const Object& o, const Mesh& mesh);
    // Each object something's cut out of, and what its cutters are (their shapes, where they are):
    // a change, and it's made again (the caches follow its version).
    std::map<uint32_t, uint64_t> cutKeys_;
    std::map<uint32_t, glm::mat4> followAt_; // (where each cut object and cutter was last frame)
    void followCuts();
    bool isCutter(const Object& o) const { return fry::cutTarget(scene_, o) != nullptr; }
    // Editable cut: the last picked has the others cut out of it, and they stay objects to move.
    void cutEditable();
    // The cut made for good: one cutter (or all of what's cut out of it) into the shape, gone.
    void applyCuts(uint32_t target, uint32_t only = 0);

    // ───────────────────────────── picking ─────────────────────────────

    struct Hit {
        uint32_t object = 0;
        int face = -1;
        float t = 1e30f;
    };
    // base: against its own mesh (face numbers of that), not the rounded or smoothed one shown.
    Hit pickObject(const Ray& ray, uint32_t only = 0, bool base = false);
    // Each object's triangles in the world, in a tree of boxes: made again only when it changes
    // (its shape, where it is), not each time something's picked.
    struct Tree { // (in the object's own space: moving it doesn't make it again)
        uint64_t version = 0;
        size_t faces = 0;
        std::shared_ptr<const fry::TriangleTree> tree;
    };
    std::map<std::pair<uint32_t, bool>, Tree> trees_; // (an object's own mesh, or the one shown)
    const fry::TriangleTree& treeOf(const Object& o, const Mesh& mesh);
    // Can the eye see this point of the object (nothing of it in the way)?
    bool visible(const Object& o, const glm::vec3& world);
    // Whether the eye can see an object's point (nothing of it in between): each tried when it's
    // first asked about, kept till the object or the view changes. (Only what's drawn is asked:
    // the outline's points, not every point of a big mesh, as the view turns.)
    struct Visibility {
        uint64_t version = 0;
        glm::vec3 eye{1e30f};
        glm::mat4 vp{0.0f}, matrix{0.0f};
        glm::vec3 eyeIn{0.0f}; // (the eye in the object's own space)
        std::vector<char> point; // 0 not tried yet, 1 seen, 2 hidden
    };
    std::map<std::pair<uint32_t, bool>, Visibility> visibility_; // (an object's own mesh, or the one shown)
    bool seen(const Object& o, const Mesh& mesh, int i);
    // Each edge of a mesh and the (first two) faces it's on, with the faces' normals: worked out
    // again only when the mesh changes.
    struct Adjacency {
        uint64_t version = 0;
        size_t verts = 0, faces = 0;
        std::vector<std::array<int, 4>> edges; // a, b, a face, the other (or -1)
        std::vector<glm::vec4> plane;
        // each face's normal, and its distance along it
    };
    std::map<std::pair<uint32_t, bool>, Adjacency> adjacency_;
    const Adjacency& adjacencyOf(const Object& o, const Mesh& mesh);
    bool pointSeen(const Object& o, int v) { return xray_ || seen(o, o.mesh, v); }

    // The edge of the edited object nearest the mouse on screen (that can be seen), or (-1, -1).
    std::pair<int, int> nearestEdge(Object& o, ImVec2 mouse, float within);

    void clickSelect(const ImGuiIO& io);

    // A whole loop: of edges (points), or a ring of faces, through the edge under the mouse.
    void loopSelect(const ImGuiIO& io, bool ring = false);

    // Selects what's on screen inside a shape (a box, a lasso, the brush).
    void regionSelect(const std::function<bool(ImVec2)>& inside, bool add, bool remove, bool clearFirst);

    // ───────────────────────────── the gizmo ─────────────────────────────

    bool hasSelection();
    // Every point of the selection, in the world.
    std::vector<glm::vec3> selectionPoints();
    glm::vec3 pivotOfSelection();
    // The selection's box, in a gizmo's axes.
    std::pair<glm::vec3, glm::vec3> selectionBox(const Axes& a);
    glm::vec3 selectionMiddle(const Axes& a);
    Orient& orient() { return mode_ == Mode::Edit ? orientEdit_ : orientObject_; }
    // The gizmo's axes: the object's own (turning with it), the world's, or (shape editing) the
    // way the selected faces face — so "up" on a side face is out of that face.
    Axes gizmoAxes();
    // The extrude / inset handle: the selected faces' middle, their normal.
    bool faceHandle(glm::vec3& at, glm::vec3& normal);
    // The round-edges handle: off to the right of the selection's middle.
    bool bevelHandle(ImVec2& centre, ImVec2& handle);
    // The scale box's handles: 0-2 the far side along each axis, 4-6 the near side, 3 the middle.
    // (Pushed out a little where the box is flat, so they don't sit on top of each other.)
    glm::vec3 scaleHandle(const Axes& a, glm::vec3 lo, glm::vec3 hi, int h);

    // Which handle is under the mouse (-1 none).
    int hoverHandle(ImVec2 mouse);

    glm::vec3 originPoint();
    glm::vec3 rotatePivot(const Axes& a) { return rotatePivot_ == Pivot::Origin ? originPoint() : selectionMiddle(a); }

    void startDrag(const ImGuiIO& io, int handle);
    // The points within reach of the selection (in the world: the object's size counts), each with
    // how much it follows: a grid of the selected points, so a big mesh stays quick.
    void findSoft();
    void cancelDrag();

    void updateDrag(const ImGuiIO& io);
    // Mirror editing: each point that moved, its twin across the shape's own X (the point that was
    // where it is, mirrored) moved the mirrored way. Points on the middle stay on it.
    void mirrorEdits(Object& o, const Mesh& before);
    // What moving snaps to near the mouse (points, edges' or faces' middles), in the world: not
    // what's being moved. Marked with a ring.
    std::optional<glm::vec3> snapTarget(ImVec2 mouse, float within = 22.0f);
    // The number typed while dragging, if any (just "-" or "." is none yet).
    std::optional<float> typed() const;
    void dragNow(const ImGuiIO& io);

    // ───────────────────────────── drawing a shape ─────────────────────────────

    // The plane shapes are drawn on: the one the view looks straight at, or the ground.
    void workPlane(glm::vec3& point, glm::vec3& normal) const;
    // The plane drawn on: the face under the first point if it was on an object, else the view's.
    void drawPlane(glm::vec3& p, glm::vec3& n);
    std::optional<glm::vec3> drawPoint(const ImGuiIO& io);
    std::string sketchTyped_; // (drawing a shape: the next line's length, typed)
    // Drawing a shape: numbers typed go to the next line's length, Enter puts the point there.
    bool sketchKeys(const ImGuiIO& io);
    void finishShape();

    // ───────────────────────────── the viewport's mouse ─────────────────────────────

    void viewport(ImGuiIO& io);
    void addDrawPoint(const ImGuiIO& io);
    // Painting: on the model under the mouse.
    void paintViewport(ImGuiIO& io);

    // The brush: whatever passes under it is selected (or, with Ctrl, let go).
    void paint(const ImGuiIO& io);

    // ───────────────────────────── keys ─────────────────────────────

    void shortcuts(ImGuiIO& io);

    // ───────────────────────────── 3D ─────────────────────────────

#if defined(__ANDROID__)
    static constexpr int kRayLight = 0; // (light bounced by ray tracing: not on phones)
#else
    static constexpr int kRayLight = 1;
#endif
    // Solid: a plain grey room and soft studio light, no shadows or effects — for seeing shapes.
    // Rendered: sky, sun, shadows and effects.
    void applyShading();
    // Painted, and shown as painted (with rounding or smoothing added since, it isn't).
    bool painted(const Object& o) const { return o.picture && o.uvsFit() && !o.hasFinish() && !cutKeys_.count(o.id); }

    keel::ModelId modelOf(const Object& o);

    const Picture& picture(const Reference& ref);

    void loadLogo();
    void logoImage(float size);
    // A picture of ours in the UI, read the way ImGui's colours are (not turned linear: too dark).
    void uiImage(ImDrawList* dl, keel::TextureId t, ImVec2 a, ImVec2 b);

    float gridStep() const { return std::pow(10.0f, std::floor(std::log10(std::max(viewHeight() / 4.0f, 1e-4f)))); }

    void draw3D(keel::Renderer& r);

    // (the Print tab's: the printer's plate and overhangs)
    // On top of the scene: outlines and wires, points, the gizmo, the selection shapes.
    // An empty scene: how to start, in the middle of the view (gone once there's anything).
    void emptyHints(ImDrawList* dl);
    void overlays();

    void gizmo(ImDrawList* dl, const ImGuiIO& io);

    // ───────────────────────────── panels ─────────────────────────────

    void setupUi();

    // The least a panel can be and still read right (its words, its rows), by the font's size.
    static ImVec2 panelMin(std::string_view name);
    // Docked panels kept at their least size: where a split leaves one side smaller, the split
    // moves (when the other side has room to give). ImGui's own splitters know only one least
    // size for everything. Gives what the node needs.
    ImVec2 keepMinSizes(ImGuiDockNode* n);

    void defaultLayout(ImGuiID dock);

    // ───────────────────────────── commands ─────────────────────────────

    // Everything that can be done, by name: for the command search, keys of your own, the pies.
    void buildCommands();
    void openSearch();
    void runCommand(const std::string& id); void commandItem(const char* id);
    void commandSearch();
    // The pies: the tools (Space), or the views (Shift+Space), round the mouse while it's held.
    void pieFrame(ImGuiIO& io);
    // Keys of your own: kept in keys.txt (a line each: the command's id, a tab, the key).
    void loadMyKeys();
    void saveMyKeys();

    void menus();
    void menuBar();

    void addMenuItems();

    // How the last item's key looks: filled when it's the one in use, sunk while held.
    static keel::ui::KeyState keyState(bool on);
    // A square button with a picture on it (the accent when it's the one in use).
    bool iconButton(const char* id, Icon icon, bool on, const char* tip, float size = 38.0f);
    // A wide one, with words beside the picture.
    bool wideButton(const char* id, Icon icon, const char* label, bool on, float width, const char* tip = nullptr, float h = 30.0f);
    // Buttons in rows, as many as fit.
    template <class F>
    void grid(int count, float size, F each) {
        const float gap = 5.0f;
        const int perRow = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (size + gap)));
        for (int i = 0; i < count; ++i) {
            if (i % perRow) ImGui::SameLine(0, gap);
            each(i);
        }
    }

    void toolbox();
    // The plug-in tool that's on: which plug-in and which of its tools (none: -1).
    std::pair<int, int> pluginToolAt() const;
    bool pluginToolFound() const { return pluginToolAt().first >= 0; }
    // A plug-in tool's click: the spot on what's under the mouse, or on the ground.
    void clickPluginTool(ImVec2 mouse);

    void paintToolbox(float w);

    void sculptOptions(float w);

    void paintOptions(float w);

    void softChoice(float w);
    void toolOptions();

    void layersPanel();

    // A value that's undoable: the undo point is taken when you start dragging or typing.
    template <class F>
    bool undoable(F widget) {
        const bool changed = widget();
        if (ImGui::IsItemActivated()) pushUndo();
        return changed;
    }

    void paintProperties(float w);

    void renderProperties(float w);

    // The Paint tab's picture: the selected object's, flat, to paint on and place pictures on.
    void picturePanel();

    void propertiesPanel();

    // A material to start from: sets colour, metal and rough in one go (each a little ball of it).
    // Yours (saved here, or from plug-ins and scripts) first; "Save its look as a material" adds one.
    void materialPicker(Object& o);

    void editProperties(Object& o, float w);

    void statusBar();

    // The view's corner: which way you're looking, buttons to look straight at a side, and
    // handles to drag to orbit, pan and zoom (for a touchpad or a pen).
    void viewButtons();

    // ───────────────────────────── scripts ─────────────────────────────

    void scriptLine(std::string text, bool error = false);
    fry::SceneScript* currentScript() { return script_ < scene_.scripts.size() ? &scene_.scripts[script_] : nullptr; }
    // "little_town.lua" -> "little town" (a script's name in the list).
    static std::string scriptName(const std::string& path);
    // Adds a script to the file's list and shows it (the same one again: just shows it).
    void addScript(std::string name, std::string code);
    // Runs the Script panel's script: one undo step; if it stops with an error, everything it did
    // is taken back.
    void runScriptNow();
    // After a script or plug-in command: kept as one undo step, or all taken back on an error.
    void finishRun(const fry::ScriptResult& r, const std::string& name, bool frame);
    void openScript(const std::string& path, const std::string& shownName);
    void saveScript(std::string file);
    // The scripts that come with it (content/Fry3DX/scripts), as menu items that add one.
    void exampleScripts();
    void exampleItems(bool selectable);

    // ── fry_bench: frames timed while the camera turns or a setting's dragged (finding what's slow) ──
    struct Bench {
        std::string what;
        int left = 0;
        std::vector<double> ms;
    } bench_;
    std::vector<std::pair<ImGuiKey, int>> holding_; // (fry_hold: keys held down for that many more frames)
    void benchStep();
    void benchEnd(std::chrono::steady_clock::time_point began);

    // ── Own materials (materials.txt in the settings folder) ──
    static std::string ownMaterialsPath() { return keel::prefPath() + "materials.txt"; }
    void loadOwnMaterials();
    void saveOwnMaterials();

    // ── Plug-ins: content/Fry3DX/plugins (they come with it) and the settings folder's plugins ──
    static std::string pluginsFolder() { return keel::prefPath() + "plugins/"; }
    std::set<std::string> pluginsOff() const;
    void setPluginOn(const std::string& file, bool on);
    void loadPlugins();
    void copyPlugin(const PluginEntry& e);
    void openPluginsFolder();
    bool pluginUsable(size_t i) const { return i < plugins_.size() && plugins_[i].p && plugins_[i].error.empty(); }
    void pluginMenuItems(const char* menu);
    void pluginMenuItem(size_t i, size_t c);
    void runPluginCommand(size_t i, size_t c, const std::vector<double>& values);
    // A plug-in command's settings, asked for before it runs.
    void commandForm();
    void pluginsWindow();
    static int growString(ImGuiInputTextCallbackData* d);
    // A one-line field for a std::string (it grows as it's typed into).
    static bool inputString(const char* label, std::string& text, const char* hint = nullptr) {
        return ImGui::InputTextWithHint(label, hint ? hint : "", text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize, growString, &text);
    }
    void scriptPanel();

    void helpWindow();

    // Keys of your own: any command on any key (they come before the usual ones).
    void myKeysTable();

    // ── (being made) ──
    bool showPrintSize_ = false, liveMirror_ = false, keepProportions_ = false;
    // Each picked object (or all, in the Print tab with none picked) made ready to print.
    void repairSelected();
    // Crease (Shift+E): the picked edges (or those between picked points / round picked faces)
    // kept sharp when the shape's smoothed; again: not.
    void creaseSelected();
    // Slide (Alt+G): the picked points glide along their edges, each along the edge that goes
    // most the way the mouse goes, never past its other end. Click: done. Esc / right click: as it was.
    struct Slide {
        bool active = false;
        uint32_t object = 0;
        Mesh from;
        std::vector<int> pts;
        std::vector<std::vector<int>> rails; // (each point's neighbours that aren't picked: where it can go)
        ImVec2 start{0, 0};
    } slide_;
    void startSlide();
    void cancelSlide();
    void slideFrame(const ImGuiIO& io);

    // Looking straight at a reference picture's plane (the front picture from the front or the back...).
    bool lookingAt(Axis a) const;
    std::string workspaceDir() const { return keel::prefPath() + "workspaces/"; }
    void resetLayout();
    void workspacesWindow();
    static std::string fileSafe(std::string n);

    // The steps that can be undone, oldest first: a click goes back (or on) to just after it.
    void historyWindow();

    void aboutWindow();
};

} // namespace fryapp
