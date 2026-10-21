// Fry3DX: the document (opening, saving, undo), files, modes and tools, keys; and main().
#include "app.h"

#if defined(__ANDROID__)
#include <SDL3/SDL_main.h> // (main becomes the SDL_main SDL's Java side calls)
#endif

using namespace fry;
namespace stdfs = std::filesystem;

namespace fryapp {

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
keel::Cvar fry_movespeed("fry_movespeed", "1", keel::CVAR_ARCHIVE, "Fry3DX: how fast the camera moves (keys, wheel, pan): Shift + wheel changes it");
keel::Cvar fry_theme("fry_theme", "0", keel::CVAR_ARCHIVE, "Fry3DX: how it looks: 0 like the desktop, 1 dark, 2 light");
keel::Cvar fry_pluginsoff("fry_pluginsoff", "", keel::CVAR_ARCHIVE, "Fry3DX: plug-ins turned off (their file names, with | between)");
keel::Cvar fry_autosave("fry_autosave", "2", keel::CVAR_ARCHIVE, "Fry3DX: autosave every this many minutes while there are changes (0: off)");

// "Spiral_stairs" is "Spiral stairs" (typed in the console).
auto App::sameCommand(const std::string& name, std::string typed) -> bool {
    std::replace(typed.begin(), typed.end(), '_', ' ');
    return materialId(name) == materialId(typed);
}

auto App::init() -> void {
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
        if (a.size() > 6) self->target_ = {std::stof(a[4]), std::stof(a[5]), std::stof(a[6])};
    }, "Fry3DX: fry_orbit yaw pitch [distance [target x y z]]");
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
        c.count = std::stoi(a[1]), c.round = a[2] == "round", c.path = a[2] == "path";
        c.follow = std::find(a.begin(), a.end(), "still") == a.end();
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
    static keel::Cmd ptool("fry_tooluse", [](const keel::Args& a) { // a plug-in tool by name, clicked at x y (0..1 of the view)
        if (a.size() < 4 || !self->viewReady()) return;
        for (const PluginEntry& e : self->plugins_)
            if (e.p)
                for (const fry::PluginTool& t : e.p->tools)
                    if (t.name == a[1]) self->pluginTool_ = {e.file, t.name};
        self->clickPluginTool(vp(a[2], a[3]));
    }, "Fry3DX: fry_tooluse \"name\" x y: click with a plug-in tool (0..1 of the view)");
    static keel::Cmd stencil("fry_stencil", [](const keel::Args& a) { // a box stencil, 0..1 of the view
        if (a.size() < 5 || !self->viewReady()) return;
        const ImVec2 p0 = vp(a[1], a[2]), p1 = vp(a[3], a[4]);
        self->stencil_ = {p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y)}, self->stencilOn_ = true;
    }, "Fry3DX: fry_stencil x0 y0 x1 y1: a box stencil for painting (0..1 of the view)");
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
    static keel::Cmd bench("fry_bench", [](const keel::Args& a) {
        if (a.size() < 3) return self->say("fry_bench orbit|smooth|angle|round frames");
        self->bench_ = {a[1], std::max(1, std::stoi(a[2])), {}};
    }, "Fry3DX: fry_bench orbit|smooth|angle|round frames: time that many frames while the camera turns or the picked object's setting is dragged");
    static keel::Cmd hold("fry_hold", [](const keel::Args& a) { // a key held down for some frames, as if pressed (for trying keys out)
        if (a.size() < 3) return self->say("fry_hold key frames (UpArrow, PageUp, LeftShift...)");
        if (a[1] == "MouseRight") { // (pressed now, let go after that many frames)
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
            return self->holding_.push_back({ImGuiKey_MouseRight, std::stoi(a[2])});
        }
        if (a[1] == "Escape") { // (the engine takes this one from the window's own events, before the panels)
            SDL_Event e{};
            e.type = SDL_EVENT_KEY_DOWN, e.key.scancode = SDL_SCANCODE_ESCAPE, e.key.key = SDLK_ESCAPE, e.key.down = true;
            SDL_PushEvent(&e);
            return;
        }
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
            if (a[1] == ImGui::GetKeyName(ImGuiKey(k))) {
                ImGui::GetIO().AddKeyEvent(ImGuiKey(k), true);
                return self->holding_.push_back({ImGuiKey(k), std::stoi(a[2])});
            }
        self->say("fry_hold: no key called " + a[1]);
    }, "Fry3DX: fry_hold key frames: hold a key down (UpArrow, PageUp...)");
    static keel::Cmd mouse("fry_mouse", [](const keel::Args& a) { // the mouse to x y (0..1 of the view)
        if (a.size() < 3) return;
        ImGui::GetIO().AddMousePosEvent(self->viewMin_.x + (self->viewMax_.x - self->viewMin_.x) * std::stof(a[1]), self->viewMin_.y + (self->viewMax_.y - self->viewMin_.y) * std::stof(a[2]));
    }, "Fry3DX: fry_mouse x y: the mouse to that point of the view");
    static keel::Cmd wheel("fry_wheel", [](const keel::Args& a) { // the wheel turned at x y (0..1 of the view)
        if (a.size() < 4) return;
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(self->viewMin_.x + (self->viewMax_.x - self->viewMin_.x) * std::stof(a[1]), self->viewMin_.y + (self->viewMax_.y - self->viewMin_.y) * std::stof(a[2]));
        io.AddMouseWheelEvent(0, std::stof(a[3]));
    }, "Fry3DX: fry_wheel x y notches: the mouse wheel at that point of the view");
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
        if (a[1] == "layer" && a.size() > 2) self->showLayer_ = true, self->layer_ = std::stoi(a[2]);
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
            const Mesh before = o->mesh;
            self->drag_.soft.clear();
            if (self->softSel_) self->findSoft();
            const glm::mat4 m = objectMatrix(*o);
            const glm::mat4 local = glm::inverse(m) * glm::translate(glm::mat4(1.0f), glm::vec3(std::stof(a[2]), std::stof(a[3]), std::stof(a[4]))) * m;
            for (int v : self->selectedPoints()) o->mesh.verts[size_t(v)] = glm::vec3(local * glm::vec4(o->mesh.verts[size_t(v)], 1.0f));
            for (const auto& [v, w] : self->drag_.soft) o->mesh.verts[size_t(v)] = glm::mix(o->mesh.verts[size_t(v)], glm::vec3(local * glm::vec4(o->mesh.verts[size_t(v)], 1.0f)), w);
            self->drag_.soft.clear();
            if (self->liveMirror_) self->mirrorEdits(*o, before);
            self->changedShape(*o, true);
        } else if (a[1] == "soft") self->softSel_ = a.size() < 3 || a[2] != "0", self->softReach_ = a.size() > 3 ? std::stof(a[3]) : self->softReach_;
    }, "Fry3DX: shape editing: fry_shapeop smooth | bridge | push metres | soft 1 [reach] | move x y z");
    static keel::Cmd search("fry_search", [](const keel::Args& a) { // fry_search [words]: the command search, typed in
        self->openSearch();
        if (a.size() > 1) std::snprintf(self->search_.text, sizeof self->search_.text, "%s", a[1].c_str());
    }, "Fry3DX: open the command search (F3), with words typed in");
    static keel::Cmd runCmd("fry_command", [](const keel::Args& a) { if (a.size() > 1) self->buildCommands(), self->runCommand(a[1]); }, "Fry3DX: run a command by its id (file.save, view.top...)");
    static keel::Cmd pie("fry_pie", [](const keel::Args& a) {
        self->pie_ = a.size() > 1 && a[1] == "views" ? 2 : 1, self->pieStays_ = true;
        self->pieAt_ = ImVec2((self->viewMin_.x + self->viewMax_.x) / 2, (self->viewMin_.y + self->viewMax_.y) / 2);
    }, "Fry3DX: show a pie menu (tools | views)");
    static keel::Cmd import("fry_import", [](const keel::Args& a) { if (a.size() > 1) self->importFile(a[1]); }, "Fry3DX: import a model (STL, OBJ, 3MF, glTF)");
    static keel::Cmd text("fry_text", [](const keel::Args& a) {
        if (a.size() > 1) std::snprintf(self->textBuf_, sizeof self->textBuf_, "%s", a[1].c_str()), self->addText();
    }, "Fry3DX: add text as a shape");
    static keel::Cmd spinCmd("fry_spin", [](const keel::Args& a) { // fry_spin degrees steps [rise_mm] [world]
        if (a.size() > 2) self->spinDegrees_ = std::stof(a[1]), self->spinSteps_ = std::stoi(a[2]);
        self->spinRise_ = a.size() > 3 ? std::stof(a[3]) / 1000 : 0.0f, self->spinWorld_ = a.size() > 4 && a[4] == "world";
        self->spinSelected();
    }, "Fry3DX: spin the picked shape round Z: degrees steps [rise mm] [world]");
    static keel::Cmd slideCmd("fry_slide", [](const keel::Args& a) { // (as though the mouse had gone dx, dy since)
        self->startSlide();
        if (a.size() > 2) self->slide_.start = ImVec2(ImGui::GetIO().MousePos.x - std::stof(a[1]), ImGui::GetIO().MousePos.y - std::stof(a[2]));
    }, "Fry3DX: start sliding the picked points, as though the mouse had moved dx dy");
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
    loadMyKeys();
    buildCommands();
    if (!startPath_.empty()) openFile(startPath_);
    findAutosaves();
    if (startPath_.empty()) resumeWork();
}

