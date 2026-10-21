// Fry3DX: modelling: the knife and holes, combining and copies, spin and text, drawing a
// shape, and the mouse in the view.
#include "app.h"

using namespace fry;
namespace stdfs = std::filesystem;

namespace fryapp {

// ───────────────────────────── the knife and holes ─────────────────────────────

// Snapped to steps of 45 degrees from the last point while Ctrl is held.
auto App::knifeEnd(const ImGuiIO& io) const -> ImVec2 {
    ImVec2 m = io.MousePos;
    const bool have = !knifePts_.empty() || knifeDown_;
    if (!io.KeyCtrl || !have) return m;
    const ImVec2 from = knifePts_.empty() ? knifePress_ : knifePts_.back();
    const float dx = m.x - from.x, dy = m.y - from.y, len = std::hypot(dx, dy);
    const float a = std::round(std::atan2(dy, dx) / (kPi / 4)) * (kPi / 4);
    return ImVec2(from.x + std::cos(a) * len, from.y + std::sin(a) * len);
}

auto App::subMesh(const Mesh& m, const std::set<int>& faces) -> Mesh {
    Mesh p;
    p.verts = m.verts;
    for (int f : faces) p.faces.push_back(m.faces[size_t(f)]);
    removeUnused(p);
    return p;
}

// A new object like `from` (its look, place and finish), with another shape.
auto App::objectLike(uint32_t from, Mesh mesh, const std::string& name) -> uint32_t {
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

auto App::knifeApply(uint32_t id, std::vector<ImVec2> pts) -> void {
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

// The outline, `across` wide in the object's own size (a rectangle keeps its proportions).
auto App::holeShapeOutline(const Object& o, int face, const glm::vec3& centre, float across) const -> std::vector<glm::vec3> {
    const float tall = across * holeTall_ / std::max(holeSize_, 1e-6f);
    return holeOutline(o.mesh, face, centre, HoleShape(holeShape_), across, tall, holeSides_, holeTurn_);
}

auto App::applyHole(uint32_t id, int face, const glm::vec3& centre, float across) -> void {
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
auto App::holePreview(const ImGuiIO& io) -> std::vector<glm::vec3> {
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
auto App::onFacePlane(const Object& o, int face, ImVec2 mouse) -> std::optional<glm::vec3> {
    const glm::mat4 inv = glm::inverse(objectMatrix(o));
    const Ray r = mouseRay(mouse);
    const Ray local{glm::vec3(inv * glm::vec4(r.origin, 1.0f)), glm::normalize(glm::vec3(inv * glm::vec4(r.dir, 0.0f)))};
    return hitPlane(local, faceCenter(o.mesh, face), faceNormal(o.mesh, face));
}

// ───────────────────────────── combining, copies, thickness ─────────────────────────────

// The last picked object with the others added, taken out of it, or only where they overlap
// kept; the others go.
auto App::combineSelected(Combine op) -> void {
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

auto App::makeCopies() -> void {
    if (mode_ == Mode::Edit) leaveEdit();
    if (selected_.empty()) return say("Pick what to copy first");
    const CopiesSetting& c = copies_;
    // Along a path: the last one picked is the path (its outline, or its line), the others go along it.
    std::vector<fry::PathPoint> along;
    int pathSkip = 0;
    if (c.path) {
        const Object* path = find(active_);
        if (!path || selected_.size() < 2) return say("Pick what to copy, then (Shift) the path last: a drawn shape, a line, a ring...");
        bool loop = false;
        Mesh m = path->mesh;
        transform(m, objectMatrix(*path));
        const std::vector<glm::vec3> line = fry::pathOf(m, loop);
        along = fry::alongPath(line, loop, c.count);
        if (along.empty()) return say("The path has no length to go along");
        // (one already on the path's start: that's the first, the copies after it)
        const float near = glm::distance(boundsMin(m), boundsMax(m)) * 1e-3f + 1e-6f;
        for (uint32_t id : selected_)
            if (const Object* o = find(id); o && id != active_ && glm::distance(o->position, along[0].at) < near) {
                along = fry::alongPath(line, loop, c.count + 1);
                pathSkip = 1;
                break;
            }
    }
    pushUndo();
    std::set<uint32_t> made = selected_;
    const std::vector<Object> originals = [&] {
        std::vector<Object> v;
        for (const Object& o : scene_.objects)
            if (selected_.count(o.id) && !(c.path && o.id == active_)) v.push_back(o);
        return v;
    }();
    const glm::vec3 axis = c.axis == 0 ? glm::vec3(1, 0, 0) : c.axis == 1 ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
    for (const Object& o : originals) {
        Object* joinInto = c.joined ? find(o.id) : nullptr;
        for (int k = c.path ? pathSkip : 1; k <= (c.path ? int(along.size()) - 1 : c.count); ++k) {
            glm::mat4 T(1.0f);
            if (c.path) { // (its middle onto the point; turned as the path turns from its start)
                const glm::quat turn = c.follow ? glm::rotation(along[0].along, along[size_t(k)].along) : glm::quat(1, 0, 0, 0);
                T = glm::translate(glm::mat4(1.0f), along[size_t(k)].at) * glm::mat4_cast(turn) * glm::translate(glm::mat4(1.0f), -o.position);
            } else if (c.round) {
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
            if (c.round || c.path) n.rotation = glm::normalize(glm::quat_cast(glm::mat3(T)) * o.rotation);
            made.insert(id);
        }
        if (joinInto) changedShape(*joinInto);
    }
    selected_ = made;
    touchAll();
    if (c.path) selected_.erase(active_), active_ = selected_.empty() ? 0 : *selected_.rbegin(); // (the path isn't one of them)
    const size_t n = c.path ? along.size() - size_t(pathSkip) : size_t(c.count);
    say(std::format("{} cop{} of each made{}{}", n, n == 1 ? "y" : "ies", c.path ? " along the path" : "", c.joined ? ", joined into it" : ""));
}

auto App::copiesWindow() -> void {
    if (!showCopies_) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Make copies", &showCopies_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) return ImGui::End();
    CopiesSetting& c = copies_;
    const float w = 280;
    ImGui::SetNextItemWidth(w);
    ImGui::SliderInt("##count", &c.count, 1, 100, "%d copies");
    int kind = c.path ? 2 : c.round ? 1 : 0;
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("##kind", &kind, "In a row\0Round in a circle\0Along a path\0")) c.round = kind == 1, c.path = kind == 2;
    if (c.path) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextDisabled("Pick what to copy, then the path last (Shift+click): a drawn shape's outline, a line, a ring. The copies are spread evenly along it.");
        ImGui::PopTextWrapPos();
        ImGui::Checkbox("Turn them as the path turns", &c.follow);
        if (const Object* p = selected_.size() >= 2 ? find(active_) : nullptr) ImGui::TextColored(accentText(), "Path: %s", p->name.c_str());
        else ImGui::TextColored(roleColor(keel::ui::Role::DangerText), "No path picked yet");
    } else if (!c.round) {
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

auto App::thickenActive() -> void {
    Object* o = mode_ == Mode::Edit ? editing() : find(active_);
    if (!o || selected_.empty()) return say("Pick the object to make thick first");
    pushUndo();
    if (o->hasFinish()) o->mesh = finishedMesh(*o), o->mirror = 0, o->round = 0, o->smoothLevels = 0;
    const float t = thickness_ / objectScale(*o);
    thicken(o->mesh, thicknessOut_ ? -t : t);
    changedShape(*o);
    say(std::format("{} made {:.1f} mm thick{}", o->name, thickness_ * 1000, checkForPrint(o->mesh).closed() ? " (closed all round)" : ""));
}

// A model made elsewhere: each of its objects, standing on the ground in the middle of the view.
auto App::importFile(const std::string& file) -> void {
    std::string err;
    std::vector<fry::ImportedMesh> got = fry::importModel(file, &err);
    if (got.empty()) return say(err.empty() ? "Nothing in " + fileName(file) : err);
    pushUndo();
    if (mode_ == Mode::Edit) leaveEdit();
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto& g : got) lo = glm::min(lo, boundsMin(g.mesh)), hi = glm::max(hi, boundsMax(g.mesh));
    const glm::vec3 shift(-(lo.x + hi.x) / 2, -(lo.y + hi.y) / 2, -lo.z), at(std::round(target_.x * 10) / 10, std::round(target_.y * 10) / 10, 0.0f);
    selected_.clear();
    size_t faces = 0;
    for (auto& g : got) {
        // (each object's own middle as its place, so it turns about itself)
        const glm::vec3 lo1 = boundsMin(g.mesh) + shift, hi1 = boundsMax(g.mesh) + shift, mid((lo1.x + hi1.x) / 2, (lo1.y + hi1.y) / 2, lo1.z);
        transform(g.mesh, glm::translate(glm::mat4(1.0f), shift - mid));
        faces += g.mesh.faces.size();
        Object& o = addObject(g.name.empty() ? "Imported" : g.name, std::move(g.mesh));
        o.position = at + mid;
        selected_.insert(o.id), active_ = o.id;
    }
    const glm::vec3 size = (hi - lo) * 1000.0f;
    say(std::format("{}: {} object{}, {} faces, {:.1f} x {:.1f} x {:.1f} mm", fileName(file), got.size(), got.size() == 1 ? "" : "s", faces, size.x, size.y, size.z));
    if (glm::length(hi - lo) > 0) frameSelected();
}

auto App::spinWindow() -> void {
    if (!showSpin_) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Spin", &showSpin_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) return ImGui::End();
    const float w = 300;
    ImGui::PushTextWrapPos(w);
    ImGui::TextUnformatted("The picked shape turned round an axis: draw half a vase's outline beside it, spin it all the way round. Its open edges become the walls.");
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(w);
    ImGui::Combo("##axis", &spinAxis_, "Round X (across)\0Round Y (deep)\0Round Z (up)\0");
    ImGui::SetNextItemWidth(w);
    int where = spinWorld_;
    if (ImGui::Combo("##where", &where, "Through the shape's own origin\0Through the middle of the world\0")) spinWorld_ = where;
    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##deg", &spinDegrees_, 1.0f, -3600, 3600, "%.0f degrees");
    ImGui::SetNextItemWidth(w);
    ImGui::DragInt("##steps", &spinSteps_, 0.5f, 3, 512, "%d steps");
    float mm = spinRise_ * 1000;
    ImGui::SetNextItemWidth(w);
    if (ImGui::DragFloat("##rise", &mm, 0.5f, -1e5f, 1e5f, mm == 0 ? "no rise (a spin)" : "rises %.1f mm (a screw)")) spinRise_ = mm / 1000;
    ImGui::Checkbox("Keep the outline too", &spinKeep_);
    pushAccent();
    if (ImGui::Button("Spin it", ImVec2(w, 0))) spinSelected();
    popAccent();
    ImGui::End();
}

auto App::spinSelected() -> void {
    Object* o = find(active_);
    if (!o || !selected_.count(o->id)) return say("Pick the shape to spin first");
    if (o->locked) return say(o->name + " is locked");
    if (checkForPrint(shownMesh(*o)).openEdges == 0)
        return say("Spin turns an outline or an open surface (a drawn shape, half a vase's side): this one is closed all round");
    if (mode_ == Mode::Edit) leaveEdit();
    pushUndo();
    Mesh world = shownMesh(*o);
    transform(world, objectMatrix(*o));
    glm::vec3 axis(0.0f);
    axis[spinAxis_] = 1.0f;
    Mesh m = fry::spin(world, spinWorld_ ? glm::vec3(0.0f) : o->position, axis, spinDegrees_, spinSteps_, spinRise_);
    if (m.faces.empty()) return takeBack(), say("Nothing to spin");
    transform(m, glm::translate(glm::mat4(1.0f), -o->position));
    Object& made = spinKeep_ ? addObject(o->name + " spun", Mesh{}) : *o;
    if (spinKeep_) made.position = find(active_)->position, made.color = find(active_)->color;
    made.mesh = std::move(m), made.rotation = glm::quat(1, 0, 0, 0), made.scale = glm::vec3(1.0f);
    made.mirror = 0, made.round = 0, made.smoothLevels = 0; // (spun as it showed: its finish is in the shape now)
    handMade(made), touched(made.id);
    selected_ = {made.id}, active_ = made.id;
    say(std::format("Spun {:.0f} degrees{}: {} faces", spinDegrees_, spinRise_ != 0 ? std::format(", rising {:.1f} mm", spinRise_ * 1000) : "", made.mesh.faces.size()));
}

auto App::textWindow() -> void {
    if (!showText_) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Add text", &showText_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) return ImGui::End();
    const float w = 300;
    ImGui::InputTextMultiline("##text", textBuf_, sizeof textBuf_, ImVec2(w, ImGui::GetTextLineHeight() * 3.6f));
    ImGui::SetNextItemWidth(w);
    const std::string fontName = textFontFile_.empty() ? kFonts[textFont_] : stdfs::path(textFontFile_).filename().string();
    if (ImGui::BeginCombo("##font", fontName.c_str())) {
        for (int i = 0; i < int(std::size(kFonts)); ++i)
            if (ImGui::Selectable(kFonts[i], textFontFile_.empty() && textFont_ == i)) textFont_ = i, textFontFile_.clear();
        if (ImGui::Selectable("A font file of your own (.ttf, .otf)...")) dialog(13);
        ImGui::EndCombo();
    }
    float mm[3] = {textHeight_ * 1000, textDepth_ * 1000, textSpacing_ * 100};
    ImGui::SetNextItemWidth(w);
    if (ImGui::DragFloat("##h", &mm[0], 0.2f, 0.5f, 1e5f, "letters %.1f mm tall")) textHeight_ = std::max(mm[0], 0.1f) / 1000;
    ImGui::SetNextItemWidth(w);
    if (ImGui::DragFloat("##d", &mm[1], 0.1f, 0.1f, 1e5f, "%.1f mm thick")) textDepth_ = std::max(mm[1], 0.05f) / 1000;
    ImGui::SetNextItemWidth(w);
    if (ImGui::DragFloat("##s", &mm[2], 0.2f, -30, 100, "letters %+.0f%% further apart")) textSpacing_ = mm[2] / 100;
    ImGui::SetNextItemWidth(w);
    ImGui::Combo("##al", &textAlign_, "Lines to the left\0Lines in the middle\0Lines to the right\0");
    ImGui::Checkbox("Standing up (a sign), not lying (a label to print)", &textStanding_);
    pushAccent();
    if (ImGui::Button("Add it", ImVec2(w, 0))) addText();
    popAccent();
    ImGui::End();
}

auto App::addText() -> void {
    std::vector<uint8_t> font;
    if (!textFontFile_.empty()) {
        std::ifstream in(textFontFile_, std::ios::binary);
        font.assign(std::istreambuf_iterator<char>(in), {});
    } else if (auto f = keel::fs::read(kFontFiles[textFont_]))
        font = std::move(*f);
    fry::TextStyle st;
    st.height = textHeight_, st.depth = textDepth_, st.spacing = textSpacing_, st.align = textAlign_;
    std::string err;
    Mesh m = fry::makeText(font, textBuf_, st, &err);
    if (m.faces.empty()) return say(err.empty() ? "No text to make" : err);
    // (the letters' middle on the ground at the origin; standing up: facing the front)
    const glm::vec3 lo = boundsMin(m), hi = boundsMax(m);
    transform(m, glm::translate(glm::mat4(1.0f), glm::vec3(-(lo.x + hi.x) / 2, -(lo.y + hi.y) / 2, 0.0f)));
    if (textStanding_) transform(m, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, (hi.y - lo.y) / 2)) * glm::rotate(glm::mat4(1.0f), kPi / 2, glm::vec3(1, 0, 0)));
    pushUndo();
    if (mode_ == Mode::Edit) leaveEdit();
    std::string name = textBuf_;
    if (const size_t nl = name.find('\n'); nl != std::string::npos) name.resize(nl);
    if (name.size() > 24) name = name.substr(0, 24) + "...";
    Object& o = addObject("Text: " + name, std::move(m));
    o.position = glm::vec3(std::round(target_.x * 10) / 10, std::round(target_.y * 10) / 10, 0.0f);
    // (reading left to right as the view looks: turned to the nearest quarter)
    const glm::vec3 f = viewDir();
    if (std::abs(f.x) + std::abs(f.y) > 1e-4f) {
        const float turn = std::round(std::atan2(-f.x, f.y) / (kPi / 2)) * (kPi / 2);
        o.rotation = glm::angleAxis(turn, glm::vec3(0, 0, 1));
    }
    selected_ = {o.id}, active_ = o.id;
    showText_ = false;
    say(std::format("Text added, {:.0f} mm tall letters, {} faces", textHeight_ * 1000, o.mesh.faces.size()));
}

auto App::thickenWindow() -> void {
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

auto App::cleanFlatFaces() -> void {
    Object* o = mode_ == Mode::Edit ? editing() : find(active_);
    if (!o || selected_.empty()) return say("Pick an object first");
    pushUndo();
    const int n = mergeFlatFaces(o->mesh);
    changedShape(*o);
    say(n ? std::format("{} faces joined into the flat ones beside them", n) : "Nothing to join: no flat faces side by side");
}

// ───────────────────────────── drawing a shape ─────────────────────────────

// The plane shapes are drawn on: the one the view looks straight at, or the ground.
auto App::workPlane(glm::vec3& point, glm::vec3& normal) const -> void {
    point = glm::vec3(0.0f);
    normal = view_ == View::Front || view_ == View::Back ? glm::vec3(1, 0, 0) : view_ == View::Left || view_ == View::Right ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
}

// The plane drawn on: the face under the first point if it was on an object, else the view's.
auto App::drawPlane(glm::vec3& p, glm::vec3& n) -> void {
    if (drawOnto_ && find(drawOnto_)) p = drawOnP_, n = drawOnN_;
    else workPlane(p, n);
}

auto App::drawPoint(const ImGuiIO& io) -> std::optional<glm::vec3> {
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
    // Exact sizes: Shift holds the line to 15 degree steps; a typed number is its length (m).
    if (!drawPts_.empty() && (io.KeyShift || !sketchTyped_.empty())) {
        const glm::vec3 last = drawPts_.back(), u = glm::normalize(std::abs(n.z) < 0.9f ? glm::cross(glm::vec3(0, 0, 1), n) : glm::cross(glm::vec3(1, 0, 0), n)), v = glm::cross(n, u);
        glm::vec3 d = *hit - last;
        float len = glm::length(d);
        float a = std::atan2(glm::dot(d, v), glm::dot(d, u));
        if (io.KeyShift) a = std::round(a / glm::radians(15.0f)) * glm::radians(15.0f);
        if (!sketchTyped_.empty() && sketchTyped_ != "." && sketchTyped_ != "-") len = std::abs(std::strtof(sketchTyped_.c_str(), nullptr));
        *hit = last + (u * std::cos(a) + v * std::sin(a)) * len;
    }
    return hit;
}

// (drawing a shape: the next line's length, typed)
// Drawing a shape: numbers typed go to the next line's length, Enter puts the point there.
auto App::sketchKeys(const ImGuiIO& io) -> bool {
    bool used = false;
    for (ImWchar c : io.InputQueueCharacters)
        if ((c >= '0' && c <= '9') || (c == '.' && sketchTyped_.find('.') == std::string::npos)) sketchTyped_ += char(c), used = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !sketchTyped_.empty()) sketchTyped_.pop_back(), used = true;
    if (!sketchTyped_.empty() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
        if (auto p = drawPoint(io)) drawPts_.push_back(*p);
        sketchTyped_.clear(), used = true;
    }
    return used;
}

auto App::finishShape() -> void {
    sketchTyped_.clear();
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

auto App::viewport(ImGuiIO& io) -> void {
    cutHover_ = {-1, -1};
    if (slide_.active) return slideFrame(io);
    if (tab_ == Tab::Paint) return paintViewport(io);
    if (tab_ == Tab::Layout) return layoutViewport(io);
    if ((tab_ == Tab::Print && printTool_ != PrintTool::None) || printTool_ == PrintTool::Measure) return printViewport(io); // (measuring: the Model tab too)
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
    if (!pluginTool_.first.empty() && mode_ == Mode::Object) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoverHandle(io.MousePos) < 0) clickPluginTool(io.MousePos);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || !pluginToolFound()) return;
    }
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

auto App::addDrawPoint(const ImGuiIO& io) -> void {
    if (auto p = drawPoint(io)) {
        ImVec2 s0, s1;
        if (drawPts_.size() >= 3 && project(drawPts_[0], s0) && project(*p, s1) && std::hypot(s0.x - s1.x, s0.y - s1.y) < (touch() ? 24 : 10)) finishShape();
        else drawPts_.push_back(*p);
    }
}

// Painting: on the model under the mouse.
auto App::paintViewport(ImGuiIO& io) -> void {
    if (sculpting_) return sculptViewport(io);
    if (drawingStencil_) { // (drawing the stencil: a drag round the area)
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && mouseInView(io)) {
            if (stencil_.empty() || std::hypot(stencil_.back().x - io.MousePos.x, stencil_.back().y - io.MousePos.y) > 3) stencil_.push_back(io.MousePos);
        } else if (!stencil_.empty()) {
            drawingStencil_ = false, stencilOn_ = stencil_.size() >= 3;
            if (!stencilOn_) stencil_.clear();
            say(stencilOn_ ? "Stencil on: paint goes only inside it" : "Too small for a stencil: drag round an area");
        }
        return;
    }
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
auto App::paint(const ImGuiIO& io) -> void {
    if (mode_ == Mode::Object) {
        const Hit h = pickObject(mouseRay(io.MousePos));
        if (h.object) io.KeyCtrl ? (void)selected_.erase(h.object) : (void)(selected_.insert(h.object), active_ = h.object);
        return;
    }
    const ImVec2 m = io.MousePos;
    const float r = brushRadius_;
    regionSelect([&](ImVec2 s) { return std::hypot(s.x - m.x, s.y - m.y) < r; }, true, io.KeyCtrl, false);
}

} // namespace fryapp
