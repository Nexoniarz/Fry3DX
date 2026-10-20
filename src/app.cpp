// Fry3DX — Frying 3D Extreme. A 3D modeller that works the way you'd expect: pick a tool from the
// toolbox, click and drag. The view in the middle is drawn by the engine's renderer: plain
// and clear while you model (Solid), or properly lit (Rendered). The panels are Dear ImGui.
//
//   fry3dx [file.fry]
#include "icons.h"
#include "mesh.h"
#include "scene.h"
#include "script.h"
#include "sculpt.h"
#include "shapes.h"

#include <SDL3/SDL.h>
#if defined(__ANDROID__)
#include <SDL3/SDL_main.h> // (main becomes the SDL_main SDL's Java side calls)
#endif
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
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
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

namespace {

double now() { return double(SDL_GetTicks()) / 1000.0; } // (seconds; works before the UI exists)

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
enum class Orient { Local, World, Normal };        // which way the gizmo's arrows point
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
const char* kFirstScript = R"(-- A script builds or changes the scene. F5 or Ctrl+Enter runs it;
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

glm::vec3 forwardOf(float yaw, float pitch) {
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
}

// The engine's view-projection (render/renderer.cpp), in metres.
glm::mat4 viewProjection(const glm::vec3& eye, float yaw, float pitch, float fov, float aspect, glm::vec2 shift) {
    const glm::vec3 f = forwardOf(yaw, pitch);
    glm::mat4 view = glm::lookAt(eye * kUnits, eye * kUnits + f, glm::vec3(0, 0, 1));
    glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, 2.0f, 16384.0f);
    proj[1][1] *= -1.0f;
    proj[2][0] -= shift.x, proj[2][1] -= shift.y;
    return proj * view * glm::scale(glm::mat4(1.0f), glm::vec3(kUnits));
}

struct Ray {
    glm::vec3 origin, dir;
};

// Möller–Trumbore; t along the ray, or nothing.
std::optional<float> hitTriangle(const Ray& r, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 e1 = b - a, e2 = c - a, p = glm::cross(r.dir, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12f) return std::nullopt;
    const float inv = 1.0f / det;
    const glm::vec3 s = r.origin - a;
    const float u = glm::dot(s, p) * inv;
    if (u < 0 || u > 1) return std::nullopt;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(r.dir, q) * inv;
    if (v < 0 || u + v > 1) return std::nullopt;
    const float t = glm::dot(e2, q) * inv;
    return t > 1e-6f ? std::optional<float>(t) : std::nullopt;
}

std::optional<glm::vec3> hitPlane(const Ray& r, const glm::vec3& point, const glm::vec3& normal) {
    const float d = glm::dot(r.dir, normal);
    if (std::abs(d) < 1e-8f) return std::nullopt;
    const float t = glm::dot(point - r.origin, normal) / d;
    return t > 0 ? std::optional<glm::vec3>(r.origin + r.dir * t) : std::nullopt;
}

// Where along the line (from `p`, along unit `axis`) the ray passes closest.
float closestOnAxis(const Ray& r, const glm::vec3& p, const glm::vec3& axis) {
    const glm::vec3 w = p - r.origin;
    const float b = glm::dot(axis, r.dir), d = glm::dot(axis, w), e = glm::dot(r.dir, w);
    const float den = 1.0f - b * b;
    return den < 1e-6f ? 0.0f : (b * e - d) / den;
}

float distToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float vx = b.x - a.x, vy = b.y - a.y, len = vx * vx + vy * vy;
    const float t = len > 0 ? std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / len, 0.0f, 1.0f) : 0.0f;
    const float dx = a.x + vx * t - p.x, dy = a.y + vy * t - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool insidePolygon(ImVec2 p, const std::vector<ImVec2>& poly) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > p.y) != (poly[j].y > p.y) && p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) in = !in;
    return in;
}

float snapTo(float v, float step) { return step > 0 ? std::round(v / step) * step : v; }

using Axes = std::array<glm::vec3, 3>;
glm::vec3 inFrame(const Axes& a, const glm::vec3& p) { return {glm::dot(p, a[0]), glm::dot(p, a[1]), glm::dot(p, a[2])}; }
glm::vec3 fromFrame(const Axes& a, const glm::vec3& c) { return a[0] * c.x + a[1] * c.y + a[2] * c.z; }

const ImU32 kAxisCol[3] = {IM_COL32(235, 70, 70, 255), IM_COL32(110, 210, 80, 255), IM_COL32(70, 130, 245, 255)};
const ImU32 kHot = IM_COL32(255, 225, 90, 255);
constexpr ImU32 kAccent = IM_COL32(255, 120, 40, 255);
// The UI's colours: Niveus's roles, in the appearance that's showing (keel/ui).
ImVec4 roleColor(keel::ui::Role r) { return ImGui::ColorConvertU32ToFloat4(keel::ui::color(r)); }
ImVec4 accentText() { return roleColor(keel::ui::Role::AccentText); }
// The one button that does it, filled with the accent (until popAccent).
void pushAccent() {
    using keel::ui::Role;
    ImGui::PushStyleColor(ImGuiCol_Button, roleColor(Role::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, roleColor(Role::AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, roleColor(Role::AccentActive));
    ImGui::PushStyleColor(ImGuiCol_Text, roleColor(Role::OnAccent));
}
void popAccent() { ImGui::PopStyleColor(4); }

// An object as it'll be printed: where it stands, with its finish, checked (kept until it changes).
struct PrintInfo {
    uint64_t key = ~0ull;
    Mesh world;
    PrintCheck check;
    glm::vec3 lo{0.0f}, hi{0.0f};
    keel::ModelId overhang{};
    bool hasOverhang = false;
};

keel::Cvar fry_printer("fry_printer", "0", keel::CVAR_ARCHIVE, "Fry3DX: the 3D printer (a number in the Print tab's list)");
keel::Cvar fry_bed_x("fry_bed_x", "220", keel::CVAR_ARCHIVE, "Fry3DX: your own printer's build plate across (mm)");
keel::Cvar fry_bed_y("fry_bed_y", "220", keel::CVAR_ARCHIVE, "Fry3DX: your own printer's build plate deep (mm)");
keel::Cvar fry_bed_z("fry_bed_z", "250", keel::CVAR_ARCHIVE, "Fry3DX: how high your own printer prints (mm)");
#if defined(__ANDROID__)
keel::Cvar fry_touch("fry_touch", "1", keel::CVAR_ARCHIVE, "Fry3DX: the phone layout and touch (takes effect on the next start)");
#else
keel::Cvar fry_touch("fry_touch", "0", keel::CVAR_ARCHIVE, "Fry3DX: the phone layout and touch (takes effect on the next start)");
#endif
keel::Cvar fry_uiscale("fry_uiscale", "0", 0, "Fry3DX: draw the UI this many times bigger (0: by the screen)");
keel::Cvar fry_uisize("fry_uisize", "1", keel::CVAR_ARCHIVE, "Fry3DX: the phone layout's size, times");
keel::Cvar fry_theme("fry_theme", "0", keel::CVAR_ARCHIVE, "Fry3DX: how it looks: 0 like the desktop, 1 dark, 2 light");
keel::Cvar fry_pluginsoff("fry_pluginsoff", "", keel::CVAR_ARCHIVE, "Fry3DX: plug-ins turned off (their file names, with | between)");
keel::Cvar fry_autosave("fry_autosave", "2", keel::CVAR_ARCHIVE, "Fry3DX: autosave every this many minutes while there are changes (0: off)");

class App : public keel::Game {
public:
    explicit App(std::string path) : startPath_(std::move(path)) {}
    const char* name() const override { return "Fry3DX"; } // (its settings folder, and bin/Fry3DX/)
    const char* title() const override { return "Fry3DX"; }
    bool wantsCursor() const override { return true; }
    bool dockingUi() const override { return true; }

    // "Spiral_stairs" is "Spiral stairs" (typed in the console).
    static bool sameCommand(const std::string& name, std::string typed) {
        std::replace(typed.begin(), typed.end(), '_', ' ');
        return materialId(name) == materialId(typed);
    }
    void init() override {
        static App* self = this;
        // Console commands: for scripts and tests (the ` key opens the console).
        static keel::Cmd open("fry_open", [](const keel::Args& a) { if (a.size() > 1) self->openFile(a[1]); }, "Fry3DX: open a .fry file");
        static keel::Cmd save("fry_save", [](const keel::Args& a) { if (a.size() > 1) self->saveFile(a[1]); }, "Fry3DX: save as");
        static keel::Cmd exp("fry_export", [](const keel::Args& a) { if (a.size() > 1) self->exportFile(a[1]); }, "Fry3DX: export glTF");
        static keel::Cmd run("fry_run", [](const keel::Args& a) {
            if (a.size() < 2) return;
            std::ifstream in(a[1], std::ios::binary);
            if (!in) return keel::log::warn("Couldn't open {}", a[1]);
            self->addScript(scriptName(a[1]), std::string(std::istreambuf_iterator<char>(in), {}));
            self->scriptPath_ = a[1];
            self->runScriptNow();
        }, "Fry3DX: run a Lua script file (fry_run path/to/script.lua)");
        static keel::Cmd add("fry_add", [](const keel::Args& a) { if (a.size() > 1) self->addShape(a[1]); }, "Fry3DX: add a shape (cube, rounded_cube, table...)");
        static keel::Cmd view("fry_view", [](const keel::Args& a) {
            static const char* names[] = {"persp", "front", "back", "left", "right", "top", "bottom"};
            for (int i = 0; i < 7; ++i)
                if (a.size() > 1 && a[1] == names[i]) self->setView(View(i));
        }, "Fry3DX: fry_view persp|front|back|left|right|top|bottom");
        static keel::Cmd orbit("fry_orbit", [](const keel::Args& a) {
            if (a.size() > 2) self->view_ = View::Perspective, self->fov_ = 50, self->yaw_ = std::stof(a[1]), self->pitch_ = std::stof(a[2]);
            if (a.size() > 3) self->dist_ = std::stof(a[3]);
        }, "Fry3DX: fry_orbit yaw pitch [distance]");
        // A sculpt stroke across the view (0..1 of it), for pictures of it: brush 1-6, then from and to.
        static keel::Cmd sculptStroke("fry_sculpt", [](const keel::Args& a) {
            if (!self->viewReady()) return self->say("fry_sculpt: the view isn't up yet (+wait 1 first)");
            if (a.size() < 6) return;
            const int brush = std::clamp(std::stoi(a[1]) - 1, 0, int(std::size(kSculptTools)) - 1);
            self->sculptBrush_ = kSculptTools[brush].brush;
            const bool other = a.size() > 6 && a[6] == "invert";
            const auto at = [&](float t) {
                const ImVec2 lo = self->viewMin_, hi = self->viewMax_;
                return ImVec2(lo.x + (hi.x - lo.x) * glm::mix(std::stof(a[2]), std::stof(a[4]), t), lo.y + (hi.y - lo.y) * glm::mix(std::stof(a[3]), std::stof(a[5]), t));
            };
            self->sculptAt(at(0), true, other, false);
            for (int i = 1; i <= 60; ++i) self->sculptAt(at(float(i) / 60.0f), false, other, false);
            self->endSculpt();
        }, "Fry3DX: fry_sculpt brush x0 y0 x1 y1 [invert] (0..1 of the view)");
        static keel::Cmd sel("fry_select", [](const keel::Args& a) {
            self->selected_.clear();
            for (const Object& o : self->scene_.objects)
                if (a.size() > 1 && o.name == a[1]) self->selected_.insert(o.id), self->active_ = o.id;
        }, "Fry3DX: select an object by name");
        static keel::Cmd place("fry_place", [](const keel::Args& a) {
            if (a.size() < 4) return;
            for (Object& o : self->scene_.objects)
                if (self->selected_.count(o.id)) o.position = {std::stof(a[1]), std::stof(a[2]), std::stof(a[3])}, self->touched(o.id);
        }, "Fry3DX: fry_place x y z (metres) for the selection");
        static keel::Cmd turn("fry_turn", [](const keel::Args& a) {
            if (a.size() < 4) return;
            for (Object& o : self->scene_.objects)
                if (self->selected_.count(o.id)) o.rotation = fromEuler({std::stof(a[1]), std::stof(a[2]), std::stof(a[3])}), self->touched(o.id);
        }, "Fry3DX: fry_turn x y z (degrees) for the selection");
        static keel::Cmd edit("fry_edit", [](const keel::Args& a) {
            self->enterEdit();
            if (a.size() > 1) self->elem_ = a[1] == "vertex" ? Elem::Vertex : a[1] == "edge" ? Elem::Edge : Elem::Face;
            if (a.size() > 2 && a[2] == "all") self->selectAllElements();
            if (a.size() > 2 && a[2] != "all") {
                self->selFaces_.clear();
                for (size_t i = 2; i < a.size(); ++i) self->selFaces_.insert(std::stoi(a[i]));
            }
        }, "Fry3DX: fry_edit [vertex|edge|face] [all|face numbers...]");
        static keel::Cmd tool("fry_tool", [](const keel::Args& a) {
            for (const ToolInfo& t : kTools)
                if (a.size() > 1 && SDL_strcasecmp(a[1].c_str(), t.name) == 0) self->setTool(t.tool);
        }, "Fry3DX: pick a tool by name");
        static keel::Cmd shading("fry_shading", [](const keel::Args& a) {
            if (a.size() > 1) self->shading_ = a[1] == "rendered" ? Shading::Rendered : a[1] == "wire" ? Shading::Wire : Shading::Solid;
        }, "Fry3DX: fry_shading solid|rendered|wire");
        static keel::Cmd finish("fry_finish", [](const keel::Args& a) {
            for (Object& o : self->scene_.objects)
                if (self->selected_.count(o.id)) {
                    if (a.size() > 1) o.round = std::stof(a[1]);
                    if (a.size() > 2) o.roundSteps = std::stoi(a[2]);
                    if (a.size() > 3) o.smoothLevels = std::stoi(a[3]);
                    if (a.size() > 4) o.mirror = std::stoi(a[4]);
                    self->touched(o.id);
                }
        }, "Fry3DX: fry_finish round steps smooth mirror (for the selection)");
        static keel::Cmd bevel("fry_bevel", [](const keel::Args& a) {
            if (a.size() > 1) self->bevelWidth_ = std::stof(a[1]);
            if (a.size() > 2) self->bevelSteps_ = std::stoi(a[2]);
            self->bevelNow();
        }, "Fry3DX: round the selected edges: fry_bevel width steps");
        static keel::Cmd cut("fry_cut", [](const keel::Args& a) {
            if (Object* o = self->editing(); o && a.size() > 2) self->cutAt(*o, {std::stoi(a[1]), std::stoi(a[2])});
        }, "Fry3DX: loop cut through the edge between two points");
        static keel::Cmd pic("fry_picture", [](const keel::Args& a) { if (a.size() > 1) self->addPicture(a[1]); }, "Fry3DX: add a reference picture");
        static keel::Cmd frame("fry_frame", [](const keel::Args&) { self->frameAll(); }, "Fry3DX: frame everything");
        static keel::Cmd tab("fry_tab", [](const keel::Args& a) {
            if (a.size() > 1)
                self->setTab(a[1] == "paint" || a[1] == "sculpt" ? Tab::Paint : a[1] == "layout" ? Tab::Layout : a[1] == "render" ? Tab::Render : a[1] == "print" ? Tab::Print : Tab::Model);
            if (a.size() > 1) self->sculpting_ = a[1] == "sculpt";
        }, "Fry3DX: fry_tab model|paint|sculpt|layout|render|print");
        // (points as parts of the view: 0..1 across, 0..1 down)
        auto viewPoint = [](const std::string& x, const std::string& y) {
            return ImVec2(self->viewMin_.x + std::stof(x) * (self->viewMax_.x - self->viewMin_.x), self->viewMin_.y + std::stof(y) * (self->viewMax_.y - self->viewMin_.y));
        };
        static auto vp = viewPoint;
        static keel::Cmd knife("fry_knife", [](const keel::Args& a) {
            if (!self->viewReady()) return self->say("fry_knife: the view isn't up yet (+wait 1 first)");
            if (a.size() < 5) return;
            if (a.size() > 5) self->knifeThrough_ = a[5] != "0";
            if (a.size() > 6) self->knifeAfter_ = std::stoi(a[6]);
            if (a.size() > 7) self->knifeFill_ = a[7] != "0";
            std::vector<ImVec2> pts;
            for (size_t i = 1; i + 1 < a.size() && i < 5; i += 2) pts.push_back(vp(a[i], a[i + 1]));
            if (Object* o = self->editing()) self->knifeApply(o->id, pts);
        }, "Fry3DX: fry_knife x0 y0 x1 y1 [through after fill] (view 0..1)");
        static keel::Cmd combineCmd("fry_combine", [](const keel::Args& a) {
            const std::string w = a.size() > 1 ? a[1] : "add";
            self->combineSelected(w == "subtract" ? Combine::Subtract : w == "overlap" ? Combine::Overlap : Combine::Add);
        }, "Fry3DX: fry_combine add|subtract|overlap (the picked, the last kept)");
        static keel::Cmd selectCmd("fry_selectall", [](const keel::Args&) {
            self->selected_.clear();
            for (const Object& o : self->scene_.objects) self->selected_.insert(o.id), self->active_ = o.id;
        }, "Fry3DX: pick every object (the first added last)");
        static keel::Cmd pickCmd("fry_pick", [](const keel::Args& a) { // fry_pick name...: those picked, the first the active one
            self->selected_.clear();
            for (size_t i = a.size(); i-- > 1;)
                for (const Object& o : self->scene_.objects)
                    if (o.name == a[i]) self->selected_.insert(o.id), self->active_ = o.id;
        }, "Fry3DX: fry_pick name... (the first kept when combining)");
        static keel::Cmd copiesCmd("fry_copies", [](const keel::Args& a) { // fry_copies n row dx dy dz | n round angle [joined]
            if (a.size() < 3) return;
            auto& c = self->copies_;
            c.count = std::stoi(a[1]), c.round = a[2] == "round";
            if (c.round && a.size() > 3) c.angle = std::stof(a[3]), c.centre = glm::vec3(0.0f);
            if (!c.round && a.size() > 5) c.step = {std::stof(a[3]), std::stof(a[4]), std::stof(a[5])};
            c.joined = a.back() == "joined";
            self->makeCopies();
        }, "Fry3DX: fry_copies n row dx dy dz [joined] | n round angle [joined]");
        static keel::Cmd thickCmd("fry_thicken", [](const keel::Args& a) {
            if (a.size() > 1) self->thickness_ = std::abs(std::stof(a[1])), self->thicknessOut_ = std::stof(a[1]) < 0;
            self->thickenActive();
        }, "Fry3DX: fry_thicken metres (negative: outwards)");
        static keel::Cmd groupCmd("fry_group", [](const keel::Args& a) {
            self->groupSelected();
            if (a.size() > 1 && !self->scene_.folders.empty()) self->scene_.folders.back().name = a[1], self->renamingFolder_ = 0;
        }, "Fry3DX: fry_group [name]: the picked into a new folder");
        static keel::Cmd panelsCmd("fry_panels", [](const keel::Args&) { self->panelsOpen_ = !self->panelsOpen_; }, "Fry3DX: phone layout: show / hide the panels");
        static keel::Cmd goCmd("fry_goto", [](const keel::Args&) { self->goTo(self->selected_); }, "Fry3DX: the camera to the picked");
        static keel::Cmd drawCmd("fry_draw", [](const keel::Args& a) { // fry_draw mode thick x y x y x y ... (view 0..1)
            if (!self->viewReady()) return self->say("fry_draw: the view isn't up yet (+wait 1 first)");
            if (a.size() < 9) return;
            self->setTool(Tool::Draw);
            self->drawOnMode_ = std::stoi(a[1]), self->drawThickness_ = std::stof(a[2]);
            self->drawPts_.clear();
            const bool snap = self->snap_;
            self->snap_ = false;
            for (size_t i = 3; i + 1 < a.size(); i += 2) {
                ImGuiIO io = ImGui::GetIO();
                io.MousePos = ImVec2(self->viewMin_.x + std::stof(a[i]) * (self->viewMax_.x - self->viewMin_.x), self->viewMin_.y + std::stof(a[i + 1]) * (self->viewMax_.y - self->viewMin_.y));
                if (auto p = self->drawPoint(io)) self->drawPts_.push_back(*p);
            }
            self->snap_ = snap;
            self->finishShape();
        }, "Fry3DX: fry_draw mode(0 new,1 add,2 cut) thickness x y x y x y...");
        static keel::Cmd extrudeCmd("fry_extrude", [](const keel::Args& a) { if (a.size() > 1) self->extrudeNow(std::stof(a[1])); }, "Fry3DX: extrude the picked faces this far (m)");
        static keel::Cmd holeCmd("fry_hole", [](const keel::Args& a) {
            Object* o = self->editing() ? self->editing() : self->find(self->active_);
            if (!o || a.size() < 3) return;
            const int face = std::stoi(a[1]);
            if (a.size() > 3) {
                const float d = std::stof(a[3]);
                self->holeMode_ = d < 0 ? 2 : d == 0 ? 0 : 1;
                if (d > 0) self->holeDepth_ = d;
            }
            if (a.size() > 4) {
                static const char* names[] = {"round", "rect", "triangle", "hex"};
                for (int i = 0; i < 4; ++i)
                    if (a[4] == names[i]) self->holeShape_ = i;
            }
            const float across = std::stof(a[2]);
            if (a.size() > 5) self->holeTall_ = std::stof(a[5]), self->holeSize_ = across;
            if (face >= 0 && face < int(o->mesh.faces.size())) self->applyHole(o->id, face, faceCenter(o->mesh, face), across);
        }, "Fry3DX: fry_hole face across [depth: 0 through, -1 outline only] [round|rect|triangle|hex] [tall] (metres, the object's own)");
        static keel::Cmd color("fry_color", [](const keel::Args& a) {
            if (a.size() > 3) self->paintColor_ = {std::stof(a[1]), std::stof(a[2]), std::stof(a[3]), 1.0f};
        }, "Fry3DX: fry_color r g b (painting, 0..1)");
        static keel::Cmd stroke("fry_stroke", [](const keel::Args& a) {
            if (!self->viewReady()) return self->say("fry_stroke: the view isn't up yet (+wait 1 first)");
            if (a.size() < 5) return;
            const ImVec2 p0 = vp(a[1], a[2]), p1 = vp(a[3], a[4]);
            for (int i = 0; i <= 40; ++i) self->paintAt(ImVec2(p0.x + (p1.x - p0.x) * float(i) / 40, p0.y + (p1.y - p0.y) * float(i) / 40), i == 0);
            self->endStroke();
        }, "Fry3DX: fry_stroke x0 y0 x1 y1: a brush stroke across the view (0..1)");
        static keel::Cmd paste("fry_pastefile", [](const keel::Args& a) { if (a.size() > 1) self->floatFromFile(a[1]); }, "Fry3DX: place a picture file onto the selected object");
        static keel::Cmd corner("fry_corner", [](const keel::Args& a) {
            if (PictureLayer* l = self->editLayer(); l && a.size() > 3) l->corners[size_t(std::clamp(std::stoi(a[1]), 0, 3))] = {std::stof(a[2]), std::stof(a[3])};
        }, "Fry3DX: fry_corner i x y: move a placed picture's corner (picture pixels)");
        static keel::Cmd placePic("fry_lock", [](const keel::Args&) { self->lockLayer(); }, "Fry3DX: lock the picture being moved (Enter)");
        static keel::Cmd flat("fry_pasteflat", [](const keel::Args& a) { if (a.size() > 1) self->pasteOnSurface_ = a[1] == "0"; }, "Fry3DX: fry_pasteflat 1: new pictures go on the flat picture");
        static keel::Cmd unlock("fry_unlock", [](const keel::Args& a) {
            Object* o = self->find(self->active_);
            if (!o || a.size() < 3 || !self->owner_.count(o->id) || !o->picture) return;
            const int x = std::stoi(a[1]), y = std::stoi(a[2]);
            if (x < 0 || y < 0 || x >= o->picture->w || y >= o->picture->h) return;
            if (const int i = self->owner_[o->id][size_t(y) * size_t(o->picture->w) + size_t(x)]; i > 0) self->unlockLayer(o->id, i - 1);
        }, "Fry3DX: fry_unlock x y: click the Picture panel there (picture pixels)");
        static keel::Cmd moveSticker("fry_sticker", [](const keel::Args& a) {
            if (!self->viewReady()) return self->say("fry_sticker: the view isn't up yet (+wait 1 first)");
            Object* o = self->find(self->ledit_.object);
            PictureLayer* l = self->editLayer();
            if (!o || !l || a.size() < 3) return;
            const Ray ray = self->mouseRay(vp(a[1], a[2]));
            const Hit h = self->pickObject(ray, o->id, true);
            if (!h.object) return self->say("fry_sticker: not on the object there");
            l->face = h.face, l->centre = glm::vec3(glm::inverse(objectMatrix(*o)) * glm::vec4(ray.origin + ray.dir * h.t, 1.0f));
            if (a.size() > 4) l->width = std::stof(a[3]), l->height = std::stof(a[4]);
            settleSticker(o->mesh, *l);
        }, "Fry3DX: fry_sticker x y [w h]: move the sticker being moved to that point of the view");
        static keel::Cmd capture("fry_capture", [](const keel::Args& a) { if (a.size() > 1) self->savePicture(a[1]); }, "Fry3DX: save a picture of the view");
        static keel::Cmd layout("fry_layout", [](const keel::Args& a) {
            Object* o = self->find(self->active_);
            if (!o || a.size() < 2) return;
            if (a[1] == "start") self->readyToPaint(o->id);
            if (a[1] == "all") self->layoutSelectAll();
            if (a[1] == "frame" && a.size() > 2) self->lay_.object = o->id, self->lay_.corners = false, self->lay_.frames = {std::stoi(a[2])};
            if (a[1] == "checker" && a.size() > 2) self->layChecker_ = a[2] == "1";
            if (a[1] == "keep" && a.size() > 2) self->layKeepPaint_ = a[2] == "1";
            if (a[1] == "again") self->layoutAgain(*o);
            if (a[1] == "turn" && a.size() > 2) {
                const float r = glm::radians(std::stof(a[2]));
                self->layoutApply([r](glm::vec2 p, glm::vec2 m) { return m + glm::vec2(std::cos(r) * (p - m).x - std::sin(r) * (p - m).y, std::sin(r) * (p - m).x + std::cos(r) * (p - m).y); });
            }
            if (a[1] == "move" && a.size() > 3) {
                const glm::vec2 d(std::stof(a[2]), std::stof(a[3]));
                self->layoutApply([d](glm::vec2 p, glm::vec2) { return p + d; });
            }
            if (a[1] == "scale" && a.size() > 2) {
                const float k = std::stof(a[2]);
                self->layoutApply([k](glm::vec2 p, glm::vec2 m) { return m + (p - m) * k; });
            }
        }, "Fry3DX: fry_layout start|all|frame N|turn deg|move x y|scale k|checker 0/1|keep 0/1|again");
        static keel::Cmd print("fry_print", [](const keel::Args& a) {
            if (a.size() < 2) return;
            if (a[1] == "bed") self->putOnBed();
            if (a[1] == "middle") self->middleOfBed();
            if (a[1] == "printer" && a.size() > 2) keel::execute("fry_printer " + a[2]);
            if (a[1] == "size" && a.size() > 3) self->pushUndo(), self->printResize(std::stoi(a[2]), std::stof(a[3]));
            if ((a[1] == "measure" || a[1] == "flat") && !self->viewReady()) return self->say("fry_print: the view isn't up yet (+wait 1 first)");
            if (a[1] == "measure" && a.size() > 5) {
                self->printTool_ = PrintTool::Measure, self->measure_.clear();
                for (int i = 0; i < 2; ++i)
                    if (auto p = self->measurePoint(vp(a[size_t(2 + i * 2)], a[size_t(3 + i * 2)]))) self->measure_.push_back(*p);
            }
            if (a[1] == "flat" && a.size() > 3) { // (as clicking with Lay flat)
                self->printTool_ = PrintTool::LayFlat;
                ImGuiIO& io = ImGui::GetIO();
                const ImVec2 keep = io.MousePos;
                io.MousePos = vp(a[2], a[3]);
                const Hit h = self->pickObject(self->mouseRay(io.MousePos));
                io.MousePos = keep;
                if (Object* o = h.object ? self->find(h.object) : nullptr) {
                    const glm::vec3 n = faceNormal(self->printInfo(*o).world, h.face);
                    self->pushUndo();
                    o->rotation = glm::normalize(glm::rotation(n, glm::vec3(0, 0, -1)) * o->rotation), self->touched(o->id);
                    o->position.z -= self->printInfo(*o).lo.z, self->touched(o->id);
                }
                self->printTool_ = PrintTool::None;
            }
        }, "Fry3DX: fry_print bed|middle|printer N|size axis mm|measure x0 y0 x1 y1|flat x y");
        static keel::Cmd autosaveNow("fry_autosavenow", [](const keel::Args&) { self->autosave(true); }, "Fry3DX: autosave now");
        static keel::Cmd closeAsk("fry_close", [](const keel::Args&) { if (self->allowQuit()) keel::execute("quit"); }, "Fry3DX: as the window's close button");
        static keel::Cmd recover("fry_recover", [](const keel::Args&) { self->findAutosaves(), self->showRecover_ = !self->recover_.empty(); }, "Fry3DX: show the autosaves to recover");
        static keel::Cmd scriptPanel("fry_scriptpanel", [](const keel::Args& a) { self->showScript_ = a.size() < 2 || a[1] != "0"; }, "Fry3DX: show (1) or hide (0) the Script panel");
        static keel::Cmd shapeOp("fry_shapeop", [](const keel::Args& a) {
            if (a.size() < 2) return;
            if (a[1] == "smooth") self->smoothPointsNow();
            else if (a[1] == "bridge") self->bridgeNow();
            else if (a[1] == "push" && a.size() > 2) {
                self->startPush();
                if (Object* o = self->find(self->pushObject_); o && self->showPush_) {
                    self->pushBy_ = std::stof(a[2]);
                    pushPoints(o->mesh, self->pushPts_, self->pushBy_);
                    self->changedShape(*o, true);
                    self->showPush_ = false;
                }
            } else if (a[1] == "move" && a.size() > 4) { // the selection (and with soft selection, what's near) moved, world metres
                Object* o = self->editing();
                if (!o) return;
                self->pushUndo();
                self->drag_.soft.clear();
                if (self->softSel_) self->findSoft();
                const glm::mat4 m = objectMatrix(*o);
                const glm::mat4 local = glm::inverse(m) * glm::translate(glm::mat4(1.0f), glm::vec3(std::stof(a[2]), std::stof(a[3]), std::stof(a[4]))) * m;
                for (int v : self->selectedPoints()) o->mesh.verts[size_t(v)] = glm::vec3(local * glm::vec4(o->mesh.verts[size_t(v)], 1.0f));
                for (const auto& [v, w] : self->drag_.soft) o->mesh.verts[size_t(v)] = glm::mix(o->mesh.verts[size_t(v)], glm::vec3(local * glm::vec4(o->mesh.verts[size_t(v)], 1.0f)), w);
                self->drag_.soft.clear();
                self->changedShape(*o, true);
            } else if (a[1] == "soft") self->softSel_ = a.size() < 3 || a[2] != "0", self->softReach_ = a.size() > 3 ? std::stof(a[3]) : self->softReach_;
        }, "Fry3DX: shape editing: fry_shapeop smooth | bridge | push metres | soft 1 [reach] | move x y z");
        static keel::Cmd about("fry_about", [](const keel::Args&) { self->showAbout_ = true; }, "Fry3DX: show the About window");
        static keel::Cmd plugins("fry_plugins", [](const keel::Args& a) {
            if (a.size() > 1 && a[1] == "reload") self->loadPlugins();
            else self->showPlugins_ = true;
        }, "Fry3DX: the Plug-ins window (fry_plugins reload: load them again)");
        static keel::Cmd plugin("fry_plugin", [](const keel::Args& a) { // fry_plugin "Command name" [settings...]
            if (a.size() < 2) return;
            for (size_t i = 0; i < self->plugins_.size(); ++i)
                if (self->plugins_[i].p)
                    for (size_t c = 0; c < self->plugins_[i].p->commands.size(); ++c)
                        if (sameCommand(self->plugins_[i].p->commands[c].name, a[1])) {
                            std::vector<double> v;
                            for (size_t k = 2; k < a.size(); ++k) v.push_back(std::stod(a[k]));
                            return self->runPluginCommand(i, c, v);
                        }
            keel::log::warn("No plug-in command called {}", a[1]);
        }, "Fry3DX: run a plug-in's command: fry_plugin Spiral_stairs 12 0.18 (_ for spaces, any case)");
        keel::execute("r_autoexposure 0; r_ssr 0; r_volumetric 0; r_shafts 0; r_dynres 0; r_renderscale 1; fps_max 144");
#if defined(__ANDROID__) // a phone: none of the heavy parts (and its GPU's driver is kept to well-trodden ground)
        keel::execute("r_gpuculling 0; r_occlusion 0; r_volumetricfog 0; r_mirrors 0; r_taa 0; r_shadowres 2048; r_lightshadows 0; fps_max 60");
#endif
        newScene();
        loadOwnMaterials();
        loadPlugins();
        if (!startPath_.empty()) openFile(startPath_);
        findAutosaves();
    }

    void frame(double) override { autosave(); }
    bool allowQuit() override {
        if (!dirty_) return dropAutosave(), true;
        askSave_ = 1;
        return false;
    }
    void shutdown() override {
        if (autosaveJob_.valid()) autosaveJob_.wait();
    }
    void onEscape() override {
        if (askSave_) askSave_ = 0;
        else if (showRecover_) showRecover_ = false;
        else if (editLayer()) cancelFloating();
        else if (!knifePts_.empty()) knifePts_.clear();
        else if (!drawPts_.empty()) drawPts_.clear();
        else if (drag_.active) cancelDrag();
        else if (mode_ == Mode::Edit) leaveEdit();
        else selected_.clear();
    }

    void render(keel::Renderer& r) override {
        r_ = &r;
        captureStep();
        setupUi();
        loadLogo();
        applyShading();
        takeDialogResult();
        ImGuiIO& io = ImGui::GetIO();
        if (touch()) touchFrame(io);
        const ImGuiID dock = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
        if (touch() && (io.DisplaySize.y > io.DisplaySize.x) != portrait_) portrait_ = !portrait_, layoutDone_ = false; // (the phone turned: its layout)
        if (!layoutDone_) defaultLayout(dock);
        if (!touch()) keepMinSizes(ImGui::DockBuilderGetNode(dock));
        if (ImGuiDockNode* c = ImGui::DockBuilderGetCentralNode(dock)) viewMin_ = c->Pos, viewMax_ = ImVec2(c->Pos.x + c->Size.x, c->Pos.y + c->Size.y);
        updateCamera(io);
        menuBar();
        toolbox();
        toolOptions();
        if (!touch() || panelsOpen_) {
            layersPanel();
            propertiesPanel();
            if (tab_ == Tab::Paint || tab_ == Tab::Layout) picturePanel();
        }
        if (!touch()) statusBar();
        if (!capturing() && (!touch() || viewsOpen_)) viewButtons();
        if (!capturing() && touch()) touchBar();
        if (showScript_) scriptPanel();
        if (showHelp_) helpWindow();
        if (showAbout_) aboutWindow();
        if (showPlugins_) pluginsWindow();
        commandForm();
        copiesWindow();
        thickenWindow();
        pushWindow();
        saveQuestion();
        recoverWindow();
        if (!askSave_ && !showRecover_) shortcuts(io);
        viewport(io);
        draw3D(r);
        overlays();
    }

private:
    // ── The document ──
    Scene scene_;
    std::string startPath_, path_;
    bool dirty_ = false;
    struct Snapshot {
        Scene scene;
        std::set<uint32_t> selected;
        uint32_t active;
        Mode mode;
        std::set<int> verts, faces;
        std::set<std::pair<int, int>> edges;
    };
    std::vector<Snapshot> undo_, redo_;
    std::vector<Snapshot> redoKept_; // (what redo had before the last undo point: back if that step is dropped)

    Mode mode_ = Mode::Object;
    Tool tool_ = Tool::Move;
    Elem elem_ = Elem::Face;
    std::set<uint32_t> selected_;
    uint32_t active_ = 0;    // the object last picked (edited in shape editing)
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
    ImVec2 viewMin_{0, 0}, viewMax_{1, 1};
    glm::mat4 vp_{1.0f};
    glm::vec3 eye_{0.0f};
    bool orbiting_ = false, panning_ = false;
    bool xray_ = false, showGrid_ = true, renderGrid_ = false;
    std::string appliedLook_;
    keel::ModelId ground_{};
    bool haveGround_ = false;
    Shading shading_ = Shading::Solid;
    int appliedShading_ = -1;

    // ── Tools ──
    bool snap_ = false;
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
    int holeMode_ = 0;                  // 0 all the way through, 1 a pocket, 2 the outline only (a new face)
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
        bool round = false, joined = false;
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
    bool measureSnap_ = true, showOverhangs_ = true, sizing_ = false;
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
        uint64_t version = 0;
        bool mirrorOnly = false;
        Mesh mesh;
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

    Object* find(uint32_t id) {
        for (Object& o : scene_.objects)
            if (o.id == id) return &o;
        return nullptr;
    }
    Reference* findRef(uint32_t id) {
        for (Reference& r : scene_.references)
            if (r.id == id) return &r;
        return nullptr;
    }
    Object* editing() { return mode_ == Mode::Edit ? find(active_) : nullptr; }
    void touched(uint32_t id) { versions_[id] = ++versionClock_, dirty_ = true, ++edits_; }
    void touchAll() {
        for (const Object& o : scene_.objects) versions_[o.id] = ++versionClock_;
    }
    // Its shape has been changed by hand: the library settings no longer apply.
    void handMade(Object& o) { o.recipe.clear(), o.settings.clear(); }
    void say(std::string text) {
        message_ = std::move(text), messageAt_ = now();
        keel::log::info("{}", message_);
    }

    void pushUndo() {
        undo_.push_back({scene_, selected_, active_, mode_, selVerts_, selFaces_, selEdges_});
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        redoKept_ = std::move(redo_), redo_.clear();
        dirty_ = true, ++edits_;
    }
    // The undo point just made, dropped (what it was for didn't happen): redo as it was before.
    void dropUndo() {
        if (!undo_.empty()) undo_.pop_back();
        redo_ = std::move(redoKept_), redoKept_.clear();
    }
    // Back to the undo point just made, and it dropped.
    void takeBack() {
        if (undo_.empty()) return;
        restore(undo_.back());
        dropUndo();
    }
    void restore(Snapshot& s) {
        // (Not the scripts: they're edited like text, not undone with the model.)
        std::vector<fry::SceneScript> scripts = std::move(scene_.scripts);
        scene_ = s.scene, selected_ = s.selected, active_ = s.active, mode_ = s.mode;
        scene_.scripts = std::move(scripts);
        selVerts_ = s.verts, selFaces_ = s.faces, selEdges_ = s.edges;
        if (mode_ == Mode::Edit && !find(active_)) mode_ = Mode::Object;
        touchAll();
        drawPts_.clear();
    }
    void undo() {
        if (undo_.empty()) return say("Nothing to undo");
        redo_.push_back({scene_, selected_, active_, mode_, selVerts_, selFaces_, selEdges_});
        restore(undo_.back());
        undo_.pop_back();
        redoKept_.clear();
    }
    void redo() {
        if (redo_.empty()) return say("Nothing to redo");
        undo_.push_back({scene_, selected_, active_, mode_, selVerts_, selFaces_, selEdges_});
        restore(redo_.back());
        redo_.pop_back();
        redoKept_.clear();
    }

    void newScene() {
        scene_ = {};
        script_ = 0, scriptPath_.clear();
        path_.clear();
        undo_.clear(), redo_.clear(), selected_.clear();
        mode_ = Mode::Object, active_ = 0;
        addShape("cube", false);
        dirty_ = false;
        target_ = {0, 0, 0.5f}, dist_ = 4.5f, view_ = View::Perspective, fov_ = 50, yaw_ = -135, pitch_ = 25;
    }
    Object& addObject(const std::string& base, Mesh mesh) {
        Object o;
        o.id = scene_.nextId++;
        o.name = uniqueName(base);
        o.mesh = std::move(mesh);
        scene_.objects.push_back(std::move(o));
        touched(scene_.objects.back().id);
        return scene_.objects.back();
    }
    std::string uniqueName(const std::string& base) {
        auto taken = [&](const std::string& n) { return std::any_of(scene_.objects.begin(), scene_.objects.end(), [&](const Object& o) { return o.name == n; }); };
        if (!taken(base)) return base;
        for (int i = 2;; ++i)
            if (!taken(std::format("{} {}", base, i))) return std::format("{} {}", base, i);
    }

    // A shape from the library, standing on the ground under the middle of the view.
    void addShape(const std::string& id, bool undoable = true) {
        const ShapeRecipe* r = findRecipe(id);
        if (!r) return say("No shape called " + id);
        if (undoable) pushUndo();
        if (mode_ == Mode::Edit) leaveEdit();
        Object& o = addObject(r->name, makeFromRecipe(*r, defaultSettings(*r)));
        o.recipe = r->id, o.settings = defaultSettings(*r), o.smooth = r->smooth;
        o.position = glm::vec3(std::round(target_.x * 10) / 10, std::round(target_.y * 10) / 10, 0.0f);
        if (view_ == View::Front || view_ == View::Back) o.position.x = 0;
        if (view_ == View::Left || view_ == View::Right) o.position.y = 0;
        selected_ = {o.id}, active_ = o.id;
        if (undoable) say(std::format("{} added: its sizes are in Properties", r->name));
    }

    void deleteSelected() {
        if (mode_ == Mode::Edit) return deleteElements();
        if (activeRef_ && selected_.empty()) {
            pushUndo();
            std::erase_if(scene_.references, [&](const Reference& r) { return r.id == activeRef_; });
            activeRef_ = 0;
            return;
        }
        if (selected_.empty()) return;
        pushUndo();
        std::erase_if(scene_.objects, [&](const Object& o) { return selected_.count(o.id) && !o.locked; });
        selected_.clear();
    }

    void duplicateSelected() {
        if (mode_ != Mode::Object || selected_.empty()) return;
        pushUndo();
        std::set<uint32_t> made;
        const std::vector<Object> copy = scene_.objects;
        for (const Object& o : copy)
            if (selected_.count(o.id)) {
                Object& n = addObject(o.name, o.mesh);
                const uint32_t id = n.id;
                const std::string name = n.name;
                n = o, n.id = id, n.name = name, n.locked = false;
                made.insert(id);
                active_ = id;
            }
        selected_ = made;
        touchAll();
        setTool(Tool::Move);
        say("Duplicated: drag the arrows to move the copy");
    }

    // The selected objects become one (the last picked keeps its place, look and name).
    void joinSelected() {
        Object* into = find(active_);
        if (mode_ != Mode::Object || selected_.size() < 2 || !into) return say("Select two or more objects to join (the last picked keeps its name)");
        pushUndo();
        const glm::mat4 toLocal = glm::inverse(objectMatrix(*into));
        for (const Object& o : scene_.objects)
            if (selected_.count(o.id) && o.id != into->id) append(into->mesh, o.hasFinish() ? finishedMesh(o) : o.mesh, toLocal * objectMatrix(o));
        handMade(*into);
        const uint32_t keep = into->id;
        std::erase_if(scene_.objects, [&](const Object& o) { return selected_.count(o.id) && o.id != keep; });
        selected_ = {keep}, active_ = keep;
        touched(keep);
    }

    // The selected faces become an object of their own.
    void separateSelected() {
        Object* o = editing();
        if (!o || elem_ != Elem::Face || selFaces_.empty()) return say("Select faces to make into their own object");
        pushUndo();
        Mesh part;
        part.verts = o->mesh.verts;
        for (int f : selFaces_) part.faces.push_back(o->mesh.faces[size_t(f)]);
        removeUnused(part);
        deleteFaces(o->mesh, selFaces_);
        removeUnused(o->mesh);
        handMade(*o);
        touched(o->id);
        Object copy = *o;
        Object& n = addObject(o->name + " part", std::move(part));
        const uint32_t id = n.id;
        const std::string name = n.name;
        Mesh m = std::move(n.mesh);
        n = copy, n.id = id, n.name = name, n.mesh = std::move(m);
        clearElements();
        leaveEdit();
        selected_ = {id}, active_ = id;
        say("Separated into " + name);
    }

    // ───────────────────────────── files ─────────────────────────────

    // ── Files a phone's file picker gives (content://...): read and written through SDL ──
    static bool isUri(const std::string& p) { return p.starts_with("content://"); }
    // A file's name as shown (a picked file's: from the end of its address, decoded).
    static std::string fileName(const std::string& p) {
        if (!isUri(p)) return stdfs::path(p).filename().string();
        std::string d;
        for (size_t i = 0; i < p.size(); ++i)
            if (p[i] == '%' && i + 2 < p.size()) d += char(std::stoi(p.substr(i + 1, 2), nullptr, 16)), i += 2;
            else d += p[i];
        return d.substr(d.find_last_of("/:") + 1);
    }
    // A picked file copied to a file of our own to read ("" if it couldn't be).
    static std::string fromUri(const std::string& uri, const std::string& name) {
        size_t n = 0;
        void* data = SDL_LoadFile(uri.c_str(), &n);
        if (!data) return "";
        std::error_code ec;
        const std::string local = (stdfs::temp_directory_path(ec) / name).string();
        std::ofstream(local, std::ios::binary).write(static_cast<const char*>(data), std::streamsize(n));
        SDL_free(data);
        return local;
    }
    // A file of ours written into the picked one.
    static bool toUri(const std::string& local, const std::string& uri) {
        std::ifstream in(local, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SDL_IOStream* io = SDL_IOFromFile(uri.c_str(), "wb");
        if (!io) return false;
        const bool ok = SDL_WriteIO(io, bytes.data(), bytes.size()) == bytes.size();
        return SDL_CloseIO(io) && ok;
    }
    static std::string tempFile(const std::string& name) {
        std::error_code ec;
        return (stdfs::temp_directory_path(ec) / name).string();
    }

    void openFile(const std::string& picked) {
        std::string err;
        const std::string path = isUri(picked) ? fromUri(picked, "opened.fry") : picked;
        if (path.empty()) return say("Couldn't read that file");
        auto s = loadPackage(path, &err);
        if (!s) return say("Couldn't open: " + err);
        dropAutosave();
        scene_ = std::move(*s);
        // (an old plain-text .fry: its pictures are PNGs beside it)
        const stdfs::path here = stdfs::path(path).parent_path();
        for (Object& o : scene_.objects) {
            if (!o.pictureFile.empty()) {
                Image im;
                if (loadImageFile((here / o.pictureFile).string(), im)) o.picture = std::make_shared<Image>(std::move(im));
                else say("Couldn't load the painted picture " + o.pictureFile);
            }
            for (PictureLayer& l : o.layers) {
                Image im;
                if (!l.image && !l.file.empty() && loadImageFile((here / l.file).string(), im)) l.image = std::make_shared<Image>(std::move(im));
            }
            std::erase_if(o.layers, [](const PictureLayer& l) { return !l.image; });
        }
        path_ = picked;
        undo_.clear(), redo_.clear(), selected_.clear();
        mode_ = Mode::Object, active_ = 0, dirty_ = false, ledit_ = {};
        touchAll();
        frameAll();
        say("Opened " + fileName(picked));
    }
    bool saveFile(std::string path) {
        if (path.empty()) return false;
        if (!isUri(path) && stdfs::path(path).extension() != ".fry") path += ".fry";
        const std::string local = isUri(path) ? tempFile("saving.fry") : path;
        std::string err;
        if (!savePackage(local, scene_, 12, &err)) return afterSave_ = 0, say("Couldn't save: " + err), false;
        if (isUri(path) && !toUri(local, path)) return afterSave_ = 0, say("Couldn't write the file you picked"), false;
        dropAutosave();
        path_ = path, dirty_ = false;
        say(std::format("Saved {} ({})", fileName(path), sizeText(stdfs::file_size(local))));
        if (const int then = std::exchange(afterSave_, 0)) doAfterAsking(then);
        return true;
    }
    static std::string sizeText(uintmax_t n) {
        return n < 10240 ? std::format("{} bytes", n) : n < 10 << 20 ? std::format("{} KB", n >> 10) : std::format("{:.1f} MB", double(n) / 1048576.0);
    }
    static std::vector<uint8_t> pngBytes(const Image& im) {
        std::vector<uint8_t> out;
        stbi_write_png_to_func([](void* ctx, void* data, int n) { auto* v = static_cast<std::vector<uint8_t>*>(ctx); v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + n); },
                               &out, im.w, im.h, 4, im.px.data(), im.w * 4);
        return out;
    }
    void exportFile(std::string path, int filter = -1, const std::string& asExt = "") {
        if (path.empty()) return;
        if (isUri(path)) { // (a picked file: made here, then written into it; one file only)
            const auto& formats = exportFormats();
            const std::string ext = !asExt.empty() ? asExt : formats[size_t(filter >= 0 && filter < int(formats.size()) ? filter : 0)].first;
            const std::string local = tempFile("export" + ext);
            exportFile(local);
            if (!toUri(local, path)) say("Couldn't write the file you picked");
            return;
        }
        std::string ext = stdfs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const auto& formats = exportFormats();
        if (std::none_of(formats.begin(), formats.end(), [&](const auto& f) { return f.first == ext; }))
            path += formats[size_t(filter >= 0 && filter < int(formats.size()) ? filter : 0)].first;
        std::vector<std::vector<uint8_t>> pngs(scene_.objects.size());
        for (size_t i = 0; i < scene_.objects.size(); ++i)
            if (const Object& o = scene_.objects[i]; o.picture && o.uvsFit()) pngs[i] = pngBytes(o.layers.empty() ? *o.picture : composePicture(o));
        std::string err;
        if (!exportModel(path, scene_, pngs, &err)) return say("Couldn't export: " + err);
        say("Exported " + stdfs::path(path).filename().string());
    }

    // ── Closing with changes, autosave ──

    // New, Open, Quit: asks about saving first if there are changes.
    void whenSaved(int action) {
        if (dirty_) askSave_ = action;
        else doAfterAsking(action);
    }
    void doAfterAsking(int action) {
        if (action == 1) dropAutosave(), keel::execute("quit");
        else if (action == 2) newScene();
        else if (action == 3) dialog(1);
        else if (action == 4) openFile(std::exchange(dropped_, std::string()));
    }
    // A file dropped on the window (or opened with Fry3DX from the desktop): a .fry opens (after
    // asking about changes), a script goes in the Script panel, a picture goes on as a reference
    // (or, in the Paint tab, as a sticker on the model).
    void onFileDropped(const std::string& path) override {
        std::string ext = stdfs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".fry") return (void)(dropped_ = path, whenSaved(4));
        if (ext == ".lua") return openScript(path, fileName(path));
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") return tab_ == Tab::Paint ? floatFromFile(path) : addPicture(path);
        say("Fry3DX opens .fry files, scripts (.lua) and pictures");
    }
    void saveQuestion() {
        if (!askSave_) return;
        if (!ImGui::IsPopupOpen("Save changes?")) ImGui::OpenPopup("Save changes?");
        const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) return;
        const std::string name = path_.empty() ? "Untitled" : fileName(path_);
        ImGui::Text("%s has changes that aren't saved.", name.c_str());
        ImGui::TextDisabled(askSave_ == 1 ? "Save them before closing?" : askSave_ == 2 ? "Save them before starting a new one?" : "Save them before opening another?");
        ImGui::Spacing();
        const float bw = 150.0f;
        const int action = askSave_;
        pushAccent();
        const bool save = ImGui::Button("Save", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        popAccent();
        ImGui::SameLine();
        const bool drop = ImGui::Button(action == 1 ? "Don't save, close" : "Don't save", ImVec2(bw, 0));
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(bw, 0));
        if (save || drop || cancel || !askSave_) askSave_ = 0, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        if (save) { // (saveFile goes on with it once saved)
            afterSave_ = action;
            path_.empty() ? dialog(2) : void(saveFile(path_));
        } else if (drop) {
            dropAutosave();
            dirty_ = false;
            doAfterAsking(action);
        }
    }
    std::string autosaveDir() const { return keel::prefPath() + "autosave/"; }
    // Every fry_autosave minutes (when there are changes and nothing's being dragged): a copy
    // packed as small as it goes, in the background.
    void autosave(bool force = false) {
        const float minutes = fry_autosave.f();
        if (!force && (minutes <= 0 || keel::isTestMode())) return;
        if (!dirty_ || (!force && (edits_ == autosavedEdits_ || now() - lastAutosave_ < minutes * 60.0))) return;
        if (!force && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || drag_.active || paint_.active)) return;
        if (autosaveJob_.valid() && autosaveJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        if (autosaveJob_.valid() && !autosaveJob_.get()) say("Couldn't autosave (is the disk full?)");
        std::error_code ec;
        stdfs::create_directories(autosaveDir(), ec);
        const std::string stem = path_.empty() ? "Untitled" : stdfs::path(path_).stem().string();
        if (autosavePath_.empty()) autosavePath_ = autosaveDir() + std::format("{}-{}.fry", stem, std::time(nullptr));
        std::ofstream(autosavePath_ + ".from") << path_; // (where it belongs)
        lastAutosave_ = now(), autosavedEdits_ = edits_;
        autosaveJob_ = std::async(std::launch::async, [copy = scene_, to = autosavePath_] { return savePackage(to, copy, 19); });
    }
    // Its autosave isn't needed (saved, or closed on purpose).
    void dropAutosave() {
        if (autosaveJob_.valid()) autosaveJob_.wait();
        if (autosavePath_.empty()) return;
        std::error_code ec;
        stdfs::remove(autosavePath_, ec), stdfs::remove(autosavePath_ + ".from", ec);
        autosavePath_.clear(), lastAutosave_ = now();
    }
    // Autosaves left behind (it didn't close properly): offered back. Only the newest 15 are
    // kept; older ones are deleted.
    void findAutosaves() {
        static constexpr size_t kKeep = 15;
        recover_.clear();
        std::error_code ec;
        std::vector<std::pair<stdfs::file_time_type, std::string>> found;
        for (const auto& e : stdfs::directory_iterator(autosaveDir(), ec))
            if (e.path().extension() == ".fry") found.emplace_back(e.last_write_time(ec), e.path().string());
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; }); // newest first
        for (size_t i = 0; i < found.size(); ++i) {
            if (i < kKeep || found[i].second == autosavePath_) recover_.push_back(found[i].second);
            else stdfs::remove(found[i].second, ec), stdfs::remove(found[i].second + ".from", ec);
        }
        showRecover_ = !recover_.empty() && !keel::isTestMode();
    }
    void recoverWindow() {
        if (!showRecover_) return;
        if (!ImGui::IsPopupOpen("Recover your work?")) ImGui::OpenPopup("Recover your work?");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Recover your work?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) return;
        ImGui::TextUnformatted("Fry3DX didn't close properly last time. These were autosaved:");
        ImGui::Spacing();
        std::string open;
        for (const std::string& f : recover_) {
            std::ifstream in(f + ".from");
            std::string from;
            std::getline(in, from);
            std::error_code ec;
            // (in local time: through time_t, as not every C++ library has time zones yet)
            const auto age = stdfs::file_time_type::clock::now() - stdfs::last_write_time(f, ec); // (file clocks differ by library: by age)
            const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() - std::chrono::duration_cast<std::chrono::system_clock::duration>(age));
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &t);
#else
            localtime_r(&t, &local);
#endif
            char when[32];
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &local);
            const std::string label = std::format("{}   {}   {}", from.empty() ? "Untitled" : stdfs::path(from).filename().string(), when, sizeText(stdfs::file_size(f, ec)));
            ImGui::PushID(f.c_str());
            if (ImGui::Button("Open", ImVec2(70, 0))) open = f;
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (ImGui::Button("Throw them all away", ImVec2(200, 0))) {
            std::error_code ec;
            for (const std::string& f : recover_) stdfs::remove(f, ec), stdfs::remove(f + ".from", ec);
            recover_.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later", ImVec2(120, 0))) showRecover_ = false;
        if (recover_.empty() || !showRecover_ || !open.empty()) showRecover_ = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        if (!open.empty()) {
            std::ifstream in(open + ".from");
            std::string from;
            std::getline(in, from);
            openFile(open);
            path_ = from, dirty_ = true, autosavePath_ = open; // (it goes on autosaving there until saved)
            say("Recovered" + (from.empty() ? std::string() : " " + stdfs::path(from).filename().string()) + ": save it to keep it (Ctrl+S)");
        }
    }

    // A picture of the 3D view only (no panels, gizmos or outlines): the screen is saved a few
    // frames on, with those hidden, then cut down to the view.
    void savePicture(std::string path) {
        if (path.empty()) return;
        if (stdfs::path(path).extension() != ".png") path += ".png";
        capturePath_ = path, captureFrames_ = 5;
    }
    bool capturing() const { return captureFrames_ > 0; }
    void captureStep() {
        if (!captureFrames_) return;
        const std::string tmp = keel::prefPath() + "fry_capture.png";
        if (--captureFrames_ == 3) keel::execute("screenshot \"" + tmp + "\"");
        if (captureFrames_) return;
        keel::waitForScreenshots();
        int w = 0, h = 0, n = 0;
        stbi_uc* px = stbi_load(tmp.c_str(), &w, &h, &n, 4);
        if (!px) return say("Couldn't save the picture");
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float sx = float(w) / std::max(size.x, 1.0f), sy = float(h) / std::max(size.y, 1.0f);
        const int x0 = std::clamp(int(viewMin_.x * sx), 0, w - 1), y0 = std::clamp(int(viewMin_.y * sy), 0, h - 1);
        const int x1 = std::clamp(int(viewMax_.x * sx), x0 + 1, w), y1 = std::clamp(int(viewMax_.y * sy), y0 + 1, h);
        const bool ok = stbi_write_png(capturePath_.c_str(), x1 - x0, y1 - y0, 4, px + (size_t(y0) * size_t(w) + size_t(x0)) * 4, w * 4);
        stbi_image_free(px);
        std::error_code ec;
        stdfs::remove(tmp, ec);
        if (const std::string uri = std::exchange(captureUri_, {}); ok && !uri.empty())
            return say(toUri(capturePath_, uri) ? "Saved a picture of the view: " + fileName(uri) : "Couldn't write the file you picked");
        say(ok ? "Saved a picture of the view: " + stdfs::path(capturePath_).filename().string() : "Couldn't write " + capturePath_);
    }
    static bool loadImageFile(const std::string& path, Image& out) {
        int w = 0, h = 0, n = 0;
        stbi_uc* px = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!px) return false;
        out.w = w, out.h = h, out.px.assign(px, px + size_t(w) * size_t(h) * 4);
        stbi_image_free(px);
        return true;
    }
    static bool writePng(const std::string& path, const Image& im) { return stbi_write_png(path.c_str(), im.w, im.h, 4, im.px.data(), im.w * 4) != 0; }
    void addPicture(const std::string& path) {
        pushUndo();
        Reference ref;
        ref.id = scene_.nextId++;
        ref.path = path;
        ref.axis = view_ == View::Front || view_ == View::Back ? Axis::Front : view_ == View::Top || view_ == View::Bottom ? Axis::Top : Axis::Side;
        ref.width = 2.0f;
        scene_.references.push_back(ref);
        activeRef_ = ref.id;
        if (view_ == View::Perspective) setView(ref.axis == Axis::Front ? View::Front : ref.axis == Axis::Top ? View::Top : View::Left);
        say("Picture added: set its width in Properties to the real size");
    }

    // SDL's own file dialogs (the desktop's): the answer comes back later, maybe on another thread.
    void dialog(int kind) {
        static const SDL_DialogFileFilter fry[] = {{"Fry3DX files", "fry"}}, pics[] = {{"Pictures", "png;jpg;jpeg;bmp;tga"}}, png[] = {{"PNG picture", "png"}};
        static std::vector<std::string> names;
        static std::vector<SDL_DialogFileFilter> models;
        if (models.empty()) {
            for (const auto& [ext, what] : exportFormats()) names.push_back(what + " (" + ext + ")"), names.push_back(ext.substr(1));
            for (size_t i = 0; i < names.size(); i += 2) models.push_back({names[i].c_str(), names[i + 1].c_str()});
        }
        pendingKind_ = kind;
        auto done = [](void* user, const char* const* files, int filter) {
            App* self = static_cast<App*>(user);
            std::lock_guard lock(self->dialogMutex_);
            if (files && files[0]) self->dialogResult_ = files[0], self->dialogKind_ = self->pendingKind_, self->dialogFilter_ = filter;
            else self->dialogCancelled_ = true;
        };
        SDL_Window* w = SDL_GetKeyboardFocus();
        const std::string here = path_.empty() || isUri(path_) ? std::string() : stdfs::path(path_).parent_path().string();
        const char* at = here.empty() ? nullptr : here.c_str();
        if (kind == 1) SDL_ShowOpenFileDialog(done, this, w, fry, 1, at, false);
        else if (kind == 2) SDL_ShowSaveFileDialog(done, this, w, fry, 1, path_.empty() || isUri(path_) ? at : path_.c_str());
        else if (kind == 3) SDL_ShowSaveFileDialog(done, this, w, models.data(), int(models.size()), at);
        else if (kind == 8 || kind == 9) {
            static const SDL_DialogFileFilter mf[] = {{"3MF for slicers", "3mf"}}, stl[] = {{"STL", "stl"}};
            SDL_ShowSaveFileDialog(done, this, w, kind == 8 ? mf : stl, 1, at);
        }
        else if (kind == 5 || kind == 7) SDL_ShowSaveFileDialog(done, this, w, png, 1, at);
        else if (kind == 10 || kind == 11) {
            static const SDL_DialogFileFilter lua[] = {{"Lua scripts", "lua"}};
            if (kind == 10) SDL_ShowOpenFileDialog(done, this, w, lua, 1, at, false);
            else SDL_ShowSaveFileDialog(done, this, w, lua, 1, scriptPath_.empty() || isUri(scriptPath_) ? at : scriptPath_.c_str()); // (at: the last folder)
        }
        else SDL_ShowOpenFileDialog(done, this, w, pics, 1, at, false);
    }
    void takeDialogResult() {
        std::string file;
        int kind, filter;
        {
            std::lock_guard lock(dialogMutex_);
            file.swap(dialogResult_), kind = dialogKind_, filter = dialogFilter_;
            if (std::exchange(dialogCancelled_, false)) afterSave_ = 0; // (Save, then quit: but the saving was cancelled)
        }
        if (file.empty()) return;
        if (isUri(file) && (kind == 4 || kind == 6)) file = fromUri(file, std::format("picked-{}.img", SDL_GetTicks())); // (pictures: read from a copy)
        if (isUri(file) && (kind == 5 || kind == 7)) { // (pictures saved: made here, then written into the picked file)
            const std::string uri = file;
            file = tempFile("picture.png");
            if (kind == 5) return savePicture(file), void(captureUri_ = uri); // (written into it once it's made)
            if (Object* o = find(active_); o && o->picture) writePng(file, composePicture(*o));
            say(toUri(file, uri) ? "Saved " + fileName(uri) : "Couldn't write the file you picked");
            return;
        }
        if (isUri(file) && (kind == 8 || kind == 9)) return exportFile(file, -1, kind == 8 ? ".3mf" : ".stl");
        if (kind == 10) return openScript(isUri(file) ? fromUri(file, "opened.lua") : file, fileName(file));
        if (kind == 11) return saveScript(file);
        if (kind == 1) openFile(file);
        else if (kind == 2) saveFile(file);
        else if (kind == 3) exportFile(file, filter);
        else if (kind == 8 || kind == 9) {
            if (stdfs::path(file).extension() != (kind == 8 ? ".3mf" : ".stl")) file += kind == 8 ? ".3mf" : ".stl";
            exportFile(file);
        }
        else if (kind == 4) addPicture(file);
        else if (kind == 5) savePicture(file);
        else if (kind == 6) floatFromFile(file);
        else if (kind == 7) {
            if (stdfs::path(file).extension() != ".png") file += ".png";
            if (Object* o = find(active_); o && o->picture) say(writePng(file, composePicture(*o)) ? "Saved " + stdfs::path(file).filename().string() : "Couldn't write " + file);
        }
    }

    // ───────────────────────────── modes and tools ─────────────────────────────

    void setTool(Tool t) {
        for (const ToolInfo& i : kTools)
            if (i.tool == t && ((mode_ == Mode::Object && !i.objectMode) || (mode_ == Mode::Edit && !i.editMode))) {
                if (mode_ == Mode::Object && i.editMode) { // (a shape tool: go and edit the shape)
                    enterEdit();
                    if (mode_ != Mode::Edit) return;
                    break;
                }
                return say(std::format("{} works on whole objects (Tab leaves shape editing)", i.name));
            }
        tool_ = t;
        drawPts_.clear(), knifePts_.clear();
        if ((t == Tool::Extrude || t == Tool::Inset) && elem_ != Elem::Face) elem_ = Elem::Face, clearElements();
    }
    void enterEdit() {
        if (mode_ == Mode::Edit) return;
        Object* o = find(active_);
        if (!o || !selected_.count(active_)) {
            if (selected_.empty()) return say("Select an object first, then edit its shape");
            active_ = *selected_.begin();
            o = find(active_);
        }
        if (!o || o->locked) return say("That object is locked");
        mode_ = Mode::Edit;
        selected_ = {active_};
        clearElements();
        if (tool_ == Tool::Draw) tool_ = Tool::Move;
        drawPts_.clear();
        touched(o->id); // (shown without its rounding and smoothing while you edit)
    }
    void leaveEdit() {
        if (mode_ != Mode::Edit) return;
        mode_ = Mode::Object;
        if (tool_ == Tool::Extrude || tool_ == Tool::Inset || tool_ == Tool::Bevel || tool_ == Tool::LoopCut || tool_ == Tool::Knife) tool_ = Tool::Move;
        knifePts_.clear();
        if (Object* o = find(active_)) touched(o->id);
    }
    void clearElements() { selVerts_.clear(), selFaces_.clear(), selEdges_.clear(); }
    void selectAllElements() {
        Object* o = editing();
        if (!o) return;
        clearElements();
        for (size_t v = 0; v < o->mesh.verts.size(); ++v) selVerts_.insert(int(v));
        for (size_t f = 0; f < o->mesh.faces.size(); ++f) selFaces_.insert(int(f));
        for (auto e : edges(o->mesh)) selEdges_.insert(e);
    }
    void selectAll() {
        if (mode_ == Mode::Edit) return selectAllElements();
        selected_.clear();
        for (const Object& o : scene_.objects)
            if (o.visible) selected_.insert(o.id), active_ = o.id;
    }
    void selectNone() {
        if (mode_ == Mode::Edit) clearElements();
        else selected_.clear();
    }
    // The points the selection covers, whichever kind is picked.
    std::set<int> selectedPoints() {
        std::set<int> pts;
        Object* o = editing();
        if (!o) return pts;
        if (elem_ == Elem::Vertex) pts = selVerts_;
        else if (elem_ == Elem::Edge)
            for (auto [a, b] : selEdges_) pts.insert(a), pts.insert(b);
        else
            for (int f : selFaces_)
                if (f < int(o->mesh.faces.size()))
                    for (int v : o->mesh.faces[size_t(f)]) pts.insert(v);
        return pts;
    }
    // Selects (in the current kind) what these points cover.
    void selectPoints(const std::set<int>& pts) {
        Object* o = editing();
        if (!o) return;
        clearElements();
        if (elem_ == Elem::Vertex) selVerts_ = pts;
        else if (elem_ == Elem::Edge) {
            for (auto [a, b] : edges(o->mesh))
                if (pts.count(a) && pts.count(b)) selEdges_.insert({a, b});
        } else
            for (size_t f = 0; f < o->mesh.faces.size(); ++f)
                if (std::all_of(o->mesh.faces[f].begin(), o->mesh.faces[f].end(), [&](int v) { return pts.count(v); })) selFaces_.insert(int(f));
    }
    void growSelection() {
        if (Object* o = editing()) selectPoints(growVerts(o->mesh, selectedPoints()));
    }
    void shrinkSelection() {
        Object* o = editing();
        if (!o) return;
        const std::set<int> sel = selectedPoints();
        std::set<int> rest;
        for (size_t v = 0; v < o->mesh.verts.size(); ++v)
            if (!sel.count(int(v))) rest.insert(int(v));
        const std::set<int> border = growVerts(o->mesh, rest);
        std::set<int> keep;
        for (int v : sel)
            if (!border.count(v)) keep.insert(v);
        selectPoints(keep);
    }
    void selectLinked() {
        if (Object* o = editing()) selectPoints(linkedVerts(o->mesh, selectedPoints()));
    }
    void invertSelection() {
        if (mode_ == Mode::Object) {
            std::set<uint32_t> inv;
            for (const Object& o : scene_.objects)
                if (o.visible && !selected_.count(o.id)) inv.insert(o.id), active_ = o.id;
            selected_ = inv;
            return;
        }
        Object* o = editing();
        if (!o) return;
        if (elem_ == Elem::Vertex) {
            std::set<int> inv;
            for (size_t v = 0; v < o->mesh.verts.size(); ++v)
                if (!selVerts_.count(int(v))) inv.insert(int(v));
            selVerts_ = inv;
        } else if (elem_ == Elem::Edge) {
            std::set<std::pair<int, int>> inv;
            for (auto e : edges(o->mesh))
                if (!selEdges_.count(e)) inv.insert(e);
            selEdges_ = inv;
        } else {
            std::set<int> inv;
            for (size_t f = 0; f < o->mesh.faces.size(); ++f)
                if (!selFaces_.count(int(f))) inv.insert(int(f));
            selFaces_ = inv;
        }
    }
    // The edges a rounding would work on: those selected, or round the selected faces, or between
    // selected points.
    std::set<std::pair<int, int>> edgesToRound() {
        std::set<std::pair<int, int>> out;
        Object* o = editing();
        if (!o) return out;
        if (elem_ == Elem::Edge) return selEdges_;
        if (elem_ == Elem::Face) {
            for (int f : selFaces_) {
                const auto& fv = o->mesh.faces[size_t(f)];
                for (size_t i = 0; i < fv.size(); ++i) out.insert({std::min(fv[i], fv[(i + 1) % fv.size()]), std::max(fv[i], fv[(i + 1) % fv.size()])});
            }
            return out;
        }
        for (auto [a, b] : edges(o->mesh))
            if (selVerts_.count(a) && selVerts_.count(b)) out.insert({a, b});
        return out;
    }

    void deleteElements() {
        Object* o = editing();
        if (!o) return;
        pushUndo();
        if (elem_ == Elem::Face) deleteFaces(o->mesh, selFaces_);
        else deleteVerts(o->mesh, selectedPoints());
        clearElements();
        handMade(*o);
        touched(o->id);
    }
    // After a change that renumbers things: nothing selected, the shape is hand-made now.
    void changedShape(Object& o, bool keepSelection = false) {
        if (!keepSelection) clearElements();
        handMade(o);
        touched(o.id);
    }
    void extrudeNow(float amount) {
        Object* o = editing();
        if (!o || elem_ != Elem::Face || selFaces_.empty()) return say("Select faces to extrude (Faces, 3)");
        pushUndo();
        extrudeFaces(o->mesh, selFaces_, amount);
        changedShape(*o, true);
    }
    void insetNow(float amount) {
        Object* o = editing();
        if (!o || elem_ != Elem::Face || selFaces_.empty()) return say("Select faces to inset (Faces, 3)");
        pushUndo();
        insetFaces(o->mesh, selFaces_, amount);
        changedShape(*o, true);
    }
    void bevelNow() {
        Object* o = editing();
        const auto es = edgesToRound();
        if (!o || es.empty()) return say("Select edges (or faces) to round");
        pushUndo();
        bevelEdges(o->mesh, es, bevelWidth_, bevelSteps_);
        changedShape(*o);
        if (bevelSteps_ > 1) o->smooth = true;
    }
    void subdivideNow(bool smooth) {
        Object* o = editing();
        if (!o) return;
        pushUndo();
        subdivide(o->mesh, smooth);
        if (smooth) o->smooth = true;
        changedShape(*o);
    }
    void cutAt(Object& o, std::pair<int, int> edge) {
        pushUndo();
        const auto made = loopCut(o.mesh, edge, cuts_);
        if (made.empty()) {
            dropUndo();
            return say("Nothing to cut there: loop cuts go across four-sided faces");
        }
        changedShape(o);
        // The new edges are selected, ready to move (along the faces, with the gizmo).
        const std::set<int> pts(made.begin(), made.end());
        elem_ = Elem::Edge;
        for (auto [a, b] : edges(o.mesh))
            if (pts.count(a) && pts.count(b)) selEdges_.insert({a, b});
        say(std::format("Cut {} time{} round. W moves the new edges.", cuts_, cuts_ == 1 ? "" : "s"));
    }
    void makeFaceNow() {
        Object* o = editing();
        if (!o) return;
        const std::set<int> pts = selectedPoints();
        pushUndo();
        const glm::vec3 towards = glm::inverse(glm::mat3(objectMatrix(*o))) * (eye_ - pivotOfSelection());
        if (!makeFace(o->mesh, std::vector<int>(pts.begin(), pts.end()), towards)) {
            dropUndo();
            return say("Pick 3 or more points (not in a line) to make a face");
        }
        changedShape(*o);
        selFaces_ = {int(o->mesh.faces.size()) - 1};
        elem_ = Elem::Face;
    }
    void mergeNow() {
        Object* o = editing();
        if (!o) return;
        const std::set<int> pts = selectedPoints();
        if (pts.size() < 2) return say("Select 2 or more points to merge");
        pushUndo();
        mergeToCenter(o->mesh, pts);
        changedShape(*o);
    }
    void smoothPointsNow() {
        Object* o = editing();
        const std::set<int> pts = selectedPoints();
        if (!o || pts.empty()) return say("Select points (or faces, edges) to smooth");
        pushUndo();
        smoothPoints(o->mesh, pts, 0.5f, 2);
        changedShape(*o, true);
        say("Smoothed (again for more)");
    }
    void startPush() {
        Object* o = editing();
        if (!o || selectedPoints().empty()) return say("Select points (or faces) to push out or pull in");
        pushUndo();
        pushPts_ = selectedPoints(), pushFrom_ = o->mesh, pushObject_ = o->id, pushBy_ = 0.0f, showPush_ = true;
    }
    void pushWindow() {
        if (!showPush_) return;
        Object* o = find(pushObject_);
        if (!o || mode_ != Mode::Edit) return void(showPush_ = false);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bool open = true;
        if (!ImGui::Begin("Push out, pull in", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) return ImGui::End();
        const float w = 260;
        float mm = pushBy_ * 1000;
        ImGui::SetNextItemWidth(w);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::DragFloat("##by", &mm, 0.5f, -1e5f, 1e5f, "%.1f mm (- pulls in)")) {
            pushBy_ = mm / 1000;
            o->mesh = pushFrom_;
            pushPoints(o->mesh, pushPts_, pushBy_);
            changedShape(*o, true);
        }
        ImGui::TextWrapped("Each point along the surface's normal there: fatter or thinner, the same shape.");
        pushAccent();
        const bool done = ImGui::Button("Done", ImVec2(w, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        popAccent();
        ImGui::End();
        if (done) showPush_ = false;
        if (!open) o->mesh = pushFrom_, changedShape(*o, true), showPush_ = false; // (closed: as it was)
    }
    void bridgeNow() {
        Object* o = editing();
        if (!o || selFaces_.size() != 2) return say("Select two faces to bridge (Faces, 3; Join two objects first if they're apart)");
        const int a = *selFaces_.begin(), b = *selFaces_.rbegin();
        if (o->mesh.faces[size_t(a)].size() != o->mesh.faces[size_t(b)].size())
            return say(std::format("Those faces have {} and {} corners: bridging needs as many on each", o->mesh.faces[size_t(a)].size(), o->mesh.faces[size_t(b)].size()));
        pushUndo();
        const std::vector<int> made = bridgeFaces(o->mesh, a, b);
        if (made.empty()) return takeBack(), say("Couldn't bridge those (they share a point)");
        changedShape(*o);
        selFaces_.insert(made.begin(), made.end());
        say(std::format("Bridged: {} faces between them", made.size()));
    }
    void flipNow() {
        Object* o = editing();
        if (!o || selFaces_.empty()) return say("Select faces to flip");
        pushUndo();
        flipFaces(o->mesh, selFaces_);
        changedShape(*o, true);
    }
    // Rotation and scale baked into the points (so the object's turn is none and its size is 1).
    void applyTransform(Object& o) {
        pushUndo();
        transform(o.mesh, glm::mat4_cast(o.rotation) * glm::scale(glm::mat4(1.0f), o.scale));
        o.rotation = glm::quat(1, 0, 0, 0), o.scale = glm::vec3(1.0f);
        handMade(o);
        touched(o.id);
    }
    void applyFinish(Object& o) {
        pushUndo();
        o.mesh = finishedMesh(o);
        o.mirror = 0, o.round = 0, o.smoothLevels = 0;
        handMade(o);
        touched(o.id);
    }
    void originToMiddle(Object& o) {
        pushUndo();
        const glm::vec3 c = (boundsMin(o.mesh) + boundsMax(o.mesh)) * 0.5f;
        for (glm::vec3& v : o.mesh.verts) v -= c;
        o.position += glm::vec3(objectMatrix(o) * glm::vec4(c, 0.0f));
        handMade(o);
        touched(o.id);
    }
    void originToBottom(Object& o) {
        pushUndo();
        const glm::vec3 c((boundsMin(o.mesh).x + boundsMax(o.mesh).x) * 0.5f, (boundsMin(o.mesh).y + boundsMax(o.mesh).y) * 0.5f, boundsMin(o.mesh).z);
        for (glm::vec3& v : o.mesh.verts) v -= c;
        o.position += glm::vec3(objectMatrix(o) * glm::vec4(c, 0.0f));
        handMade(o);
        touched(o.id);
    }
    void mirror(Object& o, int axis) {
        pushUndo();
        glm::vec3 s(1.0f);
        s[axis] = -1.0f;
        transform(o.mesh, glm::scale(glm::mat4(1.0f), s));
        handMade(o);
        touched(o.id);
    }


    // ───────────────────────────── tabs ─────────────────────────────

    void setTab(Tab t) {
        if (t == tab_) return;
        if (drag_.active) cancelDrag();
        lockLayer();
        endStroke(), endSculpt();
        knifePts_.clear(), drawPts_.clear();
        if (t != Tab::Model) leaveEdit();
        if ((t == Tab::Render || t == Tab::Print) && tool_ != Tool::Select && tool_ != Tool::Move && tool_ != Tool::Rotate && tool_ != Tool::Scale) tool_ = Tool::Move;
        if ((t == Tab::Paint || t == Tab::Layout) && tab_ != Tab::Paint && tab_ != Tab::Layout) picZoom_ = 0; // (fit the picture again)
        lay_.drag = 0, printTool_ = PrintTool::None, measure_.clear();
        tab_ = t;
    }
    Shading shading() const { return tab_ == Tab::Render ? Shading::Rendered : shading_; }

    // ───────────────────────────── the knife and holes ─────────────────────────────

    // Snapped to steps of 45 degrees from the last point while Ctrl is held.
    ImVec2 knifeEnd(const ImGuiIO& io) const {
        ImVec2 m = io.MousePos;
        const bool have = !knifePts_.empty() || knifeDown_;
        if (!io.KeyCtrl || !have) return m;
        const ImVec2 from = knifePts_.empty() ? knifePress_ : knifePts_.back();
        const float dx = m.x - from.x, dy = m.y - from.y, len = std::hypot(dx, dy);
        const float a = std::round(std::atan2(dy, dx) / (kPi / 4)) * (kPi / 4);
        return ImVec2(from.x + std::cos(a) * len, from.y + std::sin(a) * len);
    }
    static Mesh subMesh(const Mesh& m, const std::set<int>& faces) {
        Mesh p;
        p.verts = m.verts;
        for (int f : faces) p.faces.push_back(m.faces[size_t(f)]);
        removeUnused(p);
        return p;
    }
    // A new object like `from` (its look, place and finish), with another shape.
    uint32_t objectLike(uint32_t from, Mesh mesh, const std::string& name) {
        const Object copy = *find(from);
        Object& n = addObject(name, std::move(mesh));
        const uint32_t id = n.id;
        const std::string unique = n.name;
        Mesh m = std::move(n.mesh);
        n = copy, n.id = id, n.name = unique, n.mesh = std::move(m), n.locked = false;
        n.recipe.clear(), n.settings.clear(), n.uvs.clear();
        touched(id);
        return id;
    }
    void knifeApply(uint32_t id, std::vector<ImVec2> pts) {
        Object* o = find(id);
        if (!o || pts.size() < 2) return;
        if (knifeThrough_) { // (on past its ends, so a line cuts right across)
            auto past = [](ImVec2 a, ImVec2 b) {
                const float dx = a.x - b.x, dy = a.y - b.y, l = std::max(std::hypot(dx, dy), 1e-3f);
                return ImVec2(a.x + dx / l * 4000.0f, a.y + dy / l * 4000.0f);
            };
            pts.front() = past(pts[0], pts[1]);
            pts.back() = past(pts[pts.size() - 1], pts[pts.size() - 2]);
        }
        const glm::mat4 inv = glm::inverse(objectMatrix(*o));
        const glm::vec3 eye(inv * glm::vec4(eye_, 1.0f));
        std::vector<glm::vec3> path;
        for (ImVec2 p : pts) path.push_back(glm::vec3(inv * glm::vec4(eye_ + mouseRay(p).dir * dist_, 1.0f)));
        pushUndo();
        const auto made = knifeCut(o->mesh, eye, path, knifeThrough_);
        if (made.empty()) {
            dropUndo();
            return say(knifeThrough_ ? "The line didn't cross the shape" : "The line didn't cross any face you can see (Tool options: cut right through)");
        }
        clearElements();
        handMade(*o);
        touched(id);
        if (!knifeThrough_ || knifeAfter_ == 0) {
            elem_ = Elem::Edge, selEdges_ = made;
            return say(std::format("Cut: {} new edge{} (selected)", made.size(), made.size() == 1 ? "" : "s"));
        }
        auto pieces = splitAlong(o->mesh, made, knifeFill_);
        if (pieces.size() < 2) {
            elem_ = Elem::Edge, selEdges_ = made;
            return say("Cut, but not all the way round, so it's still one piece");
        }
        auto size = [&](const std::set<int>& f) {
            if (knifeFill_) return volumeOf(o->mesh, f);
            const Mesh p = subMesh(o->mesh, f);
            const glm::vec3 d = boundsMax(p) - boundsMin(p);
            return d.x * d.y * d.z + 1e-9f * float(f.size());
        };
        std::sort(pieces.begin(), pieces.end(), [&](const std::set<int>& a, const std::set<int>& b) { return size(a) > size(b); });
        const size_t n = pieces.size();
        if (knifeAfter_ == 1) {
            selFaces_ = pieces[1], elem_ = Elem::Face;
            return say(std::format("Cut into {} pieces (the smaller selected: W moves it, P makes it its own object, Del deletes it)", n));
        }
        std::set<int> others;
        for (size_t i = 1; i < n; ++i) others.insert(pieces[i].begin(), pieces[i].end());
        std::vector<Mesh> parts;
        if (knifeAfter_ == 2)
            for (size_t i = 1; i < n; ++i) parts.push_back(subMesh(o->mesh, pieces[i]));
        deleteFaces(o->mesh, others);
        removeUnused(o->mesh);
        const std::string base = o->name;
        for (Mesh& part : parts) objectLike(id, std::move(part), base + " piece");
        touched(id);
        say(knifeAfter_ == 2 ? std::format("Cut into {} objects (see Layers)", n) : "Cut off: the smaller part is gone");
    }
    static float objectScale(const Object& o) { return std::max(std::cbrt(std::abs(glm::determinant(glm::mat3(objectMatrix(o))))), 1e-6f); }
    // How wide (across) a shape is when it reaches `reach` from its middle (dragging it out).
    float holeAcrossFor(float reach) const {
        return HoleShape(holeShape_) == HoleShape::Triangle || HoleShape(holeShape_) == HoleShape::Hexagon ? reach * std::sqrt(3.0f) : reach * 2.0f;
    }
    // The outline, `across` wide in the object's own size (a rectangle keeps its proportions).
    std::vector<glm::vec3> holeShapeOutline(const Object& o, int face, const glm::vec3& centre, float across) const {
        const float tall = across * holeTall_ / std::max(holeSize_, 1e-6f);
        return holeOutline(o.mesh, face, centre, HoleShape(holeShape_), across, tall, holeSides_, holeTurn_);
    }
    void applyHole(uint32_t id, int face, const glm::vec3& centre, float across) {
        Object* o = find(id);
        if (!o || face < 0 || face >= int(o->mesh.faces.size()) || across <= 0) return;
        const float scale = objectScale(*o);
        static const char* names[] = {"A round", "A rectangular", "A triangular", "A hexagonal"};
        const auto outline = holeShapeOutline(*o, face, centre, across);
        pushUndo();
        holeTall_ *= across * scale / std::max(holeSize_, 1e-6f), holeSize_ = across * scale;
        if (holeMode_ == 2) { // just the outline: a new face, picked to extrude, bevel or paint
            const int made = imprintOutline(o->mesh, face, outline);
            if (made < 0) return dropUndo(), say("The outline has to fit inside the face");
            changedShape(*o);
            selected_ = {id}, active_ = id;
            if (mode_ != Mode::Edit) enterEdit();
            elem_ = Elem::Face, selFaces_ = {made};
            return say(std::format("{} outline cut, {:.1f} mm across: its new face is picked (Extrude, Bevel, or paint it)", names[holeShape_], holeSize_ * 1000));
        }
        if (!cutHole(o->mesh, face, outline, holeMode_ == 0 ? 0.0f : holeDepth_ / scale)) {
            dropUndo();
            return say(holeMode_ == 0 ? "The hole has to fit inside the face, and inside the face behind it (it goes through)" : "The hole has to fit inside the face");
        }
        changedShape(*o);
        say(std::format("{} hole, {:.1f} mm across{}", names[holeShape_], holeSize_ * 1000,
                        holeMode_ == 0 ? ", all the way through" : std::format(", {:.1f} mm deep", holeDepth_ * 1000)));
    }
    // The hole tool's outline under the mouse (or being dragged out), in the world.
    std::vector<glm::vec3> holePreview(const ImGuiIO& io) {
        uint32_t id = hole_.object;
        int face = hole_.face;
        glm::vec3 centre = hole_.centre;
        float across = holeAcrossFor(hole_.radius);
        if (!hole_.active) {
            const Ray ray = mouseRay(io.MousePos);
            const Hit h = pickObject(ray, mode_ == Mode::Edit ? active_ : 0, true);
            if (!h.object) return {};
            const Object* o = find(h.object);
            id = h.object, face = h.face;
            centre = glm::vec3(glm::inverse(objectMatrix(*o)) * glm::vec4(ray.origin + ray.dir * h.t, 1.0f));
            across = holeSize_ / objectScale(*o);
        }
        const Object* o = find(id);
        if (!o || face < 0 || face >= int(o->mesh.faces.size())) return {};
        const glm::mat4 m = objectMatrix(*o);
        std::vector<glm::vec3> out;
        for (const glm::vec3& p : holeShapeOutline(*o, face, centre, across)) out.push_back(glm::vec3(m * glm::vec4(p, 1.0f)));
        return out;
    }
    // Where the mouse is on a face's plane (the object's own space).
    std::optional<glm::vec3> onFacePlane(const Object& o, int face, ImVec2 mouse) {
        const glm::mat4 inv = glm::inverse(objectMatrix(o));
        const Ray r = mouseRay(mouse);
        const Ray local{glm::vec3(inv * glm::vec4(r.origin, 1.0f)), glm::normalize(glm::vec3(inv * glm::vec4(r.dir, 0.0f)))};
        return hitPlane(local, faceCenter(o.mesh, face), faceNormal(o.mesh, face));
    }

    // ───────────────────────────── painting ─────────────────────────────

    // The object ready to be painted: its rounding and smoothing made part of it, its surface laid
    // out on a picture (made the plain colour of it at first).
    Object* readyToPaint(uint32_t id) {
        Object* o = find(id);
        if (!o) return nullptr;
        if (o->locked) return say("That object is locked"), nullptr;
        if (o->picture && o->uvsFit() && !o->hasFinish()) return o;
        pushUndo();
        if (o->hasFinish()) {
            o->mesh = finishedMesh(*o);
            o->mirror = 0, o->round = 0, o->smoothLevels = 0;
            handMade(*o);
        }
        o->uvs = autoUnwrap(o->mesh);
        if (!o->picture) o->picture = std::make_shared<Image>(makeImage(pictureSize_, pictureSize_, glm::vec4(o->color, 1.0f)));
        touched(o->id);
        picZoom_ = 0;
        say(std::format("{} is ready to paint: its surface is laid out flat on a {}x{} picture", o->name, o->picture->w, o->picture->h));
        return o;
    }
    // The object's picture, copied so this change can be undone; the copy is now the object's.
    Image& changePicture(Object& o) {
        pushUndo();
        auto img = std::make_shared<Image>(*o.picture);
        o.picture = img;
        paint_.image = img;
        uploaded_[o.id].dirty = true;
        return *img;
    }
    void beginStroke(Object& o, bool in2D) {
        Image& img = changePicture(o);
        paint_.active = true, paint_.in2D = in2D, paint_.haveLast = false, paint_.object = o.id;
        paint_.stroke.begin(img, paintTool_ == PaintTool::Eraser ? glm::vec4(o.color, 1.0f) : paintColor_);
    }
    void endStroke() {
        if (!paint_.active) return;
        paint_.active = false;
        uploaded_[paint_.object].dirty = true;
        paint_.image.reset();
        paint_.stroke = {};
    }
    // Where on its picture a ray hits an object (0..1), or nothing.
    std::optional<glm::vec2> uvUnder(const Object& o, const Ray& ray) {
        if (!o.uvsFit()) return std::nullopt;
        const glm::mat4 m = objectMatrix(o);
        float best = 1e30f;
        std::optional<glm::vec2> out;
        for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
            const auto& fv = o.mesh.faces[f];
            for (const auto& t : triangulate(o.mesh, int(f))) {
                glm::vec3 w[3];
                glm::vec2 uv[3];
                for (int k = 0; k < 3; ++k) {
                    w[k] = glm::vec3(m * glm::vec4(o.mesh.verts[size_t(t[size_t(k)])], 1.0f));
                    uv[k] = o.uvs[f][size_t(std::find(fv.begin(), fv.end(), t[size_t(k)]) - fv.begin())];
                }
                const glm::vec3 e1 = w[1] - w[0], e2 = w[2] - w[0], p = glm::cross(ray.dir, e2);
                const float det = glm::dot(e1, p);
                if (std::abs(det) < 1e-12f) continue;
                const glm::vec3 sv = ray.origin - w[0], q = glm::cross(sv, e1);
                const float u = glm::dot(sv, p) / det, v = glm::dot(ray.dir, q) / det, tt = glm::dot(e2, q) / det;
                if (u < 0 || v < 0 || u + v > 1 || tt <= 0 || tt >= best) continue;
                best = tt, out = uv[0] * (1 - u - v) + uv[1] * u + uv[2] * v;
            }
        }
        return out;
    }
    // One dab of the brush on the model: on every face near it, into the picture where that face
    // lies (so it carries on across the seams between pieces of the layout).
    void dab3D(Object& o, const glm::vec3& at, float radius) {
        Image& im = *paint_.image;
        const glm::mat4 m = objectMatrix(o);
        const glm::vec2 size(float(im.w), float(im.h));
        const float inner = std::clamp(hardness_, 0.0f, 0.999f) * radius;
        for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
            const auto& fv = o.mesh.faces[f];
            for (const auto& t : triangulate(o.mesh, int(f))) {
                glm::vec3 w[3];
                glm::vec2 px[3];
                for (int k = 0; k < 3; ++k) {
                    w[k] = glm::vec3(m * glm::vec4(o.mesh.verts[size_t(t[size_t(k)])], 1.0f));
                    px[k] = o.uvs[f][size_t(std::find(fv.begin(), fv.end(), t[size_t(k)]) - fv.begin())] * size;
                }
                const glm::vec3 lo = glm::min(w[0], glm::min(w[1], w[2])) - radius, hi = glm::max(w[0], glm::max(w[1], w[2])) + radius;
                if (glm::any(glm::lessThan(at, lo)) || glm::any(glm::greaterThan(at, hi))) continue;
                const glm::vec3 n = glm::cross(w[1] - w[0], w[2] - w[0]);
                const float area3 = glm::length(n) * 0.5f;
                if (area3 < 1e-12f || glm::dot(n, eye_ - w[0]) <= 0 || std::abs(glm::dot(glm::normalize(n), at - w[0])) > radius) continue;
                const float areaPx = std::abs((px[1].x - px[0].x) * (px[2].y - px[0].y) - (px[2].x - px[0].x) * (px[1].y - px[0].y)) * 0.5f;
                if (areaPx < 1e-6f) continue;
                // The dab's middle on the picture, and how far it reaches there.
                const glm::vec3 nn = glm::normalize(n), c = at - nn * glm::dot(nn, at - w[0]);
                const glm::vec3 v0 = w[1] - w[0], v1 = w[2] - w[0], v2 = c - w[0];
                const float d00 = glm::dot(v0, v0), d01 = glm::dot(v0, v1), d11 = glm::dot(v1, v1), d20 = glm::dot(v2, v0), d21 = glm::dot(v2, v1), den = d00 * d11 - d01 * d01;
                const float bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den;
                const glm::vec2 mid = px[0] * (1 - bv - bw) + px[1] * bv + px[2] * bw;
                const float reach = radius * std::sqrt(areaPx / area3) * 1.5f + 2.0f;
                glm::vec2 plo = glm::max(glm::min(px[0], glm::min(px[1], px[2])) - 2.0f, mid - reach), phi = glm::min(glm::max(px[0], glm::max(px[1], px[2])) + 2.0f, mid + reach);
                plo = glm::max(plo, glm::vec2(0.0f)), phi = glm::min(phi, size - 1.0f);
                const float twice = areaPx * 2.0f * ((px[1].x - px[0].x) * (px[2].y - px[0].y) - (px[2].x - px[0].x) * (px[1].y - px[0].y) > 0 ? 1.0f : -1.0f);
                for (int y = int(plo.y); y <= int(phi.y); ++y)
                    for (int x = int(plo.x); x <= int(phi.x); ++x) {
                        const glm::vec2 q(float(x) + 0.5f, float(y) + 0.5f);
                        float l[3];
                        bool ok = true;
                        for (int k = 0; k < 3; ++k) {
                            const glm::vec2 a = px[(k + 1) % 3], b = px[(k + 2) % 3];
                            l[k] = ((b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y)) / twice;
                            ok &= l[k] * std::abs(twice) / std::max(glm::length(b - a), 1e-6f) >= -1.5f; // (a pixel and a half past the edge: no seams)
                        }
                        if (!ok) continue;
                        const glm::vec3 p = w[0] * l[0] + w[1] * l[1] + w[2] * l[2];
                        const float d = glm::length(p - at);
                        if (d >= radius) continue;
                        const float s = d <= inner ? 1.0f : 1.0f - (d - inner) / (radius - inner);
                        paint_.stroke.put(im, x, y, s * s * (3 - 2 * s));
                    }
            }
        }
        uploaded_[o.id].dirty = true;
    }
    // The brush at a point of the view: starts a stroke on what's there, or carries one on.
    void paintAt(ImVec2 mouse, bool start) {
        const Ray ray = mouseRay(mouse);
        if (start) {
            const Hit h = pickObject(ray, 0, true);
            if (!h.object) return;
            selected_ = {h.object}, active_ = h.object;
            Object* o = readyToPaint(h.object);
            if (!o) return;
            beginStroke(*o, false);
        }
        if (!paint_.active || paint_.in2D) return;
        Object* o = find(paint_.object);
        if (!o) return;
        const Hit h = pickObject(ray, o->id, true);
        if (!h.object) {
            paint_.haveLast = false;
            return;
        }
        const glm::vec3 at = ray.origin + ray.dir * h.t;
        const float radius = brushPx_ * pixelSize(at);
        const float gap = glm::distance(at, paint_.last);
        if (paint_.haveLast && gap < radius * 8) {
            const int steps = std::max(1, int(std::ceil(gap / (radius * 0.3f))));
            for (int i = 1; i <= steps; ++i) dab3D(*o, glm::mix(paint_.last, at, float(i) / float(steps)), radius);
        } else
            dab3D(*o, at, radius);
        paint_.last = at, paint_.haveLast = true;
    }
    void pickColor(const Object& o, glm::vec2 uv) {
        const glm::vec4 c = sample(*o.picture, uv * glm::vec2(o.picture->w, o.picture->h));
        paintColor_ = glm::vec4(glm::vec3(c), paintColor_.a);
    }
    void fillAt(Object& o, glm::vec2 uv) {
        Image& img = changePicture(o);
        floodFill(img, glm::ivec2(uv * glm::vec2(img.w, img.h)), glm::vec4(glm::vec3(paintColor_), 1.0f), fillTolerance_);
        paint_.image.reset();
    }

    // ── Sculpting ──

    // The object ready to sculpt: its finishing touches made part of it, and made finer (the same
    // shape) if it has too few points to push about.
    Object* readyToSculpt(uint32_t id) {
        Object* o = find(id);
        if (!o) return nullptr;
        if (o->locked) return say("That object is locked"), nullptr;
        pushUndo();
        if (o->hasFinish()) {
            o->mesh = finishedMesh(*o);
            o->mirror = 0, o->round = 0, o->smoothLevels = 0;
        }
        handMade(*o);
        if (finerForSculpting(o->mesh)) {
            o->smooth = true;
            say(std::format("{} was made finer to sculpt: {} points now, the same shape", o->name, o->mesh.verts.size()));
        }
        touched(o->id);
        return o;
    }
    void endSculpt() {
        if (!sculpt_.active) return;
        sculpt_ = {};
    }
    // The brush at a point of the view: starts a stroke on what's there, or carries one on.
    void sculptAt(ImVec2 mouse, bool start, bool ctrl, bool shift) {
        const Ray ray = mouseRay(mouse);
        const auto toLocal = [](const Object& o, float radius) { return radius * 3.0f / glm::max(glm::abs(o.scale.x) + glm::abs(o.scale.y) + glm::abs(o.scale.z), 1e-6f); };
        const glm::vec3 other(1, -1, 1); // (the other side: the object's Y turned round)
        if (start) {
            const Hit h = pickObject(ray, 0, true);
            if (!h.object) return;
            selected_ = {h.object}, active_ = h.object;
            Object* o = readyToSculpt(h.object);
            if (!o) return;
            sculpt_.active = true, sculpt_.object = o->id;
            sculpt_.mesh = std::make_unique<SculptMesh>(o->mesh);
            if (sculptBrush_ == SculptBrush::Grab && !shift) { // (made finer: the same shape, so the same spot)
                sculpt_.start = ray.origin + ray.dir * h.t;
                const glm::vec3 at = glm::vec3(glm::inverse(objectMatrix(*o)) * glm::vec4(sculpt_.start, 1.0f));
                const float radius = toLocal(*o, sculptPx_ * pixelSize(sculpt_.start));
                for (int side = 0; side < (sculptMirror_ ? 2 : 1); ++side) {
                    sculpt_.held[side] = grabbed(o->mesh, side ? at * other : at, radius);
                    for (const auto& [i, w] : sculpt_.held[side]) sculpt_.from[side].push_back(o->mesh.verts[size_t(i)]);
                }
            }
        }
        if (!sculpt_.active) return;
        Object* o = find(sculpt_.object);
        if (!o) return endSculpt();
        const glm::mat4 inv = glm::inverse(objectMatrix(*o));
        if (!sculpt_.from[0].empty()) { // grabbing: it follows the mouse across the view
            const auto p = hitPlane(ray, sculpt_.start, forwardOf(yaw_, pitch_));
            if (!p) return;
            const glm::vec3 d = glm::vec3(inv * glm::vec4(*p - sculpt_.start, 0.0f));
            moveGrabbed(o->mesh, sculpt_.held[0], sculpt_.from[0], d);
            if (sculptMirror_) moveGrabbed(o->mesh, sculpt_.held[1], sculpt_.from[1], d * other);
            return touched(o->id);
        }
        const Hit h = pickObject(ray, o->id, true);
        if (!h.object) {
            sculpt_.haveLast = false;
            return;
        }
        const glm::vec3 at = ray.origin + ray.dir * h.t;
        const float radius = sculptPx_ * pixelSize(at);
        const SculptBrush brush = shift ? SculptBrush::Smooth : sculptBrush_;
        const auto dab = [&](const glm::vec3& world) {
            Dab d{glm::vec3(inv * glm::vec4(world, 1.0f)), toLocal(*o, radius), sculptStrength_, ctrl};
            sculptDab(o->mesh, *sculpt_.mesh, brush, d);
            if (sculptMirror_) d.at *= other, sculptDab(o->mesh, *sculpt_.mesh, brush, d);
        };
        // A dab every quarter of the brush along the way, not one a frame (fast strokes stay even).
        const float gap = glm::distance(at, sculpt_.last);
        if (!sculpt_.haveLast) dab(at);
        else if (gap >= radius * 0.25f && gap < radius * 8) {
            const int steps = int(gap / (radius * 0.25f));
            for (int i = 1; i <= steps; ++i) dab(glm::mix(sculpt_.last, at, float(i) / float(steps)));
        } else if (gap < radius * 0.25f)
            return; // (not far enough for the next dab yet)
        else
            dab(at);
        sculpt_.last = at, sculpt_.haveLast = true;
        touched(o->id);
    }
    void sculptViewport(const ImGuiIO& io) {
        if (sculpt_.waiting) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) sculpt_.waiting = false;
            else if (pickObject(mouseRay(io.MousePos), 0, true).object) sculpt_.waiting = false, sculptAt(io.MousePos, true, io.KeyCtrl, io.KeyShift);
            return;
        }
        if (sculpt_.active) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return endSculpt();
            return sculptAt(io.MousePos, false, io.KeyCtrl, io.KeyShift);
        }
        if (!mouseInView(io) || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
        if (pickObject(mouseRay(io.MousePos), 0, true).object) sculptAt(io.MousePos, true, io.KeyCtrl, io.KeyShift);
        else sculpt_.waiting = sculptBrush_ != SculptBrush::Grab;
    }

    // ── Picture layers ──

    PictureLayer* editLayer() {
        Object* o = find(ledit_.object);
        return o && ledit_.index >= 0 && ledit_.index < int(o->layers.size()) ? &o->layers[size_t(ledit_.index)] : nullptr;
    }
    // A picture put on the selected object as a new layer, unlocked to be moved: a sticker on the
    // surface in the middle of the view, or pinned onto the flat picture.
    void startFloating(Image img) {
        Object* o = selected_.empty() ? nullptr : readyToPaint(active_);
        if (!o) return say("Select the object to put the picture on first");
        if (img.empty()) return;
        lockLayer();
        pushUndo();
        PictureLayer l;
        l.name = std::format("Picture {}", o->layers.size() + 1);
        l.image = std::make_shared<Image>(std::move(img));
        l.sticker = pasteOnSurface_;
        const float aspect = float(l.image->h) / float(std::max(l.image->w, 1));
        if (l.sticker) {
            const glm::mat4 m = objectMatrix(*o), inv = glm::inverse(m);
            const Ray ray = mouseRay(ImVec2((viewMin_.x + viewMax_.x) * 0.5f, (viewMin_.y + viewMax_.y) * 0.5f));
            const Hit h = pickObject(ray, o->id, true);
            if (h.object) l.face = h.face, l.centre = glm::vec3(inv * glm::vec4(ray.origin + ray.dir * h.t, 1.0f));
            else { // (the face looking most at you)
                float best = -2;
                const glm::vec3 toEye = glm::normalize(glm::vec3(inv * glm::vec4(-viewDir(), 0.0f)));
                for (size_t f = 0; f < o->mesh.faces.size(); ++f)
                    if (const float d = glm::dot(faceNormal(o->mesh, int(f)), toEye); d > best) best = d, l.face = int(f), l.centre = faceCenter(o->mesh, int(f));
            }
            // Upright as you see it, a third as big as the object.
            const glm::vec3 f = viewDir(), right = glm::normalize(glm::cross(f, std::abs(f.z) > 0.99f ? glm::vec3(std::cos(glm::radians(yaw_)), std::sin(glm::radians(yaw_)), 0) : glm::vec3(0, 0, 1)));
            l.up = glm::normalize(glm::vec3(inv * glm::vec4(glm::cross(right, f), 0.0f)));
            const glm::vec3 d = boundsMax(o->mesh) - boundsMin(o->mesh);
            l.width = std::max({d.x, d.y, d.z}) * 0.35f, l.height = l.width * aspect;
        } else {
            const float W = float(o->picture->w), H = float(o->picture->h);
            const float k = std::min(W * 0.6f / float(l.image->w), H * 0.6f / float(l.image->h));
            const glm::vec2 half(float(l.image->w) * k * 0.5f, float(l.image->h) * k * 0.5f), c(W * 0.5f, H * 0.5f);
            l.corners = {c - half, glm::vec2(c.x + half.x, c.y - half.y), c + half, glm::vec2(c.x - half.x, c.y + half.y)};
        }
        o->layers.push_back(std::move(l));
        ledit_ = {};
        ledit_.object = o->id, ledit_.index = int(o->layers.size()) - 1, ledit_.fresh = true;
        paintTool_ = PaintTool::Picture;
        if (tab_ != Tab::Paint) setTab(Tab::Paint);
        say(pasteOnSurface_ ? "Drag it round the model (it bends over edges); corners size it (Shift: stretch), the round handle turns it. Enter locks it."
                            : "Drag its corners in the Picture panel to fit (Shift: keep its shape), inside to move it. Enter locks it.");
    }
    void floatFromFile(const std::string& path) {
        Image img;
        if (!loadImageFile(path, img)) return say("Couldn't load " + path);
        startFloating(std::move(img));
    }
    void pasteFromClipboard() {
        for (const char* mime : {"image/png", "image/bmp", "image/jpeg"}) {
            if (!SDL_HasClipboardData(mime)) continue;
            size_t n = 0;
            void* data = SDL_GetClipboardData(mime, &n);
            if (!data) continue;
            int w = 0, h = 0, c = 0;
            stbi_uc* px = stbi_load_from_memory(static_cast<const stbi_uc*>(data), int(n), &w, &h, &c, 4);
            SDL_free(data);
            if (!px) continue;
            Image img;
            img.w = w, img.h = h, img.px.assign(px, px + size_t(w) * size_t(h) * 4);
            stbi_image_free(px);
            return startFloating(std::move(img));
        }
        say("Nothing to paste: copy a picture in another app first (or use Add a picture from a file)");
    }
    // Enter: it stays where it is, a layer (click it in the Picture panel to move it again).
    void lockLayer() {
        if (!editLayer()) return void(ledit_ = {});
        ledit_ = {};
        say("Locked in place, as a layer (see Properties). Click it in the Picture panel to move it again.");
    }
    void unlockLayer(uint32_t object, int index) {
        lockLayer();
        ledit_.object = object, ledit_.index = index;
        paintTool_ = PaintTool::Picture;
        if (const PictureLayer* l = editLayer()) say(std::format("{} unlocked: {}. Enter locks it again.", l->name, l->sticker ? "drag it on the model" : "drag its corners in the Picture panel"));
    }
    // Esc: a picture just pasted goes again; one unlocked is locked again.
    void cancelFloating() {
        Object* o = find(ledit_.object);
        if (o && editLayer() && ledit_.fresh) {
            o->layers.erase(o->layers.begin() + ledit_.index);
            ledit_ = {};
            return say("Taken away");
        }
        lockLayer();
    }
    void deleteLayer(Object& o, int i) {
        if (i < 0 || i >= int(o.layers.size())) return;
        pushUndo();
        o.layers.erase(o.layers.begin() + i);
        ledit_ = {};
    }
    // The layer painted into the painting for good (it can't be moved after).
    void mergeLayer(Object& o, int i) {
        if (i < 0 || i >= int(o.layers.size()) || !o.picture) return;
        pushUndo();
        Object only = o;
        only.layers = {o.layers[size_t(i)]};
        o.picture = std::make_shared<Image>(composePicture(only));
        o.layers.erase(o.layers.begin() + i);
        ledit_ = {};
    }
    // The handles of a sticker being moved, in the world: its corners (top-left, top-right,
    // bottom-right, bottom-left), its turning handle, its middle — on its face's plane.
    bool stickerHandles(const Object& o, const PictureLayer& l, std::array<glm::vec3, 6>& out) {
        glm::vec3 right, up, n;
        if (!stickerFrame(o.mesh, l, right, up, n)) return false;
        const int f = stickerFace(o.mesh, l);
        const glm::vec3 c = l.centre - n * glm::dot(l.centre - faceCenter(o.mesh, f), n);
        const float hw = l.width * 0.5f, hh = l.height * 0.5f;
        const glm::mat4 m = objectMatrix(o);
        auto w = [&](const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); };
        out = {w(c - right * hw + up * hh), w(c + right * hw + up * hh), w(c + right * hw - up * hh), w(c - right * hw - up * hh), w(c + up * (hh + std::max(hw, hh) * 0.4f)), w(c)};
        return true;
    }
    int stickerHandle(const Object& o, const PictureLayer& l, ImVec2 mouse) {
        std::array<glm::vec3, 6> h;
        if (!stickerHandles(o, l, h)) return -1;
        ImVec2 s[6];
        for (int i = 0; i < 6; ++i)
            if (!project(h[size_t(i)], s[i])) return -1;
        for (int i = 0; i < 4; ++i)
            if (std::abs(mouse.x - s[i].x) < 10 && std::abs(mouse.y - s[i].y) < 10) return i;
        if (std::hypot(mouse.x - s[4].x, mouse.y - s[4].y) < 11) return 5;
        return insidePolygon(mouse, {s[0], s[1], s[2], s[3]}) ? 4 : -1;
    }
    // Moving a sticker on the model: it slides over the surface (onto other faces too), its
    // corners size it, its handle turns it.
    void stickerMouse(const ImGuiIO& io) {
        Object* o = find(ledit_.object);
        PictureLayer* l = editLayer();
        if (!o || !l) return;
        const glm::mat4 inv = glm::inverse(objectMatrix(*o));
        const Ray ray = mouseRay(io.MousePos);
        if (ledit_.drag >= 0) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return void(ledit_.drag = -1);
            const PictureLayer& s = ledit_.start;
            glm::vec3 right, up, n;
            stickerFrame(o->mesh, s, right, up, n);
            if (ledit_.drag == 4) {
                const Hit h = pickObject(ray, o->id, true);
                if (!h.object) return;
                const glm::vec3 p(inv * glm::vec4(ray.origin + ray.dir * h.t, 1.0f));
                l->face = h.face, l->up = s.up;
                glm::vec3 r2, u2, n2;
                stickerFrame(o->mesh, *l, r2, u2, n2);
                l->up = u2; // (its top carried over onto the new face)
                l->centre = p - r2 * ledit_.offset.x - u2 * ledit_.offset.y;
                settleSticker(o->mesh, *l);
                return;
            }
            const Ray local{glm::vec3(inv * glm::vec4(ray.origin, 1.0f)), glm::normalize(glm::vec3(inv * glm::vec4(ray.dir, 0.0f)))};
            const auto hp = hitPlane(local, s.centre, n);
            if (!hp) return;
            const glm::vec3 d = *hp - s.centre;
            const float dx = glm::dot(d, right), dy = glm::dot(d, up);
            if (ledit_.drag < 4) {
                if (io.KeyShift) l->width = std::max(2 * std::abs(dx), 0.002f), l->height = std::max(2 * std::abs(dy), 0.002f);
                else {
                    const float k = std::max(std::abs(dx) / (s.width * 0.5f), std::abs(dy) / (s.height * 0.5f));
                    l->width = std::max(s.width * k, 0.002f), l->height = std::max(s.height * k, 0.002f);
                }
                message_ = std::format("{:.0f} x {:.0f} mm", l->width * 1000, l->height * 1000), messageAt_ = now();
            } else {
                float a = std::atan2(dx, dy);
                if (snap_ != io.KeyCtrl) a = glm::radians(snapTo(glm::degrees(a), 15.0f));
                l->up = up * std::cos(a) + right * std::sin(a);
                message_ = std::format("Turned {:.0f} degrees", glm::degrees(a)), messageAt_ = now();
            }
            return;
        }
        if (!mouseInView(io) || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
        const int h = stickerHandle(*o, *l, io.MousePos);
        if (h < 0 && !pickObject(ray, o->id, true).object) return;
        pushUndo();
        ledit_.start = *l, ledit_.drag = h < 0 ? 4 : h, ledit_.offset = {0, 0};
        if (h == 4) { // (held where it was grabbed)
            glm::vec3 right, up, n;
            stickerFrame(o->mesh, *l, right, up, n);
            const Ray local{glm::vec3(inv * glm::vec4(ray.origin, 1.0f)), glm::normalize(glm::vec3(inv * glm::vec4(ray.dir, 0.0f)))};
            if (auto hp = hitPlane(local, l->centre, n)) ledit_.offset = {glm::dot(*hp - l->centre, right), glm::dot(*hp - l->centre, up)};
        }
    }
    // A cheap fingerprint of what the object's picture shows (its painting, its layers, its shape).
    uint64_t pictureStamp(const Object& o) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
        };
        const void* base = o.picture.get();
        mix(&base, sizeof base);
        const uint64_t version = versions_[o.id];
        mix(&version, sizeof version);
        for (const PictureLayer& l : o.layers) {
            const void* im = l.image.get();
            mix(&im, sizeof im), mix(&l.sticker, 1), mix(&l.visible, 1), mix(&l.opacity, 4), mix(l.corners.data(), 32);
            mix(&l.face, 4), mix(&l.centre, 12), mix(&l.up, 12), mix(&l.width, 4), mix(&l.height, 4);
        }
        return h;
    }
    // The object's picture on the graphics card (its painting with its layers on top): sent again
    // when it changes — while painting or dragging, at most 20 times a second.
    void uploadPicture(const Object& o, bool now_ = false) {
        if (!o.picture) return;
        Uploaded& u = uploaded_[o.id];
        const bool checker = tab_ == Tab::Layout && layChecker_;
        const uint64_t stamp = pictureStamp(o) ^ (checker ? 0x9E3779B97F4A7C15ull : 0);
        const bool first = !paintTex_.count(o.id);
        if (!first && u.stamp == stamp && !u.dirty) return;
        if (!first && !now_ && now() - u.at < 0.05) return;
        if (checker) { // (squares that show where the layout stretches or squashes the picture)
            static Image board;
            if (board.empty()) {
                board = makeImage(512, 512, {1, 1, 1, 1});
                for (int y = 0; y < 512; ++y)
                    for (int x = 0; x < 512; ++x) {
                        const bool dark = ((x / 32) + (y / 32)) % 2;
                        const glm::vec3 c = glm::mix(glm::vec3(0.95f, 0.55f, 0.25f), glm::vec3(0.3f, 0.55f, 0.9f), float(y) / 511.0f) * (dark ? 0.55f : 1.0f);
                        uint8_t* p = board.at(x, y);
                        p[0] = uint8_t(c.r * 255), p[1] = uint8_t(c.g * 255), p[2] = uint8_t(c.b * 255);
                    }
            }
            paintTex_[o.id] = r_->createTexture(std::format("fry:paint:{}", o.id), 512, 512, board.px.data());
        } else if (o.layers.empty()) {
            owner_.erase(o.id);
            paintTex_[o.id] = r_->createTexture(std::format("fry:paint:{}", o.id), uint32_t(o.picture->w), uint32_t(o.picture->h), o.picture->px.data());
        } else {
            const Image shown = composePicture(o, &owner_[o.id]);
            paintTex_[o.id] = r_->createTexture(std::format("fry:paint:{}", o.id), uint32_t(shown.w), uint32_t(shown.h), shown.px.data());
        }
        u = {stamp, now(), false};
    }

    // ───────────────────────────── the Layout tab ─────────────────────────────
    // The frames an object's faces are laid out in on its picture, moved, turned and sized by
    // hand (whole frames, or their corners); what's painted goes with them unless you say not.

    // What's picked, as (face, corner) pairs of its layout.
    std::vector<std::pair<int, int>> layoutPicked(const Object& o, const std::vector<std::vector<int>>& islands) const {
        std::vector<std::pair<int, int>> out;
        if (lay_.object != o.id) return out;
        if (lay_.corners) return {lay_.points.begin(), lay_.points.end()};
        for (int i : lay_.frames)
            if (i < int(islands.size()))
                for (int f : islands[size_t(i)])
                    for (size_t c = 0; c < o.mesh.faces[size_t(f)].size(); ++c) out.push_back({f, int(c)});
        return out;
    }
    // A corner and every other on the same point of the model at the same place on the picture.
    std::set<std::pair<int, int>> layoutCorner(const Object& o, int f, int c) const {
        std::set<std::pair<int, int>> out;
        const int v = o.mesh.faces[size_t(f)][size_t(c)];
        const glm::vec2 at = o.uvs[size_t(f)][size_t(c)];
        for (size_t g = 0; g < o.mesh.faces.size(); ++g)
            for (size_t k = 0; k < o.mesh.faces[g].size(); ++k)
                if (o.mesh.faces[g][k] == v && glm::distance(o.uvs[g][k], at) < 1e-5f) out.insert({int(g), int(k)});
        return out;
    }
    void layoutSelectAll() {
        Object* o = selected_.empty() ? nullptr : find(active_);
        if (!o || !o->uvsFit()) return;
        if (lay_.object != o->id) lay_ = {}, lay_.object = o->id;
        lay_.frames.clear(), lay_.points.clear();
        if (lay_.corners)
            for (size_t f = 0; f < o->uvs.size(); ++f)
                for (size_t c = 0; c < o->uvs[f].size(); ++c) lay_.points.insert({int(f), int(c)});
        else
            for (size_t i = 0; i < uvIslands(o->mesh, o->uvs).size(); ++i) lay_.frames.insert(int(i));
    }
    // The picked corners moved by `to` (picture pixels), painting carried along if wanted.
    void layoutChange(Object& o, const std::vector<std::vector<glm::vec2>>& from, const std::shared_ptr<const Image>& picture,
                      const std::vector<std::pair<int, int>>& picked, const std::function<glm::vec2(glm::vec2)>& to, bool final) {
        const glm::vec2 size(float(o.picture->w), float(o.picture->h));
        o.uvs = from;
        std::set<int> faces;
        for (auto [f, c] : picked) o.uvs[size_t(f)][size_t(c)] = to(from[size_t(f)][size_t(c)] * size) / size, faces.insert(f);
        touched(o.id);
        if (layKeepPaint_ && picture && (final || now() - lay_.remapAt > 0.06)) {
            Image img = *picture;
            remapPicture(img, o.mesh, from, o.uvs, {faces.begin(), faces.end()});
            o.picture = std::make_shared<Image>(std::move(img));
            lay_.remapAt = now();
        }
    }
    // Buttons' changes (turn, flip, straighten): about the middle of what's picked.
    void layoutApply(const std::function<glm::vec2(glm::vec2, glm::vec2)>& to) {
        Object* o = selected_.empty() ? nullptr : find(active_);
        if (!o || !o->picture || !o->uvsFit()) return;
        const auto picked = layoutPicked(*o, uvIslands(o->mesh, o->uvs));
        if (picked.empty()) return say("Pick some frames first (click them in the Picture panel or on the model)");
        pushUndo();
        const glm::vec2 size(float(o->picture->w), float(o->picture->h));
        glm::vec2 lo(1e30f), hi(-1e30f);
        for (auto [f, c] : picked) lo = glm::min(lo, o->uvs[size_t(f)][size_t(c)] * size), hi = glm::max(hi, o->uvs[size_t(f)][size_t(c)] * size);
        const glm::vec2 mid = (lo + hi) * 0.5f;
        layoutChange(*o, o->uvs, o->picture, picked, [&](glm::vec2 p) { return to(p, mid); }, true);
    }
    void layoutStraighten() {
        Object* o = selected_.empty() ? nullptr : find(active_);
        if (!o || !o->picture || !o->uvsFit()) return;
        const auto picked = layoutPicked(*o, uvIslands(o->mesh, o->uvs));
        std::set<int> faces;
        for (auto [f, c] : picked) faces.insert(f);
        float best = -1, angle = 0; // (its longest edge, laid level or upright)
        for (int f : faces)
            for (size_t i = 0; i < o->uvs[size_t(f)].size(); ++i) {
                const glm::vec2 d = (o->uvs[size_t(f)][(i + 1) % o->uvs[size_t(f)].size()] - o->uvs[size_t(f)][i]) * glm::vec2(o->picture->w, o->picture->h);
                if (const float l = glm::length(d); l > best) best = l, angle = std::atan2(d.y, d.x);
            }
        const float quarter = glm::half_pi<float>(), turn = std::round(angle / quarter) * quarter - angle;
        layoutApply([&](glm::vec2 p, glm::vec2 m) { return m + glm::vec2(std::cos(turn) * (p - m).x - std::sin(turn) * (p - m).y, std::sin(turn) * (p - m).x + std::cos(turn) * (p - m).y); });
    }
    // Laid out again by itself, what's painted carried over onto the new layout.
    void layoutAgain(Object& o) {
        pushUndo();
        const auto before = o.uvs;
        o.uvs = autoUnwrap(o.mesh);
        if (layKeepPaint_ && o.picture && before.size() == o.uvs.size()) {
            Image img = *o.picture;
            std::vector<int> all(o.mesh.faces.size());
            for (size_t f = 0; f < all.size(); ++f) all[f] = int(f);
            Image fresh = makeImage(img.w, img.h, glm::vec4(glm::vec3(o.color), 1.0f));
            fresh.px = img.px;
            remapPicture(fresh, o.mesh, before, o.uvs, all);
            o.picture = std::make_shared<Image>(std::move(fresh));
        }
        lay_.frames.clear(), lay_.points.clear();
        touched(o.id);
        say("Laid out again" + std::string(layKeepPaint_ ? ", what's painted carried over" : ""));
    }

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
    void layoutViewport(const ImGuiIO& io) {
        if (!mouseInView(io) || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
        const Hit h = pickObject(mouseRay(io.MousePos), 0, true);
        if (!h.object) {
            if (!io.KeyShift) lay_.frames.clear(), lay_.points.clear();
            return;
        }
        if (h.object != active_ || selected_.empty()) selected_ = {h.object}, active_ = h.object, lay_ = {}, lay_.object = h.object;
        Object* o = find(h.object);
        if (!o || !o->uvsFit()) return;
        const auto islands = uvIslands(o->mesh, o->uvs);
        for (size_t i = 0; i < islands.size(); ++i)
            if (std::find(islands[i].begin(), islands[i].end(), h.face) != islands[i].end()) {
                if (lay_.corners) lay_.corners = false, lay_.points.clear();
                if (!io.KeyShift) lay_.frames.clear();
                if (io.KeyShift && lay_.frames.count(int(i))) lay_.frames.erase(int(i));
                else lay_.frames.insert(int(i));
            }
    }
    void layoutOverlay(ImDrawList* dl) {
        Object* o = selected_.empty() ? nullptr : find(active_);
        if (!o || !o->uvsFit() || lay_.object != o->id) return;
        const auto picked = layoutPicked(*o, uvIslands(o->mesh, o->uvs));
        std::set<int> faces;
        for (auto [f, c] : picked) faces.insert(f);
        const glm::mat4 m = objectMatrix(*o);
        for (int f : faces) {
            std::vector<ImVec2> pts;
            bool ok = true;
            for (int v : o->mesh.faces[size_t(f)]) {
                ImVec2 q;
                ok &= project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(v)], 1.0f)), q);
                pts.push_back(q);
            }
            if (ok) dl->AddPolyline(pts.data(), int(pts.size()), kAccent, ImDrawFlags_Closed, 2.0f);
        }
    }
    void layoutToolbox(float w) {
        grid(int(std::size(kLayTools)), 38.0f, [&](int i) {
            const LayToolInfo& t = kLayTools[i];
            const std::string id = std::format("##{}", t.name), tip = std::format("{}  ({})\n{}", t.name, t.key, t.hint);
            if (iconButton(id.c_str(), t.icon, layTool_ == t.tool, tip.c_str())) layTool_ = t.tool;
        });
        ImGui::Spacing();
        ImGui::TextDisabled("Pick");
        if (wideButton("##frames", Icon::Faces, "Frames", !lay_.corners, w, "Whole frames: faces laid out together (Tab)")) lay_.corners = false, lay_.points.clear();
        if (wideButton("##corners", Icon::Points, "Corners", lay_.corners, w, "Corners of the frames, to bend them (Tab)")) lay_.corners = true, lay_.frames.clear();
    }
    void layoutOptions(float w) {
        for (const LayToolInfo& t : kLayTools)
            if (t.tool == layTool_) ImGui::SeparatorText(t.name);
        ImGui::TextDisabled("What's painted");
        int keep = layKeepPaint_ ? 0 : 1;
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("##keep", &keep, "Stays on the model (moves too)\0Stays on the picture\0")) layKeepPaint_ = keep == 0;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stays on the model: the model looks the same, frames just get more or less of the picture.\n"
                              "Stays on the picture: the model shows whatever the frame is moved over (to fit a frame onto something drawn).");
        ImGui::Checkbox("Show squares on the model", &layChecker_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Squares instead of the painting: where they're stretched or squashed, so is the picture");
        ImGui::SeparatorText("What's picked");
        const float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        auto turn = [&](float deg) {
            const float a = glm::radians(deg);
            layoutApply([a](glm::vec2 p, glm::vec2 m) { return m + glm::vec2(std::cos(a) * (p - m).x - std::sin(a) * (p - m).y, std::sin(a) * (p - m).x + std::cos(a) * (p - m).y); });
        };
        if (ImGui::Button("Turn left", ImVec2(half, 0))) turn(-90);
        ImGui::SameLine();
        if (ImGui::Button("Turn right", ImVec2(half, 0))) turn(90);
        if (ImGui::Button("Flip across", ImVec2(half, 0))) layoutApply([](glm::vec2 p, glm::vec2 m) { return glm::vec2(2 * m.x - p.x, p.y); });
        ImGui::SameLine();
        if (ImGui::Button("Flip up-down", ImVec2(half, 0))) layoutApply([](glm::vec2 p, glm::vec2 m) { return glm::vec2(p.x, 2 * m.y - p.y); });
        if (ImGui::Button("Bigger", ImVec2(half, 0))) layoutApply([](glm::vec2 p, glm::vec2 m) { return m + (p - m) * 1.25f; });
        ImGui::SameLine();
        if (ImGui::Button("Smaller", ImVec2(half, 0))) layoutApply([](glm::vec2 p, glm::vec2 m) { return m + (p - m) * 0.8f; });
        if (ImGui::Button("Straighten", ImVec2(w, 0))) layoutStraighten();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns it so its longest edge is level or upright");
    }
    void layoutProperties(float w) {
        Object* o = selected_.empty() ? nullptr : find(active_);
        if (!o) return void(ImGui::TextWrapped("Click an object: its picture's layout shows in the Picture panel."));
        ImGui::TextColored(accentText(), "%s", o->name.c_str());
        if (!o->picture || !o->uvsFit()) {
            if (ImGui::Button("Lay it out", ImVec2(w, 0))) readyToPaint(o->id);
            return;
        }
        const auto islands = uvIslands(o->mesh, o->uvs);
        ImGui::TextDisabled("%zu frames, %zu faces, picture %d x %d", islands.size(), o->mesh.faces.size(), o->picture->w, o->picture->h);
        if (ImGui::Button("Pick all (Ctrl+A)", ImVec2(w, 0))) layoutSelectAll();
        if (ImGui::Button("Lay it all out again", ImVec2(w, 0))) layoutAgain(*o);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fry3DX lays it out by itself again (what's painted carried over, if it stays on the model)");
        ImGui::Spacing();
        ImGui::TextWrapped("Each frame is a part of the model laid flat on its picture. Make a frame bigger to give that part more detail, "
                           "turn it to run a pattern the way you want, or move corners to bend it.");
    }

    // ───────────────────────────── the Print tab ─────────────────────────────

    glm::vec3 bedSize() const { // (metres)
        const int i = std::clamp(fry_printer.i(), 0, int(std::size(kPrinters)) - 1);
        const PrinterInfo& p = kPrinters[i];
        return p.x > 0 ? glm::vec3(p.x, p.y, p.z) * 0.001f : glm::vec3(fry_bed_x.f(), fry_bed_y.f(), fry_bed_z.f()) * 0.001f;
    }
    const PrintInfo& printInfo(const Object& o) {
        PrintInfo& p = printInfos_[o.id];
        uint64_t key = versions_[o.id] * 1000003ull + uint64_t(overhangDeg_ * 10);
        for (float f : {o.position.x, o.position.y, o.position.z, o.rotation.x, o.rotation.y, o.rotation.z, o.rotation.w, o.scale.x, o.scale.y, o.scale.z}) {
            uint32_t b;
            std::memcpy(&b, &f, 4);
            key = (key ^ b) * 1099511628211ull;
        }
        if (p.key == key) return p;
        p.key = key;
        p.world = o.hasFinish() ? finishedMesh(o) : o.mesh;
        transform(p.world, objectMatrix(o));
        p.lo = boundsMin(p.world), p.hi = boundsMax(p.world);
        p.check = checkForPrint(p.world, overhangDeg_, 0.0f);
        // Overhangs: those faces again a hair above the surface, to show red.
        keel::KmdlModel km;
        keel::KmdlMaterial mat{};
        mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = mat.baseColor[3] = 1.0f;
        km.materials.push_back(mat);
        for (size_t f = 0; f < p.world.faces.size(); ++f) {
            if (!overhangs(p.world, int(f), overhangDeg_, 0.0f)) continue;
            const glm::vec3 n = faceNormal(p.world, int(f));
            for (const auto& t : triangulate(p.world, int(f)))
                for (int k : t) {
                    const glm::vec3 q = p.world.verts[size_t(k)] + n * 0.0004f;
                    km.indices.push_back(uint32_t(km.vertices.size()));
                    km.vertices.push_back({{q.x, q.y, q.z}, {n.x, n.y, n.z}, {0, 0}, {0, 0}});
                }
        }
        p.hasOverhang = !km.indices.empty();
        if (p.hasOverhang) {
            km.submeshes.push_back({0, uint32_t(km.indices.size()), 0});
            p.overhang = r_->updateModel(std::format("fry:overhang:{}", o.id), km);
        }
        return p;
    }
    std::vector<Object*> printTargets() { // (what's picked, or everything shown)
        std::vector<Object*> out;
        for (Object& o : scene_.objects)
            if (o.visible && (selected_.empty() || selected_.count(o.id))) out.push_back(&o);
        return out;
    }
    void drawPrintBed(keel::Renderer& r, const keel::ModelLighting& light) {
        const glm::vec3 b = bedSize();
        const glm::vec3 lo(-b.x * 0.5f, -b.y * 0.5f, 0.0f), hi(b.x * 0.5f, b.y * 0.5f, b.z);
        const glm::vec4 plate(0.1f, 0.1f, 0.12f, 1.0f);
        if (bedFor_ != b) {
            bedFor_ = b;
            keel::KmdlModel km;
            keel::KmdlMaterial mat{};
            mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = mat.baseColor[3] = 1.0f;
            km.materials.push_back(mat);
            for (glm::vec2 c : {glm::vec2(lo.x, lo.y), glm::vec2(hi.x, lo.y), glm::vec2(hi.x, hi.y), glm::vec2(lo.x, hi.y)}) km.vertices.push_back({{c.x, c.y, -0.0006f}, {0, 0, 1}, {0, 0}, {0, 0}});
            km.indices = {0, 1, 2, 0, 2, 3};
            km.submeshes.push_back({0, 6, 0});
            bed_ = r.updateModel("fry:bed", km);
        }
        r.drawModel(bed_, glm::scale(glm::mat4(1.0f), glm::vec3(kUnits)), plate, nullptr, &light, false);
        // Its outline, and the space the printer reaches (fainter).
        const glm::vec3 c[8] = {{lo.x, lo.y, 0}, {hi.x, lo.y, 0}, {hi.x, hi.y, 0}, {lo.x, hi.y, 0}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
        const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        bool fits = true;
        for (Object* o : printTargets()) {
            const PrintInfo& p = printInfo(*o);
            fits &= glm::all(glm::greaterThanEqual(p.lo, lo - 1e-4f)) && glm::all(glm::lessThanEqual(p.hi, hi + 1e-4f));
        }
        const glm::vec4 col = fits ? glm::vec4(0.9f, 0.9f, 0.95f, 0.5f) : glm::vec4(1.0f, 0.3f, 0.2f, 0.9f);
        for (int i = 0; i < 12; ++i)
            r.drawStreak(c[e[i][0]] * kUnits, c[e[i][1]] * kUnits, pixelSize(target_) * (i < 4 ? 2.0f : 1.0f) * kUnits, i < 4 ? col : col * glm::vec4(1, 1, 1, 0.5f),
                         keel::kWhiteTexture);
        if (showOverhangs_) {
            const keel::ModelLighting flat{{glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f)}, 0.0f};
            for (const Object& o : scene_.objects)
                if (o.visible)
                    if (const PrintInfo& p = printInfo(o); p.hasOverhang)
                        r.drawModel(p.overhang, glm::scale(glm::mat4(1.0f), glm::vec3(kUnits)), glm::vec4(0.95f, 0.15f, 0.1f, 0.55f), nullptr, &flat, false);
        }
    }
    void putOnBed() {
        const auto targets = printTargets();
        if (targets.empty()) return;
        pushUndo();
        for (Object* o : targets) o->position.z -= printInfo(*o).lo.z, touched(o->id);
        say("Put on the bed");
    }
    void middleOfBed() {
        const auto targets = printTargets();
        if (targets.empty()) return;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (Object* o : targets) lo = glm::min(lo, printInfo(*o).lo), hi = glm::max(hi, printInfo(*o).hi);
        pushUndo();
        const glm::vec2 d = -(glm::vec2(lo) + glm::vec2(hi)) * 0.5f;
        for (Object* o : targets) o->position += glm::vec3(d, 0.0f), touched(o->id);
    }
    // Everything picked made `mm` big along `axis`, keeping its shape, standing where it stood.
    void printResize(int axis, float mm) {
        const auto targets = printTargets();
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (Object* o : targets) lo = glm::min(lo, printInfo(*o).lo), hi = glm::max(hi, printInfo(*o).hi);
        const float now_ = (hi - lo)[axis] * 1000.0f;
        if (targets.empty() || now_ < 1e-3f || mm <= 0.01f) return;
        const float k = mm / now_;
        const glm::vec3 pivot((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, lo.z);
        for (Object* o : targets) o->position = pivot + (o->position - pivot) * k, o->scale *= k, touched(o->id);
    }
    void printToolbox(float w) {
        ImGui::Spacing();
        if (wideButton("##measure", Icon::Ruler, "Measure", printTool_ == PrintTool::Measure, w, "Click two points to see how far apart they are (M)"))
            printTool_ = printTool_ == PrintTool::Measure ? PrintTool::None : PrintTool::Measure, measure_.clear();
        if (wideButton("##layflat", Icon::LayFlat, "Lay flat", printTool_ == PrintTool::LayFlat, w, "Click the face it should stand on (L)"))
            printTool_ = printTool_ == PrintTool::LayFlat ? PrintTool::None : PrintTool::LayFlat;
        if (wideButton("##bed", Icon::Move, "Put on the bed", false, w, "Down (or up) so it stands on the printer's plate (B)")) putOnBed();
        if (wideButton("##mid", Icon::Box, "Middle of the bed", false, w, "To the middle of the printer's plate")) middleOfBed();
        ImGui::Spacing();
        if (wideButton("##3mf", Icon::Print, "Save for the slicer", false, w, "3MF: opens in PrusaSlicer, Bambu Studio, OrcaSlicer, Cura")) dialog(8);
        if (wideButton("##stl", Icon::Print, "Save as STL", false, w, "STL, in millimetres")) dialog(9);
    }
    void printOptions(float w) {
        if (printTool_ == PrintTool::Measure) {
            ImGui::SeparatorText("Measure");
            ImGui::Checkbox("Catch corners", &measureSnap_);
            if (measure_.size() == 2) {
                const glm::vec3 d = (measure_[1] - measure_[0]) * 1000.0f;
                ImGui::Text("%.2f mm", glm::length(d));
                ImGui::TextDisabled("across %.2f   deep %.2f   up %.2f", std::abs(d.x), std::abs(d.y), std::abs(d.z));
            } else
                ImGui::TextWrapped("Click a point on the model, then another.");
            if (ImGui::Button("Clear", ImVec2(w, 0))) measure_.clear();
        } else {
            ImGui::SeparatorText("Lay flat");
            ImGui::TextWrapped("Click the face it should stand on: it turns so that face is down, and sits on the bed. A big flat face down prints best, with least support.");
        }
    }
    void printViewport(const ImGuiIO& io) {
        if (!mouseInView(io) || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
        if (printTool_ == PrintTool::Measure) {
            if (const auto p = measurePoint(io.MousePos)) {
                if (measure_.size() >= 2) measure_.clear();
                measure_.push_back(*p);
                if (measure_.size() == 2) say(std::format("{:.2f} mm", glm::distance(measure_[0], measure_[1]) * 1000.0f));
            }
            return;
        }
        const Hit h = pickObject(mouseRay(io.MousePos));
        Object* o = h.object ? find(h.object) : nullptr;
        if (!o) return;
        const Mesh& m = printInfo(*o).world;
        if (h.face < 0 || h.face >= int(m.faces.size())) return;
        pushUndo();
        const glm::vec3 n = faceNormal(m, h.face);
        o->rotation = glm::normalize(glm::rotation(n, glm::vec3(0, 0, -1)) * o->rotation);
        touched(o->id);
        o->position.z -= printInfo(*o).lo.z;
        touched(o->id);
        selected_ = {o->id}, active_ = o->id;
        say("Laid flat on that face");
    }
    // Where the mouse is on the model (a corner if near one), or on the bed.
    std::optional<glm::vec3> measurePoint(ImVec2 mouse) {
        const Ray ray = mouseRay(mouse);
        const Hit h = pickObject(ray);
        if (h.object) {
            glm::vec3 p = ray.origin + ray.dir * h.t;
            if (measureSnap_)
                if (const Object* o = find(h.object)) {
                    float best = 12.0f;
                    for (const glm::vec3& v : printInfo(*o).world.verts) {
                        ImVec2 q;
                        if (project(v, q))
                            if (const float d = std::hypot(q.x - mouse.x, q.y - mouse.y); d < best) best = d, p = v;
                    }
                }
            return p;
        }
        if (std::abs(ray.dir.z) > 1e-5f)
            if (const float t = -ray.origin.z / ray.dir.z; t > 0) return ray.origin + ray.dir * t;
        return std::nullopt;
    }
    void printOverlay(ImDrawList* dl, const ImGuiIO& io) {
        if (printTool_ != PrintTool::Measure) return;
        std::vector<glm::vec3> pts = measure_;
        if (pts.size() == 1 && mouseInView(io))
            if (auto p = measurePoint(io.MousePos)) pts.push_back(*p);
        ImVec2 q[2];
        for (size_t i = 0; i < pts.size(); ++i)
            if (project(pts[i], q[i])) dl->AddCircleFilled(q[i], 5, kAccent);
        if (pts.size() == 2 && project(pts[0], q[0]) && project(pts[1], q[1])) {
            dl->AddLine(q[0], q[1], kAccent, 2.0f);
            const std::string t = std::format("{:.2f} mm", glm::distance(pts[0], pts[1]) * 1000.0f);
            const ImVec2 mid((q[0].x + q[1].x) * 0.5f, (q[0].y + q[1].y) * 0.5f), ts = ImGui::CalcTextSize(t.c_str());
            dl->AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 6, mid.y - ts.y - 10), ImVec2(mid.x + ts.x * 0.5f + 6, mid.y - 4), IM_COL32(20, 20, 24, 230), 4.0f);
            dl->AddText(ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y - 7), IM_COL32(255, 255, 255, 255), t.c_str());
        }
    }
    void printProperties(float w) {
        auto good = [](const char* t) { ImGui::TextColored(roleColor(keel::ui::Role::SuccessText), "%s", t); };
        auto bad = [](const std::string& t) { ImGui::PushStyleColor(ImGuiCol_Text, roleColor(keel::ui::Role::DangerText)), ImGui::TextWrapped("%s", t.c_str()), ImGui::PopStyleColor(); };
        ImGui::SeparatorText("Printer");
        int printer = std::clamp(fry_printer.i(), 0, int(std::size(kPrinters)) - 1);
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##printer", kPrinters[printer].name)) {
            for (int i = 0; i < int(std::size(kPrinters)); ++i)
                if (ImGui::Selectable(kPrinters[i].name, i == printer)) keel::execute(std::format("fry_printer {}", i));
            ImGui::EndCombo();
        }
        const glm::vec3 bed = bedSize() * 1000.0f;
        if (kPrinters[printer].x <= 0) {
            float b[3] = {bed.x, bed.y, bed.z};
            ImGui::SetNextItemWidth(w);
            if (ImGui::DragFloat3("##bed", b, 1.0f, 50.0f, 2000.0f, "%.0f mm"))
                keel::execute(std::format("fry_bed_x {}; fry_bed_y {}; fry_bed_z {}", b[0], b[1], b[2]));
        } else
            ImGui::TextDisabled("Plate %.0f x %.0f mm, %.0f mm high", bed.x, bed.y, bed.z);

        const auto targets = printTargets();
        if (targets.empty()) return void(ImGui::TextWrapped("Nothing to print: add something in the Model tab."));
        glm::vec3 lo(1e30f), hi(-1e30f);
        PrintCheck all;
        for (Object* o : targets) {
            const PrintInfo& p = printInfo(*o);
            lo = glm::min(lo, p.lo), hi = glm::max(hi, p.hi);
            all.openEdges += p.check.openEdges, all.crowdedEdges += p.check.crowdedEdges, all.flippedEdges += p.check.flippedEdges;
            all.tinyFaces += p.check.tinyFaces, all.pieces += p.check.pieces, all.insideOut |= p.check.insideOut;
            all.volume += std::abs(p.check.volume), all.area += p.check.area, all.overhangArea += p.check.overhangArea;
        }
        const glm::vec3 size = (hi - lo) * 1000.0f;
        ImGui::SeparatorText(selected_.empty() ? "Everything" : targets.size() == 1 ? targets[0]->name.c_str() : "What's picked");
        // Its size, typed in millimetres (keeps its shape).
        const char* axes[] = {"across", "deep", "high"};
        for (int a = 0; a < 3; ++a) {
            float v = size[a];
            ImGui::SetNextItemWidth(w * 0.62f);
            ImGui::PushID(a);
            if (ImGui::DragFloat("##size", &v, 0.5f, 0.1f, 100000.0f, "%.2f mm")) {
                if (ImGui::IsItemActivated() || !sizing_) pushUndo(), sizing_ = true;
                printResize(a, v);
            }
            if (ImGui::IsItemDeactivated()) sizing_ = false;
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", axes[a]);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type a size: it keeps its shape");
        const glm::vec3 b = bedSize();
        const bool fits = size.x <= bed.x + 0.05f && size.y <= bed.y + 0.05f && size.z <= bed.z + 0.05f;
        const bool onPlate = lo.x >= -b.x * 0.5f - 1e-4f && hi.x <= b.x * 0.5f + 1e-4f && lo.y >= -b.y * 0.5f - 1e-4f && hi.y <= b.y * 0.5f + 1e-4f && hi.z <= b.z + 1e-4f;
        if (!fits) {
            const float k = std::min({bed.x / std::max(size.x, 1e-3f), bed.y / std::max(size.y, 1e-3f), bed.z / std::max(size.z, 1e-3f)});
            bad(std::format("Too big for this printer: it would fit at {:.0f}% of this size (or in parts).", std::floor(k * 1000) / 10));
        } else if (!onPlate) {
            bad("It fits, but it's off the plate.");
            if (ImGui::Button("Middle of the bed", ImVec2(w, 0))) middleOfBed();
        } else
            good("Fits on the plate");
        if (std::abs(lo.z) > 1e-4f) {
            bad(lo.z > 0 ? std::format("It floats {:.2f} mm above the bed.", lo.z * 1000) : std::format("It goes {:.2f} mm into the bed (that part won't print).", -lo.z * 1000));
            if (ImGui::Button("Put it on the bed (B)", ImVec2(w, 0))) putOnBed();
        }

        ImGui::SeparatorText("Ready to print?");
        if (all.openEdges) {
            bad(std::format("Not closed: {} edges have a face on one side only (holes). A slicer may fill them wrongly or not print it.", all.openEdges));
            if (ImGui::Button("Close the holes", ImVec2(w, 0))) {
                pushUndo();
                int n = 0;
                for (Object* o : targets) n += closeHoles(o->mesh), touched(o->id);
                say(std::format("Closed {} hole{}", n, n == 1 ? "" : "s"));
            }
        } else
            good("Closed all round");
        if (all.flippedEdges || all.insideOut) {
            bad("Some faces face inwards: the slicer can't tell inside from out.");
            if (ImGui::Button("Turn them the right way", ImVec2(w, 0))) {
                pushUndo();
                int n = 0;
                for (Object* o : targets) n += faceOutward(o->mesh), touched(o->id);
                say(std::format("Turned {} face{}", n, n == 1 ? "" : "s"));
            }
        } else
            good("Faces all face outwards");
        if (all.crowdedEdges) bad(std::format("{} edges have more than two faces on them (shapes stuck together there): separate or rebuild that part.", all.crowdedEdges));
        if (all.tinyFaces) bad(std::format("{} faces have no size: Merge by distance in the Model tab cleans them up.", all.tinyFaces));
        if (all.pieces > int(targets.size())) ImGui::TextDisabled("%d separate pieces", all.pieces);
        const float hang = all.overhangArea * 1e4f;
        if (hang > 0.01f) bad(std::format("{:.1f} cm2 hangs over the air (red): it needs supports, or turn it (Lay flat).", hang));
        else good("Nothing hangs over the air");
        ImGui::Checkbox("Show overhangs in red", &showOverhangs_);
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##hang", &overhangDeg_, 20.0f, 80.0f, "steeper than %.0f degrees");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far from upright a face can lean over before it needs support (45 for most printers)");

        ImGui::SeparatorText("Material");
        ImGui::SetNextItemWidth(w);
        ImGui::Combo("##material", &material_, [](void*, int i) { return kMaterials[i].name; }, nullptr, int(std::size(kMaterials)));
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##infill", &infill_, 0.0f, 100.0f, "infill %.0f%%");
        // (walls about 1.2 mm thick all round, the inside filled as much as the infill says)
        const float vol = all.volume * 1e6f, walls = std::min(vol, all.area * 1e4f * 0.12f);
        const float used = walls + (vol - walls) * infill_ / 100.0f, grams = used * kMaterials[material_].density;
        ImGui::Text("Holds %.1f cm3", vol);
        ImGui::Text("About %.0f g, %.1f m of 1.75 mm filament", grams, used / (3.14159f * 0.0875f * 0.0875f) / 100.0f);
        ImGui::TextDisabled("(a guess: the slicer knows best)");
    }

    // ───────────────────────────── layers: folders and going to things ─────────────────────────────

    Folder* findFolder(uint32_t id) {
        for (Folder& f : scene_.folders)
            if (f.id == id) return &f;
        return nullptr;
    }
    bool inFolder(uint32_t folder, uint32_t within) { // (folder is within, or inside it somewhere)
        for (int n = 0; folder && n < 256; ++n) {
            if (folder == within) return true;
            const Folder* f = findFolder(folder);
            folder = f ? f->parent : 0;
        }
        return false;
    }
    std::set<uint32_t> objectsIn(uint32_t folder) {
        std::set<uint32_t> out;
        for (const Object& o : scene_.objects)
            if (inFolder(o.folder, folder)) out.insert(o.id);
        return out;
    }
    // The camera brought round to these (as framing them, but keeping the way it looks).
    void goTo(const std::set<uint32_t>& ids) {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Object& o : scene_.objects)
            if (ids.count(o.id)) {
                const glm::mat4 m = objectMatrix(o);
                for (const glm::vec3& v : o.mesh.verts) {
                    const glm::vec3 w(m * glm::vec4(v, 1.0f));
                    lo = glm::min(lo, w), hi = glm::max(hi, w);
                }
            }
        if (lo.x > hi.x) return;
        frameBox(lo, hi);
    }
    // The picked objects into a new folder (Ctrl+G): a group, picked and moved together.
    void groupSelected() {
        if (mode_ == Mode::Edit) leaveEdit();
        if (selected_.empty()) return say("Pick the objects to put in a folder first");
        pushUndo();
        Folder f;
        f.id = scene_.nextId++;
        int n = 1;
        while (std::any_of(scene_.folders.begin(), scene_.folders.end(), [&](const Folder& g) { return g.name == std::format("Group {}", n); })) ++n;
        f.name = std::format("Group {}", n);
        if (const Object* a = find(active_)) f.parent = a->folder; // (where the last picked was)
        for (Object& o : scene_.objects)
            if (selected_.count(o.id)) o.folder = f.id;
        scene_.folders.push_back(f);
        renamingFolder_ = f.id, std::snprintf(renameBuf_, sizeof renameBuf_, "%s", f.name.c_str());
        say(f.name + ": click it to pick all of it; type a name");
    }
    // Ungrouped: what's inside goes up a level, the folder goes.
    void ungroup(uint32_t id) {
        const Folder* f = findFolder(id);
        if (!f) return;
        pushUndo();
        const uint32_t up = f->parent;
        for (Object& o : scene_.objects)
            if (o.folder == id) o.folder = up;
        for (Folder& g : scene_.folders)
            if (g.parent == id) g.parent = up;
        std::erase_if(scene_.folders, [&](const Folder& g) { return g.id == id; });
    }
    void deleteFolder(uint32_t id) {
        pushUndo();
        const auto ids = objectsIn(id);
        std::erase_if(scene_.objects, [&](const Object& o) { return ids.count(o.id) > 0; });
        std::erase_if(scene_.folders, [&](const Folder& g) { return inFolder(g.id, id); });
        for (uint32_t i : ids) selected_.erase(i);
    }
    // A folder's eye or lock: all in it the same.
    void showFolder(Folder& f, bool on) {
        pushUndo();
        for (Folder& g : scene_.folders)
            if (inFolder(g.id, f.id)) g.visible = on;
        for (Object& o : scene_.objects)
            if (inFolder(o.folder, f.id)) o.visible = on;
    }
    void lockFolder(Folder& f, bool on) {
        pushUndo();
        for (Folder& g : scene_.folders)
            if (inFolder(g.id, f.id)) g.locked = on;
        for (Object& o : scene_.objects)
            if (inFolder(o.folder, f.id)) o.locked = on;
    }
    void moveToFolder(const std::set<uint32_t>& ids, uint32_t folder) {
        pushUndo();
        for (Object& o : scene_.objects)
            if (ids.count(o.id)) o.folder = folder;
    }
    // Only these shown (again: everything shown).
    void showOnly(const std::set<uint32_t>& ids) {
        pushUndo();
        const bool already = std::all_of(scene_.objects.begin(), scene_.objects.end(), [&](const Object& o) { return o.visible == (ids.count(o.id) > 0); });
        for (Object& o : scene_.objects) o.visible = already || ids.count(o.id);
        for (Folder& f : scene_.folders) f.visible = true;
        say(already ? "Everything shown again" : "Only that shown (do it again to show everything)");
    }
    void folderMenu(uint32_t into, const std::set<uint32_t>& ids, int depth = 0) { // (Move to folder: the folders inside `into`)
        for (const Folder& f : scene_.folders)
            if (f.parent == into) {
                const std::string label = std::string(size_t(depth) * 3, ' ') + f.name;
                if (ImGui::MenuItem(label.c_str())) moveToFolder(ids, f.id);
                folderMenu(f.id, ids, depth + 1);
            }
    }
    // What a right-click on an object (or the picked ones) offers.
    void objectMenu(Object& o) {
        const std::set<uint32_t> ids = selected_.count(o.id) ? selected_ : std::set<uint32_t>{o.id};
        if (ImGui::MenuItem("Go to it", "F")) goTo(ids);
        if (ImGui::MenuItem("Rename")) renaming_ = o.id, std::snprintf(renameBuf_, sizeof renameBuf_, "%s", o.name.c_str());
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) selected_ = ids, active_ = o.id, duplicateSelected();
        if (ImGui::MenuItem("Show only this (again: all)")) showOnly(ids);
        if (ImGui::BeginMenu("Move to folder")) {
            if (ImGui::MenuItem("(the top, no folder)")) moveToFolder(ids, 0);
            folderMenu(0, ids);
            ImGui::Separator();
            if (ImGui::MenuItem("A new folder", "Ctrl+G")) selected_ = ids, active_ = o.id, groupSelected();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) selected_ = ids, deleteSelected();
    }
    // A row's show / hide or lock button: the picture of how it is now, dim when it's the usual way.
    bool rowToggle(const char* id, Icon icon, bool usual, const char* tip) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight() - 4;
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
        const ImU32 col = ImGui::IsItemHovered() ? kHot : usual ? IM_COL32(200, 200, 205, 150) : ImGui::GetColorU32(accentText());
        drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(at.x + h * 0.1f, at.y + h * 0.1f), h * 0.8f, col);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return pressed;
    }
    // The ⌖ button at the end of a row: brings the camera to it.
    bool goButton(const char* tip) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight() - 4;
        const bool pressed = ImGui::InvisibleButton("##go", ImVec2(h, h));
        const ImU32 col = ImGui::IsItemHovered() ? kHot : IM_COL32(200, 200, 205, 200);
        const ImVec2 c(at.x + h * 0.5f, at.y + h * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircle(c, h * 0.3f, col, 16, 1.5f);
        dl->AddLine(ImVec2(c.x - h * 0.48f, c.y), ImVec2(c.x - h * 0.16f, c.y), col, 1.5f), dl->AddLine(ImVec2(c.x + h * 0.16f, c.y), ImVec2(c.x + h * 0.48f, c.y), col, 1.5f);
        dl->AddLine(ImVec2(c.x, c.y - h * 0.48f), ImVec2(c.x, c.y - h * 0.16f), col, 1.5f), dl->AddLine(ImVec2(c.x, c.y + h * 0.16f), ImVec2(c.x, c.y + h * 0.48f), col, 1.5f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return pressed;
    }
    // Rows for what's in a folder (0: the top): its folders, then its objects.
    void layerRows(uint32_t parent, int depth) {
        const float indent = float(depth) * 14.0f;
        // (copies of the ids: rows may change the lists)
        std::vector<uint32_t> folders, objects;
        for (const Folder& f : scene_.folders)
            if (f.parent == parent) folders.push_back(f.id);
        for (const Object& o : scene_.objects)
            if (o.folder == parent) objects.push_back(o.id);
        for (uint32_t id : folders) {
            Folder* f = findFolder(id);
            if (!f) continue;
            const auto inside = objectsIn(id);
            ImGui::PushID(int(id) + 1000000);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (rowToggle("##eye", f->visible ? Icon::Eye : Icon::EyeShut, f->visible, f->visible ? "Hide all in it" : "Show all in it")) showFolder(*f, !f->visible);
            ImGui::TableNextColumn();
            if (rowToggle("##lock", f->locked ? Icon::Lock : Icon::Unlock, !f->locked, f->locked ? "Unlock all in it" : "Lock all in it")) lockFolder(*f, !f->locked);
            ImGui::TableNextColumn();
            ImGui::Indent(indent + 1);
            if (ImGui::ArrowButton("##open", f->open ? ImGuiDir_Down : ImGuiDir_Right)) f->open = !f->open;
            ImGui::SameLine(0, 4);
            const bool picked = !inside.empty() && std::all_of(inside.begin(), inside.end(), [&](uint32_t i) { return selected_.count(i) > 0; });
            if (renamingFolder_ == id) {
                ImGui::SetNextItemWidth(-1);
                if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    if (renameBuf_[0]) f->name = renameBuf_;
                    renamingFolder_ = 0, dirty_ = true;
                }
                if (ImGui::IsItemDeactivated()) renamingFolder_ = 0;
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, accentText());
                const std::string label = std::format("{}  ({})", f->name, inside.size());
                if (ImGui::Selectable(label.c_str(), picked, ImGuiSelectableFlags_AllowDoubleClick)) { // a folder is a group: all of it picked
                    if (mode_ == Mode::Edit) leaveEdit();
                    if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) selected_.insert(inside.begin(), inside.end());
                    else selected_ = inside;
                    if (!inside.empty()) active_ = *inside.rbegin();
                    activeRef_ = 0;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) renamingFolder_ = id, std::snprintf(renameBuf_, sizeof renameBuf_, "%s", f->name.c_str());
                }
                ImGui::PopStyleColor();
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("FRY_FOLDER", &id, sizeof id);
                    ImGui::Text("%s", f->name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FRY_OBJECT")) {
                        const uint32_t oid = *static_cast<const uint32_t*>(p->Data);
                        moveToFolder(selected_.count(oid) ? selected_ : std::set<uint32_t>{oid}, id);
                    }
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FRY_FOLDER")) {
                        const uint32_t fid = *static_cast<const uint32_t*>(p->Data);
                        if (Folder* g = findFolder(fid); g && !inFolder(id, fid)) pushUndo(), g->parent = id; // (not into itself)
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) ImGui::SetTooltip("Click: pick all of it (a group). Double-click: rename. Drag things onto it.");
                if (ImGui::BeginPopupContextItem("folder")) {
                    if (ImGui::MenuItem("Go to it")) goTo(inside);
                    if (ImGui::MenuItem("Pick all of it")) selected_ = inside;
                    if (ImGui::MenuItem("Rename")) renamingFolder_ = id, std::snprintf(renameBuf_, sizeof renameBuf_, "%s", f->name.c_str());
                    if (ImGui::MenuItem("Show only this (again: all)")) showOnly(inside);
                    if (ImGui::MenuItem("A folder inside it")) {
                        pushUndo();
                        Folder g;
                        g.id = scene_.nextId++, g.name = "Folder", g.parent = id;
                        scene_.folders.push_back(g);
                        f = findFolder(id), f->open = true;
                        renamingFolder_ = g.id, std::snprintf(renameBuf_, sizeof renameBuf_, "%s", g.name.c_str());
                    }
                    if (ImGui::MenuItem("Take it to the top") && f->parent) pushUndo(), f->parent = 0;
                    ImGui::Separator();
                    if (ImGui::MenuItem("Ungroup (keep what's in it)")) ungroup(id);
                    if (ImGui::MenuItem("Delete it and all in it")) deleteFolder(id);
                    ImGui::EndPopup();
                }
            }
            ImGui::Unindent(indent + 1);
            ImGui::TableNextColumn();
            if (goButton("Go to it: the camera comes round to all of it")) goTo(inside);
            ImGui::PopID();
            if ((f = findFolder(id)) && f->open) layerRows(id, depth + 1);
        }
        for (uint32_t id : objects) {
            Object* op = find(id);
            if (!op) continue;
            Object& o = *op;
            ImGui::PushID(int(o.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (rowToggle("##eye", o.visible ? Icon::Eye : Icon::EyeShut, o.visible, o.visible ? "Shown (click to hide)" : "Hidden (click to show)")) o.visible = !o.visible, dirty_ = true;
            ImGui::TableNextColumn();
            if (rowToggle("##lock", o.locked ? Icon::Lock : Icon::Unlock, !o.locked, o.locked ? "Locked: can't be moved or changed (click to unlock)" : "Click to lock")) o.locked = !o.locked, dirty_ = true;
            ImGui::TableNextColumn();
            ImGui::Indent(indent + 1);
            if (renaming_ == o.id) {
                ImGui::SetNextItemWidth(-1);
                if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    pushUndo();
                    if (renameBuf_[0]) o.name = renameBuf_;
                    renaming_ = 0;
                }
                if (ImGui::IsItemDeactivated()) renaming_ = 0;
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, o.visible ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                if (ImGui::Selectable(o.name.c_str(), selected_.count(o.id) > 0, ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (mode_ == Mode::Edit && o.id != active_) leaveEdit();
                    if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) selected_.count(o.id) ? (void)selected_.erase(o.id) : (void)selected_.insert(o.id);
                    else selected_ = {o.id};
                    active_ = o.id, activeRef_ = 0;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) renaming_ = o.id, std::snprintf(renameBuf_, sizeof renameBuf_, "%s", o.name.c_str());
                }
                ImGui::PopStyleColor();
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("FRY_OBJECT", &id, sizeof id);
                    ImGui::Text("%s", selected_.count(id) && selected_.size() > 1 ? std::format("{} objects", selected_.size()).c_str() : o.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) ImGui::SetTooltip("Double-click to rename; drag onto a folder; right-click for more");
                if (ImGui::BeginPopupContextItem("object")) {
                    objectMenu(o);
                    ImGui::EndPopup();
                }
            }
            ImGui::Unindent(indent + 1);
            ImGui::TableNextColumn();
            if (goButton("Go to it: the camera comes round to it")) goTo({id});
            ImGui::PopID();
        }
    }

    // ───────────────────────────── combining, copies, thickness ─────────────────────────────

    // The last picked object with the others added, taken out of it, or only where they overlap
    // kept; the others go.
    void combineSelected(Combine op) {
        if (mode_ == Mode::Edit) leaveEdit();
        Object* a = find(active_);
        if (!a || selected_.size() < 2) return say("Pick two or more objects: the last picked is kept, the others are added to it / taken out of it");
        pushUndo();
        Mesh result = a->hasFinish() ? finishedMesh(*a) : a->mesh;
        const glm::mat4 into = glm::inverse(objectMatrix(*a));
        int used = 0;
        for (const Object& b : scene_.objects) {
            if (b.id == a->id || !selected_.count(b.id)) continue;
            Mesh m = b.hasFinish() ? finishedMesh(b) : b.mesh;
            transform(m, into * objectMatrix(b));
            if (!combine(result, m, op)) {
                takeBack();
                return say(op == Combine::Overlap ? "They don't overlap: nothing would be left" : "That didn't leave anything");
            }
            ++used;
        }
        a = find(active_);
        a->mesh = std::move(result);
        a->mirror = 0, a->round = 0, a->smoothLevels = 0;
        const std::set<uint32_t> others = [&] {
            std::set<uint32_t> s = selected_;
            s.erase(a->id);
            return s;
        }();
        changedShape(*a);
        std::erase_if(scene_.objects, [&](const Object& o) { return others.count(o.id) > 0; });
        selected_ = {active_};
        static const char* done[] = {"added to it", "taken out of it", "overlapping kept"};
        say(std::format("{}: {} other{} {} ({} faces)", find(active_)->name, used, used == 1 ? "" : "s", done[int(op)], find(active_)->mesh.faces.size()));
    }
    void makeCopies() {
        if (mode_ == Mode::Edit) leaveEdit();
        if (selected_.empty()) return say("Pick what to copy first");
        pushUndo();
        const CopiesSetting& c = copies_;
        std::set<uint32_t> made = selected_;
        const std::vector<Object> originals = [&] {
            std::vector<Object> v;
            for (const Object& o : scene_.objects)
                if (selected_.count(o.id)) v.push_back(o);
            return v;
        }();
        const glm::vec3 axis = c.axis == 0 ? glm::vec3(1, 0, 0) : c.axis == 1 ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
        for (const Object& o : originals) {
            Object* joinInto = c.joined ? find(o.id) : nullptr;
            for (int k = 1; k <= c.count; ++k) {
                glm::mat4 T(1.0f);
                if (c.round) {
                    const float a = glm::radians(c.angle) * float(k) / float(c.angle >= 359.99f ? c.count + 1 : c.count);
                    T = glm::translate(glm::mat4(1.0f), c.centre) * glm::rotate(glm::mat4(1.0f), a, axis) * glm::translate(glm::mat4(1.0f), -c.centre);
                } else
                    T = glm::translate(glm::mat4(1.0f), c.step * float(k));
                if (joinInto) { // (into the original's own shape)
                    Mesh m = o.mesh;
                    transform(m, glm::inverse(objectMatrix(o)) * T * objectMatrix(o));
                    append(joinInto->mesh, m);
                    continue;
                }
                Object& n = addObject(o.name, o.mesh);
                const uint32_t id = n.id;
                const std::string name = n.name;
                n = o, n.id = id, n.name = name, n.locked = false;
                n.position = glm::vec3(T * glm::vec4(o.position, 1.0f));
                if (c.round) n.rotation = glm::normalize(glm::quat_cast(glm::mat3(T)) * o.rotation);
                made.insert(id);
            }
            if (joinInto) changedShape(*joinInto);
        }
        selected_ = made;
        touchAll();
        say(std::format("{} cop{} of each made{}", c.count, c.count == 1 ? "y" : "ies", c.joined ? ", joined into it" : ""));
    }
    void copiesWindow() {
        if (!showCopies_) return;
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Make copies", &showCopies_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) return ImGui::End();
        CopiesSetting& c = copies_;
        const float w = 280;
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##count", &c.count, 1, 100, "%d copies");
        int kind = c.round ? 1 : 0;
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("##kind", &kind, "In a row\0Round in a circle\0")) c.round = kind == 1;
        if (!c.round) {
            glm::vec3 mm = c.step * 1000.0f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::DragFloat3("##step", &mm.x, 1.0f, -1e6f, 1e6f, "%.1f mm")) c.step = mm / 1000.0f;
            ImGui::TextDisabled("each one this far on from the last (X Y Z)");
        } else {
            ImGui::SetNextItemWidth(w);
            ImGui::Combo("##axis", &c.axis, "Round the X axis\0Round the Y axis\0Round the Z axis (up)\0");
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##angle", &c.angle, 1.0f, 360.0f, "over %.0f degrees");
            glm::vec3 mm = c.centre * 1000.0f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::DragFloat3("##centre", &mm.x, 1.0f, -1e6f, 1e6f, "%.1f mm")) c.centre = mm / 1000.0f;
            ImGui::TextDisabled("round this point (X Y Z)");
            if (ImGui::Button("Round the picked one's middle", ImVec2(w, 0)))
                if (const Object* o = find(active_)) c.centre = o->position;
        }
        ImGui::Checkbox("Joined into one object", &c.joined);
        ImGui::Spacing();
        pushAccent();
        if (ImGui::Button("Make them", ImVec2(w, 0))) makeCopies(), showCopies_ = false;
        popAccent();
        ImGui::End();
    }
    void thickenActive() {
        Object* o = mode_ == Mode::Edit ? editing() : find(active_);
        if (!o || selected_.empty()) return say("Pick the object to make thick first");
        pushUndo();
        if (o->hasFinish()) o->mesh = finishedMesh(*o), o->mirror = 0, o->round = 0, o->smoothLevels = 0;
        const float t = thickness_ / objectScale(*o);
        thicken(o->mesh, thicknessOut_ ? -t : t);
        changedShape(*o);
        say(std::format("{} made {:.1f} mm thick{}", o->name, thickness_ * 1000, checkForPrint(o->mesh).closed() ? " (closed all round)" : ""));
    }
    void thickenWindow() {
        if (!showThicken_) return;
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Make it thick", &showThicken_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) return ImGui::End();
        const float w = 260;
        float mm = thickness_ * 1000;
        ImGui::SetNextItemWidth(w);
        if (ImGui::DragFloat("##t", &mm, 0.1f, 0.1f, 1e5f, "%.2f mm thick")) thickness_ = mm / 1000;
        int out = thicknessOut_ ? 1 : 0;
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("##dir", &out, "Inwards (a closed shape: hollow)\0Outwards\0")) thicknessOut_ = out == 1;
        ImGui::TextWrapped("A flat or open surface gets a back and edges: a solid you can print. A closed shape becomes hollow, walls this thick (to save material or make a box).");
        pushAccent();
        if (ImGui::Button("Make it thick", ImVec2(w, 0))) thickenActive(), showThicken_ = false;
        popAccent();
        ImGui::End();
    }
    void cleanFlatFaces() {
        Object* o = mode_ == Mode::Edit ? editing() : find(active_);
        if (!o || selected_.empty()) return say("Pick an object first");
        pushUndo();
        const int n = mergeFlatFaces(o->mesh);
        changedShape(*o);
        say(n ? std::format("{} faces joined into the flat ones beside them", n) : "Nothing to join: no flat faces side by side");
    }

    // ───────────────────────────── touch: phones and tablets ─────────────────────────────
    // A phone: everything drawn bigger, the side panels only when asked for, a bar of big buttons
    // at the bottom (undo, Shift and Ctrl that stay down, editing the shape...), and fingers:
    // one drags the view round (or a tool's arrows), two move it and pinch to zoom, holding one
    // still is a right-click.

    bool touch() const { return touchMode_; }
    // How much bigger than its pixels the UI is drawn.
    float touchScale() const {
        if (fry_uiscale.f() > 0) return fry_uiscale.f();
        if (!touch()) return 1.0f;
        const float density = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        return std::clamp((density > 0 ? density : 2.0f) * 0.9f, 1.0f, 4.0f) * std::clamp(fry_uisize.f(), 0.5f, 2.0f);
    }
    void touchFrame(ImGuiIO& io) {
        if (shiftLatch_) io.KeyShift = true;
        if (ctrlLatch_) io.KeyCtrl = true;
        ImGui::GetStyle().FontScaleMain = 1.0f; // (the sizes come from touchScale)
        if (gest_.rightUp) { // (the long press's right-click: let go the frame after)
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
            gest_.rightUp = false;
        }
        // The fingers on the screen now (where, in the UI's units).
        std::vector<ImVec2> fingers;
        int devices = 0;
        if (SDL_TouchID* ids = SDL_GetTouchDevices(&devices)) {
            for (int d = 0; d < devices; ++d) {
                int n = 0;
                if (SDL_Finger** f = SDL_GetTouchFingers(ids[d], &n)) {
                    for (int i = 0; i < n; ++i) fingers.push_back(ImVec2(f[i]->x * io.DisplaySize.x, f[i]->y * io.DisplaySize.y));
                    SDL_free(f);
                }
            }
            SDL_free(ids);
        }
        auto inView = [&](ImVec2 p) { return p.x >= viewMin_.x && p.y >= viewMin_.y && p.x < viewMax_.x && p.y < viewMax_.y; };
        if (fingers.size() >= 2) {
            const ImVec2 mid((fingers[0].x + fingers[1].x) * 0.5f, (fingers[0].y + fingers[1].y) * 0.5f);
            const float spread = std::max(std::hypot(fingers[0].x - fingers[1].x, fingers[0].y - fingers[1].y), 1.0f);
            if (!gest_.two && !drag_.active && inView(mid) && !ImGui::IsAnyItemActive()) { // two fingers: the first's tap or drag is off
                gest_.two = true, gest_.mid = mid, gest_.spread = spread;
                boxing_ = lassoing_ = touchOrbit_ = tapDraw_ = false;
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            }
            if (gest_.two) {
                panBy(ImVec2(mid.x - gest_.mid.x, mid.y - gest_.mid.y));
                dist_ = std::clamp(dist_ * gest_.spread / spread, 0.01f, 5000.0f);
                gest_.mid = mid, gest_.spread = spread;
            }
            gest_.downAt = -1;
            return;
        }
        if (fingers.empty()) gest_.two = false;
        if (fingers.size() != 1 || gest_.two) return void(gest_.downAt = -1);
        // One finger held still: a right-click there (menus in the panels, orbit in the view).
        const ImVec2 p = fingers[0];
        if (gest_.downAt < 0) gest_.downAt = now(), gest_.downPos = p, gest_.longDone = false;
        if (std::hypot(p.x - gest_.downPos.x, p.y - gest_.downPos.y) > 12) gest_.downAt = now() + 1e9; // (moved: not a long press)
        if (!gest_.longDone && now() - gest_.downAt > 0.55 && !drag_.active) {
            gest_.longDone = true;
            boxing_ = lassoing_ = touchOrbit_ = tapDraw_ = false;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
            gest_.rightUp = true;
        }
    }
    // The bar along the bottom of the view: big buttons, in rows if the view's narrow.
    void touchBar() {
        const float maxW = std::max(viewMax_.x - viewMin_.x - 16, 120.0f);
        ImGui::SetNextWindowPos(ImVec2((viewMin_.x + viewMax_.x) * 0.5f, viewMax_.y - 6), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##touchbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        if (now() - messageAt_ < 4.0) ImGui::PushTextWrapPos(maxW), ImGui::TextColored(accentText(), "%s", message_.c_str()), ImGui::PopTextWrapPos();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
        const ImGuiStyle& st = ImGui::GetStyle();
        float x = 0; // (where the row's got to)
        auto button = [&](const char* label, bool on = false, const char* tip = nullptr) {
            const float w = ImGui::CalcTextSize(label).x + st.FramePadding.x * 2;
            if (x > 0 && x + st.ItemSpacing.x + w <= maxW) ImGui::SameLine(), x += st.ItemSpacing.x;
            else x = 0;
            x += w;
            if (on) pushAccent();
            const bool pressed = ImGui::Button(label);
            if (on) popAccent();
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return pressed;
        };
        if (button("Undo")) undo();
        if (button("Redo")) redo();
        if (button("Shift", shiftLatch_, "Held down until tapped again: adds to the selection, picks loops, scales both sides")) shiftLatch_ = !shiftLatch_;
        if (button("Ctrl", ctrlLatch_, "Held down until tapped again: takes from the selection, snaps")) ctrlLatch_ = !ctrlLatch_;
        if (tab_ == Tab::Model) {
            if (button(mode_ == Mode::Edit ? "Done" : "Edit shape", mode_ == Mode::Edit)) mode_ == Mode::Edit ? leaveEdit() : enterEdit();
            if (button("Delete")) deleteSelected();
        }
        if (button("Frame")) frameSelected();
        if (button("Views", viewsOpen_)) viewsOpen_ = !viewsOpen_;
        if (button("Panels", panelsOpen_, "Layers, Properties (and the Picture when painting)")) panelsOpen_ = !panelsOpen_;
        ImGui::PopStyleVar();
        ImGui::End();
    }

    // ───────────────────────────── the camera ─────────────────────────────

    glm::vec3 viewDir() const { return forwardOf(yaw_, pitch_); }
    float viewHeight() const { return 2.0f * dist_ * std::tan(glm::radians(fov_) * 0.5f); } // metres across the view's height at the target
    void setView(View v) {
        const float size = viewHeight();
        view_ = v;
        switch (v) {
        case View::Front: yaw_ = 180, pitch_ = 0; break; // from +X, looking back at the front
        case View::Back: yaw_ = 0, pitch_ = 0; break;
        case View::Left: yaw_ = -90, pitch_ = 0; break; // from +Y (the model's left side)
        case View::Right: yaw_ = 90, pitch_ = 0; break;
        case View::Top: yaw_ = 0, pitch_ = 89.9f; break; // front up the screen
        case View::Bottom: yaw_ = 0, pitch_ = -89.9f; break;
        default: break;
        }
        // Side views: nearly flat (a narrow lens from far away), the same size on screen.
        fov_ = v == View::Perspective ? 50.0f : 2.0f;
        dist_ = size / (2.0f * std::tan(glm::radians(fov_) * 0.5f));
    }
    void leaveFlatView() {
        if (view_ == View::Perspective) return;
        const float size = viewHeight();
        view_ = View::Perspective, fov_ = 50.0f;
        dist_ = size / (2.0f * std::tan(glm::radians(fov_) * 0.5f));
    }
    void frameBox(glm::vec3 lo, glm::vec3 hi) {
        if (lo.x > hi.x) return;
        target_ = (lo + hi) * 0.5f;
        const float radius = std::max(glm::length(hi - lo) * 0.5f, 0.05f);
        dist_ = radius * 1.4f / std::tan(glm::radians(fov_) * 0.5f);
    }
    void frameAll() {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Object& o : scene_.objects) {
            if (!o.visible) continue;
            const glm::mat4 m = objectMatrix(o);
            for (const glm::vec3& v : o.mesh.verts) {
                const glm::vec3 w(m * glm::vec4(v, 1.0f));
                lo = glm::min(lo, w), hi = glm::max(hi, w);
            }
        }
        frameBox(lo, hi);
    }
    void frameSelected() {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const glm::vec3& w : selectionPoints()) lo = glm::min(lo, w), hi = glm::max(hi, w);
        if (lo.x > hi.x) return frameAll();
        frameBox(lo, hi);
    }

    bool mouseInView(const ImGuiIO& io) const {
        const ImVec2 m = io.MousePos;
        return m.x >= viewMin_.x && m.y >= viewMin_.y && m.x < viewMax_.x && m.y < viewMax_.y && !io.WantCaptureMouse;
    }

    void orbitBy(ImVec2 d) {
        if (std::abs(d.x) + std::abs(d.y) > 0.5f) leaveFlatView();
        yaw_ -= d.x * 0.3f;
        pitch_ = std::clamp(pitch_ + d.y * 0.3f, -89.9f, 89.9f);
    }
    void panBy(ImVec2 d) {
        const glm::vec3 f = viewDir(), right = glm::normalize(glm::cross(f, std::abs(f.z) > 0.99f ? glm::vec3(std::cos(glm::radians(yaw_)), std::sin(glm::radians(yaw_)), 0) : glm::vec3(0, 0, 1)));
        const glm::vec3 up = glm::cross(right, f);
        const float perPixel = viewHeight() / std::max(ImGui::GetIO().DisplaySize.y, 1.0f);
        target_ += (-right * d.x + up * d.y) * perPixel;
    }

    void updateCamera(ImGuiIO& io) {
        const bool inView = mouseInView(io);
        // Orbit: right drag. Pan: middle drag (or Shift + right). Zoom: the wheel.
        const bool rightDown = ImGui::IsMouseDown(ImGuiMouseButton_Right), midDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        if (inView && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) orbiting_ = !io.KeyShift;
        if (inView && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right)))) panning_ = true, orbiting_ = false;
        if (!rightDown) orbiting_ = false;
        if (!midDown && !rightDown) panning_ = false;
        const ImVec2 d = io.MouseDelta;
        if (orbiting_ && (d.x != 0 || d.y != 0)) orbitBy(d);
        if (panning_ && (d.x != 0 || d.y != 0)) panBy(d);
        if (inView && io.MouseWheel != 0 && drag_.active && softSel_ && mode_ == Mode::Edit) { // the soft selection's reach, not zoom
            softReach_ = std::clamp(softReach_ * std::pow(1.15f, io.MouseWheel), 0.001f, 1000.0f);
            findSoft();
            message_ = std::format("Soft selection reaches {:.3f} m (wheel)", softReach_), messageAt_ = now();
        } else if (inView && io.MouseWheel != 0) {
            // Towards what's under the mouse: the target slides that way a little.
            const float k = std::pow(0.85f, io.MouseWheel);
            const Ray ray = mouseRay(io.MousePos);
            if (auto p = hitPlane(ray, target_, -viewDir())) target_ += (*p - target_) * (1.0f - k);
            dist_ = std::clamp(dist_ * k, 0.01f, 5000.0f);
        }
        const float aspect = io.DisplaySize.x / std::max(io.DisplaySize.y, 1.0f);
        shift_ = glm::vec2((viewMin_.x + viewMax_.x) / std::max(io.DisplaySize.x, 1.0f) - 1.0f, (viewMin_.y + viewMax_.y) / std::max(io.DisplaySize.y, 1.0f) - 1.0f);
        eye_ = target_ - viewDir() * dist_;
        vp_ = viewProjection(eye_, yaw_, pitch_, fov_, aspect, shift_);
    }

    // World point → screen (false behind the eye).
    // The window and its view are up (commands given at the start run before the first frame).
    bool viewReady() const { return ImGui::GetCurrentContext() && viewMax_.x - viewMin_.x > 1 && viewMax_.y - viewMin_.y > 1; }
    bool project(const glm::vec3& p, ImVec2& out) const {
        const glm::vec4 c = vp_ * glm::vec4(p, 1.0f);
        if (c.w < 1e-4f) return false;
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        out = ImVec2((c.x / c.w * 0.5f + 0.5f) * size.x, (c.y / c.w * 0.5f + 0.5f) * size.y);
        return true;
    }
    Ray mouseRay(ImVec2 m) const {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const glm::vec2 ndc(m.x / size.x * 2.0f - 1.0f, m.y / size.y * 2.0f - 1.0f);
        const glm::mat4 inv = glm::inverse(vp_);
        glm::vec4 a = inv * glm::vec4(ndc, 0.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
        a /= a.w, b /= b.w;
        return {glm::vec3(a), glm::normalize(glm::vec3(b - a))};
    }
    // How many metres a pixel is, at a point.
    float pixelSize(const glm::vec3& at) const {
        const float d = std::max(glm::dot(at - eye_, viewDir()), 0.001f);
        return 2.0f * d * std::tan(glm::radians(fov_) * 0.5f) / std::max(ImGui::GetIO().DisplaySize.y, 1.0f);
    }

    // ───────────────────────────── what's shown ─────────────────────────────

    // The mesh as seen: with its rounding, smoothing and mirror — but while its shape is being
    // edited, only the mirror (the rest would hide the points you're moving).
    const Mesh& shownMesh(const Object& o) {
        const bool editingThis = mode_ == Mode::Edit && o.id == active_;
        if (!o.hasFinish() || (editingThis && !o.mirror)) return o.mesh;
        Shown& s = shown_[o.id];
        const uint64_t v = versions_[o.id];
        if (!(s.version == v && v && s.mirrorOnly == editingThis)) s.mesh = finishedMesh(o, editingThis), s.version = v, s.mirrorOnly = editingThis;
        return s.mesh;
    }

    // ───────────────────────────── picking ─────────────────────────────

    struct Hit {
        uint32_t object = 0;
        int face = -1;
        float t = 1e30f;
    };
    // base: against its own mesh (face numbers of that), not the rounded or smoothed one shown.
    Hit pickObject(const Ray& ray, uint32_t only = 0, bool base = false) {
        Hit best;
        for (const Object& o : scene_.objects) {
            if (!o.visible || (only && o.id != only)) continue;
            const Mesh& mesh = base || (mode_ == Mode::Edit && o.id == active_) ? o.mesh : shownMesh(o);
            const glm::mat4 m = objectMatrix(o);
            std::vector<glm::vec3> w(mesh.verts.size());
            for (size_t i = 0; i < w.size(); ++i) w[i] = glm::vec3(m * glm::vec4(mesh.verts[i], 1.0f));
            for (size_t f = 0; f < mesh.faces.size(); ++f)
                for (const auto& tri : triangulate(mesh, int(f)))
                    if (auto t = hitTriangle(ray, w[size_t(tri[0])], w[size_t(tri[1])], w[size_t(tri[2])]); t && *t < best.t) best = {o.id, int(f), *t};
        }
        return best;
    }
    // Can the eye see this point of the object (nothing of it in the way)?
    bool visible(const Object& o, const glm::vec3& world) {
        if (xray_) return true;
        const glm::vec3 d = world - eye_;
        const float len = glm::length(d);
        const Hit h = pickObject({eye_, d / len}, o.id);
        return h.t > len - std::max(len * 0.002f, 0.0005f);
    }
    // Which of an object's points the eye can see, worked out again only when it or the view
    // changes. The triangles are sorted into a grid on the screen first: a point is tried only
    // against those over its own square (what could be between it and the eye).
    struct Visibility {
        uint64_t version = 0;
        glm::vec3 eye{1e30f};
        glm::mat4 vp{0.0f};
        std::vector<char> point;
    };
    std::map<std::pair<uint32_t, bool>, Visibility> visibility_; // (an object's own mesh, or the one shown)
    const std::vector<char>& visiblePoints(const Object& o) { return visiblePoints(o, o.mesh); }
    const std::vector<char>& visiblePoints(const Object& o, const Mesh& mesh) {
        Visibility& v = visibility_[{o.id, &mesh != &o.mesh}];
        const uint64_t version = versions_[o.id];
        if (v.version == version && glm::distance(v.eye, eye_) < 1e-6f && v.vp == vp_ && v.point.size() == mesh.verts.size()) return v.point;
        v.version = version, v.eye = eye_, v.vp = vp_;
        v.point.assign(mesh.verts.size(), 1);
        const glm::mat4 m = objectMatrix(o);
        std::vector<glm::vec3> w(mesh.verts.size());
        std::vector<glm::vec4> clip(w.size());
        for (size_t i = 0; i < w.size(); ++i) w[i] = glm::vec3(m * glm::vec4(mesh.verts[i], 1.0f)), clip[i] = vp_ * glm::vec4(w[i], 1.0f);
        std::vector<std::array<int, 3>> tris;
        for (size_t f = 0; f < mesh.faces.size(); ++f)
            for (const auto& t : triangulate(mesh, int(f))) tris.push_back(t);
        constexpr int G = 64;
        auto cell = [](float ndc) { return std::clamp(int((ndc * 0.5f + 0.5f) * G), 0, G - 1); };
        std::vector<std::vector<int>> grid(G * G);
        std::vector<int> everywhere; // (reaching behind the eye: in every square)
        for (size_t t = 0; t < tris.size(); ++t) {
            glm::vec2 lo(1e30f), hi(-1e30f);
            bool behind = false;
            for (int k : tris[t]) {
                const glm::vec4 c = clip[size_t(k)];
                if (c.w < 1e-5f) { behind = true; break; }
                lo = glm::min(lo, glm::vec2(c) / c.w), hi = glm::max(hi, glm::vec2(c) / c.w);
            }
            if (behind) { everywhere.push_back(int(t)); continue; }
            if (hi.x < -1 || hi.y < -1 || lo.x > 1 || lo.y > 1) continue; // (off the screen: covers nothing on it)
            for (int y = cell(lo.y); y <= cell(hi.y); ++y)
                for (int x = cell(lo.x); x <= cell(hi.x); ++x) grid[size_t(y * G + x)].push_back(int(t));
        }
        for (size_t i = 0; i < w.size(); ++i) {
            const glm::vec4 c = clip[i];
            if (c.w < 1e-5f) continue; // (behind the eye: not drawn anyway)
            const glm::vec2 n = glm::vec2(c) / c.w;
            if (std::abs(n.x) > 1 || std::abs(n.y) > 1) continue;
            const glm::vec3 d = w[i] - eye_;
            const float len = glm::length(d);
            const Ray ray{eye_, d / len};
            const float limit = len - std::max(len * 0.002f, 0.0005f);
            auto hides = [&](int t) {
                const auto& tri = tris[size_t(t)];
                if (tri[0] == int(i) || tri[1] == int(i) || tri[2] == int(i)) return false;
                auto hit = hitTriangle(ray, w[size_t(tri[0])], w[size_t(tri[1])], w[size_t(tri[2])]);
                return hit && *hit < limit;
            };
            if (std::any_of(everywhere.begin(), everywhere.end(), hides)) { v.point[i] = 0; continue; }
            const auto& here = grid[size_t(cell(n.y) * G + cell(n.x))];
            if (std::any_of(here.begin(), here.end(), hides)) v.point[i] = 0;
        }
        return v.point;
    }
    bool pointSeen(const Object& o, int v) { return xray_ || visiblePoints(o)[size_t(v)]; }

    // The edge of the edited object nearest the mouse on screen (that can be seen), or (-1, -1).
    std::pair<int, int> nearestEdge(Object& o, ImVec2 mouse, float within) {
        const glm::mat4 m = objectMatrix(o);
        std::pair<int, int> best{-1, -1};
        for (auto [a, b] : edges(o.mesh)) {
            ImVec2 sa, sb;
            if (!project(glm::vec3(m * glm::vec4(o.mesh.verts[size_t(a)], 1.0f)), sa) || !project(glm::vec3(m * glm::vec4(o.mesh.verts[size_t(b)], 1.0f)), sb)) continue;
            const float d = distToSegment(mouse, sa, sb);
            if (d < within && (pointSeen(o, a) || pointSeen(o, b))) within = d, best = {a, b};
        }
        return best;
    }

    void clickSelect(const ImGuiIO& io) {
        const Ray ray = mouseRay(io.MousePos);
        bool add = io.KeyShift, remove = io.KeyCtrl;
        if (Object* o = editing()) {
            // Shapes: Shift picks the whole loop (Shift+Ctrl on an edge: the ring across it), Ctrl
            // adds or takes away one.
            if (io.KeyShift) return loopSelect(io, io.KeyCtrl);
            add = io.KeyCtrl, remove = false;
            const glm::mat4 m = objectMatrix(*o);
            auto toggle = [&](auto& set, auto key) {
                if (remove) set.erase(key);
                else if (add && set.count(key)) set.erase(key);
                else set.insert(key);
            };
            if (!add && !remove) clearElements();
            if (elem_ == Elem::Vertex) {
                int best = -1;
                float bestD = 14.0f;
                for (size_t v = 0; v < o->mesh.verts.size(); ++v) {
                    ImVec2 s;
                    if (!project(glm::vec3(m * glm::vec4(o->mesh.verts[v], 1.0f)), s)) continue;
                    const float d = std::hypot(s.x - io.MousePos.x, s.y - io.MousePos.y);
                    if (d < bestD && pointSeen(*o, int(v))) bestD = d, best = int(v);
                }
                if (best >= 0) toggle(selVerts_, best);
            } else if (elem_ == Elem::Edge) {
                const auto e = nearestEdge(*o, io.MousePos, 10.0f);
                if (e.first >= 0) toggle(selEdges_, e);
            } else {
                const Hit h = pickObject(ray, o->id);
                if (h.face >= 0) toggle(selFaces_, h.face);
            }
            return;
        }
        const Hit h = pickObject(ray);
        if (!add && !remove) selected_.clear();
        if (h.object) {
            if (remove || (add && selected_.count(h.object))) selected_.erase(h.object);
            else selected_.insert(h.object), active_ = h.object;
            activeRef_ = 0;
        }
    }

    // A whole loop: of edges (points), or a ring of faces, through the edge under the mouse.
    void loopSelect(const ImGuiIO& io, bool ring = false) {
        Object* o = editing();
        if (!o) return;
        if (!io.KeyShift) clearElements();
        if (elem_ == Elem::Face) {
            const Hit h = pickObject(mouseRay(io.MousePos), o->id);
            if (h.face < 0) return;
            // The face's edge nearest the mouse: the ring runs across it.
            const auto& fv = o->mesh.faces[size_t(h.face)];
            const glm::mat4 m = objectMatrix(*o);
            std::pair<int, int> best{-1, -1};
            float bestD = 1e30f;
            for (size_t i = 0; i < fv.size(); ++i) {
                ImVec2 sa, sb;
                if (project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(fv[i])], 1.0f)), sa) &&
                    project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(fv[(i + 1) % fv.size()])], 1.0f)), sb))
                    if (const float d = distToSegment(io.MousePos, sa, sb); d < bestD) bestD = d, best = {fv[i], fv[(i + 1) % fv.size()]};
            }
            if (best.first < 0) return;
            const auto ring = faceRing(o->mesh, best);
            selFaces_.insert(ring.begin(), ring.end());
            if (ring.empty()) selFaces_.insert(h.face);
            return;
        }
        const auto e = nearestEdge(*o, io.MousePos, 14.0f);
        if (e.first < 0) return;
        const auto loop = ring ? edgeRing(o->mesh, e) : edgeLoop(o->mesh, e);
        if (ring) say(std::format("A ring of {} edges", loop.size()));
        if (elem_ == Elem::Edge) selEdges_.insert(loop.begin(), loop.end());
        else
            for (auto [a, b] : loop) selVerts_.insert(a), selVerts_.insert(b);
    }

    // Selects what's on screen inside a shape (a box, a lasso, the brush).
    void regionSelect(const std::function<bool(ImVec2)>& inside, bool add, bool remove, bool clearFirst) {
        auto in = [&](const glm::vec3& w) {
            ImVec2 s;
            return project(w, s) && inside(s);
        };
        if (Object* o = editing()) {
            if (clearFirst) clearElements();
            const glm::mat4 m = objectMatrix(*o);
            std::vector<char> hit(o->mesh.verts.size());
            for (size_t v = 0; v < hit.size(); ++v) hit[v] = in(glm::vec3(m * glm::vec4(o->mesh.verts[v], 1.0f))) && pointSeen(*o, int(v));
            auto apply = [&](auto& set, auto key, bool yes) {
                if (!yes) return;
                if (remove) set.erase(key);
                else set.insert(key);
            };
            if (elem_ == Elem::Vertex)
                for (size_t v = 0; v < hit.size(); ++v) apply(selVerts_, int(v), bool(hit[v]));
            else if (elem_ == Elem::Edge)
                for (auto e : edges(o->mesh)) {
                    const glm::vec3 mid(m * glm::vec4((o->mesh.verts[size_t(e.first)] + o->mesh.verts[size_t(e.second)]) * 0.5f, 1.0f));
                    apply(selEdges_, e, (hit[size_t(e.first)] && hit[size_t(e.second)]) || (in(mid) && pointSeen(*o, e.first) && pointSeen(*o, e.second)));
                }
            else
                for (size_t f = 0; f < o->mesh.faces.size(); ++f) {
                    const glm::vec3 c(m * glm::vec4(faceCenter(o->mesh, int(f)), 1.0f));
                    if (!in(c)) continue;
                    const glm::vec3 n = glm::transpose(glm::inverse(glm::mat3(m))) * faceNormal(o->mesh, int(f));
                    apply(selFaces_, int(f), xray_ || (glm::dot(n, eye_ - c) > 0 && visible(*o, c)));
                }
            return;
        }
        if (clearFirst) selected_.clear();
        (void)add;
        for (const Object& o : scene_.objects) {
            if (!o.visible) continue;
            const glm::vec3 c(objectMatrix(o) * glm::vec4((boundsMin(o.mesh) + boundsMax(o.mesh)) * 0.5f, 1.0f));
            if (!in(c)) continue;
            if (remove) selected_.erase(o.id);
            else selected_.insert(o.id), active_ = o.id;
        }
    }

    // ───────────────────────────── the gizmo ─────────────────────────────

    bool hasSelection() {
        if (mode_ == Mode::Edit) return !selectedPoints().empty();
        return std::any_of(selected_.begin(), selected_.end(), [&](uint32_t id) { const Object* o = find(id); return o && !o->locked; });
    }
    // Every point of the selection, in the world.
    std::vector<glm::vec3> selectionPoints() {
        std::vector<glm::vec3> out;
        if (Object* o = editing()) {
            const glm::mat4 m = objectMatrix(*o);
            for (int v : selectedPoints()) out.push_back(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(v)], 1.0f)));
            return out;
        }
        for (const Object& o : scene_.objects) {
            if (!selected_.count(o.id) || o.locked) continue;
            const glm::mat4 m = objectMatrix(o);
            for (const glm::vec3& v : shownMesh(o).verts) out.push_back(glm::vec3(m * glm::vec4(v, 1.0f)));
        }
        return out;
    }
    glm::vec3 pivotOfSelection() {
        glm::vec3 c(0.0f);
        int n = 0;
        if (Object* o = editing()) {
            const glm::mat4 m = objectMatrix(*o);
            for (int v : selectedPoints()) c += glm::vec3(m * glm::vec4(o->mesh.verts[size_t(v)], 1.0f)), ++n;
        } else {
            for (const Object& o : scene_.objects)
                if (selected_.count(o.id)) c += o.position, ++n;
        }
        return n ? c / float(n) : glm::vec3(0.0f);
    }
    // The selection's box, in a gizmo's axes.
    std::pair<glm::vec3, glm::vec3> selectionBox(const Axes& a) {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const glm::vec3& p : selectionPoints()) {
            const glm::vec3 c = inFrame(a, p);
            lo = glm::min(lo, c), hi = glm::max(hi, c);
        }
        if (lo.x > hi.x) lo = hi = inFrame(a, pivotOfSelection());
        return {lo, hi};
    }
    glm::vec3 selectionMiddle(const Axes& a) {
        const auto [lo, hi] = selectionBox(a);
        return fromFrame(a, (lo + hi) * 0.5f);
    }
    Orient& orient() { return mode_ == Mode::Edit ? orientEdit_ : orientObject_; }
    // The gizmo's axes: the object's own (turning with it), the world's, or (shape editing) the
    // way the selected faces face — so "up" on a side face is out of that face.
    Axes gizmoAxes() {
        Orient o = orient();
        if (mode_ == Mode::Object && tool_ == Tool::Scale) o = Orient::Local; // (sizes are along an object's own axes)
        const Object* a = find(active_);
        if (!a && !selected_.empty()) a = find(*selected_.begin());
        const glm::quat q = a && o != Orient::World ? a->rotation : glm::quat(1, 0, 0, 0);
        Axes local{q * glm::vec3(1, 0, 0), q * glm::vec3(0, 1, 0), q * glm::vec3(0, 0, 1)};
        if (o != Orient::Normal || mode_ != Mode::Edit) return local;
        Object* e = editing();
        if (!e) return local;
        const glm::mat4 m = objectMatrix(*e);
        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m)));
        const std::set<int> pts = selectedPoints();
        glm::vec3 n(0.0f), along(0.0f);
        for (size_t f = 0; f < e->mesh.faces.size(); ++f) {
            const auto& fv = e->mesh.faces[f];
            const bool whole = elem_ == Elem::Face ? selFaces_.count(int(f)) > 0 : std::any_of(fv.begin(), fv.end(), [&](int v) { return pts.count(v); });
            if (!whole) continue;
            n += nm * faceNormal(e->mesh, int(f)) * std::max(faceArea(e->mesh, int(f)), 1e-6f);
            if (glm::length(along) < 1e-9f) // the first face's longest edge: "across" it
                for (size_t i = 0; i < fv.size(); ++i) {
                    const glm::vec3 d = glm::mat3(m) * (e->mesh.verts[size_t(fv[(i + 1) % fv.size()])] - e->mesh.verts[size_t(fv[i])]);
                    if (glm::length(d) > glm::length(along)) along = d;
                }
        }
        if (elem_ == Elem::Edge && !selEdges_.empty()) {
            const auto [a0, b0] = *selEdges_.begin();
            along = glm::mat3(m) * (e->mesh.verts[size_t(b0)] - e->mesh.verts[size_t(a0)]);
        }
        if (glm::length(n) < 1e-9f) return local;
        const glm::vec3 z = glm::normalize(n);
        glm::vec3 x = along - z * glm::dot(along, z);
        if (glm::length(x) < 1e-6f) x = glm::cross(std::abs(z.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0), z);
        x = glm::normalize(x);
        return {x, glm::cross(z, x), z};
    }
    // The extrude / inset handle: the selected faces' middle, their normal.
    bool faceHandle(glm::vec3& at, glm::vec3& normal) {
        Object* o = editing();
        if (!o || elem_ != Elem::Face || selFaces_.empty()) return false;
        const glm::mat4 m = objectMatrix(*o);
        glm::vec3 c(0.0f), n(0.0f);
        for (int f : selFaces_) {
            if (f >= int(o->mesh.faces.size())) return false;
            c += glm::vec3(m * glm::vec4(faceCenter(o->mesh, f), 1.0f));
            n += glm::normalize(glm::transpose(glm::inverse(glm::mat3(m))) * faceNormal(o->mesh, f));
        }
        at = c / float(selFaces_.size());
        normal = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0, 0, 1);
        return true;
    }
    // The round-edges handle: off to the right of the selection's middle.
    bool bevelHandle(ImVec2& centre, ImVec2& handle) {
        if (!editing() || edgesToRound().empty()) return false;
        if (!project(drag_.active ? drag_.pivot : pivotOfSelection(), centre)) return false;
        handle = ImVec2(centre.x + 80.0f + (drag_.active ? ImGui::GetIO().MousePos.x - drag_.startMouse.x : 0.0f), centre.y);
        return true;
    }
    // The scale box's handles: 0-2 the far side along each axis, 4-6 the near side, 3 the middle.
    // (Pushed out a little where the box is flat, so they don't sit on top of each other.)
    glm::vec3 scaleHandle(const Axes& a, glm::vec3 lo, glm::vec3 hi, int h) {
        const glm::vec3 c = (lo + hi) * 0.5f;
        if (h == 3) return fromFrame(a, c);
        const int k = h % 4 == 3 ? 0 : (h < 3 ? h : h - 4);
        glm::vec3 q = c;
        const float minHalf = pixelSize(fromFrame(a, c)) * 22.0f;
        const float half = std::max((hi[k] - lo[k]) * 0.5f, minHalf);
        q[k] = c[k] + (h < 3 ? half : -half);
        return fromFrame(a, q);
    }

    // Which handle is under the mouse (-1 none).
    int hoverHandle(ImVec2 mouse) {
        if (drag_.active) return drag_.handle;
        if (tool_ == Tool::Extrude || tool_ == Tool::Inset) {
            glm::vec3 at, n;
            if (!faceHandle(at, n)) return -1;
            ImVec2 a, b;
            if (!project(at, a) || !project(at + n * pixelSize(at) * 90.0f, b)) return -1;
            return distToSegment(mouse, a, b) < 10.0f ? 0 : -1;
        }
        if (tool_ == Tool::Bevel) {
            ImVec2 c, h;
            return bevelHandle(c, h) && std::hypot(mouse.x - h.x, mouse.y - h.y) < 12.0f ? 0 : -1;
        }
        if (tool_ != Tool::Move && tool_ != Tool::Rotate && tool_ != Tool::Scale) return -1;
        if (!hasSelection()) return -1;
        const Axes axes = gizmoAxes();
        if (tool_ == Tool::Scale) {
            const auto [lo, hi] = selectionBox(axes);
            for (int h : {3, 0, 1, 2, 4, 5, 6}) {
                ImVec2 s;
                if (project(scaleHandle(axes, lo, hi, h), s) && std::abs(mouse.x - s.x) < 9 && std::abs(mouse.y - s.y) < 9) return h;
            }
            return -1;
        }
        const glm::vec3 p = tool_ == Tool::Rotate ? rotatePivot(axes) : selectionMiddle(axes);
        ImVec2 c;
        if (!project(p, c)) return -1;
        const float len = pixelSize(p) * 90.0f;
        if (tool_ == Tool::Rotate) {
            int best = -1;
            float bestD = 9.0f;
            for (int k = 0; k < 3; ++k) {
                const glm::vec3 u = glm::normalize(glm::cross(axes[size_t(k)], std::abs(axes[size_t(k)].z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
                const glm::vec3 v = glm::cross(axes[size_t(k)], u);
                ImVec2 prev;
                bool havePrev = false;
                for (int i = 0; i <= 48; ++i) {
                    const float a = 2 * kPi * float(i) / 48.0f;
                    ImVec2 s;
                    const glm::vec3 w = p + (u * std::cos(a) + v * std::sin(a)) * len;
                    if (!project(w, s)) {
                        havePrev = false;
                        continue;
                    }
                    if (havePrev && glm::dot(w - p, eye_ - p) > -0.2f * len * glm::length(eye_ - p)) // (the front half only)
                        if (const float d = distToSegment(mouse, prev, s); d < bestD) bestD = d, best = k;
                    prev = s, havePrev = true;
                }
            }
            const float r = std::hypot(mouse.x - c.x, mouse.y - c.y);
            if (best < 0 && r > 95.0f && r < 110.0f) best = 3; // the outer ring: about the view
            return best;
        }
        if (std::abs(mouse.x - c.x) < 9 && std::abs(mouse.y - c.y) < 9) return 3;
        for (int k = 0; k < 3; ++k) {
            ImVec2 e;
            if (project(p + axes[size_t(k)] * len, e) && distToSegment(mouse, c, e) < 9.0f) return k;
        }
        return -1;
    }

    glm::vec3 originPoint() {
        if (Object* o = editing()) return o->position;
        glm::vec3 c(0.0f);
        int n = 0;
        for (const Object& o : scene_.objects)
            if (selected_.count(o.id)) c += o.position, ++n;
        return n ? c / float(n) : c;
    }
    glm::vec3 rotatePivot(const Axes& a) { return rotatePivot_ == Pivot::Origin ? originPoint() : selectionMiddle(a); }

    void startDrag(const ImGuiIO& io, int handle) {
        drag_ = {};
        drag_.active = true, drag_.handle = handle, drag_.tool = tool_;
        drag_.startMouse = io.MousePos;
        drag_.before = scene_;
        pushUndo();
        if (softSel_ && (tool_ == Tool::Move || tool_ == Tool::Rotate || tool_ == Tool::Scale)) findSoft();
        const Ray ray = mouseRay(io.MousePos);
        if (tool_ == Tool::Extrude || tool_ == Tool::Inset) {
            glm::vec3 at, n;
            faceHandle(at, n);
            drag_.pivot = at, drag_.startT = closestOnAxis(ray, at, n);
            drag_.startHit = n;
            return;
        }
        if (tool_ == Tool::Bevel) {
            drag_.pivot = pivotOfSelection();
            drag_.bevelEdges = edgesToRound();
            return;
        }
        drag_.axes = gizmoAxes();
        const Axes& a = drag_.axes;
        if (tool_ == Tool::Scale) {
            const auto [lo, hi] = selectionBox(a);
            const glm::vec3 c = (lo + hi) * 0.5f;
            drag_.handlePoint = scaleHandle(a, lo, hi, handle);
            const int k = handle < 3 ? handle : handle - 4;
            glm::vec3 q = c;
            switch (scalePivot_) {
            case Pivot::Opposite:
                if (handle == 3) q[2] = lo[2];
                else q[k] = handle < 3 ? lo[k] : hi[k];
                drag_.anchor = fromFrame(a, q);
                break;
            case Pivot::Bottom: q[2] = lo[2], drag_.anchor = fromFrame(a, q); break;
            case Pivot::Origin: drag_.anchor = originPoint(); break;
            default: drag_.anchor = fromFrame(a, c); break;
            }
            drag_.pivot = fromFrame(a, c);
            if (handle != 3) drag_.startDist = glm::dot(drag_.handlePoint - drag_.anchor, a[size_t(k)]), drag_.startMid = glm::dot(drag_.handlePoint - drag_.pivot, a[size_t(k)]);
            return;
        }
        drag_.pivot = tool_ == Tool::Rotate ? rotatePivot(a) : selectionMiddle(a);
        if (handle < 3) drag_.startT = closestOnAxis(ray, drag_.pivot, a[size_t(handle)]);
        if (auto hit = hitPlane(ray, drag_.pivot, -viewDir())) drag_.startHit = *hit;
    }
    // The points within reach of the selection (in the world: the object's size counts), each with
    // how much it follows: a grid of the selected points, so a big mesh stays quick.
    void findSoft() {
        drag_.soft.clear();
        const Object* o = editing();
        if (!o) return;
        const std::set<int> pts = selectedPoints();
        if (pts.empty()) return;
        const glm::mat4 m = objectMatrix(*o);
        const float r = std::max(softReach_, 1e-5f);
        auto cell = [&](const glm::vec3& p) { return glm::ivec3(glm::floor(p / r)); };
        auto keyOf = [](glm::ivec3 c) { return (int64_t(c.x & 0x1FFFFF) << 42) | (int64_t(c.y & 0x1FFFFF) << 21) | int64_t(c.z & 0x1FFFFF); };
        std::unordered_map<int64_t, std::vector<glm::vec3>> grid;
        for (int v : pts) {
            const glm::vec3 w(m * glm::vec4(o->mesh.verts[size_t(v)], 1.0f));
            grid[keyOf(cell(w))].push_back(w);
        }
        for (size_t v = 0; v < o->mesh.verts.size(); ++v) {
            if (pts.count(int(v))) continue;
            const glm::vec3 w(m * glm::vec4(o->mesh.verts[v], 1.0f));
            const glm::ivec3 c = cell(w);
            float best = r;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz)
                        if (auto it = grid.find(keyOf(c + glm::ivec3(dx, dy, dz))); it != grid.end())
                            for (const glm::vec3& q : it->second) best = std::min(best, glm::distance(w, q));
            if (best < r) drag_.soft.push_back({int(v), sculptFalloff(best, r)});
        }
    }
    void cancelDrag() {
        if (!drag_.active) return;
        scene_ = drag_.before;
        dropUndo();
        touchAll();
        drag_.active = false;
    }

    void updateDrag(const ImGuiIO& io) {
        const Ray ray = mouseRay(io.MousePos);
        const bool snapping = snap_ != io.KeyCtrl; // Ctrl flips snapping while held
        scene_ = drag_.before; // every frame from where it started: no drift
        const Axes& axes = drag_.axes;
        const glm::vec3 p = drag_.pivot;
        glm::mat4 T(1.0f); // what happens to the selection, in the world
        if (drag_.tool == Tool::Extrude || drag_.tool == Tool::Inset) {
            Object* o = editing();
            if (!o) return;
            const glm::vec3 n = drag_.startHit;
            float d = closestOnAxis(ray, p, n) - drag_.startT;
            if (drag_.tool == Tool::Extrude) {
                if (snapping) d = snapTo(d, snapMove_);
                // (the pull in the object's own size, so it follows the mouse whatever its scale)
                const float scale = std::cbrt(std::abs(glm::determinant(glm::mat3(objectMatrix(*o)))));
                extrudeFaces(o->mesh, selFaces_, d / std::max(scale, 1e-6f));
                extrudeAmount_ = d;
                message_ = std::format("Extrude {:.3f} m", d), messageAt_ = now();
            } else {
                float t = std::clamp(-d / (pixelSize(p) * 120.0f), 0.0f, 0.99f);
                if (snapping) t = snapTo(t, 0.05f);
                insetFaces(o->mesh, selFaces_, t);
                insetAmount_ = t;
                message_ = std::format("Inset {:.0f}%", t * 100), messageAt_ = now();
            }
            changedShape(*o, true);
            return;
        }
        if (drag_.tool == Tool::Bevel) {
            Object* o = editing();
            if (!o) return;
            float w = std::max(0.0f, io.MousePos.x - drag_.startMouse.x) * pixelSize(p) * 0.5f;
            if (snapping) w = snapTo(w, 0.005f);
            if (w > 0) bevelEdges(o->mesh, drag_.bevelEdges, w, bevelSteps_);
            bevelWidth_ = w;
            handMade(*o);
            touched(o->id);
            message_ = std::format("Round edges {:.3f} m, {} step{}", w, bevelSteps_, bevelSteps_ == 1 ? "" : "s"), messageAt_ = now();
            return;
        }
        glm::vec3 objectScale(1.0f);
        float even = 1.0f;
        if (drag_.tool == Tool::Move) {
            glm::vec3 delta(0.0f);
            if (drag_.handle < 3) {
                float t = closestOnAxis(ray, p, axes[size_t(drag_.handle)]) - drag_.startT;
                if (snapping) t = snapTo(t, snapMove_);
                delta = axes[size_t(drag_.handle)] * t;
            } else if (auto hit = hitPlane(ray, p, -viewDir())) {
                delta = *hit - drag_.startHit;
                if (snapping) delta = glm::vec3(snapTo(delta.x, snapMove_), snapTo(delta.y, snapMove_), snapTo(delta.z, snapMove_));
            }
            T = glm::translate(glm::mat4(1.0f), delta);
            message_ = std::format("Move {:.3f}, {:.3f}, {:.3f} m", delta.x, delta.y, delta.z), messageAt_ = now();
        } else if (drag_.tool == Tool::Rotate) {
            ImVec2 c;
            if (!project(p, c)) return;
            const float a0 = std::atan2(drag_.startMouse.y - c.y, drag_.startMouse.x - c.x), a1 = std::atan2(io.MousePos.y - c.y, io.MousePos.x - c.x);
            float deg = glm::degrees(std::remainder(a1 - a0, 2 * kPi));
            const glm::vec3 axis = drag_.handle < 3 ? axes[size_t(drag_.handle)] : -viewDir();
            if (glm::dot(axis, viewDir()) < 0) deg = -deg; // (turning the way the mouse goes round, as seen)
            if (snapping) deg = snapTo(deg, snapAngle_);
            T = glm::translate(glm::mat4(1.0f), p) * glm::rotate(glm::mat4(1.0f), glm::radians(deg), axis) * glm::translate(glm::mat4(1.0f), -p);
            message_ = std::format("Rotate {:.1f} degrees", deg), messageAt_ = now();
        } else if (drag_.tool == Tool::Scale) {
            glm::vec3 s(1.0f);
            // Shift (or Alt) held: from both sides, the middle staying put.
            const bool both = (io.KeyShift || io.KeyAlt) && drag_.handle != 3;
            if (drag_.handle != 3) {
                const int k = drag_.handle < 3 ? drag_.handle : drag_.handle - 4;
                const float from = both ? drag_.startMid : drag_.startDist;
                float dist = from + closestOnAxis(ray, drag_.handlePoint, axes[size_t(k)]);
                if (snapping) dist = both ? snapTo(dist * 2, snapMove_) / 2 : snapTo(dist, snapMove_);
                const float f = std::abs(from) > 1e-6f ? dist / from : 1.0f;
                s[k] = f;
                message_ = std::format("Scale {:.2f}  ({:.3f} m){}", f, std::abs(dist) * (both ? 2 : 1), both ? "  from both sides" : "   (Shift: from both sides)"),
                messageAt_ = now();
            } else {
                even = std::max(0.01f, 1.0f + ((io.MousePos.x - drag_.startMouse.x) - (io.MousePos.y - drag_.startMouse.y)) / 200.0f);
                if (snapping) even = std::max(snapScale_, snapTo(even, snapScale_));
                s = glm::vec3(even);
                message_ = std::format("Scale {:.2f} (drag right or up to grow)", even), messageAt_ = now();
            }
            const glm::mat3 basis(axes[0], axes[1], axes[2]);
            const glm::vec3 a = both ? drag_.pivot : drag_.anchor;
            T = glm::translate(glm::mat4(1.0f), a) * glm::mat4(basis) * glm::scale(glm::mat4(1.0f), s) * glm::mat4(glm::transpose(basis)) * glm::translate(glm::mat4(1.0f), -a);
            objectScale = s;
        }
        if (Object* o = editing()) {
            const glm::mat4 m = objectMatrix(*o), local = glm::inverse(m) * T * m;
            for (int v : selectedPoints()) o->mesh.verts[size_t(v)] = glm::vec3(local * glm::vec4(o->mesh.verts[size_t(v)], 1.0f));
            for (const auto& [v, w] : drag_.soft) { // (part of the way, by how near)
                glm::vec3& p = o->mesh.verts[size_t(v)];
                p = glm::mix(p, glm::vec3(local * glm::vec4(p, 1.0f)), w);
            }
            handMade(*o);
            touched(o->id);
            return;
        }
        for (Object& o : scene_.objects) {
            if (!selected_.count(o.id) || o.locked) continue;
            o.position = glm::vec3(T * glm::vec4(o.position, 1.0f));
            if (drag_.tool == Tool::Rotate) o.rotation = glm::normalize(glm::quat_cast(glm::mat3(T)) * o.rotation);
            if (drag_.tool == Tool::Scale) {
                // On its own axes: the gizmo's where they're the same, else how much each of its
                // axes lies along each of the gizmo's.
                for (int j = 0; j < 3; ++j) {
                    const glm::vec3 own = o.rotation * glm::vec3(j == 0, j == 1, j == 2);
                    float f = 1.0f;
                    for (int k = 0; k < 3; ++k) f += (objectScale[k] - 1.0f) * glm::dot(own, axes[size_t(k)]) * glm::dot(own, axes[size_t(k)]);
                    o.scale[j] *= f;
                }
            }
            touched(o.id);
        }
    }

    // ───────────────────────────── drawing a shape ─────────────────────────────

    // The plane shapes are drawn on: the one the view looks straight at, or the ground.
    void workPlane(glm::vec3& point, glm::vec3& normal) const {
        point = glm::vec3(0.0f);
        normal = view_ == View::Front || view_ == View::Back ? glm::vec3(1, 0, 0) : view_ == View::Left || view_ == View::Right ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
    }
    // The plane drawn on: the face under the first point if it was on an object, else the view's.
    void drawPlane(glm::vec3& p, glm::vec3& n) {
        if (drawOnto_ && find(drawOnto_)) p = drawOnP_, n = drawOnN_;
        else workPlane(p, n);
    }
    std::optional<glm::vec3> drawPoint(const ImGuiIO& io) {
        const Ray ray = mouseRay(io.MousePos);
        if (drawPts_.empty()) { // (the first point: on an object, its face is where it's drawn)
            drawOnto_ = 0;
            const Hit h = pickObject(ray, 0, true);
            if (const Object* o = h.object ? find(h.object) : nullptr) {
                const glm::mat4 m = objectMatrix(*o);
                drawOnto_ = o->id;
                drawOnP_ = ray.origin + ray.dir * h.t;
                drawOnN_ = glm::normalize(glm::transpose(glm::inverse(glm::mat3(m))) * faceNormal(o->mesh, h.face));
            }
        }
        glm::vec3 p, n;
        drawPlane(p, n);
        auto hit = hitPlane(ray, p, n);
        if (!hit) return std::nullopt;
        if (snap_ != io.KeyCtrl) { // (on the grid, then back onto the plane)
            const float step = gridStep() / 2.0f;
            *hit = glm::vec3(snapTo(hit->x, step), snapTo(hit->y, step), snapTo(hit->z, step));
            *hit -= n * glm::dot(*hit - p, n);
        }
        return hit;
    }
    void finishShape() {
        if (drawPts_.size() < 3) {
            drawPts_.clear();
            return say("A shape needs 3 points or more");
        }
        glm::vec3 p, n;
        drawPlane(p, n);
        const uint32_t onto = drawOnto_ && find(drawOnto_) ? drawOnto_ : 0;
        drawOnto_ = 0;
        glm::vec3 c(0.0f);
        for (const glm::vec3& q : drawPts_) c += q;
        c /= float(drawPts_.size());
        std::vector<glm::vec3> local;
        for (const glm::vec3& q : drawPts_) local.push_back(q - c);
        // On an object it stands on the face (a hair into it, so they join), or goes down into it.
        const float t = drawThickness_, e = 0.0001f;
        float thick = t;
        if (onto && drawOnMode_ == 2) thick = t + 2 * e, c += n * (e - thick / 2);
        else if (onto) thick = t + e, c += n * (t - thick / 2);
        Mesh m = extrudeOutline(local, n, thick);
        drawPts_.clear();
        if (m.faces.empty()) return say("That shape doesn't work (points on top of each other?)");
        if (onto && drawOnMode_ > 0 && find(onto)->locked) return say("That object is locked");
        pushUndo();
        if (onto && drawOnMode_ > 0) {
            Object* o = find(onto);
            if (o->hasFinish()) o->mesh = finishedMesh(*o), o->mirror = 0, o->round = 0, o->smoothLevels = 0;
            Mesh shape = m;
            transform(shape, glm::inverse(objectMatrix(*o)) * glm::translate(glm::mat4(1.0f), c));
            Mesh result = o->mesh;
            if (!combine(result, shape, drawOnMode_ == 2 ? Combine::Subtract : Combine::Add)) {
                takeBack();
                return say("That would leave nothing");
            }
            o = find(onto);
            o->mesh = std::move(result);
            changedShape(*o);
            selected_ = {o->id}, active_ = o->id;
            return say(std::format("{} {:.0f} mm {} {}", drawOnMode_ == 2 ? "Cut" : "Added", t * 1000, drawOnMode_ == 2 ? "deep into" : "high onto", o->name));
        }
        Object& o = addObject("Shape", std::move(m));
        o.position = c;
        selected_ = {o.id}, active_ = o.id;
        say(std::format("Shape made, {:.0f} mm thick{}. Tab edits its shape; Thickness is in Tool options.", t * 1000, onto ? ", standing on " + find(onto)->name : ""));
    }

    // ───────────────────────────── the viewport's mouse ─────────────────────────────

    void viewport(ImGuiIO& io) {
        cutHover_ = {-1, -1};
        if (tab_ == Tab::Paint) return paintViewport(io);
        if (tab_ == Tab::Layout) return layoutViewport(io);
        if (tab_ == Tab::Print && printTool_ != PrintTool::None) return printViewport(io);
        if (knifeDown_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            knifeDown_ = false;
            const ImVec2 end = knifeEnd(io);
            if (knifePts_.empty() && std::hypot(end.x - knifePress_.x, end.y - knifePress_.y) > 8) {
                if (Object* o = editing()) knifeApply(o->id, {knifePress_, end});
            } else
                knifePts_.push_back(end);
            return;
        }
        if (hole_.active) {
            Object* o = find(hole_.object);
            if (!o) hole_.active = false;
            else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (auto p = onFacePlane(*o, hole_.face, io.MousePos)) hole_.radius = glm::distance(*p, hole_.centre);
                message_ = std::format("{:.1f} mm across", holeAcrossFor(hole_.radius) * objectScale(*o) * 1000), messageAt_ = now();
            } else {
                hole_.active = false;
                const bool dragged = std::hypot(io.MousePos.x - hole_.press.x, io.MousePos.y - hole_.press.y) > 4;
                applyHole(hole_.object, hole_.face, hole_.centre, dragged ? holeAcrossFor(hole_.radius) : holeSize_ / objectScale(*o));
            }
            return;
        }
        if (drag_.active) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) updateDrag(io);
            else {
                drag_.active = false;
                if (drag_.tool == Tool::Bevel) clearElements(); // (renumbered)
            }
            return;
        }
        if (boxing_) {
            const bool moved = std::hypot(io.MousePos.x - boxStart_.x, io.MousePos.y - boxStart_.y) > 6;
            if (touchOrbit_ && moved && ImGui::IsMouseDown(ImGuiMouseButton_Left)) orbitBy(io.MouseDelta);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                boxing_ = false;
                if (std::exchange(touchOrbit_, false) && moved) return void(tapDraw_ = false);
                if (std::exchange(tapDraw_, false)) return addDrawPoint(io);
                if (std::hypot(io.MousePos.x - boxStart_.x, io.MousePos.y - boxStart_.y) > 4) {
                    const ImVec2 a(std::min(boxStart_.x, io.MousePos.x), std::min(boxStart_.y, io.MousePos.y)), b(std::max(boxStart_.x, io.MousePos.x), std::max(boxStart_.y, io.MousePos.y));
                    regionSelect([&](ImVec2 s) { return s.x >= a.x && s.x <= b.x && s.y >= a.y && s.y <= b.y; }, io.KeyShift, io.KeyCtrl, !io.KeyShift && !io.KeyCtrl);
                } else
                    clickSelect(io);
            }
            return;
        }
        if (lassoing_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (std::hypot(io.MousePos.x - lasso_.back().x, io.MousePos.y - lasso_.back().y) > 3) lasso_.push_back(io.MousePos);
            } else {
                lassoing_ = false;
                if (lasso_.size() > 4) regionSelect([&](ImVec2 s) { return insidePolygon(s, lasso_); }, io.KeyShift, io.KeyCtrl, !io.KeyShift && !io.KeyCtrl);
                else clickSelect(io);
            }
            return;
        }
        if (painting_) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) painting_ = false;
            else paint(io);
            return;
        }
        if (!mouseInView(io)) return;
        if (tool_ == Tool::Draw) {
            if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
            if (touch()) return void((boxing_ = touchOrbit_ = tapDraw_ = true, boxStart_ = io.MousePos)); // (a finger: a tap puts a point down when it lifts, a drag turns the view)
            addDrawPoint(io);
            return;
        }
        if (tool_ == Tool::Knife) {
            if (!editing()) return;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && knifePts_.size() >= 2) {
                    std::vector<ImVec2> pts = knifePts_;
                    knifePts_.clear();
                    return knifeApply(active_, pts);
                }
                knifeDown_ = true, knifePress_ = io.MousePos;
            }
            return;
        }
        if (tool_ == Tool::Hole) {
            if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
            const Ray ray = mouseRay(io.MousePos);
            const Hit h = pickObject(ray, mode_ == Mode::Edit ? active_ : 0, true);
            if (!h.object) return;
            Object* o = find(h.object);
            if (o->locked) return say("That object is locked");
            if (mode_ == Mode::Object) selected_ = {h.object}, active_ = h.object;
            hole_ = {true, h.object, h.face, glm::vec3(glm::inverse(objectMatrix(*o)) * glm::vec4(ray.origin + ray.dir * h.t, 1.0f)), 0.0f, io.MousePos};
            return;
        }
        if (tool_ == Tool::LoopCut) {
            if (Object* o = editing()) {
                cutHover_ = nearestEdge(*o, io.MousePos, 30.0f);
                if (cutHover_.first >= 0 && ringQuads(o->mesh, cutHover_).empty()) cutHover_ = {-1, -1};
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && cutHover_.first >= 0) cutAt(*o, cutHover_);
            }
            return;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (const int h = hoverHandle(io.MousePos); h >= 0) return startDrag(io, h);
            if (mode_ == Mode::Edit && (io.KeyAlt || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) return loopSelect(io);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && mode_ == Mode::Object) { // double click: edit its shape
                const Hit hit = pickObject(mouseRay(io.MousePos));
                if (hit.object) {
                    selected_ = {hit.object}, active_ = hit.object;
                    enterEdit();
                    return;
                }
            }
            if (selShape_ == SelShape::Brush) {
                painting_ = true;
                if (!io.KeyShift && !io.KeyCtrl) selectNone();
                paint(io);
            } else if (selShape_ == SelShape::Lasso) lassoing_ = true, lasso_ = {io.MousePos};
            else boxing_ = true, boxStart_ = io.MousePos, touchOrbit_ = touch() && tool_ != Tool::Select; // (a finger: dragging turns the view, but Select's box)
        }
    }
    void addDrawPoint(const ImGuiIO& io) {
        if (auto p = drawPoint(io)) {
            ImVec2 s0, s1;
            if (drawPts_.size() >= 3 && project(drawPts_[0], s0) && project(*p, s1) && std::hypot(s0.x - s1.x, s0.y - s1.y) < (touch() ? 24 : 10)) finishShape();
            else drawPts_.push_back(*p);
        }
    }
    // Painting: on the model under the mouse.
    void paintViewport(ImGuiIO& io) {
        if (sculpting_) return sculptViewport(io);
        if (paint_.waiting) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) paint_.waiting = false;
            else if (pickObject(mouseRay(io.MousePos), 0, true).object) paint_.waiting = false, paintAt(io.MousePos, true);
            return;
        }
        if (paint_.active) {
            if (paint_.in2D) return;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return endStroke();
            return paintAt(io.MousePos, false);
        }
        if (PictureLayer* l = editLayer()) { // (a picture being moved: on the model if it's a sticker)
            if (l->sticker) stickerMouse(io);
            return;
        }
        if (!mouseInView(io) || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
        const Ray ray = mouseRay(io.MousePos);
        const Hit h = pickObject(ray, 0, true);
        if (!h.object) {
            paint_.waiting = paintTool_ == PaintTool::Brush || paintTool_ == PaintTool::Eraser;
            return;
        }
        const bool picking = paintTool_ == PaintTool::Picker || io.KeyAlt;
        if (paintTool_ == PaintTool::Picture) {
            selected_ = {h.object}, active_ = h.object;
            return;
        }
        if (picking) {
            const Object* o = find(h.object);
            if (o->picture) {
                if (auto uv = uvUnder(*o, ray)) pickColor(*o, *uv);
            } else
                paintColor_ = glm::vec4(o->color, paintColor_.a);
            return;
        }
        if (paintTool_ == PaintTool::Fill) {
            selected_ = {h.object}, active_ = h.object;
            Object* o = readyToPaint(h.object);
            if (!o) return;
            if (auto uv = uvUnder(*o, ray)) fillAt(*o, *uv);
            return;
        }
        paintAt(io.MousePos, true);
    }

    // The brush: whatever passes under it is selected (or, with Ctrl, let go).
    void paint(const ImGuiIO& io) {
        if (mode_ == Mode::Object) {
            const Hit h = pickObject(mouseRay(io.MousePos));
            if (h.object) io.KeyCtrl ? (void)selected_.erase(h.object) : (void)(selected_.insert(h.object), active_ = h.object);
            return;
        }
        const ImVec2 m = io.MousePos;
        const float r = brushRadius_;
        regionSelect([&](ImVec2 s) { return std::hypot(s.x - m.x, s.y - m.y) < r; }, true, io.KeyCtrl, false);
    }

    // ───────────────────────────── keys ─────────────────────────────

    void shortcuts(ImGuiIO& io) {
        if (io.WantTextInput) return;
        const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
        auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
        if (key(ImGuiKey_F5)) return runScriptNow();
        if (ctrl && key(ImGuiKey_1)) setTab(Tab::Model);
        if (ctrl && key(ImGuiKey_2)) setTab(Tab::Paint);
        if (ctrl && key(ImGuiKey_3)) setTab(Tab::Layout);
        if (tab_ == Tab::Paint && !ctrl && !alt && key(ImGuiKey_Tab)) return (void)(sculpting_ = !sculpting_, endStroke(), endSculpt());
        if (tab_ == Tab::Paint && sculpting_ && !ctrl && !alt)
            for (int i = 0; i < int(std::size(kSculptTools)); ++i)
                if (key(ImGuiKey(int(ImGuiKey_1) + i))) sculptBrush_ = kSculptTools[i].brush;
        if (tab_ == Tab::Paint && sculpting_ && !ctrl && !alt && key(ImGuiKey_X)) sculptMirror_ = !sculptMirror_;
        if (tab_ == Tab::Paint && !ctrl && !alt) {
            if (key(ImGuiKey_B)) paintTool_ = PaintTool::Brush;
            if (key(ImGuiKey_E)) paintTool_ = PaintTool::Eraser;
            if (key(ImGuiKey_G)) paintTool_ = PaintTool::Fill;
            if (key(ImGuiKey_I)) paintTool_ = PaintTool::Picker;
            float& size = sculpting_ ? sculptPx_ : brushPx_;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) size = std::max(1.0f, size / 1.2f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) size = std::min(500.0f, size * 1.2f);
            if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) lockLayer();
            if (key(ImGuiKey_Delete) && editLayer()) deleteLayer(*find(ledit_.object), ledit_.index);
            if (key(ImGuiKey_F1)) showHelp_ = !showHelp_;
            if (key(ImGuiKey_F12)) dialog(5);
            if (key(ImGuiKey_F)) frameSelected();
            if (key(ImGuiKey_Home)) frameAll();
            if (key(ImGuiKey_Z)) shading_ = shading_ == Shading::Solid ? Shading::Rendered : Shading::Solid;
            return;
        }
        if (tab_ == Tab::Paint && ctrl && key(ImGuiKey_V)) return pasteFromClipboard();
        if (tab_ == Tab::Paint && ctrl && key(ImGuiKey_Z) && editLayer() && ledit_.fresh) return cancelFloating();
        if (ctrl && key(ImGuiKey_4)) setTab(Tab::Render);
        if (ctrl && key(ImGuiKey_5)) setTab(Tab::Print);
        if (tab_ == Tab::Layout && !ctrl && !alt) {
            if (key(ImGuiKey_Q)) layTool_ = LayTool::Select;
            if (key(ImGuiKey_W)) layTool_ = LayTool::Move;
            if (key(ImGuiKey_E)) layTool_ = LayTool::Rotate;
            if (key(ImGuiKey_R)) layTool_ = LayTool::Scale;
            if (key(ImGuiKey_Tab)) lay_.corners = !lay_.corners, lay_.frames.clear(), lay_.points.clear();
            if (key(ImGuiKey_F1)) showHelp_ = !showHelp_;
            if (key(ImGuiKey_F)) frameSelected();
            if (key(ImGuiKey_Home)) frameAll();
            return;
        }
        if (tab_ == Tab::Layout && ctrl && key(ImGuiKey_A)) return layoutSelectAll();
        if (tab_ == Tab::Print && !ctrl && !alt) {
            if (key(ImGuiKey_M)) printTool_ = printTool_ == PrintTool::Measure ? PrintTool::None : PrintTool::Measure, measure_.clear();
            if (key(ImGuiKey_L)) printTool_ = printTool_ == PrintTool::LayFlat ? PrintTool::None : PrintTool::LayFlat;
            if (key(ImGuiKey_B)) putOnBed();
            if (key(ImGuiKey_Q) || key(ImGuiKey_W) || key(ImGuiKey_E) || key(ImGuiKey_R)) printTool_ = PrintTool::None;
        }
        if ((tab_ == Tab::Render || tab_ == Tab::Print) && !ctrl && !alt) {
            if (key(ImGuiKey_Q)) setTool(Tool::Select);
            if (key(ImGuiKey_W)) setTool(Tool::Move);
            if (key(ImGuiKey_E)) setTool(Tool::Rotate);
            if (key(ImGuiKey_R)) setTool(Tool::Scale);
            if (key(ImGuiKey_F1)) showHelp_ = !showHelp_;
            if (key(ImGuiKey_F12)) dialog(5);
            if (key(ImGuiKey_F)) frameSelected();
            if (key(ImGuiKey_Home)) frameAll();
            if (key(ImGuiKey_Delete)) deleteSelected();
            return;
        }
        if (ctrl) {
            if (key(ImGuiKey_Z)) shift ? redo() : undo();
            if (key(ImGuiKey_Y)) redo();
            if (key(ImGuiKey_S)) shift || path_.empty() ? dialog(2) : void(saveFile(path_));
            if (key(ImGuiKey_O)) whenSaved(3);
            if (key(ImGuiKey_N)) whenSaved(2);
            if (key(ImGuiKey_E)) dialog(3);
            if (key(ImGuiKey_Q)) whenSaved(1);
            if (key(ImGuiKey_D)) duplicateSelected();
            if (key(ImGuiKey_A)) selectAll();
            if (key(ImGuiKey_I)) invertSelection();
            if (key(ImGuiKey_J)) joinSelected();
            if (key(ImGuiKey_G)) groupSelected();
            if (shift && (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd))) return combineSelected(Combine::Add);
            if (shift && (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract))) return combineSelected(Combine::Subtract);
            if (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) growSelection();
            if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) shrinkSelection();
            return;
        }
        if (alt) {
            if (key(ImGuiKey_S) && mode_ == Mode::Edit) startPush();
            if (key(ImGuiKey_A)) selectNone();
            if (key(ImGuiKey_Z)) xray_ = !xray_;
            if (key(ImGuiKey_H))
                for (Object& o : scene_.objects) o.visible = true;
            return;
        }
        if (key(ImGuiKey_F1)) showHelp_ = !showHelp_;
        if (key(ImGuiKey_F12)) dialog(5);
        if (key(ImGuiKey_Q)) setTool(Tool::Select);
        if (key(ImGuiKey_W)) setTool(Tool::Move);
        if (key(ImGuiKey_E)) setTool(Tool::Rotate);
        if (key(ImGuiKey_R)) setTool(Tool::Scale);
        if (key(ImGuiKey_D)) setTool(Tool::Draw);
        if (key(ImGuiKey_X)) setTool(Tool::Extrude);
        if (key(ImGuiKey_I)) setTool(Tool::Inset);
        if (key(ImGuiKey_B)) setTool(Tool::Bevel);
        if (key(ImGuiKey_C)) setTool(Tool::LoopCut);
        if (key(ImGuiKey_K)) setTool(Tool::Knife);
        if (key(ImGuiKey_O) && !shift) setTool(Tool::Hole);
        if (key(ImGuiKey_Tab)) mode_ == Mode::Edit ? leaveEdit() : enterEdit();
        if (key(ImGuiKey_Delete) || key(ImGuiKey_Backspace)) deleteSelected();
        if (key(ImGuiKey_F)) frameSelected();
        if (key(ImGuiKey_Home)) frameAll();
        if (key(ImGuiKey_Z)) shading_ = shift ? Shading::Wire : shading_ == Shading::Solid ? Shading::Rendered : Shading::Solid;
        if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) {
            if (tool_ == Tool::Draw) finishShape();
            if (tool_ == Tool::Knife && knifePts_.size() >= 2) {
                std::vector<ImVec2> pts = knifePts_;
                knifePts_.clear();
                knifeApply(active_, pts);
            }
        }
        if (mode_ == Mode::Edit) {
            if (key(ImGuiKey_M)) mergeNow();
            if (key(ImGuiKey_L)) selectLinked();
            if (key(ImGuiKey_P)) separateSelected();
            if (key(ImGuiKey_1)) elem_ = Elem::Vertex, clearElements();
            if (key(ImGuiKey_2)) elem_ = Elem::Edge, clearElements();
            if (key(ImGuiKey_3)) elem_ = Elem::Face, clearElements();
        }
        if (key(ImGuiKey_H) && shift && !selected_.empty()) showOnly(selected_); // (the rest hidden)
        else if (key(ImGuiKey_H))
            for (Object& o : scene_.objects)
                if (selected_.count(o.id) && mode_ == Mode::Object) o.visible = false;
        if ((key(ImGuiKey_Slash) || key(ImGuiKey_KeypadDivide)) && mode_ == Mode::Object && !selected_.empty()) showOnly(selected_);
        if (key(ImGuiKey_O) && shift) softSel_ = !softSel_, say(softSel_ ? "Soft selection on: points near follow (the wheel while dragging: how far)" : "Soft selection off");
        if (key(ImGuiKey_Keypad1)) setView(shift ? View::Back : View::Front);
        if (key(ImGuiKey_Keypad3)) setView(shift ? View::Right : View::Left);
        if (key(ImGuiKey_Keypad7)) setView(shift ? View::Bottom : View::Top);
        if (key(ImGuiKey_Keypad5)) view_ == View::Perspective ? setView(View::Front) : leaveFlatView();
    }

    // ───────────────────────────── 3D ─────────────────────────────

#if defined(__ANDROID__)
    static constexpr int kRayLight = 0; // (light bounced by ray tracing: not on phones)
#else
    static constexpr int kRayLight = 1;
#endif
    // Solid: a plain grey room and soft studio light, no shadows or effects — for seeing shapes.
    // Rendered: sky, sun, shadows and effects.
    void applyShading() {
        const Look& k = scene_.look;
        const std::string want =
            shading() == Shading::Rendered
                ? std::format("r_ssao {}; r_bloom {}; r_vignette {}; r_exposure {}; r_saturation {}; r_sharpen {}; r_shadowsoft {}; r_gi {}; r_fog 0; r_skycolor \"{} {} {}\"", k.ao,
                              k.bloom, k.vignette, k.exposure, k.saturation, k.sharpen, k.shadowSoft, kRayLight, k.background.r, k.background.g, k.background.b)
                : std::string("r_ssao 0; r_bloom 0; r_vignette 0; r_exposure 1; r_saturation 1; r_sharpen 0; r_gi 0; r_fog 0; r_skycolor \"0.2 0.2 0.215\"");
        if (want == appliedLook_) return;
        appliedLook_ = want;
        keel::execute(want);
    }
    // Painted, and shown as painted (with rounding or smoothing added since, it isn't).
    bool painted(const Object& o) const { return o.picture && o.uvsFit() && !o.hasFinish(); }

    keel::ModelId modelOf(const Object& o) {
        const bool editingThis = mode_ == Mode::Edit && o.id == active_;
        const uint64_t want = (versions_[o.id] * 2 + editingThis) * 2 + painted(o);
        auto it = models_.find(o.id);
        if (it != models_.end() && it->second.second == want && versions_[o.id]) return it->second.first;
        const Mesh& mesh = shownMesh(o);
        keel::KmdlModel km;
        keel::KmdlMaterial mat{};
        mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = mat.baseColor[3] = 1.0f;
        const bool withPicture = painted(o);
        if (withPicture) std::snprintf(mat.texture, sizeof mat.texture, "fry:paint:%u", o.id);
        km.materials.push_back(mat);
        const auto normals = cornerNormals(mesh, o.smooth, o.smoothAngle);
        for (size_t f = 0; f < mesh.faces.size(); ++f) {
            const auto& face = mesh.faces[f];
            for (const auto& tri : triangulate(mesh, int(f)))
                for (int k = 0; k < 3; ++k) {
                    const size_t corner = size_t(std::find(face.begin(), face.end(), tri[size_t(k)]) - face.begin());
                    const glm::vec3 p = mesh.verts[size_t(tri[size_t(k)])], n = normals[f][corner];
                    km.indices.push_back(uint32_t(km.vertices.size()));
                    const glm::vec2 uv = withPicture ? o.uvs[f][corner] : glm::vec2(0.0f);
                    km.vertices.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, {uv.x, uv.y}, {0, 0}});
                }
        }
        if (km.vertices.empty()) km.vertices.push_back({}), km.indices = {0, 0, 0};
        km.submeshes.push_back({0, uint32_t(km.indices.size()), 0});
        const glm::vec3 lo = boundsMin(mesh), hi = boundsMax(mesh);
        std::copy_n(&lo.x, 3, km.header.boundsMin);
        std::copy_n(&hi.x, 3, km.header.boundsMax);
        const keel::ModelId id = r_->updateModel(std::format("fry:object{}", o.id), km);
        models_[o.id] = {id, want};
        return id;
    }

    const Picture& picture(const Reference& ref) {
        auto it = pictures_.find(ref.path);
        if (it != pictures_.end()) return it->second;
        Picture& p = pictures_[ref.path];
        int w = 0, h = 0, n = 0;
        if (stbi_uc* px = stbi_load(ref.path.c_str(), &w, &h, &n, 4)) {
            r_->createTexture("fry:picture:" + ref.path, uint32_t(w), uint32_t(h), px);
            stbi_image_free(px);
            p.ok = true, p.aspect = float(h) / float(std::max(w, 1));
        } else {
            say("Couldn't load the picture " + ref.path);
        }
        return p;
    }

    void loadLogo() {
        if (logoTried_) return;
        logoTried_ = true;
        if (auto png = keel::fs::read("branding/logo.png")) {
            int w = 0, h = 0, n = 0;
            if (stbi_uc* px = stbi_load_from_memory(png->data(), int(png->size()), &w, &h, &n, 4)) {
                logo_ = r_->createTexture("fry:logo", uint32_t(w), uint32_t(h), px);
                stbi_image_free(px);
                haveLogo_ = true;
            }
        }
    }
    void logoImage(float size) {
        if (!haveLogo_) return;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, size));
        uiImage(ImGui::GetWindowDrawList(), logo_, at, ImVec2(at.x + size, at.y + size));
    }
    // A picture of ours in the UI, read the way ImGui's colours are (not turned linear: too dark).
    void uiImage(ImDrawList* dl, keel::TextureId t, ImVec2 a, ImVec2 b) {
        const keel::UiRawRead raw = r_->uiRawRead(t);
        if (raw.begin) dl->AddCallback(raw.begin, raw.data);
        dl->AddImage(ImTextureID(r_->uiTexture(t)), a, b);
        if (raw.end) dl->AddCallback(raw.end, raw.data);
    }

    float gridStep() const { return std::pow(10.0f, std::floor(std::log10(std::max(viewHeight() / 4.0f, 1e-4f)))); }

    void draw3D(keel::Renderer& r) {
        keel::Camera cam;
        cam.origin = eye_ * kUnits, cam.yaw = yaw_, cam.pitch = pitch_, cam.fovY = fov_, cam.shift = shift_;
        r.setCamera(cam);
        const Look& look = scene_.look;
        const bool rendered = shading() == Shading::Rendered;
        const float turn = glm::radians(look.sunTurn), up = glm::radians(look.sunHeight);
        r.setSun(rendered && look.sunStrength > 0, glm::vec3(std::cos(up) * std::cos(turn), std::cos(up) * std::sin(turn), std::sin(up)), look.sunColor * look.sunStrength, true);
        keel::Renderer::Atmosphere atm;
        atm.sky = rendered && look.sky ? "skies/day.ksky" : "";
        atm.fogDensity = 0.0f;
        r.setAtmosphere(atm);
        // Rendered: light from the sky all round. Solid: a studio's — bright from above and the
        // front-left, darker below, so every side of a shape reads differently.
        const keel::ModelLighting light =
            rendered ? keel::ModelLighting{{{0.22f, 0.22f, 0.24f}, {0.13f, 0.13f, 0.15f}, {0.2f, 0.2f, 0.22f}, {0.16f, 0.16f, 0.18f}, {0.3f, 0.31f, 0.35f}, {0.06f, 0.06f, 0.06f}}, 1.0f}
                     : keel::ModelLighting{{{0.5f, 0.49f, 0.47f}, {0.2f, 0.21f, 0.24f}, {0.72f, 0.71f, 0.69f}, {0.3f, 0.31f, 0.34f}, {0.95f, 0.95f, 0.95f}, {0.08f, 0.08f, 0.09f}}, 0.0f};
        const glm::mat4 toUnits = glm::scale(glm::mat4(1.0f), glm::vec3(kUnits));
        // Painted pictures: laid out again if the shape's faces changed, and sent to the card.
        for (Object& o : scene_.objects)
            if (o.picture && !o.uvsFit() && !o.hasFinish()) o.uvs = autoUnwrap(o.mesh), touched(o.id);
        for (const Object& o : scene_.objects)
            if (o.picture) uploadPicture(o, !(paint_.active && paint_.object == o.id));
        if (shading() != Shading::Wire)
            for (const Object& o : scene_.objects) {
                if (!o.visible) continue;
                const glm::vec4 tint = painted(o) ? glm::vec4(1.0f) : glm::vec4(glm::pow(o.color, glm::vec3(2.2f)), 1.0f); // (colours are picked as seen: sRGB)
                const keel::Surface surface{o.metal, o.rough};
                r.drawModel(modelOf(o), toUnits * objectMatrix(o), tint, nullptr, &light, rendered, nullptr, keel::LiquidStyle::None, &surface);
            }
        // Rendered: a ground to stand on and catch the shadows (not part of what you make).
        if (rendered && look.ground) {
            if (!haveGround_) {
                haveGround_ = true;
                keel::KmdlModel km;
                keel::KmdlMaterial mat{};
                mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = mat.baseColor[3] = 1.0f;
                km.materials.push_back(mat);
                const float h = 500.0f;
                for (glm::vec2 c : {glm::vec2(-h, -h), glm::vec2(h, -h), glm::vec2(h, h), glm::vec2(-h, h)}) km.vertices.push_back({{c.x, c.y, -0.0005f}, {0, 0, 1}, {0, 0}, {0, 0}});
                km.indices = {0, 1, 2, 0, 2, 3};
                km.submeshes.push_back({0, 6, 0});
                ground_ = r.updateModel("fry:ground", km);
            }
            r.drawModel(ground_, toUnits, glm::vec4(glm::pow(look.groundColor, glm::vec3(2.2f)), 1.0f), nullptr, &light, false);
        }
        // Reference pictures: flat, see-through, a little behind the plane through the middle.
        for (const Reference& ref : scene_.references) {
            if (!ref.visible) continue;
            const Picture& pic = picture(ref);
            if (!pic.ok) continue;
            const auto c = referenceCorners(ref, pic.aspect);
            const glm::vec3 n = glm::normalize(glm::cross(c[1] - c[0], c[3] - c[0]));
            keel::KmdlModel km;
            keel::KmdlMaterial mat{};
            std::snprintf(mat.texture, sizeof mat.texture, "fry:picture:%s", ref.path.c_str());
            mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = 1.0f, mat.baseColor[3] = 1.0f;
            km.materials.push_back(mat);
            const glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
            for (int side = 0; side < 2; ++side) // (both sides: seen from behind too)
                for (int k : {0, 1, 2, 0, 2, 3}) {
                    const int i = side ? (3 - k + 1) % 4 : k;
                    const glm::vec3 p = c[size_t(i)] - n * 0.001f, nn = side ? -n : n;
                    km.indices.push_back(uint32_t(km.vertices.size()));
                    km.vertices.push_back({{p.x, p.y, p.z}, {nn.x, nn.y, nn.z}, {uv[i].x, uv[i].y}, {0, 0}});
                }
            km.submeshes.push_back({0, uint32_t(km.indices.size()), 0});
            const keel::ModelId id = r.updateModel(std::format("fry:ref{}", ref.id), km);
            const keel::ModelLighting flat{{glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f)}, 0.0f};
            r.drawModel(id, toUnits, glm::vec4(1.0f, 1.0f, 1.0f, ref.opacity), nullptr, &flat, false);
        }
        // The grid on the work plane (the ground, or what a flat view looks at), and the axes.
        if (tab_ == Tab::Render ? renderGrid_ : showGrid_) {
            glm::vec3 p, n;
            workPlane(p, n);
            const glm::vec3 u = n.z > 0.5f ? glm::vec3(1, 0, 0) : n.x > 0.5f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            const glm::vec3 v = glm::cross(n, u);
            const float step = gridStep();
            const int count = 20;
            const glm::vec3 centre = target_ - n * glm::dot(target_, n);
            const float cu = snapTo(glm::dot(centre, u), step * 10), cv = snapTo(glm::dot(centre, v), step * 10);
            const float shade = rendered ? 0.0f : 1.0f; // (dark lines on the sunny ground, light ones in the grey room)
            for (int i = -count; i <= count; ++i)
                for (int dir = 0; dir < 2; ++dir) {
                    const glm::vec3 along = dir ? u : v, across = dir ? v : u;
                    const float at = (dir ? cv : cu) + float(i) * step, mid = dir ? cu : cv;
                    const glm::vec3 a = across * at + along * (mid - count * step), b = across * at + along * (mid + count * step);
                    const bool major = std::abs(snapTo(at, step * 10) - at) < step * 0.01f, axis = std::abs(at) < step * 0.01f;
                    glm::vec4 col = glm::vec4(glm::vec3(shade), major ? (rendered ? 0.35f : 0.13f) : (rendered ? 0.16f : 0.06f));
                    if (axis) col = dir ? (n.x > 0.5f ? glm::vec4(0.3f, 0.75f, 0.25f, 0.9f) : glm::vec4(0.9f, 0.25f, 0.25f, 0.9f))
                                        : (n.z > 0.5f ? glm::vec4(0.3f, 0.75f, 0.25f, 0.9f) : glm::vec4(0.25f, 0.45f, 0.95f, 0.9f));
                    const float w = pixelSize(target_) * (major || axis ? 1.6f : 1.0f) * kUnits;
                    r.drawStreak(a * kUnits, b * kUnits, w, col, keel::kWhiteTexture);
                }
        }
        if (tab_ == Tab::Print) drawPrintBed(r, light);
    }

    // (the Print tab's: the printer's plate and overhangs)
    // On top of the scene: outlines and wires, points, the gizmo, the selection shapes.
    void overlays() {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->PushClipRect(viewMin_, viewMax_);
        const ImGuiIO& io = ImGui::GetIO();
        // Every edge of a mesh (X-ray, wireframe), or just the edges the eye can see.
        auto lines = [&](const Object& o, const Mesh& mesh, ImU32 col, float width, bool seenOnly) {
            const glm::mat4 m = objectMatrix(o);
            const std::vector<char>* vis = seenOnly && !xray_ ? &visiblePoints(o) : nullptr;
            std::vector<ImVec2> s(mesh.verts.size());
            std::vector<char> ok(mesh.verts.size());
            for (size_t i = 0; i < s.size(); ++i) ok[i] = project(glm::vec3(m * glm::vec4(mesh.verts[i], 1.0f)), s[i]);
            for (auto [a, b] : edges(mesh)) {
                if (vis && !((*vis)[size_t(a)] && (*vis)[size_t(b)])) continue;
                if (ok[size_t(a)] && ok[size_t(b)]) dl->AddLine(s[size_t(a)], s[size_t(b)], col, width);
            }
        };
        // The outline round a selected object (where it turns away from you, and open edges).
        auto outline = [&](const Object& o, ImU32 col, float width) {
            const Mesh& mesh = shownMesh(o);
            const glm::mat4 m = objectMatrix(o);
            std::vector<glm::vec3> w(mesh.verts.size());
            for (size_t i = 0; i < w.size(); ++i) w[i] = glm::vec3(m * glm::vec4(mesh.verts[i], 1.0f));
            std::map<std::pair<int, int>, int> side; // bit 1: a face towards you, bit 2: away; + 4 each face
            for (size_t f = 0; f < mesh.faces.size(); ++f) {
                const auto& fv = mesh.faces[f];
                glm::vec3 n(0.0f);
                for (size_t i = 0; i < fv.size(); ++i) n += glm::cross(w[size_t(fv[i])], w[size_t(fv[(i + 1) % fv.size()])]);
                const int facing = glm::dot(n, eye_ - w[size_t(fv[0])]) > 0 ? 1 : 2;
                for (size_t i = 0; i < fv.size(); ++i) {
                    int& s = side[{std::min(fv[i], fv[(i + 1) % fv.size()]), std::max(fv[i], fv[(i + 1) % fv.size()])}];
                    s = (s | facing) + 4;
                }
            }
            const std::vector<char>* vis = xray_ ? nullptr : &visiblePoints(o, mesh); // (not the edges of a pocket behind its rim)
            for (auto [e, s] : side) {
                const bool silhouette = (s & 3) == 3, open = (s >> 2) == 1 && (s & 1);
                if (vis && !((*vis)[size_t(e.first)] && (*vis)[size_t(e.second)])) continue;
                ImVec2 a, b;
                if ((silhouette || open) && project(w[size_t(e.first)], a) && project(w[size_t(e.second)], b)) dl->AddLine(a, b, col, width);
            }
        };
        const bool clean = capturing(); // (a picture being saved: no outlines or gizmos)
        Object* ed = editing();
        for (const Object& o : scene_.objects) {
            if (!o.visible) continue;
            const bool sel = selected_.count(o.id) > 0 && !clean && tab_ != Tab::Paint;
            if (ed && o.id == ed->id) continue;
            if (shading() == Shading::Wire || xray_) lines(o, shownMesh(o), sel ? kAccent : IM_COL32(200, 200, 205, 150), sel ? 1.6f : 1.0f, false);
            else if (sel) outline(o, kAccent, 2.5f);
        }
        if (tab_ == Tab::Paint) {
            if (Object* so = find(ledit_.object); so && editLayer() && editLayer()->sticker) { // the sticker's frame and handles
                std::array<glm::vec3, 6> hw;
                ImVec2 q[6];
                bool ok = stickerHandles(*so, *editLayer(), hw);
                for (int i = 0; ok && i < 6; ++i) ok = project(hw[size_t(i)], q[i]);
                if (ok) {
                    const int hot = ledit_.drag >= 0 ? ledit_.drag : stickerHandle(*so, *editLayer(), io.MousePos);
                    dl->AddPolyline(q, 4, hot == 4 ? kHot : kAccent, ImDrawFlags_Closed, 2.0f);
                    const ImVec2 top((q[0].x + q[1].x) * 0.5f, (q[0].y + q[1].y) * 0.5f);
                    dl->AddLine(top, q[4], kAccent, 1.5f);
                    dl->AddCircleFilled(q[4], 7.0f, hot == 5 ? kHot : IM_COL32(255, 255, 255, 240));
                    dl->AddCircle(q[4], 7.0f, IM_COL32(20, 20, 25, 255));
                    for (int i = 0; i < 4; ++i) {
                        dl->AddRectFilled(ImVec2(q[i].x - 6, q[i].y - 6), ImVec2(q[i].x + 6, q[i].y + 6), hot == i ? kHot : IM_COL32(255, 255, 255, 240));
                        dl->AddRect(ImVec2(q[i].x - 6, q[i].y - 6), ImVec2(q[i].x + 6, q[i].y + 6), IM_COL32(20, 20, 25, 255));
                    }
                }
            }
            if (sculpting_) {
                if (mouseInView(io)) dl->AddCircle(io.MousePos, sculptPx_, io.KeyCtrl ? IM_COL32(120, 180, 255, 220) : IM_COL32(255, 255, 255, 200), 40, 1.5f);
            } else if (mouseInView(io) && !editLayer() && paintTool_ != PaintTool::Picture)
                dl->AddCircle(io.MousePos, brushPx_, paintTool_ == PaintTool::Fill || paintTool_ == PaintTool::Picker ? IM_COL32(255, 255, 255, 90) : IM_COL32(255, 255, 255, 200), 40, 1.5f);
            dl->PopClipRect();
            return;
        }
        if (tab_ == Tab::Layout) {
            layoutOverlay(dl);
            dl->PopClipRect();
            return;
        }
        if (tab_ == Tab::Print && !clean) printOverlay(dl, io);
        if (clean) {
            dl->PopClipRect();
            return;
        }
        if (ed) {
            if (ed->hasFinish() && ed->mirror && shading() != Shading::Wire) // (the mirrored half: faint)
                lines(*ed, shownMesh(*ed), IM_COL32(20, 20, 25, 70), 1.0f, false);
            lines(*ed, ed->mesh, shading() == Shading::Wire ? IM_COL32(230, 230, 235, 230) : IM_COL32(12, 12, 15, 235), 1.3f, true);
        }
        if (Object* o = ed) {
            const glm::mat4 m = objectMatrix(*o);
            if (elem_ == Elem::Vertex) {
                for (size_t v = 0; v < o->mesh.verts.size(); ++v) {
                    ImVec2 s;
                    const bool sel = selVerts_.count(int(v));
                    if (!project(glm::vec3(m * glm::vec4(o->mesh.verts[v], 1.0f)), s) || (!sel && !pointSeen(*o, int(v)))) continue;
                    dl->AddRectFilled(ImVec2(s.x - 3, s.y - 3), ImVec2(s.x + 3, s.y + 3), sel ? kAccent : IM_COL32(15, 15, 18, 255));
                }
            } else if (elem_ == Elem::Edge) {
                for (auto [a, b] : selEdges_) {
                    ImVec2 sa, sb;
                    if (a < int(o->mesh.verts.size()) && b < int(o->mesh.verts.size()) && project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(a)], 1.0f)), sa) &&
                        project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(b)], 1.0f)), sb))
                        dl->AddLine(sa, sb, kAccent, 3.0f);
                }
            } else { // the selected faces the eye can see: an orange skin, a dot in the middle
                const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m)));
                for (int f : selFaces_) {
                    if (f >= int(o->mesh.faces.size())) continue;
                    const glm::vec3 c(m * glm::vec4(faceCenter(o->mesh, f), 1.0f));
                    const bool front = glm::dot(nm * faceNormal(o->mesh, f), eye_ - c) > 0;
                    if (!xray_ && !front) continue;
                    const auto& fv = o->mesh.faces[size_t(f)];
                    if (!xray_ && !std::all_of(fv.begin(), fv.end(), [&](int v) { return pointSeen(*o, v); }) && !visible(*o, c)) continue;
                    std::vector<ImVec2> poly(fv.size());
                    bool ok = true;
                    for (size_t k = 0; k < fv.size(); ++k) ok &= project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(fv[k])], 1.0f)), poly[k]);
                    if (ok) dl->AddConcavePolyFilled(poly.data(), int(poly.size()), IM_COL32(255, 120, 40, 70));
                    ImVec2 s;
                    if (project(c, s)) dl->AddCircleFilled(s, 3.5f, kAccent);
                }
            }
            // The loop cut to be: across each face of the ring.
            if (tool_ == Tool::LoopCut && cutHover_.first >= 0)
                for (const auto& q : ringQuads(o->mesh, cutHover_))
                    for (int c = 1; c <= cuts_; ++c) {
                        const float t = float(c) / float(cuts_ + 1);
                        const glm::vec3 a = glm::mix(o->mesh.verts[size_t(q[0])], o->mesh.verts[size_t(q[1])], t), b = glm::mix(o->mesh.verts[size_t(q[3])], o->mesh.verts[size_t(q[2])], t);
                        ImVec2 sa, sb;
                        if (project(glm::vec3(m * glm::vec4(a, 1.0f)), sa) && project(glm::vec3(m * glm::vec4(b, 1.0f)), sb)) dl->AddLine(sa, sb, kHot, 2.5f);
                    }
        }
        // The knife's line: its points, on to the mouse; cutting through, on past the ends.
        if (tool_ == Tool::Knife && (!knifePts_.empty() || knifeDown_)) {
            std::vector<ImVec2> pts = knifePts_;
            if (knifePts_.empty()) pts.push_back(knifePress_);
            if (mouseInView(io) || knifeDown_) pts.push_back(knifeEnd(io));
            for (size_t i = 0; i + 1 < pts.size(); ++i) dl->AddLine(pts[i], pts[i + 1], kHot, 2.0f);
            if (knifeThrough_ && pts.size() >= 2)
                for (int end = 0; end < 2; ++end) {
                    const ImVec2 a = end ? pts.back() : pts.front(), b = end ? pts[pts.size() - 2] : pts[1];
                    const float dx = a.x - b.x, dy = a.y - b.y, l = std::max(std::hypot(dx, dy), 1e-3f);
                    for (int k = 0; k < 60; k += 2)
                        dl->AddLine(ImVec2(a.x + dx / l * float(k) * 12, a.y + dy / l * float(k) * 12), ImVec2(a.x + dx / l * float(k + 1) * 12, a.y + dy / l * float(k + 1) * 12),
                                    (kHot & 0x00FFFFFF) | 0x90000000, 1.5f);
                }
            for (const ImVec2& p : knifePts_) dl->AddCircleFilled(p, 4.0f, kHot);
        }
        // The hole to be.
        if (tool_ == Tool::Hole && (mouseInView(io) || hole_.active)) {
            const auto ring = holePreview(io);
            std::vector<ImVec2> pts;
            for (const glm::vec3& w : ring) {
                ImVec2 sp;
                if (project(w, sp)) pts.push_back(sp);
            }
            if (pts.size() >= 3) dl->AddPolyline(pts.data(), int(pts.size()), hole_.active ? kHot : IM_COL32(255, 255, 255, 200), ImDrawFlags_Closed, 2.0f);
        }
        if (boxing_ && !touchOrbit_ && std::hypot(io.MousePos.x - boxStart_.x, io.MousePos.y - boxStart_.y) > 4) {
            dl->AddRectFilled(boxStart_, io.MousePos, IM_COL32(255, 140, 60, 30));
            dl->AddRect(boxStart_, io.MousePos, kAccent);
        }
        if (lassoing_ && lasso_.size() > 1) dl->AddPolyline(lasso_.data(), int(lasso_.size()), kAccent, ImDrawFlags_Closed, 1.5f);
        if (selShape_ == SelShape::Brush && tool_ == Tool::Select && mode_ == Mode::Edit && mouseInView(io))
            dl->AddCircle(io.MousePos, brushRadius_, painting_ ? kAccent : IM_COL32(255, 255, 255, 160), 32, 1.5f);
        if (tool_ == Tool::Draw) {
            ImVec2 prev, s;
            for (size_t i = 0; i < drawPts_.size(); ++i) {
                if (!project(drawPts_[i], s)) continue;
                if (i) dl->AddLine(prev, s, kAccent, 2.0f);
                dl->AddCircleFilled(s, i == 0 ? 6.0f : 4.0f, i == 0 ? IM_COL32(255, 255, 255, 255) : kAccent);
                prev = s;
            }
            if (mouseInView(io) && !touch()) // (a finger isn't hovering anywhere)
                if (auto p = drawPoint(io); p && project(*p, s)) {
                    if (!drawPts_.empty()) dl->AddLine(prev, s, IM_COL32(255, 160, 90, 180), 1.5f);
                    dl->AddCircle(s, 5.0f, IM_COL32(255, 255, 255, 220), 0, 1.5f);
                }
        }
        gizmo(dl, io);
        dl->PopClipRect();
    }

    void gizmo(ImDrawList* dl, const ImGuiIO& io) {
        const int hot = mouseInView(io) || drag_.active ? hoverHandle(io.MousePos) : -1;
        if (softSel_ && mode_ == Mode::Edit && (tool_ == Tool::Move || tool_ == Tool::Rotate || tool_ == Tool::Scale) && !selectedPoints().empty()) {
            const glm::vec3 at = drag_.active ? drag_.pivot : pivotOfSelection();
            ImVec2 c;
            if (project(at, c)) dl->AddCircle(c, softReach_ / std::max(pixelSize(at), 1e-9f), keel::ui::accent(0.8f), 64, 1.5f);
        }
        if (tool_ == Tool::Extrude || tool_ == Tool::Inset) {
            glm::vec3 at, n;
            if (!faceHandle(at, n)) return;
            ImVec2 a, b;
            if (!project(at, a) || !project(at + n * pixelSize(at) * 90.0f, b)) return;
            const ImU32 col = hot == 0 ? kHot : IM_COL32(255, 255, 255, 255);
            dl->AddLine(a, b, col, 3.0f);
            if (tool_ == Tool::Extrude) dl->AddCircleFilled(b, 7.0f, col);
            else dl->AddRectFilled(ImVec2(b.x - 6, b.y - 6), ImVec2(b.x + 6, b.y + 6), col);
            return;
        }
        if (tool_ == Tool::Bevel) {
            ImVec2 c, h;
            if (!bevelHandle(c, h)) return;
            const ImU32 col = hot == 0 ? kHot : IM_COL32(255, 255, 255, 255);
            dl->AddLine(c, h, (col & 0x00FFFFFF) | 0x90000000, 2.0f);
            dl->AddCircleFilled(h, 9.0f, col);
            drawIcon(dl, Icon::Bevel, ImVec2(h.x - 7, h.y - 7), 14.0f, IM_COL32(30, 30, 35, 255));
            return;
        }
        if (tool_ != Tool::Move && tool_ != Tool::Rotate && tool_ != Tool::Scale) return;
        if (!hasSelection()) return;
        const Axes axes = drag_.active ? drag_.axes : gizmoAxes();
        if (tool_ == Tool::Scale) { // the box round the selection, a handle on each side
            const auto [lo, hi] = selectionBox(axes);
            for (int e = 0; e < 12; ++e) {
                const int k = e / 4, u = (k + 1) % 3, v = (k + 2) % 3;
                glm::vec3 a, b;
                a[k] = lo[k], b[k] = hi[k];
                a[u] = b[u] = e & 1 ? hi[u] : lo[u];
                a[v] = b[v] = e & 2 ? hi[v] : lo[v];
                ImVec2 sa, sb;
                if (project(fromFrame(axes, a), sa) && project(fromFrame(axes, b), sb)) dl->AddLine(sa, sb, IM_COL32(255, 255, 255, 110), 1.0f);
            }
            for (int h : {0, 1, 2, 4, 5, 6, 3}) {
                ImVec2 s;
                if (!project(scaleHandle(axes, lo, hi, h), s)) continue;
                const ImU32 col = hot == h ? kHot : h == 3 ? IM_COL32(255, 255, 255, 235) : kAxisCol[h < 3 ? h : h - 4];
                dl->AddRectFilled(ImVec2(s.x - 6, s.y - 6), ImVec2(s.x + 6, s.y + 6), col);
                dl->AddRect(ImVec2(s.x - 6, s.y - 6), ImVec2(s.x + 6, s.y + 6), IM_COL32(20, 20, 25, 200));
            }
            return;
        }
        const glm::vec3 p = drag_.active ? (tool_ == Tool::Rotate ? drag_.pivot : selectionMiddle(axes)) : (tool_ == Tool::Rotate ? rotatePivot(axes) : selectionMiddle(axes));
        ImVec2 c;
        if (!project(p, c)) return;
        const float len = pixelSize(p) * 90.0f;
        if (tool_ == Tool::Rotate) {
            for (int k = 0; k < 3; ++k) {
                const glm::vec3 u = glm::normalize(glm::cross(axes[size_t(k)], std::abs(axes[size_t(k)].z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
                const glm::vec3 v = glm::cross(axes[size_t(k)], u);
                ImVec2 prev;
                bool havePrev = false;
                for (int i = 0; i <= 64; ++i) {
                    const float a = 2 * kPi * float(i) / 64.0f;
                    const glm::vec3 w = p + (u * std::cos(a) + v * std::sin(a)) * len;
                    ImVec2 s;
                    if (!project(w, s)) {
                        havePrev = false;
                        continue;
                    }
                    const bool front = glm::dot(w - p, eye_ - p) > -0.2f * len * glm::length(eye_ - p);
                    if (havePrev) dl->AddLine(prev, s, hot == k ? kHot : (front ? kAxisCol[k] : (kAxisCol[k] & 0x00FFFFFF) | 0x50000000), hot == k ? 3.5f : 2.5f);
                    prev = s, havePrev = true;
                }
            }
            dl->AddCircle(c, 102.0f, hot == 3 ? kHot : IM_COL32(255, 255, 255, 160), 64, 2.0f);
            return;
        }
        for (int k = 0; k < 3; ++k) {
            ImVec2 e;
            if (!project(p + axes[size_t(k)] * len, e)) continue;
            const ImU32 col = hot == k ? kHot : kAxisCol[k];
            dl->AddLine(c, e, col, 3.0f);
            const float dx = e.x - c.x, dy = e.y - c.y, l = std::max(std::hypot(dx, dy), 1.0f);
            const ImVec2 dir(dx / l, dy / l), side(-dir.y, dir.x);
            dl->AddTriangleFilled(ImVec2(e.x + dir.x * 14, e.y + dir.y * 14), ImVec2(e.x + side.x * 6, e.y + side.y * 6), ImVec2(e.x - side.x * 6, e.y - side.y * 6), col);
        }
        dl->AddRect(ImVec2(c.x - 8, c.y - 8), ImVec2(c.x + 8, c.y + 8), hot == 3 ? kHot : IM_COL32(255, 255, 255, 230), 0, 0, 2.0f);
    }

    // ───────────────────────────── panels ─────────────────────────────

    void setupUi() {
        keel::ui::setPixelScale(touchScale());
        if (iniPath_.empty()) {
            touchMode_ = fry_touch.b();
            iniPath_ = keel::prefPath() + (touchMode_ ? "layout_phone.ini" : "layout_v3.ini");
            ImGui::GetIO().IniFilename = iniPath_.c_str();
            if (stdfs::exists(iniPath_)) ImGui::LoadIniSettingsFromDisk(iniPath_.c_str());
        }
        if (fry_theme.i() != appliedTheme_) appliedTheme_ = fry_theme.i(), keel::ui::setAppearance(keel::ui::Appearance(std::clamp(appliedTheme_, 0, 2)));
        if (styled_) return;
        styled_ = true;
        ImGui::GetStyle().FontScaleDpi = 0.92f; // (the look itself: keel/ui, Niveus)
    }

    // The least a panel can be and still read right (its words, its rows), by the font's size.
    static ImVec2 panelMin(std::string_view name) {
        const float f = ImGui::GetFontSize();
        if (name == "Toolbox") return {15.5f * f, 9 * f};
        if (name == "Tool options") return {15.5f * f, 8 * f};
        if (name == "Properties") return {23 * f, 12 * f};
        if (name == "Layers") return {20 * f, 8 * f};
        if (name == "Picture") return {18 * f, 14 * f};
        if (name == "Script") return {30 * f, 16 * f};
        return {0, 0};
    }
    // Docked panels kept at their least size: where a split leaves one side smaller, the split
    // moves (when the other side has room to give). ImGui's own splitters know only one least
    // size for everything. Gives what the node needs.
    ImVec2 keepMinSizes(ImGuiDockNode* n) {
        if (!n) return {0, 0};
        if (n->IsLeafNode()) {
            const float f = ImGui::GetFontSize();
            ImVec2 need = n->IsCentralNode() ? ImVec2(16 * f, 12 * f) : ImVec2(0, 0);
            for (ImGuiWindow* w : n->Windows) need = ImMax(need, panelMin(w->Name));
            if (need.y > 0 && !n->IsCentralNode()) need.y += ImGui::GetFrameHeight(); // (its tab)
            return need;
        }
        ImGuiDockNode *a = n->ChildNodes[0], *b = n->ChildNodes[1];
        const ImVec2 na = keepMinSizes(a), nb = keepMinSizes(b);
        if (!a->IsVisible) return nb;
        if (!b->IsVisible) return na;
        const int axis = n->SplitAxis;
        const float gap = ImGui::GetStyle().DockingSeparatorSize, room = n->Size[axis] - gap;
        float sa = a->Size[axis];
        if (sa < na[axis] && room - na[axis] >= nb[axis]) sa = na[axis];
        else if (room - sa < nb[axis] && room - nb[axis] >= na[axis]) sa = room - nb[axis];
        if (sa != a->Size[axis]) a->SizeRef[axis] = sa, b->SizeRef[axis] = room - sa;
        ImVec2 need = ImMax(na, nb);
        need[axis] = na[axis] + nb[axis] + gap;
        return need;
    }

    void defaultLayout(ImGuiID dock) {
        layoutDone_ = true;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
        if (!touch() && node && (node->IsSplitNode() || node->Windows.Size)) return; // (the user's own, from the .ini)
        ImGui::DockBuilderRemoveNodeChildNodes(dock);
        ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize); // (splits are shares of this)
        ImGuiID center = dock;
        if (touch() && portrait_) { // a phone held upright: the view on top, the panels as tabs under it
            const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.36f, nullptr, &center);
            const ImVec2 size = ImGui::GetMainViewport()->WorkSize;
            ImGui::DockBuilderSetNodeSize(bottom, ImVec2(size.x, size.y * 0.36f));
            for (const char* w : {"Toolbox", "Tool options", "Properties", "Layers", "Picture"}) ImGui::DockBuilderDockWindow(w, bottom);
            ImGui::DockBuilderFinish(dock);
            showToolbox_ = true; // (its tab first)
            return;
        }
        if (touch()) { // a phone: tools down the left, the panels on the right (shown when asked for)
            ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.2f, nullptr, &center);
            const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45f, nullptr, &left);
            const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.36f, nullptr, &center);
            ImGui::DockBuilderDockWindow("Toolbox", left);
            ImGui::DockBuilderDockWindow("Tool options", leftBottom);
            ImGui::DockBuilderDockWindow("Properties", right);
            ImGui::DockBuilderDockWindow("Layers", right);
            ImGui::DockBuilderDockWindow("Picture", right);
            return ImGui::DockBuilderFinish(dock);
        }
        // (shares of the window, but never less than the panels' least sizes)
        const float wide = ImGui::GetMainViewport()->WorkSize.x, leftShare = std::min(0.3f, std::max(0.14f, (panelMin("Toolbox").x + 8) / wide));
        const float rightShare = std::min(0.45f, std::max(0.22f, (panelMin("Properties").x + 8) / (wide * (1 - leftShare))));
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, leftShare, nullptr, &center);
        const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.5f, nullptr, &left);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightShare, nullptr, &center);
        const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.66f, nullptr, &right);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.04f, nullptr, &center);
        const ImGuiID picture = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.45f, nullptr, &center); // (the Paint tab's)
        ImGui::DockBuilderDockWindow("Picture", picture);
        ImGui::DockBuilderDockWindow("Script", picture);
        ImGui::DockBuilderDockWindow("Toolbox", left);
        ImGui::DockBuilderDockWindow("Tool options", leftBottom);
        ImGui::DockBuilderDockWindow("Layers", right);
        ImGui::DockBuilderDockWindow("Properties", rightBottom);
        ImGui::DockBuilderDockWindow("Status", bottom);
        if (ImGuiDockNode* b = ImGui::DockBuilderGetNode(bottom)) b->SetLocalFlags(ImGuiDockNodeFlags_NoTabBar);
        ImGui::DockBuilderFinish(dock);
    }

    void menus() {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New", "Ctrl+N")) whenSaved(2);
            if (ImGui::MenuItem("Open...", "Ctrl+O")) whenSaved(3);
            if (ImGui::MenuItem("Save", "Ctrl+S")) path_.empty() ? dialog(2) : void(saveFile(path_));
            if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S")) dialog(2);
            ImGui::Separator();
            if (ImGui::MenuItem("Export (GLB, glTF, OBJ, STL, PLY)...", "Ctrl+E")) dialog(3);
            if (ImGui::BeginMenu("Autosave")) {
                static const int every[] = {1, 2, 5, 10, 0};
                for (int m : every)
                    if (ImGui::MenuItem(m ? std::format("Every {} minute{}", m, m > 1 ? "s" : "").c_str() : "Off", nullptr, fry_autosave.i() == m))
                        keel::execute(std::format("fry_autosave {}", m));
                ImGui::Separator();
                if (ImGui::MenuItem("Recover autosaved work...")) findAutosaves(), showRecover_ = !recover_.empty();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Save a picture of the view...", "F12")) dialog(5);
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) whenSaved(1);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_.empty())) undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_.empty())) redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, mode_ == Mode::Object)) duplicateSelected();
            if (ImGui::MenuItem("Join into one", "Ctrl+J", false, mode_ == Mode::Object && selected_.size() > 1)) joinSelected();
            if (ImGui::MenuItem("Delete", "Del")) deleteSelected();
            ImGui::Separator();
            if (ImGui::MenuItem(mode_ == Mode::Edit ? "Stop editing the shape" : "Edit the shape", "Tab")) mode_ == Mode::Edit ? leaveEdit() : enterEdit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Select")) {
            if (ImGui::MenuItem("All", "Ctrl+A")) selectAll();
            if (ImGui::MenuItem("None", "Alt+A")) selectNone();
            if (ImGui::MenuItem("Invert", "Ctrl+I")) invertSelection();
            ImGui::Separator();
            ImGui::BeginDisabled(mode_ != Mode::Edit);
            if (ImGui::MenuItem("More (grow)", "Ctrl +")) growSelection();
            if (ImGui::MenuItem("Less (shrink)", "Ctrl -")) shrinkSelection();
            if (ImGui::MenuItem("Everything joined to it", "L")) selectLinked();
            ImGui::MenuItem("A loop: Alt+click or double-click", nullptr, false, false);
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Select with a box", nullptr, selShape_ == SelShape::Box)) selShape_ = SelShape::Box;
            if (ImGui::MenuItem("Select with a brush", nullptr, selShape_ == SelShape::Brush)) selShape_ = SelShape::Brush;
            if (ImGui::MenuItem("Select with a lasso", nullptr, selShape_ == SelShape::Lasso)) selShape_ = SelShape::Lasso;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Add")) {
            addMenuItems();
            pluginMenuItems("add");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Object")) {
            const bool many = selected_.size() >= 2, any = !selected_.empty();
            if (ImGui::MenuItem("Go to it", "F", false, any)) goTo(selected_);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, any)) duplicateSelected();
            if (ImGui::MenuItem("Make copies (in a row, round)...", nullptr, false, any)) showCopies_ = true;
            if (ImGui::MenuItem("Group into a folder", "Ctrl+G", false, any)) groupSelected();
            ImGui::Separator();
            ImGui::TextDisabled("Two or more: the last picked is kept");
            if (ImGui::MenuItem("Add them together", "Ctrl+Shift++", false, many)) combineSelected(Combine::Add);
            if (ImGui::MenuItem("Take the others out of it", "Ctrl+Shift+-", false, many)) combineSelected(Combine::Subtract);
            if (ImGui::MenuItem("Keep only where they overlap", nullptr, false, many)) combineSelected(Combine::Overlap);
            if (ImGui::MenuItem("Join (just as they are)", "Ctrl+J", false, many)) joinSelected();
            ImGui::Separator();
            if (ImGui::MenuItem("Make it thick (or hollow)...", nullptr, false, any)) showThicken_ = true;
            if (ImGui::MenuItem("Join flat faces side by side", nullptr, false, any)) cleanFlatFaces();
            pluginMenuItems("object");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Shape")) {
            const bool ed = mode_ == Mode::Edit;
            if (!ed) ImGui::TextDisabled("Edit an object's shape first (Tab)");
            ImGui::BeginDisabled(!ed);
            if (ImGui::MenuItem("Extrude", "X")) setTool(Tool::Extrude);
            if (ImGui::MenuItem("Inset", "I")) setTool(Tool::Inset);
            if (ImGui::MenuItem("Round edges", "B")) setTool(Tool::Bevel);
            if (ImGui::MenuItem("Loop cut", "C")) setTool(Tool::LoopCut);
            ImGui::Separator();
            if (ImGui::MenuItem("Cut every face in four")) subdivideNow(false);
            if (ImGui::MenuItem("Smooth it all (cut and round)")) subdivideNow(true);
            if (ImGui::MenuItem("Merge points", "M")) mergeNow();
            if (ImGui::MenuItem("Make a face")) makeFaceNow();
            if (ImGui::MenuItem("Flip faces")) flipNow();
            ImGui::Separator();
            if (ImGui::MenuItem("Smooth points")) smoothPointsNow();
            if (ImGui::MenuItem("Push out / pull in...", "Alt+S")) startPush();
            if (ImGui::MenuItem("Bridge two faces")) bridgeNow();
            ImGui::MenuItem("Soft selection", "Shift+O", &softSel_);
            if (ImGui::MenuItem("Make into its own object", "P")) separateSelected();
            if (ImGui::MenuItem("Delete", "Del")) deleteElements();
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Solid (for modelling)", "Z", shading_ == Shading::Solid)) shading_ = Shading::Solid;
            if (ImGui::MenuItem("Rendered (sky, sun, shadows)", "Z", shading_ == Shading::Rendered)) shading_ = Shading::Rendered;
            if (ImGui::MenuItem("Wireframe", "Shift+Z", shading_ == Shading::Wire)) shading_ = Shading::Wire;
            ImGui::MenuItem("X-ray (see and pick through)", "Alt+Z", &xray_);
            ImGui::MenuItem("Grid", nullptr, &showGrid_);
            ImGui::MenuItem("Ground (in Rendered)", nullptr, &scene_.look.ground);
            if (ImGui::BeginMenu("Appearance")) {
                const char* looks[] = {"Like the desktop", "Dark", "Light"};
                for (int i = 0; i < 3; ++i)
                    if (ImGui::MenuItem(looks[i], nullptr, fry_theme.i() == i)) keel::execute(std::format("fry_theme {}", i));
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Perspective", "Numpad 5", view_ == View::Perspective)) leaveFlatView();
            if (ImGui::MenuItem("Front", "Numpad 1", view_ == View::Front)) setView(View::Front);
            if (ImGui::MenuItem("Back", "Shift+Numpad 1", view_ == View::Back)) setView(View::Back);
            if (ImGui::MenuItem("Left side", "Numpad 3", view_ == View::Left)) setView(View::Left);
            if (ImGui::MenuItem("Right side", "Shift+Numpad 3", view_ == View::Right)) setView(View::Right);
            if (ImGui::MenuItem("Top", "Numpad 7", view_ == View::Top)) setView(View::Top);
            if (ImGui::MenuItem("Bottom", "Shift+Numpad 7", view_ == View::Bottom)) setView(View::Bottom);
            ImGui::Separator();
            if (ImGui::MenuItem("Frame selected", "F")) frameSelected();
            if (ImGui::MenuItem("Frame everything", "Home")) frameAll();
            if (ImGui::MenuItem("Show only the selected (again: all)", "/", false, !selected_.empty())) showOnly(selected_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Script")) {
            ImGui::MenuItem("The Script panel", nullptr, &showScript_);
            if (ImGui::MenuItem("Run it", "F5", false, currentScript())) runScriptNow();
            if (!scene_.scripts.empty() && ImGui::BeginMenu("This file's scripts")) {
                for (size_t i = 0; i < scene_.scripts.size(); ++i)
                    if (ImGui::MenuItem(scene_.scripts[i].name.c_str(), nullptr, i == script_)) script_ = i, showScript_ = true;
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New script")) addScript("Script", kFirstScript);
            if (ImGui::MenuItem("Open a script...")) showScript_ = true, dialog(10);
            if (ImGui::MenuItem("Save the script as...", nullptr, false, currentScript())) dialog(11);
            exampleScripts();
            ImGui::Separator();
            if (ImGui::BeginMenu("Plug-ins")) {
                bool any = false;
                for (size_t i = 0; i < plugins_.size(); ++i)
                    if (plugins_[i].p)
                        for (size_t c = 0; c < plugins_[i].p->commands.size(); ++c)
                            if (plugins_[i].p->commands[c].menu == "script") pluginMenuItem(i, c), any = true;
                if (any) ImGui::Separator();
                if (ImGui::MenuItem("Plug-ins...")) showPlugins_ = true;
                if (ImGui::MenuItem("Open the plug-ins folder")) openPluginsFolder();
                if (ImGui::MenuItem("Load them again")) loadPlugins();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("What scripts can do")) showScript_ = showScriptHelp_ = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Keys and mouse", "F1")) showHelp_ = true;
            if (ImGui::MenuItem("About Fry3DX")) showAbout_ = true;
            ImGui::EndMenu();
        }
    }
    void menuBar() {
        if (!ImGui::BeginMainMenuBar()) return;
        if (haveLogo_) {
            const float h = ImGui::GetFrameHeight() - 4;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1);
            logoImage(h);
        }
        ImGui::PushFont(keel::ui::font(keel::ui::Font::Bold), 0.0f), ImGui::TextColored(accentText(), "Fry3DX"), ImGui::PopFont();
        if (!touch()) menus();
        else if (ImGui::BeginMenu("Menu")) { // (a phone: one menu, the others inside it)
            menus();
            ImGui::EndMenu();
        }
        const std::string file = (path_.empty() ? "Untitled" : fileName(path_)) + (dirty_ ? "  *" : "");
        // The tabs: what you're doing (as wide as there's room for between the menus and the file's
        // name; the pictures alone when it's tight).
        {
            const char* names[] = {"Model", "Paint", "Layout", "Render", "Print"};
            const Icon icons[] = {Icon::Objects, Icon::PaintBrush, Icon::Layout, Icon::Render, Icon::Print};
            const char* tips[] = {"Make the shape (Ctrl+1)", "Paint pictures onto it (Ctrl+2)", "Change how its picture is laid out: move, turn and size its frames (Ctrl+3)",
                                  "Light it and save pictures of it (Ctrl+4)", "Get it ready for a 3D printer: size, checks, the printer's plate (Ctrl+5)"};
            const float h = ImGui::GetFrameHeight(), from = ImGui::GetCursorPosX() + 20.0f;
            const float room = ImGui::GetWindowWidth() - from - (touch() ? 8.0f : ImGui::CalcTextSize(file.c_str()).x + 32.0f);
            float tw = std::min(touch() ? 72.0f : 92.0f, room / 5 - 4);
            const bool words = tw >= h + 2 + ImGui::CalcTextSize("Layout").x + 6;
            if (!words) tw = h + 4;
            ImGui::SetCursorPosX(std::max(from, std::min((ImGui::GetWindowWidth() - (tw + 4) * 5) * 0.5f, from + room - (tw + 4) * 5)));
            for (int i = 0; i < 5; ++i) {
                if (i) ImGui::SameLine(0, 4);
                ImGui::PushID(i);
                if (wideButton("##tab", icons[i], words ? names[i] : "", int(tab_) == i, tw, words ? tips[i] : std::format("{}: {}", names[i], tips[i]).c_str(), h)) setTab(Tab(i));
                ImGui::PopID();
            }
        }
        if (touch()) return ImGui::EndMainMenuBar(); // (no room on a phone)
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(file.c_str()).x - 16);
        ImGui::TextDisabled("%s", file.c_str());
        ImGui::EndMainMenuBar();
    }

    void addMenuItems() {
        ImGui::SeparatorText("Shapes");
        for (const ShapeRecipe& r : shapeRecipes())
            if (!r.starter && ImGui::MenuItem(r.name)) addShape(r.id);
        ImGui::SeparatorText("Starter models");
        for (const ShapeRecipe& r : shapeRecipes())
            if (r.starter && ImGui::MenuItem(r.name)) addShape(r.id);
        ImGui::Separator();
        if (ImGui::MenuItem("Draw a shape", "D")) {
            if (mode_ == Mode::Edit) leaveEdit();
            setTool(Tool::Draw);
        }
        if (ImGui::MenuItem("Reference picture...")) dialog(4);
    }

    // How the last item's key looks: filled when it's the one in use, sunk while held.
    static keel::ui::KeyState keyState(bool on) {
        using keel::ui::KeyState;
        return on ? KeyState::On : ImGui::IsItemActive() ? KeyState::Pressed : ImGui::IsItemHovered() ? KeyState::Hovered : KeyState::Normal;
    }
    // A square button with a picture on it (the accent when it's the one in use).
    bool iconButton(const char* id, Icon icon, bool on, const char* tip, float size = 38.0f) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool hit = ImGui::InvisibleButton(id, ImVec2(size, size));
        const keel::ui::KeyState k = keyState(on);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        keel::ui::drawKey(dl, p, ImVec2(p.x + size, p.y + size), 7.0f, k);
        const float sink = k == keel::ui::KeyState::Pressed ? 1.0f : 0.0f;
        drawIcon(dl, icon, ImVec2(p.x + size * 0.14f, p.y + size * 0.14f + sink), size * 0.72f, keel::ui::keyText(k));
        const bool hov = ImGui::IsItemHovered();
        if (hov && tip) ImGui::SetTooltip("%s", tip);
        return hit;
    }
    // A wide one, with words beside the picture.
    bool wideButton(const char* id, Icon icon, const char* label, bool on, float width, const char* tip = nullptr, float h = 30.0f) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool hit = ImGui::InvisibleButton(id, ImVec2(width, h));
        const keel::ui::KeyState k = keyState(on);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        keel::ui::drawKey(dl, p, ImVec2(p.x + width, p.y + h), 7.0f, k);
        const float sink = k == keel::ui::KeyState::Pressed ? 1.0f : 0.0f;
        drawIcon(dl, icon, ImVec2(p.x + h * 0.15f, p.y + h * 0.13f + sink), h * 0.74f, keel::ui::keyText(k));
        dl->AddText(ImVec2(p.x + h + 2, p.y + (h - ImGui::GetFontSize()) * 0.5f + sink), keel::ui::keyText(k), label);
        const bool hov = ImGui::IsItemHovered();
        if (hov && tip) ImGui::SetTooltip("%s", tip);
        return hit;
    }
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

    void toolbox() {
        if (std::exchange(showToolbox_, false)) ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowSizeConstraints(panelMin("Toolbox"), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin("Toolbox", nullptr, ImGuiWindowFlags_NoCollapse);
        const float w = ImGui::GetContentRegionAvail().x;
        if (tab_ == Tab::Paint) {
            paintToolbox(w);
            return ImGui::End();
        }
        if (tab_ == Tab::Layout) {
            layoutToolbox(w);
            return ImGui::End();
        }
        if (tab_ == Tab::Render || tab_ == Tab::Print) {
            const Tool tools[] = {Tool::Select, Tool::Move, Tool::Rotate, Tool::Scale};
            grid(4, 38.0f, [&](int i) {
                const ToolInfo& t = kTools[int(tools[i])];
                const std::string id = std::format("##{}", t.name), tip = std::format("{}  ({})\n{}", t.name, t.key, t.hint);
                if (iconButton(id.c_str(), t.icon, tool_ == t.tool, tip.c_str())) setTool(t.tool);
            });
            ImGui::Spacing();
            if (tab_ == Tab::Print) printToolbox(w);
            else if (wideButton("##save", Icon::Picture, "Save a picture", false, w, "Saves what the view shows, without the panels or gizmos (F12)")) dialog(5);
            return ImGui::End();
        }
        // The mode: whole objects, or the shape of one.
        if (wideButton("##objects", Icon::Objects, "Objects", mode_ == Mode::Object, (w - 5) / 2, "Work with whole objects")) leaveEdit();
        ImGui::SameLine(0, 5);
        if (wideButton("##shape", Icon::Shape, "Shape", mode_ == Mode::Edit, (w - 5) / 2, "Edit the selected object's shape: its points, edges and faces (Tab, or double-click it)")) enterEdit();
        if (mode_ == Mode::Edit) {
            const Icon icons[] = {Icon::Points, Icon::Edges, Icon::Faces};
            const char* tips[] = {"Points (1)", "Edges (2)", "Faces (3)"};
            const char* names[] = {"Points", "Edges", "Faces"};
            const float third = (w - 10) / 3;
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine(0, 5);
                ImGui::PushID(i);
                if (wideButton("##elem", icons[i], names[i], int(elem_) == i, third, tips[i])) elem_ = Elem(i), clearElements();
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        // The tools, as pictures (hover for the name and key).
        std::vector<const ToolInfo*> tools;
        for (const ToolInfo& t : kTools)
            if ((mode_ == Mode::Object && t.objectMode) || (mode_ == Mode::Edit && t.editMode)) tools.push_back(&t);
        grid(int(tools.size()) + 1, 38.0f, [&](int i) {
            if (i == int(tools.size())) {
                if (iconButton("##add", Icon::Add, false, "Add a shape or a starter model")) ImGui::OpenPopup("add");
                return;
            }
            const ToolInfo& t = *tools[size_t(i)];
            const std::string id = std::format("##{}", t.name), tip = std::format("{}  ({})\n{}", t.name, t.key, t.hint);
            if (iconButton(id.c_str(), t.icon, tool_ == t.tool, tip.c_str())) setTool(t.tool);
        });
        if (ImGui::BeginPopup("add")) {
            addMenuItems();
            ImGui::EndPopup();
        }
        ImGui::Spacing();
        for (const ToolInfo& t : kTools)
            if (t.tool == tool_) ImGui::TextColored(accentText(), "%s", t.name);
        ImGui::End();
    }

    void paintToolbox(float w) {
        const float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (wideButton("##colour-side", Icon::PaintBrush, "Colour", !sculpting_, half, "Paint colours on it (Tab)")) sculpting_ = false, endSculpt();
        ImGui::SameLine();
        if (wideButton("##shape-side", Icon::Sculpt, "Sculpt", sculpting_, half, "Push its surface about like clay (Tab)")) sculpting_ = true, endStroke();
        ImGui::Spacing();
        if (sculpting_) {
            grid(int(std::size(kSculptTools)), 38.0f, [&](int i) {
                const SculptToolInfo& t = kSculptTools[i];
                const std::string id = std::format("##{}", t.name), tip = std::format("{}  ({})\n{}", t.name, t.key, t.hint);
                if (iconButton(id.c_str(), t.icon, sculptBrush_ == t.brush, tip.c_str())) sculptBrush_ = t.brush;
            });
            for (const SculptToolInfo& t : kSculptTools)
                if (t.brush == sculptBrush_) ImGui::TextColored(accentText(), "%s", t.name);
            return;
        }
        grid(int(std::size(kPaintTools)), 38.0f, [&](int i) {
            const PaintToolInfo& t = kPaintTools[i];
            const std::string id = std::format("##{}", t.name), tip = std::format("{}  ({})\n{}", t.name, t.key, t.hint);
            if (iconButton(id.c_str(), t.icon, paintTool_ == t.tool, tip.c_str())) paintTool_ = t.tool;
        });
        ImGui::Spacing();
        // The colour, big, and a few to pick from.
        ImGui::ColorButton("##colour", ImVec4(paintColor_.r, paintColor_.g, paintColor_.b, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(w, 34));
        static const glm::vec3 palette[] = {{0.08f, 0.08f, 0.09f}, {0.95f, 0.95f, 0.95f}, {0.55f, 0.55f, 0.57f}, {0.85f, 0.2f, 0.12f}, {1.0f, 0.55f, 0.1f}, {0.98f, 0.85f, 0.2f},
                                            {0.3f, 0.7f, 0.25f}, {0.15f, 0.45f, 0.85f}, {0.45f, 0.25f, 0.7f}, {0.45f, 0.3f, 0.18f}, {0.75f, 0.6f, 0.45f}, {0.95f, 0.6f, 0.7f}};
        grid(int(std::size(palette)), 20.0f, [&](int i) {
            ImGui::PushID(i);
            if (ImGui::ColorButton("##p", ImVec4(palette[i].r, palette[i].g, palette[i].b, 1.0f), 0, ImVec2(20, 20))) paintColor_ = glm::vec4(palette[i], paintColor_.a);
            ImGui::PopID();
        });
        for (const PaintToolInfo& t : kPaintTools)
            if (t.tool == paintTool_) ImGui::TextColored(accentText(), "%s", t.name);
    }

    void sculptOptions(float w) {
        for (const SculptToolInfo& t : kSculptTools)
            if (t.brush == sculptBrush_) ImGui::SeparatorText(t.name), ImGui::TextWrapped("%s", t.hint);
        ImGui::Spacing();
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##size", &sculptPx_, 4.0f, 400.0f, "size %.0f px", ImGuiSliderFlags_Logarithmic);
        if (sculptBrush_ != SculptBrush::Grab) {
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##strength", &sculptStrength_, 0.02f, 1.0f, "strength %.2f");
        }
        ImGui::Checkbox("Same on both sides (X)", &sculptMirror_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("What you do on its left is done on its right too");
        if (Object* o = selected_.size() == 1 ? find(active_) : nullptr) {
            ImGui::SeparatorText(o->name.c_str());
            ImGui::TextDisabled("%zu points", o->mesh.verts.size());
            size_t after = 0;
            for (const auto& f : o->mesh.faces) after += f.size();
            ImGui::BeginDisabled(after > 400000 || o->locked);
            if (ImGui::Button("Finer, for small details", ImVec2(w, 0))) {
                pushUndo();
                if (o->hasFinish()) o->mesh = finishedMesh(*o), o->mirror = 0, o->round = 0, o->smoothLevels = 0;
                handMade(*o);
                subdivide(o->mesh, false);
                touched(o->id);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Each face split in four: the same shape, more points to push about");
            if (ImGui::Button("Smooth all of it", ImVec2(w, 0)) && !o->locked) {
                pushUndo();
                handMade(*o);
                const SculptMesh sm(o->mesh);
                const glm::vec3 mid = (boundsMin(o->mesh) + boundsMax(o->mesh)) * 0.5f;
                sculptDab(o->mesh, sm, SculptBrush::Smooth, {mid, glm::distance(boundsMin(o->mesh), boundsMax(o->mesh)) * 2.0f, 0.5f});
                touched(o->id);
            }
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Ctrl: the other way   Shift: smooth\n[ and ]: brush size   1-6: brushes");
    }

    void paintOptions(float w) {
        if (sculpting_) return sculptOptions(w);
        for (const PaintToolInfo& t : kPaintTools)
            if (t.tool == paintTool_) ImGui::SeparatorText(t.name);
        if (paintTool_ == PaintTool::Brush || paintTool_ == PaintTool::Eraser) {
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##size", &brushPx_, 1.0f, 300.0f, "size %.0f px", ImGuiSliderFlags_Logarithmic);
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##hard", &hardness_, 0.0f, 1.0f, "hard edge %.2f");
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##strength", &paintColor_.a, 0.02f, 1.0f, "strength %.2f");
        }
        if (paintTool_ == PaintTool::Fill) {
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##tol", &fillTolerance_, 0.0f, 1.0f, "how alike %.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How different a colour can be and still be filled");
        }
        if (paintTool_ == PaintTool::Brush || paintTool_ == PaintTool::Fill || paintTool_ == PaintTool::Picker) {
            ImGui::SetNextItemWidth(w);
            ImGui::ColorPicker3("##picker", &paintColor_.x, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_PickerHueBar);
        }
        if (paintTool_ == PaintTool::Eraser) ImGui::TextWrapped("Paints the object's plain colour back (its colour in the Model tab).");
        if (paintTool_ == PaintTool::Picture) {
            if (ImGui::Button("Paste (Ctrl+V)", ImVec2(w, 0))) pasteFromClipboard();
            if (ImGui::Button("Add a picture from a file...", ImVec2(w, 0))) dialog(6);
            if (PictureLayer* l = editLayer()) {
                Object* o = find(ledit_.object);
                ImGui::SeparatorText(l->name.c_str());
                if (ImGui::Button("Lock it in place (Enter)", ImVec2(w, 0))) lockLayer();
                if (ImGui::Button(ledit_.fresh ? "Take it away (Esc)" : "Delete it (Del)", ImVec2(w, 0))) ledit_.fresh ? cancelFloating() : deleteLayer(*o, ledit_.index);
                ImGui::SetNextItemWidth(w);
                undoable([&] { return ImGui::SliderFloat("##op", &l->opacity, 0.0f, 1.0f, "see-through %.2f"); });
                if (l->sticker) {
                    float size[2] = {l->width, l->height};
                    ImGui::SetNextItemWidth(w);
                    if (undoable([&] { return ImGui::DragFloat2("##wh", size, 0.002f, 0.002f, 100.0f, "%.3f m"); })) l->width = size[0], l->height = size[1];
                } else if (o && o->picture) {
                    if (ImGui::Button("Cover the whole picture", ImVec2(w, 0))) {
                        pushUndo();
                        const float W = float(o->picture->w), H = float(o->picture->h);
                        l->corners = {glm::vec2(0, 0), glm::vec2(W, 0), glm::vec2(W, H), glm::vec2(0, H)};
                    }
                    if (ImGui::Button("Square it up again", ImVec2(w, 0))) {
                        pushUndo();
                        glm::vec2 c(0.0f);
                        for (const glm::vec2& p : l->corners) c += p * 0.25f;
                        const float across = (glm::distance(l->corners[0], l->corners[1]) + glm::distance(l->corners[3], l->corners[2])) * 0.5f;
                        const glm::vec2 half(across * 0.5f, across * 0.5f * float(l->image->h) / float(std::max(l->image->w, 1)));
                        l->corners = {c - half, glm::vec2(c.x + half.x, c.y - half.y), c + half, glm::vec2(c.x - half.x, c.y + half.y)};
                    }
                }
                ImGui::Separator();
            }
            ImGui::TextDisabled("New pictures go");
            int where = pasteOnSurface_ ? 0 : 1;
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo("##where", &where, "On the model (a sticker)\0On the flat picture\0")) pasteOnSurface_ = where == 0;
            ImGui::TextWrapped(pasteOnSurface_ ? "Copy a picture in any app (GIMP, a browser...) and paste it. Drag it round the model: over an edge it bends like a sticker. "
                                                 "Corners size it (Shift: stretch), the round handle turns it (Ctrl: steps). Enter locks it as a layer; click it in the Picture panel to move it again."
                                               : "Copy a picture in any app and paste it. In the Picture panel drag its corners where you want them: it bends to fit "
                                                 "(Shift keeps its shape; drag inside to move it). Enter locks it as a layer; click it there to move it again.");
        }
    }

    void softChoice(float w) {
        if (mode_ != Mode::Edit) return;
        ImGui::Checkbox("Soft selection (Shift+O)", &softSel_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Points near the selection follow it, less the further they are");
        if (!softSel_) return;
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("##reach", &softReach_, 0.002f, 0.001f, 1000.0f, "reaches %.3f m");
        ImGui::TextDisabled("(the wheel while dragging too)");
    }
    void toolOptions() {
        ImGui::SetNextWindowSizeConstraints(panelMin("Tool options"), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin("Tool options", nullptr, ImGuiWindowFlags_NoCollapse);
        const float w = ImGui::GetContentRegionAvail().x;
        if (tab_ == Tab::Paint) {
            paintOptions(w);
            return ImGui::End();
        }
        if (tab_ == Tab::Layout) {
            layoutOptions(w);
            return ImGui::End();
        }
        if (tab_ == Tab::Print && printTool_ != PrintTool::None) {
            printOptions(w);
            return ImGui::End();
        }
        const char* name = "";
        for (const ToolInfo& t : kTools)
            if (t.tool == tool_) name = t.name;
        ImGui::SeparatorText(name);
        auto orientChoice = [&] {
            ImGui::TextDisabled("Arrows point");
            int o = int(orient());
            const char* items = mode_ == Mode::Edit ? "The object's way\0The world's way\0Out of the faces\0" : "The object's way\0The world's way\0";
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo("##orient", &o, items)) orient() = Orient(o);
        };
        auto snapChoice = [&](bool angle) {
            ImGui::Checkbox("Snap", &snap_);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("In steps (holding Ctrl does the opposite)");
            if (!snap_) return;
            ImGui::SetNextItemWidth(w);
            if (angle) ImGui::DragFloat("##snapangle", &snapAngle_, 0.5f, 1.0f, 90.0f, "every %.0f deg");
            else ImGui::DragFloat("##snapmove", &snapMove_, 0.001f, 0.001f, 10.0f, "every %.3f m");
        };
        switch (tool_) {
        case Tool::Select: {
            ImGui::TextDisabled("Drag to select with");
            const Icon icons[] = {Icon::Box, Icon::Brush, Icon::Lasso};
            const char* tips[] = {"A box", "A brush: paint over what you want", "A lasso: draw round it"};
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine(0, 5);
                ImGui::PushID(i);
                if (iconButton("##shape", icons[i], int(selShape_) == i, tips[i], 34.0f)) selShape_ = SelShape(i);
                ImGui::PopID();
            }
            if (selShape_ == SelShape::Brush) {
                ImGui::SetNextItemWidth(w);
                ImGui::SliderFloat("##brush", &brushRadius_, 4.0f, 200.0f, "brush %.0f px");
            }
            ImGui::Checkbox("See through (X-ray)", &xray_);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select things behind others too (Alt+Z)");
            break;
        }
        case Tool::Move:
            orientChoice();
            snapChoice(false);
            softChoice(w);
            break;
        case Tool::Rotate: {
            orientChoice();
            ImGui::TextDisabled("Turn around");
            int p = rotatePivot_ == Pivot::Origin ? 1 : 0;
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo("##rpivot", &p, "The middle\0The origin\0")) rotatePivot_ = p ? Pivot::Origin : Pivot::Middle;
            snapChoice(true);
            softChoice(w);
            break;
        }
        case Tool::Scale: {
            if (mode_ == Mode::Edit) orientChoice();
            ImGui::TextDisabled("What stays put");
            int p = scalePivot_ == Pivot::Opposite ? 0 : scalePivot_ == Pivot::Middle ? 1 : scalePivot_ == Pivot::Bottom ? 2 : 3;
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo("##spivot", &p, "The other side\0The middle\0The bottom\0The origin\0"))
                scalePivot_ = p == 0 ? Pivot::Opposite : p == 1 ? Pivot::Middle : p == 2 ? Pivot::Bottom : Pivot::Origin;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The other side: drag a side, the opposite side stays (like stretching in a paint program).\nThe middle: both sides move.\nThe bottom: it stays standing on the ground.");
            snapChoice(false);
            softChoice(w);
            break;
        }
        case Tool::Draw:
            ImGui::SetNextItemWidth(w);
            ImGui::DragFloat("##thick", &drawThickness_, 0.001f, 0.0f, 10.0f, "thickness %.3f m");
            ImGui::TextWrapped("Drawn on the face you start on, or on %s.",
                               view_ == View::Perspective || view_ == View::Top || view_ == View::Bottom ? "the ground (a side view draws upright)" : "the plane you look at");
            ImGui::TextDisabled("Drawn on an object:");
            ImGui::SetNextItemWidth(w);
            ImGui::Combo("##drawon", &drawOnMode_, "A new object on it\0Added to it (raised)\0Cut into it\0");
            snapChoice(false);
            if (!drawPts_.empty() && ImGui::Button("Finish (Enter)", ImVec2(w, 0))) finishShape();
            break;
        case Tool::Extrude:
            ImGui::SetNextItemWidth(w);
            ImGui::DragFloat("##ext", &extrudeAmount_, 0.001f, -10, 10, "%.3f m");
            if (ImGui::Button("Extrude by that", ImVec2(w, 0))) extrudeNow(extrudeAmount_);
            snapChoice(false);
            break;
        case Tool::Inset:
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##ins", &insetAmount_, 0.01f, 0.99f, "%.2f of the way in");
            if (ImGui::Button("Inset by that", ImVec2(w, 0))) insetNow(insetAmount_);
            break;
        case Tool::Bevel:
            ImGui::SetNextItemWidth(w);
            ImGui::DragFloat("##bw", &bevelWidth_, 0.001f, 0.0f, 10.0f, "width %.3f m");
            ImGui::SetNextItemWidth(w);
            ImGui::SliderInt("##bs", &bevelSteps_, 1, 10, bevelSteps_ == 1 ? "1 step (a flat cut)" : "%d steps round");
            if (ImGui::Button("Round them", ImVec2(w, 0))) bevelNow();
            break;
        case Tool::LoopCut:
            ImGui::SetNextItemWidth(w);
            ImGui::SliderInt("##cuts", &cuts_, 1, 16, cuts_ == 1 ? "1 cut" : "%d cuts");
            ImGui::TextWrapped("Cuts go across four-sided faces, all the way round.");
            break;
        case Tool::Knife: {
            ImGui::Checkbox("Cut right through", &knifeThrough_);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Through everything in the line's way, the back too (else only what you can see).\nThe line goes on past its ends.");
            if (knifeThrough_) {
                ImGui::TextDisabled("Then");
                ImGui::SetNextItemWidth(w);
                ImGui::Combo("##after", &knifeAfter_, "Just add the edges\0Pieces of the one object\0Each piece its own object\0Delete the smaller piece\0");
                if (knifeAfter_ > 0) {
                    ImGui::TextDisabled("Inside, where it's cut");
                    int fill = knifeFill_ ? 0 : 1;
                    ImGui::SetNextItemWidth(w);
                    if (ImGui::Combo("##fill", &fill, "Filled (solid)\0Left open (hollow)\0")) knifeFill_ = fill == 0;
                }
            }
            ImGui::TextWrapped("Click points along the cut, or drag a straight line. Ctrl: at 45 degree steps. Enter or double-click cuts, Esc lets go. "
                               "Each face it crosses is cut from edge to edge.");
            if (knifePts_.size() >= 2 && ImGui::Button("Cut (Enter)", ImVec2(w, 0))) {
                std::vector<ImVec2> pts = knifePts_;
                knifePts_.clear();
                knifeApply(active_, pts);
            }
            break;
        }
        case Tool::Hole: {
            ImGui::SetNextItemWidth(w);
            ImGui::Combo("##shape", &holeShape_, "Round\0Rectangle\0Triangle\0Hexagon (for nuts)\0");
            const HoleShape shape = HoleShape(holeShape_);
            // Ready-made sizes for 3D prints: screws go through round holes, nuts sit in hexagons
            // (a little room added, as prints come out a touch tight).
            struct Fit {
                const char* name;
                float across, deep; // mm
            };
            static const Fit screws[] = {{"M2 screw (2.4 mm)", 2.4f, 0}, {"M2.5 screw (2.9 mm)", 2.9f, 0}, {"M3 screw (3.4 mm)", 3.4f, 0}, {"M4 screw (4.5 mm)", 4.5f, 0},
                                         {"M5 screw (5.5 mm)", 5.5f, 0},  {"M6 screw (6.6 mm)", 6.6f, 0},  {"M8 screw (9 mm)", 9.0f, 0}};
            static const Fit nuts[] = {{"M2 nut (4 mm, 1.6 thick)", 4.0f, 1.6f},     {"M2.5 nut (5 mm, 2 thick)", 5.0f, 2.0f}, {"M3 nut (5.5 mm, 2.4 thick)", 5.5f, 2.4f},
                                       {"M4 nut (7 mm, 3.2 thick)", 7.0f, 3.2f},     {"M5 nut (8 mm, 4 thick)", 8.0f, 4.0f},   {"M6 nut (10 mm, 5 thick)", 10.0f, 5.0f},
                                       {"M8 nut (13 mm, 6.5 thick)", 13.0f, 6.5f}};
            if (shape == HoleShape::Round || shape == HoleShape::Hexagon) {
                const bool nut = shape == HoleShape::Hexagon;
                ImGui::SetNextItemWidth(w);
                if (ImGui::BeginCombo("##fit", nut ? "Size for a nut..." : "Size for a screw...")) {
                    for (const Fit& f : nut ? std::span<const Fit>(nuts) : std::span<const Fit>(screws))
                        if (ImGui::Selectable(f.name)) {
                            holeSize_ = (f.across + (nut ? holeRoom_ * 1000 : 0)) / 1000;
                            if (nut) holeMode_ = 1, holeDepth_ = (f.deep + 0.2f) / 1000;
                            else holeMode_ = 0;
                        }
                    ImGui::EndCombo();
                }
                if (nut) {
                    float room = holeRoom_ * 1000;
                    ImGui::SetNextItemWidth(w);
                    if (ImGui::SliderFloat("##room", &room, 0.0f, 0.8f, "+ %.2f mm room")) holeRoom_ = room / 1000;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Added to a nut's size so it pushes in (0.2 to 0.4 mm suits most printers)");
                }
            }
            if (shape == HoleShape::Round) {
                ImGui::SetNextItemWidth(w);
                ImGui::SliderInt("##sides", &holeSides_, 6, 96, "%d sides round");
            }
            const char* acrossName = shape == HoleShape::Round ? "%.2f mm across" : shape == HoleShape::Rectangle ? "%.2f mm wide" : shape == HoleShape::Triangle ? "%.2f mm sides" : "%.2f mm across flats";
            float across = holeSize_ * 1000, tall = holeTall_ * 1000;
            ImGui::SetNextItemWidth(w);
            if (ImGui::DragFloat("##size", &across, 0.1f, 0.1f, 100000.0f, acrossName)) holeSize_ = across / 1000;
            if (shape == HoleShape::Rectangle) {
                ImGui::SetNextItemWidth(w);
                if (ImGui::DragFloat("##tall", &tall, 0.1f, 0.1f, 100000.0f, "%.2f mm high")) holeTall_ = tall / 1000;
            }
            if (shape != HoleShape::Round) {
                ImGui::SetNextItemWidth(w);
                ImGui::SliderFloat("##turn", &holeTurn_, -180.0f, 180.0f, "turned %.0f degrees");
            }
            ImGui::TextDisabled("Cut");
            ImGui::SetNextItemWidth(w);
            ImGui::Combo("##mode", &holeMode_, "All the way through\0A pocket\0Only the outline (a new face)\0");
            if (holeMode_ == 1) {
                float deep = holeDepth_ * 1000;
                ImGui::SetNextItemWidth(w);
                if (ImGui::DragFloat("##depth", &deep, 0.1f, 0.1f, 100000.0f, "%.2f mm deep")) holeDepth_ = deep / 1000;
            }
            ImGui::TextWrapped(holeMode_ == 2 ? "Press on a face and drag out the size, or just click. Nothing's taken away: the outline is cut into the face, "
                                                "and the new face inside it is picked, ready to Extrude, Bevel or paint."
                                              : "Press on a face and drag out the size, or just click (the size above). It has to fit inside the face%s. "
                                                "Round edges in Properties rounds its rim too.",
                               holeMode_ == 0 ? ", and the face behind" : "");
            break;
        }
        }
        ImGui::End();
    }

    void layersPanel() {
        ImGui::SetNextWindowSizeConstraints(panelMin("Layers"), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin("Layers", nullptr, ImGuiWindowFlags_NoCollapse);
        // Along the top: a new folder, the picked ones into one.
        if (ImGui::SmallButton("+ Folder")) {
            pushUndo();
            Folder f;
            f.id = scene_.nextId++, f.name = "Folder";
            scene_.folders.push_back(f);
            renamingFolder_ = f.id, std::snprintf(renameBuf_, sizeof renameBuf_, "%s", f.name.c_str());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new folder: drag things onto it");
        ImGui::SameLine();
        ImGui::BeginDisabled(selected_.empty());
        if (ImGui::SmallButton("Group (Ctrl+G)")) groupSelected();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The picked objects into a new folder: picked and moved together");
        if (ImGui::BeginTable("objects", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("eye", ImGuiTableColumnFlags_WidthFixed, 22);
            ImGui::TableSetupColumn("lock", ImGuiTableColumnFlags_WidthFixed, 22);
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("go", ImGuiTableColumnFlags_WidthFixed, 22);
            layerRows(0, 0);
            for (Reference& ref : scene_.references) {
                ImGui::PushID(int(ref.id));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (rowToggle("##eye", ref.visible ? Icon::Eye : Icon::EyeShut, ref.visible, ref.visible ? "Shown (click to hide)" : "Hidden (click to show)")) ref.visible = !ref.visible;
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                const std::string label = "Picture: " + stdfs::path(ref.path).filename().string();
                if (ImGui::Selectable(label.c_str(), activeRef_ == ref.id)) activeRef_ = ref.id, selected_.clear(), leaveEdit();
                ImGui::TableNextColumn();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        // (dropped below the list: out of its folder, to the top)
        ImGui::InvisibleButton("##top", ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 8.0f)));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FRY_OBJECT")) {
                const uint32_t oid = *static_cast<const uint32_t*>(p->Data);
                moveToFolder(selected_.count(oid) ? selected_ : std::set<uint32_t>{oid}, 0);
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FRY_FOLDER"))
                if (Folder* g = findFolder(*static_cast<const uint32_t*>(p->Data))) pushUndo(), g->parent = 0;
            ImGui::EndDragDropTarget();
        }
        ImGui::End();
    }

    // A value that's undoable: the undo point is taken when you start dragging or typing.
    template <class F>
    bool undoable(F widget) {
        const bool changed = widget();
        if (ImGui::IsItemActivated()) pushUndo();
        return changed;
    }

    void paintProperties(float w) {
        Object* o = selected_.empty() ? nullptr : find(active_);
        if (sculpting_) { // (its look, while you shape it: what it's made of shows how the light runs over it)
            if (!o) return ImGui::TextWrapped("Brush over an object to sculpt it.\n\nA shape with few points is made finer first, keeping its shape. Tab goes back to painting colours.");
            ImGui::TextColored(accentText(), "%s", o->name.c_str());
            ImGui::TextDisabled("%zu points, %zu faces", o->mesh.verts.size(), o->mesh.faces.size());
            ImGui::SeparatorText("Look");
            materialPicker(*o);
            undoable([&] { return ImGui::ColorEdit3("Colour", &o->color.x); });
            if (ImGui::Checkbox("Smooth shading", &o->smooth)) pushUndo(), touched(o->id);
            return;
        }
        if (!o) {
            ImGui::TextWrapped("Click an object to paint on it.\n\nIts surface gets laid out flat on a picture (the Picture panel), which you can paint on too, "
                               "or put pictures onto: copy one in any app and press Ctrl+V.");
            return;
        }
        ImGui::TextColored(accentText(), "%s", o->name.c_str());
        if (!o->picture) {
            int size = pictureSize_ <= 512 ? 0 : pictureSize_ <= 1024 ? 1 : pictureSize_ <= 2048 ? 2 : 3;
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo("##psize", &size, "512 x 512\0001024 x 1024\0002048 x 2048\0004096 x 4096\0")) pictureSize_ = 512 << size;
            if (ImGui::Button("Start painting it", ImVec2(w, 0))) readyToPaint(o->id);
            ImGui::TextDisabled("(or just paint on it)");
            return;
        }
        ImGui::TextDisabled("Picture %d x %d", o->picture->w, o->picture->h);
        if (o->hasFinish()) ImGui::TextWrapped("It's had rounding or smoothing added since: the picture shows again when you paint (they become part of the shape).");
        ImGui::Checkbox("Show the layout lines", &showUvLines_);
        // The pictures put on it, top first: click one to move it (again), the eye hides it.
        ImGui::SeparatorText("Pictures on it (layers)");
        if (o->layers.empty()) ImGui::TextDisabled("None yet: paste one (Ctrl+V)");
        int move = -1, by = 0, drop = -1, merge = -1;
        for (int i = int(o->layers.size()) - 1; i >= 0; --i) {
            PictureLayer& l = o->layers[size_t(i)];
            const bool on = ledit_.object == o->id && ledit_.index == i;
            ImGui::PushID(i);
            if (rowToggle("##eye", l.visible ? Icon::Eye : Icon::EyeShut, l.visible, l.visible ? "Hide it" : "Show it")) pushUndo(), l.visible = !l.visible;
            ImGui::SameLine();
            if (ImGui::Selectable(std::format("{}{}", l.name, on ? "   (moving)" : l.sticker ? "" : "   (flat)").c_str(), on, 0, ImVec2(w - 92, 0)))
                on ? lockLayer() : unlockLayer(o->id, i);
            ImGui::SameLine(w - 62);
            ImGui::BeginDisabled(i == int(o->layers.size()) - 1);
            if (ImGui::ArrowButton("up", ImGuiDir_Up)) move = i, by = 1;
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("down", ImGuiDir_Down)) move = i, by = -1;
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            if (ImGui::SmallButton("x")) drop = i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete it");
            if (ImGui::BeginPopupContextItem("menu")) {
                if (ImGui::MenuItem("Paint it in for good")) merge = i;
                if (ImGui::MenuItem("Delete")) drop = i;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (move >= 0) pushUndo(), std::swap(o->layers[size_t(move)], o->layers[size_t(move + by)]), ledit_ = {};
        if (drop >= 0) deleteLayer(*o, drop);
        if (merge >= 0) mergeLayer(*o, merge);
        ImGui::Spacing();
        if (ImGui::Button("Fill it all with the colour", ImVec2(w, 0))) {
            Image& img = changePicture(*o);
            img = makeImage(img.w, img.h, glm::vec4(glm::vec3(paintColor_), 1.0f));
            paint_.image.reset();
        }
        if (ImGui::Button("Save the picture as PNG...", ImVec2(w, 0))) dialog(7);
        if (ImGui::Button("Lay it out again", ImVec2(w, 0))) {
            pushUndo();
            o->uvs = autoUnwrap(o->mesh);
            touched(o->id);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lays its surface out flat on the picture again (what's painted won't fit it any more)");
        if (ImGui::Button("Remove the picture", ImVec2(w, 0))) {
            pushUndo();
            o->picture.reset(), o->uvs.clear();
            touched(o->id);
        }
        ImGui::Spacing();
        ImGui::TextWrapped("Tip: paint last. Changing the shape's faces (extrude, knife...) lays it out again, and what's painted moves.");
    }

    void renderProperties(float w) {
        Look& k = scene_.look;
        auto slider = [&](const char* label, float* v, float lo, float hi, const char* fmt) {
            if (undoable([&] { return ImGui::SliderFloat(label, v, lo, hi, fmt); })) dirty_ = true;
        };
        ImGui::SeparatorText("Sun");
        slider("Round", &k.sunTurn, 0, 360, "%.0f deg");
        slider("Up", &k.sunHeight, 2, 90, "%.0f deg");
        slider("Strength", &k.sunStrength, 0, 2, "%.2f");
        undoable([&] { return ImGui::ColorEdit3("Sun colour", &k.sunColor.x); });
        ImGui::SeparatorText("Round it");
        if (ImGui::Checkbox("Sky", &k.sky)) dirty_ = true;
        if (!k.sky) undoable([&] { return ImGui::ColorEdit3("Background", &k.background.x); });
        if (ImGui::Checkbox("Ground", &k.ground)) dirty_ = true;
        if (k.ground) undoable([&] { return ImGui::ColorEdit3("Ground colour", &k.groundColor.x); });
        ImGui::Checkbox("Grid", &renderGrid_);
        ImGui::SeparatorText("Look");
        slider("Brightness", &k.exposure, 0.2f, 3.0f, "%.2f");
        slider("Shade in corners", &k.ao, 0, 1, "%.2f");
        slider("Soft shadows", &k.shadowSoft, 0, 4, "%.2f");
        slider("Glow", &k.bloom, 0, 0.3f, "%.3f");
        slider("Colour", &k.saturation, 0, 2, "%.2f");
        slider("Dark edges", &k.vignette, 0, 1, "%.2f");
        slider("Sharpen", &k.sharpen, 0, 1, "%.2f");
        if (ImGui::Button("Back to how it was", ImVec2(w, 0))) pushUndo(), k = Look{};
        if (ImGui::Button("Save a picture (F12)", ImVec2(w, 0))) dialog(5);
        if (Object* o = selected_.size() == 1 ? find(active_) : nullptr) {
            ImGui::SeparatorText(o->name.c_str());
            materialPicker(*o);
            undoable([&] { return ImGui::ColorEdit3("Colour", &o->color.x); });
            undoable([&] { return ImGui::SliderFloat("Metal", &o->metal, 0, 1); });
            undoable([&] { return ImGui::SliderFloat("Rough", &o->rough, 0, 1); });
            ImGui::TextDisabled("(metal and rough go with it into games)");
        }
    }

    // The Paint tab's picture: the selected object's, flat, to paint on and place pictures on.
    void picturePanel() {
        ImGui::SetNextWindowSizeConstraints(panelMin("Picture"), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin("Picture", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        Object* o = selected_.empty() ? nullptr : find(active_);
        if (!o || !o->picture) {
            if (tab_ == Tab::Layout) {
                ImGui::TextWrapped(o ? "It has no picture yet, so nothing's laid out." : "Click an object: how its picture is laid out shows here.");
                if (o && ImGui::Button("Lay it out (and start its picture)")) readyToPaint(o->id);
                return ImGui::End();
            }
            ImGui::TextWrapped(sculpting_ ? "Sculpting changes its shape, not its picture. Tab goes back to painting colours."
                               : o ? "Paint on it (or press Start painting it in Properties) and its picture shows here." : "Click an object to paint it: its picture shows here.");
            return ImGui::End();
        }
        uploadPicture(*o, !paint_.active);
        const int W = o->picture->w, H = o->picture->h;
        const ImVec2 origin = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
        if (avail.x < 16 || avail.y < 16) return ImGui::End();
        if (picZoom_ <= 0 || picFor_ != o->id) {
            picZoom_ = std::min(avail.x / float(W), avail.y / float(H)) * 0.96f;
            picPan_ = ImVec2((avail.x - float(W) * picZoom_) * 0.5f, (avail.y - float(H) * picZoom_) * 0.5f), picFor_ = o->id;
        }
        ImGui::InvisibleButton("canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        ImGuiIO& io = ImGui::GetIO();
        auto toScreen = [&](glm::vec2 p) { return ImVec2(origin.x + picPan_.x + p.x * picZoom_, origin.y + picPan_.y + p.y * picZoom_); };
        const glm::vec2 mouse((io.MousePos.x - origin.x - picPan_.x) / picZoom_, (io.MousePos.y - origin.y - picPan_.y) / picZoom_);
        if (hovered && io.MouseWheel != 0) { // zoom about the mouse
            const float z = std::clamp(picZoom_ * std::pow(1.2f, io.MouseWheel), 0.05f, 64.0f);
            picPan_ = ImVec2(io.MousePos.x - origin.x - mouse.x * z, io.MousePos.y - origin.y - mouse.y * z), picZoom_ = z;
        }
        if (ImGui::IsItemActive() && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)))
            picPan_.x += io.MouseDelta.x, picPan_.y += io.MouseDelta.y;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
        dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(24, 24, 28, 255));
        uiImage(dl, paintTex_[o->id], toScreen({0, 0}), toScreen({float(W), float(H)}));
        dl->AddRect(toScreen({0, 0}), toScreen({float(W), float(H)}), IM_COL32(0, 0, 0, 255));
        if (tab_ == Tab::Layout) {
            if (o->uvsFit()) layoutCanvas(*o, dl, toScreen, mouse, hovered, W, H);
            dl->PopClipRect();
            return ImGui::End();
        }
        if (showUvLines_ && o->uvsFit())
            for (const auto& face : o->uvs) {
                std::vector<ImVec2> pts;
                for (const glm::vec2& q : face) pts.push_back(toScreen(q * glm::vec2(W, H)));
                dl->AddPolyline(pts.data(), int(pts.size()), IM_COL32(255, 255, 255, 60), ImDrawFlags_Closed, 1.0f);
            }
        PictureLayer* el = ledit_.object == o->id ? editLayer() : nullptr;
        if (el && !el->sticker) {
            ImVec2 q[4];
            for (int i = 0; i < 4; ++i) q[i] = toScreen(el->corners[size_t(i)]);
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ledit_.drag = -1;
                for (int i = 0; i < 4; ++i)
                    if (std::abs(io.MousePos.x - q[i].x) < 10 && std::abs(io.MousePos.y - q[i].y) < 10) ledit_.drag = i;
                if (ledit_.drag < 0 && insidePolygon(io.MousePos, {q[0], q[1], q[2], q[3]})) ledit_.drag = 4;
                if (ledit_.drag >= 0) pushUndo();
                else lockLayer();
                ledit_.grab = mouse, ledit_.startCorners = el->corners;
            }
            if (ledit_.drag >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const glm::vec2 d = mouse - ledit_.grab;
                const int i = ledit_.drag;
                if (i == 4)
                    for (int k = 0; k < 4; ++k) el->corners[size_t(k)] = ledit_.startCorners[size_t(k)] + d;
                else if (io.KeyShift) { // bigger or smaller, the opposite corner staying
                    const glm::vec2 opp = ledit_.startCorners[size_t((i + 2) % 4)];
                    const float k = glm::length(mouse - opp) / std::max(glm::length(ledit_.startCorners[size_t(i)] - opp), 1e-3f);
                    for (int j = 0; j < 4; ++j) el->corners[size_t(j)] = opp + (ledit_.startCorners[size_t(j)] - opp) * k;
                } else
                    el->corners[size_t(i)] = ledit_.startCorners[size_t(i)] + d;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) ledit_.drag = -1;
            for (int i = 0; i < 4; ++i) q[i] = toScreen(el->corners[size_t(i)]);
            dl->AddPolyline(q, 4, kAccent, ImDrawFlags_Closed, 2.0f);
            for (int i = 0; i < 4; ++i) {
                const bool hot = ledit_.drag == i || (std::abs(io.MousePos.x - q[i].x) < 10 && std::abs(io.MousePos.y - q[i].y) < 10);
                dl->AddRectFilled(ImVec2(q[i].x - 6, q[i].y - 6), ImVec2(q[i].x + 6, q[i].y + 6), hot ? kHot : IM_COL32(255, 255, 255, 240));
                dl->AddRect(ImVec2(q[i].x - 6, q[i].y - 6), ImVec2(q[i].x + 6, q[i].y + 6), IM_COL32(20, 20, 25, 255));
            }
        } else {
            const bool onPicture = mouse.x >= 0 && mouse.y >= 0 && mouse.x < float(W) && mouse.y < float(H);
            const glm::vec2 uv = mouse / glm::vec2(W, H);
            // Which layer is on top under the mouse (0: the painting).
            int under = 0;
            if (const auto it = owner_.find(o->id); onPicture && it != owner_.end() && it->second.size() == size_t(W) * size_t(H))
                under = it->second[size_t(int(mouse.y)) * size_t(W) + size_t(int(mouse.x))];
            if (hovered && under && !paint_.active) ImGui::SetTooltip("%s: click to move it", o->layers[size_t(under - 1)].name.c_str());
            if (hovered && onPicture && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (under || el)) {
                if (under) unlockLayer(o->id, under - 1); // (clicking a placed picture here unlocks it)
                else lockLayer();
            } else if (hovered && onPicture && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (paintTool_ == PaintTool::Picker || io.KeyAlt) pickColor(*o, uv);
                else if (paintTool_ == PaintTool::Fill) fillAt(*o, uv);
                else if (paintTool_ == PaintTool::Brush || paintTool_ == PaintTool::Eraser) {
                    beginStroke(*o, true);
                    paint_.stroke.dab(*paint_.image, mouse, brushPx_ / picZoom_, hardness_);
                    paint_.last2D = mouse, paint_.haveLast = true;
                }
            }
            if (paint_.active && paint_.in2D) {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) endStroke();
                else {
                    const float r = brushPx_ / picZoom_, gap = glm::distance(mouse, paint_.last2D);
                    const int steps = std::max(1, int(std::ceil(gap / std::max(r * 0.3f, 0.5f))));
                    for (int i = 1; i <= steps; ++i) paint_.stroke.dab(*paint_.image, glm::mix(paint_.last2D, mouse, float(i) / float(steps)), r, hardness_);
                    paint_.last2D = mouse;
                    uploaded_[o->id].dirty = true;
                }
            }
            if (hovered && (paintTool_ == PaintTool::Brush || paintTool_ == PaintTool::Eraser)) dl->AddCircle(io.MousePos, brushPx_, IM_COL32(255, 255, 255, 200), 40, 1.5f);
        }
        dl->PopClipRect();
        ImGui::End();
    }

    void propertiesPanel() {
        ImGui::SetNextWindowSizeConstraints(panelMin("Properties"), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin("Properties", nullptr, ImGuiWindowFlags_NoCollapse);
        const float w = ImGui::GetContentRegionAvail().x;
        if (tab_ == Tab::Paint) {
            paintProperties(w);
            return ImGui::End();
        }
        if (tab_ == Tab::Render) {
            renderProperties(w);
            return ImGui::End();
        }
        if (tab_ == Tab::Layout) {
            layoutProperties(w);
            return ImGui::End();
        }
        if (tab_ == Tab::Print) {
            printProperties(w);
            return ImGui::End();
        }
        if (Reference* ref = activeRef_ && selected_.empty() ? findRef(activeRef_) : nullptr) {
            ImGui::TextWrapped("%s", ref->path.c_str());
            int axis = int(ref->axis);
            if (ImGui::Combo("Facing", &axis, "Front view\0Side view\0Top view\0")) pushUndo(), ref->axis = Axis(axis);
            undoable([&] { return ImGui::DragFloat2("Middle (m)", &ref->offset.x, 0.005f); });
            undoable([&] { return ImGui::DragFloat("Width (m)", &ref->width, 0.005f, -100.0f, 100.0f, "%.3f"); });
            ImGui::TextDisabled("A minus width flips it.");
            undoable([&] { return ImGui::SliderFloat("See-through", &ref->opacity, 0.05f, 1.0f); });
            if (ImGui::Button("Remove picture", ImVec2(w, 0))) deleteSelected();
            ImGui::End();
            return;
        }
        if (Object* o = editing()) {
            editProperties(*o, w);
            ImGui::End();
            return;
        }
        if (selected_.empty()) {
            ImGui::TextWrapped("Nothing selected.\n\nClick an object to select it, or add one with + in the toolbox (or the Add menu): "
                               "shapes and starter models, with sizes you can change. Draw shape (D) turns an outline you click into a solid.");
            ImGui::End();
            return;
        }
        if (selected_.size() > 1) {
            ImGui::Text("%zu objects selected", selected_.size());
            glm::vec3 col = find(active_) ? find(active_)->color : glm::vec3(0.8f);
            if (undoable([&] { return ImGui::ColorEdit3("Colour (all)", &col.x); }))
                for (uint32_t id : selected_)
                    if (Object* o = find(id)) o->color = col, dirty_ = true;
            if (ImGui::Button("Join into one (Ctrl+J)", ImVec2(w, 0))) joinSelected();
            ImGui::End();
            return;
        }
        Object* o = find(*selected_.begin());
        if (!o) {
            ImGui::End();
            return;
        }
        ImGui::BeginDisabled(o->locked);
        char name[128];
        std::snprintf(name, sizeof name, "%s", o->name.c_str());
        if (undoable([&] { return ImGui::InputText("Name", name, sizeof name); }) && name[0]) o->name = name;
        // A library shape: its own sizes, until its shape's edited by hand.
        if (const ShapeRecipe* r = findRecipe(o->recipe)) {
            ImGui::SeparatorText(std::format("{} settings", r->name).c_str());
            bool changed = false;
            o->settings.resize(r->settings.size());
            for (size_t i = 0; i < r->settings.size(); ++i) {
                const ShapeSetting& s = r->settings[i];
                const float speed = s.whole ? 0.1f : std::max(0.001f, (s.max - s.min) * 0.0005f);
                changed |= undoable([&] { return ImGui::DragFloat(s.name, &o->settings[i], speed, s.min, s.max, s.whole ? "%.0f" : "%.3f"); });
            }
            if (changed) o->mesh = makeFromRecipe(*r, o->settings), touched(o->id);
            ImGui::TextDisabled("(These go once you edit the shape by hand.)");
        }
        ImGui::SeparatorText("Place");
        undoable([&] { return ImGui::DragFloat3("Position (m)", &o->position.x, 0.005f, 0, 0, "%.3f"); });
        glm::vec3 euler = toEuler(o->rotation);
        if (undoable([&] { return ImGui::DragFloat3("Turn (deg)", &euler.x, 0.5f, 0, 0, "%.1f"); })) o->rotation = fromEuler(euler);
        if (undoable([&] { return ImGui::DragFloat3("Size", &o->scale.x, 0.005f, 0, 0, "%.3f"); })) touched(o->id);
        const glm::vec3 lo = boundsMin(shownMesh(*o)) * glm::abs(o->scale), hi = boundsMax(shownMesh(*o)) * glm::abs(o->scale);
        ImGui::TextDisabled("%.3f x %.3f x %.3f m", hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
        ImGui::SeparatorText("Look");
        materialPicker(*o);
        undoable([&] { return ImGui::ColorEdit3("Colour", &o->color.x); });
        undoable([&] { return ImGui::SliderFloat("Metal", &o->metal, 0, 1); });
        undoable([&] { return ImGui::SliderFloat("Rough", &o->rough, 0, 1); });
        if (ImGui::Checkbox("Smooth shading", &o->smooth)) pushUndo(), touched(o->id);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Round surfaces look round; edges sharper than the angle stay sharp");
        if (o->smooth) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (undoable([&] { return ImGui::SliderFloat("##angle", &o->smoothAngle, 1, 180, "creases > %.0f deg"); })) touched(o->id);
        }
        // Finishing touches that stay changeable: rounded edges, smoothing, a mirrored half.
        ImGui::SeparatorText("Round, smooth, mirror");
        if (undoable([&] { return ImGui::DragFloat("Round edges", &o->round, 0.001f, 0.0f, 10.0f, o->round > 0 ? "%.3f m" : "off"); })) {
            if (o->round > 0 && o->roundSteps > 1) o->smooth = true;
            touched(o->id);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rounds off every sharp edge and corner (drag right)");
        if (o->round > 0) {
            if (undoable([&] { return ImGui::SliderInt("Steps round", &o->roundSteps, 1, 8); })) touched(o->id);
            if (undoable([&] { return ImGui::SliderFloat("Edges sharper than", &o->roundAngle, 1, 179, "%.0f deg"); })) touched(o->id);
        }
        if (undoable([&] { return ImGui::SliderInt("Smooth out", &o->smoothLevels, 0, 3, o->smoothLevels ? "%d times" : "off"); })) {
            if (o->smoothLevels) o->smooth = true;
            touched(o->id);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cuts every face into smaller ones and rounds the whole shape (like clay)");
        if (o->smoothLevels && ImGui::Checkbox("Keep sharp edges sharp", &o->smoothKeepSharp)) pushUndo(), touched(o->id);
        ImGui::TextDisabled("Mirror across");
        for (int a = 0; a < 3; ++a) {
            ImGui::SameLine();
            bool on = o->mirror & (1 << a);
            if (ImGui::Checkbox(a == 0 ? "X" : a == 1 ? "Y" : "Z", &on)) pushUndo(), o->mirror ^= 1 << a, touched(o->id);
        }
        if (o->hasFinish() && ImGui::Button("Make it part of the shape", ImVec2(w, 0))) applyFinish(*o);
        if (o->hasFinish() && ImGui::IsItemHovered()) ImGui::SetTooltip("Bake the rounding, smoothing and mirror into the points (so you can edit them)");
        ImGui::SeparatorText("Do");
        if (ImGui::Button("Edit shape (Tab)", ImVec2(w, 0))) enterEdit();
        if (ImGui::Button("Apply turn and size", ImVec2(w, 0))) applyTransform(*o);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bake them into the shape: turn becomes 0, size 1");
        if (ImGui::Button("Origin to middle", ImVec2((w - 6) / 2, 0))) originToMiddle(*o);
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Origin to bottom", ImVec2(-1, 0))) originToBottom(*o);
        if (ImGui::Button("Flip X", ImVec2((w - 12) / 3, 0))) mirror(*o, 0);
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Flip Y", ImVec2((w - 12) / 3, 0))) mirror(*o, 1);
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Flip Z", ImVec2(-1, 0))) mirror(*o, 2);
        ImGui::EndDisabled();
        if (ImGui::Button(o->locked ? "Unlock" : "Lock", ImVec2((w - 6) / 2, 0))) o->locked = !o->locked;
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Delete", ImVec2(-1, 0))) deleteSelected();
        ImGui::End();
    }

    // A material to start from: sets colour, metal and rough in one go (each a little ball of it).
    // Yours (saved here, or from plug-ins and scripts) first; "Save its look as a material" adds one.
    void materialPicker(Object& o) {
        auto matches = [&](const MaterialPreset& m) {
            return std::abs(m.metal - o.metal) < 0.01f && std::abs(m.rough - o.rough) < 0.01f &&
                   (m.keepColor || glm::all(glm::lessThan(glm::abs(m.color - o.color), glm::vec3(0.01f))));
        };
        const MaterialPreset* now = nullptr;
        for (const MaterialPreset& m : materialPresets())
            if (matches(m)) now = &m;
        for (const MaterialPreset& m : ownMaterials())
            if (matches(m)) now = &m;
        ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
        if (ImGui::BeginCombo("Material", now ? now->name.c_str() : "(its own)", ImGuiComboFlags_HeightLarge)) {
            std::optional<std::string> remove;
            auto item = [&](const MaterialPreset& m, bool own) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float h = ImGui::GetTextLineHeight();
                ImGui::PushID(m.id.c_str());
                ImGui::PushID(own);
                if (ImGui::Selectable("##m", now == &m, 0, ImVec2(0, h))) pushUndo(), applyMaterial(o, m), touched(o.id);
                if (own && m.from.empty() && ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Remove it")) remove = m.id;
                    ImGui::EndPopup();
                }
                if (own && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", m.from.empty() ? "Yours (right-click to remove)" : ("From " + m.from).c_str());
                ImGui::PopID();
                ImGui::PopID();
                // A ball: its colour, darker round the edge, a highlight as sharp as it's smooth.
                const glm::vec3 c = m.keepColor ? o.color : m.color;
                const ImVec2 mid(p.x + h * 0.5f, p.y + h * 0.5f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddCircleFilled(mid, h * 0.45f, ImGui::ColorConvertFloat4ToU32(ImVec4(c.r * 0.55f, c.g * 0.55f, c.b * 0.55f, 1)));
                dl->AddCircleFilled(ImVec2(mid.x - h * 0.05f, mid.y - h * 0.05f), h * 0.36f, ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1)));
                dl->AddCircleFilled(ImVec2(mid.x - h * 0.14f, mid.y - h * 0.14f), h * (0.06f + 0.1f * m.rough), IM_COL32(255, 255, 255, int(230 * (1.0f - m.rough * 0.8f))));
                dl->AddText(ImVec2(p.x + h * 1.4f, p.y), ImGui::GetColorU32(ImGuiCol_Text), m.name.c_str());
            };
            if (!ownMaterials().empty()) {
                ImGui::SeparatorText("Yours");
                for (const MaterialPreset& m : ownMaterials()) item(m, true);
                ImGui::SeparatorText("Fry3DX's");
            }
            for (const MaterialPreset& m : materialPresets()) item(m, false);
            ImGui::Separator();
            if (ImGui::Selectable("Save its look as a material...")) nameMaterial_ = true, materialName_.clear(), materialKeepColor_ = false;
            ImGui::EndCombo();
            if (remove && removeOwnMaterial(*remove)) saveOwnMaterials();
        }
        if (std::exchange(nameMaterial_, false)) ImGui::OpenPopup("Save as a material");
        ImGui::SetNextWindowSize(ImVec2(340, 0));
        if (ImGui::BeginPopupModal("Save as a material", nullptr, ImGuiWindowFlags_NoResize)) {
            ImGui::TextWrapped("Its colour, metal and rough as one of your materials, in the picker for every file.");
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            inputString("##name", materialName_, "Its name (\"Teal plastic\")");
            ImGui::Checkbox("Only how metal and rough (keep each object's colour)", &materialKeepColor_);
            const bool taken = std::any_of(materialPresets().begin(), materialPresets().end(), [&](const MaterialPreset& m) { return m.id == materialId(materialName_); });
            if (taken) ImGui::TextColored(roleColor(keel::ui::Role::DangerText), "That's one of Fry3DX's own: another name");
            ImGui::BeginDisabled(materialId(materialName_).empty() || taken);
            pushAccent();
            const bool save = ImGui::Button("Save", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false);
            popAccent();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (save && !materialId(materialName_).empty() && !taken) {
                addOwnMaterial({materialId(materialName_), materialName_, o.color, o.metal, o.rough, materialKeepColor_, ""});
                saveOwnMaterials();
                say("Saved \"" + materialName_ + "\": it's in the Material picker now");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void editProperties(Object& o, float w) {
        ImGui::Text("Editing the shape of %s", o.name.c_str());
        ImGui::TextDisabled("%zu points, %zu faces", o.mesh.verts.size(), o.mesh.faces.size());
        if (o.hasFinish()) ImGui::TextDisabled("(its rounding and smoothing show again when you're done)");
        const std::set<int> pts = selectedPoints();
        if (!pts.empty()) {
            // Where the selection's middle is (in the world); typing moves it there.
            const glm::vec3 c = pivotOfSelection();
            glm::vec3 at = c;
            if (undoable([&] { return ImGui::DragFloat3("Middle (m)", &at.x, 0.002f, 0, 0, "%.3f"); })) {
                const glm::mat4 m = objectMatrix(o), local = glm::inverse(m) * glm::translate(glm::mat4(1.0f), at - c) * m;
                for (int v : pts) o.mesh.verts[size_t(v)] = glm::vec3(local * glm::vec4(o.mesh.verts[size_t(v)], 1.0f));
                handMade(o);
                touched(o.id);
            }
        }
        ImGui::SeparatorText("Faces");
        ImGui::SetNextItemWidth(w * 0.45f);
        ImGui::DragFloat("##ext", &extrudeAmount_, 0.001f, -10, 10, "%.3f m");
        ImGui::SameLine();
        if (ImGui::Button("Extrude", ImVec2(-1, 0))) extrudeNow(extrudeAmount_);
        ImGui::SetNextItemWidth(w * 0.45f);
        ImGui::SliderFloat("##ins", &insetAmount_, 0.01f, 0.99f, "%.2f");
        ImGui::SameLine();
        if (ImGui::Button("Inset", ImVec2(-1, 0))) insetNow(insetAmount_);
        if (ImGui::Button("Flip", ImVec2((w - 6) / 2, 0))) flipNow();
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Make face", ImVec2(-1, 0))) makeFaceNow();
        if (ImGui::Button("Bridge two faces", ImVec2(w, 0))) bridgeNow();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Two faces with as many corners: taken out and joined by a tunnel");
        if (ImGui::Button("Make into its own object (P)", ImVec2(w, 0))) separateSelected();
        ImGui::SeparatorText("Edges");
        ImGui::SetNextItemWidth(w * 0.45f);
        ImGui::DragFloat("##bw", &bevelWidth_, 0.001f, 0.0f, 10.0f, "%.3f m");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w * 0.2f);
        ImGui::DragInt("##bs", &bevelSteps_, 0.1f, 1, 10);
        ImGui::SameLine();
        if (ImGui::Button("Round", ImVec2(-1, 0))) bevelNow();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Round the selected edges (or the edges round the selected faces): width, steps");
        ImGui::SeparatorText("Points");
        if (ImGui::Button("Merge to middle (M)", ImVec2(w, 0))) mergeNow();
        if (ImGui::Button("Smooth", ImVec2((w - 6) / 2, 0))) smoothPointsNow();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Even out bumps: each point towards its neighbours");
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Push / pull (Alt+S)", ImVec2(-1, 0))) startPush();
        ImGui::SetNextItemWidth(w * 0.45f);
        ImGui::DragFloat("##md", &mergeDistance_, 0.0001f, 0.0f, 1.0f, "%.4f m");
        ImGui::SameLine();
        if (ImGui::Button("Merge close", ImVec2(-1, 0))) {
            pushUndo();
            const int gone = mergeByDistance(o.mesh, mergeDistance_);
            changedShape(o);
            say(std::format("{} points merged", gone));
        }
        ImGui::SeparatorText("Whole shape");
        if (ImGui::Button("Cut in four", ImVec2((w - 6) / 2, 0))) subdivideNow(false);
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Smooth it", ImVec2(-1, 0))) subdivideNow(true);
        ImGui::SeparatorText("Select");
        if (ImGui::Button("All", ImVec2((w - 12) / 3, 0))) selectAllElements();
        ImGui::SameLine(0, 6);
        if (ImGui::Button("None", ImVec2((w - 12) / 3, 0))) clearElements();
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Invert", ImVec2(-1, 0))) invertSelection();
        if (ImGui::Button("More", ImVec2((w - 12) / 3, 0))) growSelection();
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Less", ImVec2((w - 12) / 3, 0))) shrinkSelection();
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Joined", ImVec2(-1, 0))) selectLinked();
        ImGui::Separator();
        if (ImGui::Button("Delete (Del)", ImVec2(w, 0))) deleteElements();
        if (ImGui::Button("Done (Tab)", ImVec2(w, 0))) leaveEdit();
    }

    void statusBar() {
        ImGui::Begin("Status", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
        if (now() - messageAt_ < 4.0) ImGui::TextColored(accentText(), "%s", message_.c_str());
        else if (tab_ == Tab::Layout) {
            for (const LayToolInfo& t : kLayTools)
                if (t.tool == layTool_)
                    ImGui::TextDisabled("%s:  %s   ·   Tab: frames or corners   Right drag: orbit (pan in the Picture panel)   F1: all keys", t.name, t.hint);
        } else if (tab_ == Tab::Print && printTool_ != PrintTool::None) {
            ImGui::TextDisabled(printTool_ == PrintTool::Measure ? "Measure:  click two points on the model (corners catch the mouse); a third click starts again   ·   M: done"
                                                                 : "Lay flat:  click the face it should stand on: it turns so that face is down, on the bed   ·   L: done");
        } else if (tab_ == Tab::Paint && sculpting_) {
            for (const SculptToolInfo& t : kSculptTools)
                if (t.brush == sculptBrush_) ImGui::TextDisabled("%s:  %s   ·   Shift: smooth   X: both sides   Tab: colour   Right drag: orbit   F1: all keys", t.name, t.hint);
        } else if (tab_ == Tab::Paint) {
            for (const PaintToolInfo& t : kPaintTools)
                if (t.tool == paintTool_) ImGui::TextDisabled("%s:  %s   ·   Right drag: orbit   Wheel: zoom (in the Picture panel too)   F1: all keys", t.name, t.hint);
        } else
            for (const ToolInfo& t : kTools)
                if (t.tool == tool_) ImGui::TextDisabled("%s:  %s   ·   Right drag: orbit   Middle drag: pan   Wheel: zoom   F1: all keys", t.name, t.hint);
        ImGui::End();
    }

    // The view's corner: which way you're looking, buttons to look straight at a side, and
    // handles to drag to orbit, pan and zoom (for a touchpad or a pen).
    void viewButtons() {
        const ImVec2 at(viewMax_.x - 10, viewMin_.y + 10);
        ImGui::SetNextWindowPos(at, ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##views", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                              ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        static const char* names[] = {"Perspective", "Front", "Back", "Left", "Right", "Top", "Bottom"};
        static const char* shadings[] = {"Solid", "Rendered", "Wireframe"};
        ImGui::TextColored(accentText(), "%s", names[int(view_)]);
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", shadings[int(shading_)], xray_ ? ", X-ray" : "");
        for (int i = 1; i < 7; ++i) {
            if (i != 1 && i != 4) ImGui::SameLine(0, 3);
            if (ImGui::Button(names[i], ImVec2(56, 0))) setView(View(i));
        }
        if (ImGui::Button("3D", ImVec2(56, 0))) leaveFlatView();
        ImGui::SameLine(0, 3);
        if (ImGui::Button("Frame", ImVec2(56, 0))) frameSelected();
        ImGui::SameLine(0, 3);
        if (ImGui::Button(xray_ ? "Solid" : "X-ray", ImVec2(56, 0))) xray_ = !xray_;
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        ImGui::Button("Orbit", ImVec2(56, 0));
        if (ImGui::IsItemActive()) orbitBy(d);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to orbit");
        ImGui::SameLine(0, 3);
        ImGui::Button("Pan", ImVec2(56, 0));
        if (ImGui::IsItemActive()) panBy(d);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to pan");
        ImGui::SameLine(0, 3);
        ImGui::Button("Zoom", ImVec2(56, 0));
        if (ImGui::IsItemActive()) dist_ = std::clamp(dist_ * std::pow(1.01f, d.y), 0.01f, 5000.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag up and down to zoom");
        if (tab_ != Tab::Render && ImGui::Button(shading_ == Shading::Rendered ? "Show: Solid" : "Show: Rendered", ImVec2(56 * 3 + 6, 0)))
            shading_ = shading_ == Shading::Rendered ? Shading::Solid : Shading::Rendered;
        ImGui::End();
    }

    // ───────────────────────────── scripts ─────────────────────────────

    void scriptLine(std::string text, bool error = false) {
        size_t at = 0;
        for (size_t nl; (nl = text.find('\n', at)) != std::string::npos; at = nl + 1) scriptLog_.push_back({text.substr(at, nl - at), error});
        scriptLog_.push_back({text.substr(at), error});
        if (scriptLog_.size() > 2000) scriptLog_.erase(scriptLog_.begin(), scriptLog_.begin() + 500);
        scriptLogToBottom_ = true;
    }
    fry::SceneScript* currentScript() { return script_ < scene_.scripts.size() ? &scene_.scripts[script_] : nullptr; }
    // "little_town.lua" -> "little town" (a script's name in the list).
    static std::string scriptName(const std::string& path) {
        std::string n = stdfs::path(fileName(path)).stem().string();
        std::replace(n.begin(), n.end(), '_', ' ');
        return n.empty() ? "Script" : n;
    }
    // Adds a script to the file's list and shows it (the same one again: just shows it).
    void addScript(std::string name, std::string code) {
        for (size_t i = 0; i < scene_.scripts.size(); ++i)
            if (scene_.scripts[i].name == name && scene_.scripts[i].code == code) return void((script_ = i, showScript_ = true));
        const std::string base = name;
        for (int n = 2; std::any_of(scene_.scripts.begin(), scene_.scripts.end(), [&](const fry::SceneScript& s) { return s.name == name; }); ++n)
            name = std::format("{} {}", base, n);
        scene_.scripts.push_back({name, std::move(code)});
        script_ = scene_.scripts.size() - 1;
        showScript_ = true, dirty_ = true;
    }
    // Runs the Script panel's script: one undo step; if it stops with an error, everything it did
    // is taken back.
    void runScriptNow() {
        const fry::SceneScript* sc = currentScript();
        if (!sc) return say("No script to run: Script > New script, or open one");
        if (mode_ == Mode::Edit) leaveEdit();
        const std::string name = sc->name, code = sc->code; // (the list itself may change while it runs)
        pushUndo();
        bool frame = false;
        fry::ScriptHost host{&scene_, &selected_, &active_, [this](const std::string& t) { scriptLine(t); }, [&] { frame = true; }};
        const fry::ScriptResult r = fry::runScript(host, code, name);
        finishRun(r, name, frame);
    }
    // After a script or plug-in command: kept as one undo step, or all taken back on an error.
    void finishRun(const fry::ScriptResult& r, const std::string& name, bool frame) {
        touchAll();
        if (r.ok && frame) frameAll();
        if (!r.ok) {
            takeBack();
            scriptLine(r.error, true);
            showScript_ = true;
            return say(name + " stopped (nothing it did was kept): " + r.error.substr(0, r.error.find('\n')));
        }
        dirty_ = true;
        say(r.objectsMade ? std::format("{} ran: {} object{} made (Ctrl+Z takes it all back)", name, r.objectsMade, r.objectsMade == 1 ? "" : "s")
                          : std::format("{} ran (Ctrl+Z takes it back)", name));
    }
    void openScript(const std::string& path, const std::string& shownName) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return say("Couldn't open " + shownName);
        addScript(scriptName(shownName), std::string(std::istreambuf_iterator<char>(in), {}));
        scriptPath_ = isUri(path) ? std::string() : path;
        say("Opened " + shownName + ": it's kept in this file now. F5 runs it");
    }
    void saveScript(std::string file) {
        const fry::SceneScript* sc = currentScript();
        if (!sc) return;
        if (isUri(file)) {
            const std::string temp = tempFile("script.lua");
            std::string error;
            const bool ok = writeFileSafely(temp, sc->code.data(), sc->code.size(), &error) && toUri(temp, file);
            return say(ok ? "Saved " + fileName(file) : "Couldn't write the file you picked");
        }
        if (stdfs::path(file).extension() != ".lua") file += ".lua";
        std::string error;
        if (!writeFileSafely(file, sc->code.data(), sc->code.size(), &error)) return say("Couldn't save the script: " + error);
        scriptPath_ = file;
        say("Saved " + fileName(file));
    }
    // The scripts that come with it (content/Fry3DX/scripts), as menu items that add one.
    void exampleScripts() {
        if (!ImGui::BeginMenu("Examples")) return;
        exampleItems(false);
        ImGui::EndMenu();
    }
    void exampleItems(bool selectable) {
        for (const std::string& f : keel::fs::list("scripts", ".lua")) {
            const std::string name = scriptName(f);
            if (selectable ? ImGui::Selectable(name.c_str()) : ImGui::MenuItem(name.c_str()))
                if (auto text = keel::fs::readText(f)) addScript(name, *text);
        }
    }

    // ── Own materials (materials.txt in the settings folder) ──
    static std::string ownMaterialsPath() { return keel::prefPath() + "materials.txt"; }
    void loadOwnMaterials() {
        std::ifstream in(ownMaterialsPath(), std::ios::binary);
        if (in) parseOwnMaterials(std::string(std::istreambuf_iterator<char>(in), {}));
    }
    void saveOwnMaterials() {
        const std::string text = ownMaterialsText();
        std::string error;
        if (!writeFileSafely(ownMaterialsPath(), text.data(), text.size(), &error)) say("Couldn't keep your materials: " + error);
    }

    // ── Plug-ins: content/Fry3DX/plugins (they come with it) and the settings folder's plugins ──
    static std::string pluginsFolder() { return keel::prefPath() + "plugins/"; }
    std::set<std::string> pluginsOff() const {
        std::set<std::string> off;
        std::istringstream in(fry_pluginsoff.str());
        for (std::string f; std::getline(in, f, '|');)
            if (!f.empty()) off.insert(f);
        return off;
    }
    void setPluginOn(const std::string& file, bool on) {
        std::set<std::string> off = pluginsOff();
        if (on) off.erase(file);
        else off.insert(file);
        std::string text;
        for (const std::string& f : off) text += (text.empty() ? "" : "|") + f;
        fry_pluginsoff.set(text);
        loadPlugins();
    }
    void loadPlugins() {
        for (const PluginEntry& e : plugins_) fry::clearOwnMaterials(e.file);
        plugins_.clear();
        std::error_code ec;
        stdfs::create_directories(pluginsFolder(), ec);
        std::vector<std::pair<PluginEntry, std::string>> found; // (and its code)
        for (const std::string& f : keel::fs::list("plugins", ".lua"))
            if (auto text = keel::fs::readText(f)) found.push_back({PluginEntry{f, fileName(f), "", true}, *text});
        std::vector<stdfs::path> mine;
        for (const auto& d : stdfs::directory_iterator(pluginsFolder(), ec))
            if (d.path().extension() == ".lua") mine.push_back(d.path());
        std::sort(mine.begin(), mine.end());
        for (const stdfs::path& f : mine) {
            std::ifstream in(f, std::ios::binary);
            found.push_back({PluginEntry{f.string(), f.filename().string()}, std::string(std::istreambuf_iterator<char>(in), {})});
        }
        const std::set<std::string> off = pluginsOff();
        fry::Scene scratch; // (loading a plug-in can't touch the open file: only its commands, when run, do)
        std::set<uint32_t> none;
        uint32_t noActive = 0;
        for (auto& [e, code] : found) {
            e.on = !off.count(e.file);
            if (e.on) {
                e.p = std::make_unique<fry::Plugin>(e.file);
                fry::ScriptHost host{&scratch, &none, &noActive, [this, f = e.file](const std::string& t) { scriptLine(f + ": " + t); }, {}};
                if (!e.p->load(host, code, &e.error)) {
                    fry::clearOwnMaterials(e.file);
                    scriptLine(e.error, true);
                    keel::log::warn("Plug-in {} didn't load: {}", e.file, e.error.substr(0, e.error.find('\n')));
                }
            }
            plugins_.push_back(std::move(e));
        }
        size_t loaded = 0;
        for (const PluginEntry& e : plugins_) loaded += e.p && e.error.empty();
        if (loaded) keel::log::info("Plug-ins: {} loaded", loaded);
    }
    void openPluginsFolder() {
        std::error_code ec;
        stdfs::create_directories(pluginsFolder(), ec);
        SDL_OpenURL(("file://" + pluginsFolder()).c_str());
    }
    bool pluginUsable(size_t i) const { return i < plugins_.size() && plugins_[i].p && plugins_[i].error.empty(); }
    void pluginMenuItems(const char* menu) {
        bool first = true;
        for (size_t i = 0; i < plugins_.size(); ++i)
            if (pluginUsable(i))
                for (size_t c = 0; c < plugins_[i].p->commands.size(); ++c)
                    if (plugins_[i].p->commands[c].menu == menu) {
                        if (std::exchange(first, false)) ImGui::SeparatorText("Plug-ins");
                        pluginMenuItem(i, c);
                    }
    }
    void pluginMenuItem(size_t i, size_t c) {
        const fry::PluginCommand& cmd = plugins_[i].p->commands[c];
        if (ImGui::MenuItem((cmd.name + (cmd.settings.empty() ? "" : "...")).c_str())) {
            if (cmd.settings.empty()) runPluginCommand(i, c, {});
            else {
                pendingCommand_ = PendingCommand{i, c, {}};
                for (const auto& [n, v] : cmd.settings) pendingCommand_->values.push_back(v);
            }
        }
        if (!cmd.help.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)", cmd.help.c_str(), plugins_[i].p->name.c_str());
    }
    void runPluginCommand(size_t i, size_t c, const std::vector<double>& values) {
        if (!pluginUsable(i) || c >= plugins_[i].p->commands.size()) return;
        if (mode_ == Mode::Edit) leaveEdit();
        pushUndo();
        bool frame = false;
        fry::ScriptHost host{&scene_, &selected_, &active_, [this](const std::string& t) { scriptLine(t); }, [&] { frame = true; }};
        const fry::ScriptResult r = plugins_[i].p->run(host, c, values);
        finishRun(r, plugins_[i].p->commands[c].name, frame);
    }
    // A plug-in command's settings, asked for before it runs.
    void commandForm() {
        if (!pendingCommand_) return;
        if (!pluginUsable(pendingCommand_->plugin)) return pendingCommand_.reset();
        const fry::PluginCommand& cmd = plugins_[pendingCommand_->plugin].p->commands[pendingCommand_->command];
        const std::string title = cmd.name + "###plugincommand";
        if (!ImGui::IsPopupOpen("###plugincommand")) ImGui::OpenPopup(title.c_str());
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(340, 0));
        if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize)) return;
        if (!cmd.help.empty()) ImGui::TextWrapped("%s", cmd.help.c_str());
        ImGui::TextDisabled("From %s", plugins_[pendingCommand_->plugin].p->name.c_str());
        for (size_t k = 0; k < cmd.settings.size(); ++k) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
            std::string label = cmd.settings[k].first;
            if (!label.empty()) label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
            std::replace(label.begin(), label.end(), '_', ' ');
            ImGui::InputDouble(label.c_str(), &pendingCommand_->values[k], 0, 0, "%.4g");
        }
        ImGui::Spacing();
        pushAccent();
        const bool go = ImGui::Button("Make it", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        popAccent();
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(120, 0));
        if (go || cancel) {
            ImGui::CloseCurrentPopup();
            const PendingCommand p = *pendingCommand_;
            pendingCommand_.reset();
            if (go) runPluginCommand(p.plugin, p.command, p.values);
        }
        ImGui::EndPopup();
    }
    void pluginsWindow() {
        using keel::ui::Role;
        ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Plug-ins", &showPlugins_, ImGuiWindowFlags_NoDocking)) return ImGui::End();
        ImGui::PushStyleColor(ImGuiCol_Text, roleColor(Role::TextSecondary));
        ImGui::TextWrapped("Plug-ins are scripts that stay loaded: they add commands to the menus and materials to the picker. "
                           "Put a .lua file in the plug-ins folder and load them again.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Open the plug-ins folder")) openPluginsFolder();
        ImGui::SameLine();
        if (ImGui::Button("Load them again")) loadPlugins();
        ImGui::SameLine();
        if (ImGui::Button("How to write one")) showScript_ = showScriptHelp_ = true;
        ImGui::Spacing();
        if (plugins_.empty()) ImGui::TextDisabled("None yet.");
        std::optional<std::pair<std::string, bool>> toggle;
        for (size_t i = 0; i < plugins_.size(); ++i) {
            const PluginEntry& e = plugins_[i];
            ImGui::PushID(int(i));
            ImGui::BeginChild("plugin", ImVec2(0, 0), ImGuiChildFlags_FrameStyle | ImGuiChildFlags_AutoResizeY);
            bool on = e.on;
            if (ImGui::Checkbox("##on", &on)) toggle = {e.file, on};
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(on ? "On: turn it off" : "Off: turn it on");
            ImGui::SameLine();
            ImGui::PushFont(keel::ui::font(keel::ui::Font::Bold), 0.0f);
            ImGui::TextUnformatted(e.p ? e.p->name.c_str() : scriptName(e.file).c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s", e.file.c_str(), e.bundled ? "  (comes with Fry3DX)" : "");
            if (e.p && !e.p->about.empty()) ImGui::TextWrapped("%s", e.p->about.c_str());
            if (!e.error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, roleColor(Role::DangerText));
                ImGui::TextWrapped("Didn't load: %s", e.error.c_str());
                ImGui::PopStyleColor();
            } else if (e.p) {
                std::string adds;
                for (const fry::PluginCommand& c : e.p->commands) adds += std::format("{}{} ({} menu)", adds.empty() ? "" : ", ", c.name, c.menu == "add" ? "Add" : c.menu == "object" ? "Object" : "Script > Plug-ins");
                size_t mats = 0;
                for (const MaterialPreset& m : ownMaterials()) mats += m.from == e.file;
                if (mats) adds += std::format("{}{} material{}", adds.empty() ? "" : ", ", mats, mats == 1 ? "" : "s");
                ImGui::TextColored(roleColor(Role::TextTertiary), "Adds: %s", adds.empty() ? "nothing yet" : adds.c_str());
            } else ImGui::TextDisabled("Turned off");
            ImGui::EndChild();
            ImGui::PopID();
        }
        ImGui::End();
        if (toggle) setPluginOn(toggle->first, toggle->second);
    }
    static int growString(ImGuiInputTextCallbackData* d) {
        if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* str = static_cast<std::string*>(d->UserData);
            str->resize(size_t(d->BufTextLen));
            d->Buf = str->data();
        }
        return 0;
    }
    // A one-line field for a std::string (it grows as it's typed into).
    static bool inputString(const char* label, std::string& text, const char* hint = nullptr) {
        return ImGui::InputTextWithHint(label, hint ? hint : "", text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize, growString, &text);
    }
    void scriptPanel() {
        using keel::ui::Role;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(780.0f, vp->WorkSize.x * 0.55f), vp->WorkSize.y * 0.7f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(panelMin("Script"), ImVec2(FLT_MAX, FLT_MAX));
        if (!ImGui::Begin("Script", &showScript_)) return ImGui::End();

        // The file's scripts (saved in the .fry with it).
        const float listW = std::min(180.0f, ImGui::GetContentRegionAvail().x * 0.3f);
        ImGui::BeginChild("scripts", ImVec2(listW, 0));
        ImGui::TextColored(roleColor(Role::TextTertiary), "In this file");
        std::optional<size_t> remove, copy;
        for (size_t i = 0; i < scene_.scripts.size(); ++i) {
            ImGui::PushID(int(i));
            if (ImGui::Selectable(scene_.scripts[i].name.c_str(), i == script_)) script_ = i;
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Run it")) script_ = i, runScriptNow();
                if (ImGui::MenuItem("Duplicate")) copy = i;
                if (ImGui::MenuItem("Remove from the file")) remove = i;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (scene_.scripts.empty()) ImGui::TextDisabled("None yet");
        ImGui::Spacing();
        if (ImGui::Button("+ New", ImVec2(-FLT_MIN, 0))) addScript("Script", kFirstScript);
        if (ImGui::Button("Examples", ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("examples");
        if (ImGui::BeginPopup("examples")) {
            exampleItems(true);
            ImGui::EndPopup();
        }
        if (ImGui::Button("Open...", ImVec2(-FLT_MIN, 0))) dialog(10);
        ImGui::EndChild();
        if (copy) addScript(scene_.scripts[*copy].name, scene_.scripts[*copy].code + "\n");
        if (remove) {
            scene_.scripts.erase(scene_.scripts.begin() + long(*remove));
            if (script_ >= scene_.scripts.size()) script_ = scene_.scripts.empty() ? 0 : scene_.scripts.size() - 1;
            dirty_ = true;
        }
        ImGui::SameLine();

        ImGui::BeginGroup();
        fry::SceneScript* sc = currentScript();
        if (!sc) {
            ImGui::Dummy(ImVec2(0, 40));
            ImGui::TextWrapped("This file has no scripts yet. A script is Lua that builds or changes the scene; it's saved in the .fry "
                               "with the model.");
            pushAccent();
            if (ImGui::Button("+ New script")) addScript("Script", kFirstScript);
            popAccent();
            ImGui::SameLine();
            if (ImGui::Button("What scripts can do")) addScript("Script", kFirstScript), showScriptHelp_ = true;
            ImGui::EndGroup();
            return ImGui::End();
        }
        pushAccent();
        if (ImGui::Button("Run  (F5)")) runScriptNow();
        popAccent();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 190));
        if (inputString("##name", sc->name)) dirty_ = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its name (click to change)");
        ImGui::SameLine();
        if (ImGui::Button("Save as .lua")) dialog(11);
        ImGui::SameLine();
        if (ImGui::Button(showScriptHelp_ ? "Hide help" : "?")) showScriptHelp_ = !showScriptHelp_;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everything a script can use");

        const float logH = std::max(90.0f, ImGui::GetContentRegionAvail().y * 0.22f);
        ImGui::PushFont(keel::ui::font(keel::ui::Font::Mono), 0.0f);
        if (showScriptHelp_) {
            ImGui::BeginChild("help", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.45f), ImGuiChildFlags_FrameStyle);
            ImGui::TextUnformatted(fry::scriptHelp());
            ImGui::EndChild();
        }
        const ImVec2 editor(-FLT_MIN, std::max(120.0f, ImGui::GetContentRegionAvail().y - logH - ImGui::GetStyle().ItemSpacing.y));
        if (ImGui::InputTextMultiline("##code", sc->code.data(), sc->code.capacity() + 1, editor,
                                      ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize, growString, &sc->code))
            dirty_ = true;
        const bool typing = ImGui::IsItemActive();
        ImGui::BeginChild("log", ImVec2(-FLT_MIN, 0), ImGuiChildFlags_FrameStyle);
        if (scriptLog_.empty()) ImGui::TextDisabled("What it prints shows here.");
        for (const auto& [line, error] : scriptLog_) {
            if (error) ImGui::PushStyleColor(ImGuiCol_Text, roleColor(keel::ui::Role::DangerText));
            ImGui::TextUnformatted(line.c_str());
            if (error) ImGui::PopStyleColor();
        }
        if (std::exchange(scriptLogToBottom_, false)) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopFont();
        ImGui::EndGroup();
        if (typing && ImGui::GetIO().KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) runScriptNow();
        if (typing && ImGui::IsKeyPressed(ImGuiKey_F5, false)) runScriptNow();
        ImGui::End();
    }

    void helpWindow() {
        ImGui::SetNextWindowSize(ImVec2(580, 620), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Keys and mouse", &showHelp_)) return ImGui::End();
        auto rows = [](const char* title, std::initializer_list<std::pair<const char*, const char*>> list) {
            ImGui::SeparatorText(title);
            if (!ImGui::BeginTable(title, 2)) return;
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 180);
            for (auto [k, what] : list) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(accentText(), "%s", k);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", what);
            }
            ImGui::EndTable();
        };
        rows("Looking around", {{"Right drag", "Orbit round what you look at"}, {"Middle drag / Shift+Right drag", "Pan"}, {"Wheel", "Zoom (towards the mouse)"},
                                {"Orbit / Pan / Zoom buttons", "Drag them (touchpads)"}, {"F", "Frame the selection"}, {"Home", "Frame everything"},
                                {"Numpad 1 / 3 / 7", "Front / side / top (Shift: the opposite)"}, {"Numpad 5", "Flat view / 3D"},
                                {"Z / Shift+Z", "Solid or Rendered / Wireframe"}, {"Alt+Z", "X-ray: see and pick through"}});
        rows("Tools", {{"Q", "Select"}, {"W", "Move"}, {"E", "Rotate"}, {"R", "Scale"}, {"D", "Draw shape"}, {"X", "Extrude (shape editing)"}, {"I", "Inset (shape editing)"},
                       {"B", "Round edges (shape editing)"}, {"C", "Loop cut (shape editing)"}, {"K", "Knife (shape editing)"}, {"O", "Hole"},
                       {"Ctrl (held)", "Snap while dragging (or don't, if Snap is on)"}});
        rows("Tabs", {{"Ctrl+1", "Model"}, {"Ctrl+2", "Paint"}, {"Ctrl+3", "Layout"}, {"Ctrl+4", "Render"}, {"Ctrl+5", "Print"}});
        rows("Layout", {{"Q W E R", "select, move, turn, size frames"}, {"Tab", "frames or corners"}, {"Ctrl+A", "all frames"}});
        rows("Print", {{"M", "measure"}, {"L", "lay a face flat on the bed"}, {"B", "put it on the bed"}});
        rows("Painting", {{"B / E / G / I", "Brush / eraser / fill / pick a colour"}, {"Alt+click", "Pick a colour"}, {"[ / ]", "Smaller / bigger brush"},
                          {"Ctrl+V", "Paste a picture onto it"}, {"Enter / Esc", "Place the picture / cancel"}, {"Shift + corner drag", "Resize keeping its shape"},
                          {"Wheel, middle or right drag", "Zoom and move the Picture panel"}});
        rows("Sculpting", {{"Tab (Paint tab)", "Colour or Sculpt"}, {"1 - 6", "Raise, puff out, smooth, flatten, pinch, grab"}, {"Ctrl + brush", "The other way (dig, shrink, push apart)"},
                           {"Shift + brush", "Smooth"}, {"X", "The same on both sides"}, {"[ / ]", "Smaller / bigger brush"}});
        rows("Selecting", {{"Click", "Select"}, {"Drag", "Select with a box, brush or lasso (Tool options)"}, {"Shift + click", "Objects: add to the selection. Shape: a whole loop"},
                           {"Ctrl + click", "Objects: take away. Shape: add or take one"}, {"Shift+Ctrl + click an edge", "The ring across it (shape)"}, {"Alt+click / double-click", "A whole loop (shape editing)"}, {"Ctrl+A / Alt+A", "All / none"},
                           {"Ctrl+I", "Invert"}, {"Ctrl + / Ctrl -", "More / less"}, {"L", "Everything joined to it"}, {"Esc", "Let go / cancel / leave shape editing"}});
        rows("Objects and shape", {{"Tab / double-click", "Edit an object's shape"}, {"1 / 2 / 3", "Points / edges / faces (shape editing)"}, {"M", "Merge points"},
                                   {"P", "Selected faces into their own object"}, {"Shift + Scale drag", "Scale both sides (from the middle)"}, {"Ctrl+G", "Group into a folder"},
                                   {"Ctrl+Shift + / -", "Add together / take the others out (last picked kept)"}, {"Del", "Delete"}, {"Ctrl+D", "Duplicate"}, {"Ctrl+J", "Join objects into one"},
                                   {"H / Alt+H", "Hide / show all"}, {"/ or Shift+H", "Show only the selected (again: all)"},
                                   {"Shift+O", "Soft selection: near points follow (wheel while dragging: how far)"}, {"Alt+S", "Push points out / pull in (shape)"}});
        rows("Files", {{"Ctrl+N / O / S", "New / open / save"}, {"Ctrl+Shift+S", "Save as"}, {"Ctrl+E", "Export (GLB, glTF, FBX, OBJ, 3MF, STL, PLY)"}, {"F12", "Save a picture of the view"},
                       {"Ctrl+Z / Ctrl+Y", "Undo / redo"}});
        ImGui::End();
    }

    void aboutWindow() {
        using keel::ui::Role;
        const float w = 460.0f;
        ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
        const bool open = ImGui::Begin("About Fry3DX", &showAbout_, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);
        ImGui::PopStyleVar();
        if (!open) return ImGui::End();
        auto centred = [](const char* text, ImVec4 colour) {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(text).x) * 0.5f);
            ImGui::TextColored(colour, "%s", text);
        };
        // The logo, warmed from behind by the stove.
        const float logo = 128.0f;
        const ImVec2 at(ImGui::GetWindowPos().x + (ImGui::GetWindowWidth() - logo) * 0.5f, ImGui::GetCursorScreenPos().y + 4);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mid(at.x + logo * 0.5f, at.y + logo * 0.62f);
        for (int i = 16; i >= 1; --i) dl->AddCircleFilled(mid, logo * (0.3f + 0.04f * float(i)), keel::ui::accent(0.018f), 64);
        ImGui::SetCursorScreenPos(at);
        logoImage(logo);
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushFont(keel::ui::font(keel::ui::Font::Title), 30.0f);
        centred("Fry3DX", roleColor(Role::Text));
        ImGui::PopFont();
        centred("Frying 3D Extreme", accentText());
        centred("Version " FRY_VERSION, roleColor(Role::TextSecondary));
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushStyleColor(ImGuiCol_Text, roleColor(Role::TextSecondary));
        ImGui::TextWrapped("A 3D modeller that does what you'd expect: pick a tool, click and drag. Model, sculpt, paint, render and get it ready "
                           "to print, and write a script when clicking gets repetitive.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));

        // What it's running on (for a bug report: Copy).
        const keel::GpuInfo gpu = r_->gpuInfo();
        const auto& machine = keel::sys::machine();
        const std::vector<std::pair<const char*, std::string>> rows = {
            {"Version", FRY_VERSION}, {"Drawn with", r_->apiName()}, {"Graphics", gpu.name.empty() ? "?" : gpu.name}, {"System", machine.os}, {"Processor", machine.cpu},
            {"Plug-ins", std::format("{} loaded", std::count_if(plugins_.begin(), plugins_.end(), [](const PluginEntry& e) { return e.p && e.error.empty(); }))}};
        ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_FrameStyle | ImGuiChildFlags_AutoResizeY);
        if (ImGui::BeginTable("details", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& [k, v] : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(roleColor(Role::TextTertiary), "%s", k);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", v.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0, 4));
        const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
        pushAccent();
        if (ImGui::Button("Website", ImVec2(bw, 0))) SDL_OpenURL("https://nexoniarz.github.io/Fry3DX/");
        popAccent();
        ImGui::SameLine();
        if (ImGui::Button("Source on GitHub", ImVec2(bw, 0))) SDL_OpenURL("https://github.com/Nexoniarz/Fry3DX");
        ImGui::SameLine();
        if (ImGui::Button("Copy details", ImVec2(bw, 0))) {
            std::string text = "Fry3DX";
            for (const auto& [k, v] : rows) text += std::format("\n{}: {}", k, v);
            SDL_SetClipboardText(text.c_str());
            say("The details are copied: paste them into a bug report");
        }
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, roleColor(Role::TextTertiary));
        ImGui::TextWrapped("Made by Nexoniarz, open source (Apache License 2.0). Built on SDL 3, Dear ImGui, Lua 5.4, GLM, zstd, stb, volk and VMA; "
                           "the look is Niveus.");
        ImGui::PopStyleColor();
        ImGui::End();
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string path;
    std::vector<char*> rest = {argv[0]};
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]).ends_with(".fry") && argv[i - 1][0] != '+') path = argv[i]; // (not a console command's own)
        else rest.push_back(argv[i]);
    }
    App app(path);
    return keel::run(app, int(rest.size()), rest.data());
}