auto App::onBackground() -> void {
    if (dirty_) autosave(true);
    if (autosaveJob_.valid()) autosaveJob_.wait();
#if defined(__ANDROID__)
    std::ofstream(resumeFile()) << (dirty_ ? autosavePath_ : std::string()) << '\n'
                                << path_ << '\n'
                                << int(tab_) << ' ' << target_.x << ' ' << target_.y << ' ' << target_.z << ' ' << yaw_ << ' ' << pitch_ << ' ' << dist_ << '\n';
#endif
}

auto App::resumeWork() -> void {
#if defined(__ANDROID__)
    std::ifstream in(resumeFile());
    if (!in) return;
    std::string saved, from;
    std::getline(in, saved), std::getline(in, from);
    int tab = 0;
    glm::vec3 target = target_;
    float yaw = yaw_, pitch = pitch_, dist = dist_;
    const bool view = bool(in >> tab >> target.x >> target.y >> target.z >> yaw >> pitch >> dist);
    in.close();
    std::error_code ec;
    stdfs::remove(resumeFile(), ec);
    if (!saved.empty() && stdfs::exists(saved, ec)) { // (unsaved work: as recovering it, without asking)
        openFile(saved);
        path_ = from, dirty_ = true, autosavePath_ = saved;
        showRecover_ = false;
    } else if (!from.empty())
        openFile(from);
    else
        return;
    if (view) setTab(Tab(std::clamp(tab, 0, 4))), target_ = target, yaw_ = yaw, pitch_ = std::clamp(pitch, -89.0f, 89.0f), dist_ = std::max(dist, 0.01f);
    say("Carried on where you left off");
#endif
}

auto App::allowQuit() -> bool {
    std::error_code ec;
    stdfs::remove(resumeFile(), ec); // (closed on purpose: the next start is a fresh one)
    if (!dirty_) return dropAutosave(), true;
    askSave_ = 1;
    return false;
}

auto App::onEscape() -> void {
    // (the engine takes Escape before the panels see it: whatever's open on top goes first)
    if (search_.open) search_.open = false;
    else if (pie_) pie_ = 0;
    else if (askSave_) askSave_ = 0;
    else if (showRecover_) showRecover_ = false;
    else if (editLayer()) cancelFloating();
    else if (!knifePts_.empty()) knifePts_.clear();
    else if (!drawPts_.empty()) drawPts_.clear(), sketchTyped_.clear();
    else if (drag_.active) cancelDrag();
    else if (slide_.active) cancelSlide();
    else if (printTool_ == PrintTool::Measure) printTool_ = PrintTool::None, measure_.clear();
    else if (!pluginTool_.first.empty()) pluginTool_ = {};
    else if (drawingStencil_ || stencilOn_) drawingStencil_ = stencilOn_ = false, stencil_.clear();
    else if (mode_ == Mode::Edit) leaveEdit();
    else selected_.clear();
}

// (fry_bench) Each frame: the change being timed.
auto App::benchStep() -> void {
    for (auto& [k, left] : holding_)
        if (--left <= 0) k == ImGuiKey_MouseRight ? ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false) : ImGui::GetIO().AddKeyEvent(k, false);
    std::erase_if(holding_, [](const auto& h) { return h.second <= 0; });
    if (bench_.left <= 0) return;
    const int i = int(bench_.ms.size());
    Object* o = find(active_);
    if (bench_.what == "orbit") yaw_ += 2.0f;
    else if (o && bench_.what == "smooth") o->smoothLevels = 1 + i / 4 % 3, o->smooth = true, touched(o->id);
    else if (o && bench_.what == "angle") o->smooth = true, o->smoothAngle = 20.0f + float(i % 40), touched(o->id);
    else if (o && bench_.what == "round") o->round = 0.01f + 0.001f * float(i % 30), touched(o->id);
}

auto App::benchEnd(std::chrono::steady_clock::time_point began) -> void {
    if (bench_.left <= 0) return;
    bench_.ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
    if (--bench_.left) return;
    double sum = 0, most = 0;
    for (double m : bench_.ms) sum += m, most = std::max(most, m);
    say(std::format("fry_bench {}: {} frames, {:.2f} ms each, the slowest {:.2f} ms", bench_.what, bench_.ms.size(), sum / double(bench_.ms.size()), most));
}

auto App::render(keel::Renderer& r) -> void {
    struct Timed { // (for fry_bench: the whole frame, however it ends)
        App* app;
        std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
        ~Timed() { app->benchEnd(began); }
    } timed{this};
    benchStep();
    r_ = &r;
    captureStep();
    setupUi();
    loadLogo();
    applyShading();
    takeDialogResult();
    ImGuiIO& io = ImGui::GetIO();
    if (touch()) touchFrame(io);
    if (!pendingLayout_.empty()) ImGui::LoadIniSettingsFromMemory(pendingLayout_.data(), pendingLayout_.size()), pendingLayout_.clear(); // (a workspace: before any panel)
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
    if (showHistory_) historyWindow();
    if (showWorkspaces_) workspacesWindow();
    if (showPlugins_) pluginsWindow();
    commandForm();
    copiesWindow();
    thickenWindow();
    textWindow();
    printSizeWindow();
    spinWindow();
    pushWindow();
    saveQuestion();
    recoverWindow();
    commandSearch();
    if (!askSave_ && !showRecover_ && !search_.open) shortcuts(io);
    if (pie_) return draw3D(r), overlays(), pieFrame(io);
    viewport(io);
    draw3D(r);
    overlays();
}

// (each object's shape as last kept: the next step shares it if it's the same)
auto App::snapshot(std::string label, double at) -> Snapshot {
    Snapshot s;
    s.scene.look = scene_.look, s.scene.folders = scene_.folders, s.scene.references = scene_.references, s.scene.nextId = scene_.nextId; // (not the scripts: not undone)
    s.scene.objects.reserve(scene_.objects.size());
    std::map<uint32_t, Shape> kept;
    for (Object& o : scene_.objects) {
        Mesh mesh = std::move(o.mesh);
        auto uvs = std::move(o.uvs);
        s.scene.objects.push_back(o); // (all but its shape)
        o.mesh = std::move(mesh), o.uvs = std::move(uvs);
        Shape k = lastShapes_.count(o.id) ? lastShapes_[o.id] : Shape{};
        if (!k.mesh || k.mesh->verts != o.mesh.verts || k.mesh->faces != o.mesh.faces) k.mesh = std::make_shared<const Mesh>(o.mesh);
        if (!k.uvs || *k.uvs != o.uvs) k.uvs = std::make_shared<const std::vector<std::vector<glm::vec2>>>(o.uvs);
        s.shapes[o.id] = kept[o.id] = k;
    }
    lastShapes_ = std::move(kept); // (gone objects' shapes: only in the steps that have them)
    s.selected = selected_, s.active = active_, s.mode = mode_, s.verts = selVerts_, s.faces = selFaces_, s.edges = selEdges_;
    s.label = std::move(label), s.at = at;
    return s;
}

// ───────────────────────────── document ─────────────────────────────

auto App::find(uint32_t id) -> Object* {
    for (Object& o : scene_.objects)
        if (o.id == id) return &o;
    return nullptr;
}

auto App::findRef(uint32_t id) -> Reference* {
    for (Reference& r : scene_.references)
        if (r.id == id) return &r;
    return nullptr;
}

auto App::shapeKey(const Object& o) -> uint64_t {
    ShapeKey& k = shapeKeys_[o.id];
    const uint64_t v = versions_[o.id];
    if (k.version == v && v) return k.key;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        size_t i = 0;
        for (; i + 8 <= n; i += 8) { // (8 bytes at a time: a big mesh in a few ms)
            uint64_t w;
            std::memcpy(&w, b + i, 8);
            h = (h ^ w) * 1099511628211ull;
        }
        for (; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(o.mesh.verts.data(), o.mesh.verts.size() * sizeof(glm::vec3));
    for (const auto& f : o.mesh.faces) {
        const uint64_t n = f.size();
        mix(&n, 8), mix(f.data(), f.size() * sizeof(int));
    }
    for (const auto& [a, b] : o.creases) mix(&a, 4), mix(&b, 4);
    for (const auto& f : o.uvs) mix(f.data(), f.size() * sizeof(glm::vec2));
    const uint64_t cut = cutKeys_.count(o.id) ? cutKeys_[o.id] : 0;
    const void* pic = o.picture.get();
    mix(&cut, 8), mix(&pic, sizeof pic);
    mix(&o.mirror, 4), mix(&o.round, 4), mix(&o.roundAngle, 4), mix(&o.roundSteps, 4), mix(&o.smoothLevels, 4), mix(&o.smoothKeepSharp, 1);
    // (not smooth shading and its angle: they change only how it's lit, see modelOf)
    k = {v, h ? h : 1};
    return k.key;
}

auto App::say(std::string text) -> void {
    message_ = std::move(text), messageAt_ = now();
    if (!undo_.empty() && undo_.back().label.empty() && now() - undo_.back().at < 1.0) undo_.back().label = message_; // (names the step just done)
    keel::log::info("{}", message_);
}

auto App::pushUndo() -> void {
    undo_.push_back(snapshot("", now()));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redoKept_ = std::move(redo_), redo_.clear();
    dirty_ = true, ++edits_;
}

// The undo point just made, dropped (what it was for didn't happen): redo as it was before.
auto App::dropUndo() -> void {
    if (!undo_.empty()) undo_.pop_back();
    redo_ = std::move(redoKept_), redoKept_.clear();
}

// Back to the undo point just made, and it dropped.
auto App::takeBack() -> void {
    if (undo_.empty()) return;
    restore(undo_.back());
    dropUndo();
}

auto App::restore(Snapshot& s) -> void {
    // (Not the scripts: they're edited like text, not undone with the model.)
    std::vector<fry::SceneScript> scripts = std::move(scene_.scripts);
    scene_ = s.scene, selected_ = s.selected, active_ = s.active, mode_ = s.mode, followAt_.clear();
    scene_.scripts = std::move(scripts);
    for (Object& o : scene_.objects)
        if (auto it = s.shapes.find(o.id); it != s.shapes.end()) o.mesh = *it->second.mesh, o.uvs = *it->second.uvs;
    lastShapes_ = s.shapes;
    selVerts_ = s.verts, selFaces_ = s.faces, selEdges_ = s.edges;
    if (mode_ == Mode::Edit && !find(active_)) mode_ = Mode::Object;
    touchAll();
    drawPts_.clear();
}

auto App::undo() -> void {
    if (undo_.empty()) return say("Nothing to undo");
    redo_.push_back(snapshot(undo_.back().label, now())); // (its name: the step it brings back)
    restore(undo_.back());
    undo_.pop_back();
    redoKept_.clear();
}

auto App::redo() -> void {
    if (redo_.empty()) return say("Nothing to redo");
    undo_.push_back(snapshot(redo_.back().label, now() - 10));
    restore(redo_.back());
    redo_.pop_back();
    redoKept_.clear();
}

auto App::newScene() -> void {
    scene_ = {}, followAt_.clear();
    script_ = 0, scriptPath_.clear();
    path_.clear();
    undo_.clear(), redo_.clear(), selected_.clear(), lastShapes_.clear();
    mode_ = Mode::Object, active_ = 0;
    addShape("cube", false);
    dirty_ = false;
    target_ = {0, 0, 0.5f}, dist_ = 4.5f, view_ = View::Perspective, fov_ = 50, yaw_ = -135, pitch_ = 25;
}

auto App::addObject(const std::string& base, Mesh mesh) -> Object& {
    Object o;
    o.id = scene_.nextId++;
    o.name = uniqueName(base);
    o.mesh = std::move(mesh);
    scene_.objects.push_back(std::move(o));
    touched(scene_.objects.back().id);
    return scene_.objects.back();
}

auto App::uniqueName(const std::string& base) -> std::string {
    auto taken = [&](const std::string& n) { return std::any_of(scene_.objects.begin(), scene_.objects.end(), [&](const Object& o) { return o.name == n; }); };
    if (!taken(base)) return base;
    for (int i = 2;; ++i)
        if (!taken(std::format("{} {}", base, i))) return std::format("{} {}", base, i);
}

// A shape from the library, standing on the ground under the middle of the view.
auto App::addShape(const std::string& id, bool undoable) -> void {
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

auto App::deleteSelected() -> void {
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

auto App::duplicateSelected() -> void {
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
auto App::joinSelected() -> void {
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
auto App::separateSelected() -> void {
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

// A file's name as shown (a picked file's: from the end of its address, decoded).
auto App::fileName(const std::string& p) -> std::string {
    if (!isUri(p)) return stdfs::path(p).filename().string();
    std::string d;
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i] == '%' && i + 2 < p.size()) d += char(std::stoi(p.substr(i + 1, 2), nullptr, 16)), i += 2;
        else d += p[i];
    return d.substr(d.find_last_of("/:") + 1);
}

// A picked file copied to a file of our own to read ("" if it couldn't be).
auto App::fromUri(const std::string& uri, const std::string& name) -> std::string {
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
auto App::toUri(const std::string& local, const std::string& uri) -> bool {
    std::ifstream in(local, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SDL_IOStream* io = SDL_IOFromFile(uri.c_str(), "wb");
    if (!io) return false;
    const bool ok = SDL_WriteIO(io, bytes.data(), bytes.size()) == bytes.size();
    return SDL_CloseIO(io) && ok;
}

auto App::tempFile(const std::string& name) -> std::string {
    std::error_code ec;
    return (stdfs::temp_directory_path(ec) / name).string();
}

auto App::openFile(const std::string& picked) -> void {
    std::string err;
    const std::string path = isUri(picked) ? fromUri(picked, "opened.fry") : picked;
    if (path.empty()) return say("Couldn't read that file");
    auto s = loadPackage(path, &err);
    if (!s) return say("Couldn't open: " + err);
    dropAutosave();
    scene_ = std::move(*s), followAt_.clear();
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
    undo_.clear(), redo_.clear(), selected_.clear(), lastShapes_.clear();
    mode_ = Mode::Object, active_ = 0, dirty_ = false, ledit_ = {};
    touchAll();
    frameAll();
    say("Opened " + fileName(picked));
}

auto App::saveFile(std::string path) -> bool {
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

auto App::pngBytes(const Image& im) -> std::vector<uint8_t> {
    std::vector<uint8_t> out;
    stbi_write_png_to_func([](void* ctx, void* data, int n) { auto* v = static_cast<std::vector<uint8_t>*>(ctx); v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + n); },
                           &out, im.w, im.h, 4, im.px.data(), im.w * 4);
    return out;
}

auto App::exportFile(std::string path, int filter, const std::string& asExt) -> void {
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
    if (!exportModel(path, fry::withCutsMade(scene_), pngs, &err)) return say("Couldn't export: " + err);
    say("Exported " + stdfs::path(path).filename().string());
}

// ── Closing with changes, autosave ──

// New, Open, Quit: asks about saving first if there are changes.
auto App::whenSaved(int action) -> void {
    if (dirty_) askSave_ = action;
    else doAfterAsking(action);
}

auto App::doAfterAsking(int action) -> void {
    if (action == 1) dropAutosave(), keel::execute("quit");
    else if (action == 2) newScene();
    else if (action == 3) dialog(1);
    else if (action == 4) openFile(std::exchange(dropped_, std::string()));
}

// A file dropped on the window (or opened with Fry3DX from the desktop): a .fry opens (after
// asking about changes), a script goes in the Script panel, a picture goes on as a reference
// (or, in the Paint tab, as a sticker on the model).
auto App::onFileDropped(const std::string& path) -> void {
    std::string ext = stdfs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".fry") return (void)(dropped_ = path, whenSaved(4));
    if (ext == ".lua") return openScript(path, fileName(path));
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") return tab_ == Tab::Paint ? floatFromFile(path) : addPicture(path);
    say("Fry3DX opens .fry files, scripts (.lua) and pictures");
}

auto App::saveQuestion() -> void {
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

// Every fry_autosave minutes (when there are changes and nothing's being dragged): a copy
// packed as small as it goes, in the background.
auto App::autosave(bool force) -> void {
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
auto App::dropAutosave() -> void {
    if (autosaveJob_.valid()) autosaveJob_.wait();
    if (autosavePath_.empty()) return;
    std::error_code ec;
    stdfs::remove(autosavePath_, ec), stdfs::remove(autosavePath_ + ".from", ec);
    autosavePath_.clear(), lastAutosave_ = now();
}

// Autosaves left behind (it didn't close properly): offered back. Only the newest 15 are
// kept; older ones are deleted.
auto App::findAutosaves() -> void {
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

auto App::recoverWindow() -> void {
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
auto App::savePicture(std::string path) -> void {
    if (path.empty()) return;
    if (stdfs::path(path).extension() != ".png") path += ".png";
    capturePath_ = path, captureFrames_ = 5;
}

auto App::captureStep() -> void {
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

auto App::loadImageFile(const std::string& path, Image& out) -> bool {
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return false;
    out.w = w, out.h = h, out.px.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return true;
}

auto App::addPicture(const std::string& path) -> void {
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
auto App::dialog(int kind) -> void {
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
    else if (kind == 12) {
        static const SDL_DialogFileFilter in[] = {{"3D models (STL, OBJ, 3MF, glTF)", "stl;obj;3mf;glb;gltf"}};
        SDL_ShowOpenFileDialog(done, this, w, in, 1, at, true);
    }
    else if (kind == 13) {
        static const SDL_DialogFileFilter fonts[] = {{"Fonts", "ttf;otf"}};
        SDL_ShowOpenFileDialog(done, this, w, fonts, 1, at, false);
    }
    else SDL_ShowOpenFileDialog(done, this, w, pics, 1, at, false);
}

auto App::takeDialogResult() -> void {
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
    if (kind == 12) return importFile(isUri(file) ? fromUri(file, "imported-" + fileName(file)) : file);
    if (kind == 13) {
        textFontFile_ = isUri(file) ? fromUri(file, "font-" + fileName(file)) : file;
        return;
    }
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

auto App::setTool(Tool t) -> void {
    for (const ToolInfo& i : kTools)
        if (i.tool == t && ((mode_ == Mode::Object && !i.objectMode) || (mode_ == Mode::Edit && !i.editMode))) {
            if (mode_ == Mode::Object && i.editMode) { // (a shape tool: go and edit the shape)
                enterEdit();
                if (mode_ != Mode::Edit) return;
                break;
            }
            return say(std::format("{} works on whole objects (Tab leaves shape editing)", i.name));
        }
    tool_ = t, pluginTool_ = {};
    drawPts_.clear(), knifePts_.clear();
    if ((t == Tool::Extrude || t == Tool::Inset) && elem_ != Elem::Face) elem_ = Elem::Face, clearElements();
}

auto App::enterEdit() -> void {
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

auto App::leaveEdit() -> void {
    if (mode_ != Mode::Edit) return;
    mode_ = Mode::Object;
    if (tool_ == Tool::Extrude || tool_ == Tool::Inset || tool_ == Tool::Bevel || tool_ == Tool::LoopCut || tool_ == Tool::Knife) tool_ = Tool::Move;
    knifePts_.clear();
    if (Object* o = find(active_)) touched(o->id);
}

auto App::selectAllElements() -> void {
    Object* o = editing();
    if (!o) return;
    clearElements();
    for (size_t v = 0; v < o->mesh.verts.size(); ++v) selVerts_.insert(int(v));
    for (size_t f = 0; f < o->mesh.faces.size(); ++f) selFaces_.insert(int(f));
    for (auto e : edges(o->mesh)) selEdges_.insert(e);
}

auto App::selectAll() -> void {
    if (mode_ == Mode::Edit) return selectAllElements();
    selected_.clear();
    for (const Object& o : scene_.objects)
        if (o.visible) selected_.insert(o.id), active_ = o.id;
}

auto App::selectNone() -> void {
    if (mode_ == Mode::Edit) clearElements();
    else selected_.clear();
}

// The points the selection covers, whichever kind is picked.
auto App::selectedPoints() -> std::set<int> {
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
auto App::selectPoints(const std::set<int>& pts) -> void {
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

auto App::shrinkSelection() -> void {
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

// Like the picked: faces that face the same way, edges as long, points with as many edges.
auto App::selectSimilar() -> void {
    Object* o = editing();
    if (!o && mode_ == Mode::Object) { // whole objects: the others of the same shape (copies of one part: every bolt)
        std::vector<const Object*> like;
        for (const Object& p : scene_.objects)
            if (selected_.count(p.id)) like.push_back(&p);
        if (like.empty()) return say("Pick an object first");
        auto same = [](const Object& a, const Object& b) {
            if (!a.recipe.empty() || !b.recipe.empty()) return a.recipe == b.recipe && a.settings == b.settings;
            return a.mesh.verts.size() == b.mesh.verts.size() && a.mesh.faces.size() == b.mesh.faces.size();
        };
        const size_t before = selected_.size();
        for (const Object& p : scene_.objects)
            if (p.visible && !p.locked && std::any_of(like.begin(), like.end(), [&](const Object* l) { return same(*l, p); })) selected_.insert(p.id);
        const size_t more = selected_.size() - before;
        return say(more ? std::format("{} more of the same shape picked", more) : "No others of the same shape");
    }
    if (!o) return;
    const Mesh& m = o->mesh;
    if (elem_ == Elem::Face) {
        std::vector<glm::vec3> ns;
        for (int f : selFaces_) ns.push_back(faceNormal(m, f));
        if (ns.empty()) return say("Pick a face first");
        for (size_t f = 0; f < m.faces.size(); ++f)
            for (const glm::vec3& n : ns)
                if (glm::dot(faceNormal(m, int(f)), n) > 0.996f) selFaces_.insert(int(f));
    } else if (elem_ == Elem::Edge) {
        std::vector<float> lens;
        for (auto [a, b] : selEdges_) lens.push_back(glm::distance(m.verts[size_t(a)], m.verts[size_t(b)]));
        if (lens.empty()) return say("Pick an edge first");
        for (auto [a, b] : edges(m)) {
            const float l = glm::distance(m.verts[size_t(a)], m.verts[size_t(b)]);
            for (float k : lens)
                if (std::abs(l - k) <= k * 0.02f + 1e-6f) selEdges_.insert({a, b});
        }
    } else {
        std::vector<int> count(m.verts.size(), 0);
        for (auto [a, b] : edges(m)) ++count[size_t(a)], ++count[size_t(b)];
        std::set<int> want;
        for (int v : selVerts_) want.insert(count[size_t(v)]);
        if (want.empty()) return say("Pick a point first");
        for (size_t v = 0; v < m.verts.size(); ++v)
            if (want.count(count[size_t(v)])) selVerts_.insert(int(v));
    }
    say("Picked the similar ones");
}

// The picked edges' whole loops (or the rings across them).
auto App::loopsOfSelected(bool ring) -> void {
    Object* o = editing();
    if (!o) return;
    if (elem_ != Elem::Edge || selEdges_.empty()) return say("Pick an edge or two first (Edges, 2)");
    std::set<std::pair<int, int>> add;
    for (auto e : selEdges_)
        for (auto f : ring ? edgeRing(o->mesh, e) : edgeLoop(o->mesh, e)) add.insert(f);
    selEdges_.insert(add.begin(), add.end());
}

auto App::invertSelection() -> void {
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
auto App::edgesToRound() -> std::set<std::pair<int, int>> {
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

auto App::deleteElements() -> void {
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
auto App::changedShape(Object& o, bool keepSelection) -> void {
    if (!keepSelection) clearElements();
    handMade(o);
    touched(o.id);
}

auto App::extrudeNow(float amount) -> void {
    Object* o = editing();
    if (!o || elem_ != Elem::Face || selFaces_.empty()) return say("Select faces to extrude (Faces, 3)");
    pushUndo();
    extrudeFaces(o->mesh, selFaces_, amount);
    changedShape(*o, true);
}

auto App::insetNow(float amount) -> void {
    Object* o = editing();
    if (!o || elem_ != Elem::Face || selFaces_.empty()) return say("Select faces to inset (Faces, 3)");
    pushUndo();
    insetFaces(o->mesh, selFaces_, amount);
    changedShape(*o, true);
}

auto App::bevelNow() -> void {
    Object* o = editing();
    const auto es = edgesToRound();
    if (!o || es.empty()) return say("Select edges (or faces) to round");
    pushUndo();
    bevelEdges(o->mesh, es, bevelWidth_, bevelSteps_);
    changedShape(*o);
    if (bevelSteps_ > 1) o->smooth = true;
}

auto App::subdivideNow(bool smooth) -> void {
    Object* o = editing();
    if (!o) return;
    pushUndo();
    subdivide(o->mesh, smooth);
    if (smooth) o->smooth = true;
    changedShape(*o);
}

auto App::cutAt(Object& o, std::pair<int, int> edge) -> void {
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

auto App::makeFaceNow() -> void {
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

auto App::mergeNow() -> void {
    Object* o = editing();
    if (!o) return;
    const std::set<int> pts = selectedPoints();
    if (pts.size() < 2) return say("Select 2 or more points to merge");
    pushUndo();
    mergeToCenter(o->mesh, pts);
    changedShape(*o);
}

auto App::smoothPointsNow() -> void {
    Object* o = editing();
    const std::set<int> pts = selectedPoints();
    if (!o || pts.empty()) return say("Select points (or faces, edges) to smooth");
    pushUndo();
    smoothPoints(o->mesh, pts, 0.5f, 2);
    changedShape(*o, true);
    say("Smoothed (again for more)");
}

auto App::startPush() -> void {
    Object* o = editing();
    if (!o || selectedPoints().empty()) return say("Select points (or faces) to push out or pull in");
    pushUndo();
    pushPts_ = selectedPoints(), pushFrom_ = o->mesh, pushObject_ = o->id, pushBy_ = 0.0f, showPush_ = true;
}

auto App::pushWindow() -> void {
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

auto App::bridgeNow() -> void {
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

auto App::flipNow() -> void {
    Object* o = editing();
    if (!o || selFaces_.empty()) return say("Select faces to flip");
    pushUndo();
    flipFaces(o->mesh, selFaces_);
    changedShape(*o, true);
}

// Rotation and scale baked into the points (so the object's turn is none and its size is 1).
auto App::applyTransform(Object& o) -> void {
    pushUndo();
    transform(o.mesh, glm::mat4_cast(o.rotation) * glm::scale(glm::mat4(1.0f), o.scale));
    o.rotation = glm::quat(1, 0, 0, 0), o.scale = glm::vec3(1.0f);
    handMade(o);
    touched(o.id);
}

auto App::applyFinish(Object& o) -> void {
    pushUndo();
    o.mesh = finishedMesh(o);
    o.mirror = 0, o.round = 0, o.smoothLevels = 0;
    handMade(o);
    touched(o.id);
}

auto App::originToMiddle(Object& o) -> void {
    pushUndo();
    const glm::vec3 c = (boundsMin(o.mesh) + boundsMax(o.mesh)) * 0.5f;
    for (glm::vec3& v : o.mesh.verts) v -= c;
    o.position += glm::vec3(objectMatrix(o) * glm::vec4(c, 0.0f));
    handMade(o);
    touched(o.id);
}

auto App::originToBottom(Object& o) -> void {
    pushUndo();
    const glm::vec3 c((boundsMin(o.mesh).x + boundsMax(o.mesh).x) * 0.5f, (boundsMin(o.mesh).y + boundsMax(o.mesh).y) * 0.5f, boundsMin(o.mesh).z);
    for (glm::vec3& v : o.mesh.verts) v -= c;
    o.position += glm::vec3(objectMatrix(o) * glm::vec4(c, 0.0f));
    handMade(o);
    touched(o.id);
}

auto App::mirror(Object& o, int axis) -> void {
    pushUndo();
    glm::vec3 s(1.0f);
    s[axis] = -1.0f;
    transform(o.mesh, glm::scale(glm::mat4(1.0f), s));
    handMade(o);
    touched(o.id);
}

// ───────────────────────────── tabs ─────────────────────────────

auto App::setTab(Tab t) -> void {
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

// ───────────────────────────── keys ─────────────────────────────

auto App::shortcuts(ImGuiIO& io) -> void {
    if (io.WantTextInput || slide_.active || drag_.active || looking_) return; // (a slide, a drag: their own keys; turning the camera: WASD, Q and E move it)
    if (tool_ == Tool::Draw && !drawPts_.empty() && sketchKeys(io)) return;
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    if (key(ImGuiKey_F3) || (ctrl && key(ImGuiKey_Space))) return openSearch();
    if (!settingKey_.empty()) return; // (Keys and mouse is waiting for a key)
    if (const auto chord = fry::pressedChord())
        for (const auto& [id, c] : myKeys_)
            if (c == *chord) return void(runCommand(id));
    if (key(ImGuiKey_Space) && !ctrl && !alt && mouseInView(io) && (tab_ == Tab::Model || tab_ == Tab::Render || tab_ == Tab::Print))
        return void((pie_ = shift ? 2 : 1, pieAt_ = io.MousePos));
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
        if (key(ImGuiKey_O)) shift ? dialog(12) : whenSaved(3);
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
        if (key(ImGuiKey_G) && mode_ == Mode::Edit) return startSlide();
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
    if (key(ImGuiKey_E) && shift && mode_ == Mode::Edit) return creaseSelected();
    if (key(ImGuiKey_G) && shift) return selectSimilar();
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

} // namespace fryapp

int main(int argc, char** argv) {
    std::string path;
    std::vector<char*> rest = {argv[0]};
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]).ends_with(".fry") && argv[i - 1][0] != '+') path = argv[i]; // (not a console command's own)
        else rest.push_back(argv[i]);
    }
#if defined(__ANDROID__) // (a phone: sideways only; SDL would otherwise let a resizable window turn upright)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    fryapp::App app(path);
    return keel::run(app, int(rest.size()), rest.data());
}
