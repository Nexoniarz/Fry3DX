// Fry3DX: the panels: layers, properties, menus, the commands and their keys.
#include "app.h"

using namespace fry;
namespace stdfs = std::filesystem;

namespace fryapp {

// ───────────────────────────── layers: folders and going to things ─────────────────────────────

auto App::findFolder(uint32_t id) -> Folder* {
    for (Folder& f : scene_.folders)
        if (f.id == id) return &f;
    return nullptr;
}

auto App::inFolder(uint32_t folder, uint32_t within) -> bool { // (folder is within, or inside it somewhere)
    for (int n = 0; folder && n < 256; ++n) {
        if (folder == within) return true;
        const Folder* f = findFolder(folder);
        folder = f ? f->parent : 0;
    }
    return false;
}

auto App::objectsIn(uint32_t folder) -> std::set<uint32_t> {
    std::set<uint32_t> out;
    for (const Object& o : scene_.objects)
        if (inFolder(o.folder, folder)) out.insert(o.id);
    return out;
}

// The camera brought round to these (as framing them, but keeping the way it looks).
auto App::goTo(const std::set<uint32_t>& ids) -> void {
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
auto App::groupSelected() -> void {
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
auto App::ungroup(uint32_t id) -> void {
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

auto App::deleteFolder(uint32_t id) -> void {
    pushUndo();
    const auto ids = objectsIn(id);
    std::erase_if(scene_.objects, [&](const Object& o) { return ids.count(o.id) > 0; });
    std::erase_if(scene_.folders, [&](const Folder& g) { return inFolder(g.id, id); });
    for (uint32_t i : ids) selected_.erase(i);
}

// A folder's eye or lock: all in it the same.
auto App::showFolder(Folder& f, bool on) -> void {
    pushUndo();
    for (Folder& g : scene_.folders)
        if (inFolder(g.id, f.id)) g.visible = on;
    for (Object& o : scene_.objects)
        if (inFolder(o.folder, f.id)) o.visible = on;
}

auto App::lockFolder(Folder& f, bool on) -> void {
    pushUndo();
    for (Folder& g : scene_.folders)
        if (inFolder(g.id, f.id)) g.locked = on;
    for (Object& o : scene_.objects)
        if (inFolder(o.folder, f.id)) o.locked = on;
}

auto App::moveToFolder(const std::set<uint32_t>& ids, uint32_t folder) -> void {
    pushUndo();
    for (Object& o : scene_.objects)
        if (ids.count(o.id)) o.folder = folder;
}

// Only these shown (again: everything shown).
auto App::showOnly(const std::set<uint32_t>& ids) -> void {
    pushUndo();
    const bool already = std::all_of(scene_.objects.begin(), scene_.objects.end(), [&](const Object& o) { return o.visible == (ids.count(o.id) > 0); });
    for (Object& o : scene_.objects) o.visible = already || ids.count(o.id);
    for (Folder& f : scene_.folders) f.visible = true;
    say(already ? "Everything shown again" : "Only that shown (do it again to show everything)");
}

auto App::folderMenu(uint32_t into, const std::set<uint32_t>& ids, int depth) -> void { // (Move to folder: the folders inside `into`)
    for (const Folder& f : scene_.folders)
        if (f.parent == into) {
            const std::string label = std::string(size_t(depth) * 3, ' ') + f.name;
            if (ImGui::MenuItem(label.c_str())) moveToFolder(ids, f.id);
            folderMenu(f.id, ids, depth + 1);
        }
}

// What a right-click on an object (or the picked ones) offers.
auto App::objectMenu(Object& o) -> void {
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
auto App::rowToggle(const char* id, Icon icon, bool usual, const char* tip) -> bool {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight() - 4;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
    const ImU32 col = ImGui::IsItemHovered() ? kHot : usual ? IM_COL32(200, 200, 205, 150) : ImGui::GetColorU32(accentText());
    drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(at.x + h * 0.1f, at.y + h * 0.1f), h * 0.8f, col);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return pressed;
}

// The ⌖ button at the end of a row: brings the camera to it.
auto App::goButton(const char* tip) -> bool {
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
auto App::layerRows(uint32_t parent, int depth) -> void {
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

// ───────────────────────────── panels ─────────────────────────────

auto App::setupUi() -> void {
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
auto App::panelMin(std::string_view name) -> ImVec2 {
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
auto App::keepMinSizes(ImGuiDockNode* n) -> ImVec2 {
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

auto App::defaultLayout(ImGuiID dock) -> void {
    layoutDone_ = true;
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
    if (!touch() && !std::exchange(layoutAgain_, false) && node && (node->IsSplitNode() || node->Windows.Size)) return; // (the user's own, from the .ini)
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

// ───────────────────────────── commands ─────────────────────────────

// Everything that can be done, by name: for the command search, keys of your own, the pies.
auto App::buildCommands() -> void {
    commands_.clear();
    auto add = [&](std::string id, std::string name, std::string group, std::string keys, std::function<void()> run, std::function<bool()> ok = {}, std::string words = {}) {
        commands_.push_back({std::move(id), std::move(name), std::move(group), std::move(keys), std::move(run), std::move(ok), std::move(words)});
    };
    auto editing = [this] { return mode_ == Mode::Edit; };
    auto picked = [this] { return !selected_.empty(); };
    auto many = [this] { return selected_.size() >= 2 && mode_ == Mode::Object; };
    auto model = [this] { return tab_ == Tab::Model; };
    add("file.new", "New", "File", "Ctrl+N", [this] { whenSaved(2); }, {}, "empty start");
    add("file.open", "Open...", "File", "Ctrl+O", [this] { whenSaved(3); }, {}, "load fry");
    add("file.import", "Import a model (STL, OBJ, 3MF, glTF)...", "File", "Ctrl+Shift+O", [this] { dialog(12); }, {}, "load mesh stl obj 3mf");
    add("file.save", "Save", "File", "Ctrl+S", [this] { path_.empty() ? dialog(2) : void(saveFile(path_)); });
    add("file.saveas", "Save as...", "File", "Ctrl+Shift+S", [this] { dialog(2); });
    add("file.export", "Export (GLB, glTF, OBJ, STL, PLY, 3MF, FBX)...", "File", "Ctrl+E", [this] { dialog(3); }, {}, "stl obj gltf glb ply fbx 3mf print");
    add("file.picture", "Save a picture of the view...", "File", "F12", [this] { dialog(5); }, {}, "screenshot render png");
    add("file.recover", "Recover autosaved work...", "File", "", [this] { findAutosaves(), showRecover_ = !recover_.empty(); });
    add("file.quit", "Quit", "File", "Ctrl+Q", [this] { whenSaved(1); }, {}, "exit close");
    add("edit.undo", "Undo", "Edit", "Ctrl+Z", [this] { undo(); }, [this] { return !undo_.empty(); });
    add("edit.redo", "Redo", "Edit", "Ctrl+Y", [this] { redo(); }, [this] { return !redo_.empty(); });
    add("edit.history", "Undo history", "Edit", "", [this] { showHistory_ = true; }, {}, "steps back");
    add("edit.duplicate", "Duplicate", "Edit", "Ctrl+D", [this] { duplicateSelected(); }, [=, this] { return picked() && mode_ == Mode::Object; }, "copy clone");
    add("edit.join", "Join into one", "Edit", "Ctrl+J", [this] { joinSelected(); }, many, "merge objects");
    add("edit.delete", "Delete", "Edit", "Del", [this] { deleteSelected(); }, {}, "remove erase");
    add("edit.shape", "Edit the shape / stop", "Edit", "Tab", [this] { mode_ == Mode::Edit ? leaveEdit() : enterEdit(); }, [=] { return editing() || picked(); }, "edit mode points faces");
    add("select.all", "Select all", "Select", "Ctrl+A", [this] { selectAll(); });
    add("select.none", "Select none", "Select", "Alt+A", [this] { selectNone(); }, {}, "deselect");
    add("select.invert", "Invert the selection", "Select", "Ctrl+I", [this] { invertSelection(); });
    add("select.grow", "Select more (grow)", "Select", "Ctrl +", [this] { growSelection(); }, editing);
    add("select.shrink", "Select less (shrink)", "Select", "Ctrl -", [this] { shrinkSelection(); }, editing);
    add("select.linked", "Select everything joined to it", "Select", "L", [this] { selectLinked(); }, editing, "linked connected");
    add("select.similar", "Select similar (faces the same way; objects of the same shape)", "Select", "Shift+G", [this] { selectSimilar(); }, [=] { return editing() || picked(); }, "same normal size copies");
    add("select.loop", "Select whole loops (of the picked edges)", "Select", "", [this] { loopsOfSelected(false); }, editing, "edge loop");
    add("select.ring", "Select rings (across the picked edges)", "Select", "", [this] { loopsOfSelected(true); }, editing, "edge ring");
    for (const auto& [t, n] : std::initializer_list<std::pair<SelShape, const char*>>{{SelShape::Box, "box"}, {SelShape::Brush, "brush"}, {SelShape::Lasso, "lasso"}})
        add(std::string("select.with.") + n, std::string("Select with a ") + n, "Select", "", [this, t] { selShape_ = t; });
    for (const ShapeRecipe& r : shapeRecipes())
        add(std::string("add.") + r.id, std::string("Add ") + (r.starter ? "a starter " : "a ") + r.name, "Add", "", [this, id = std::string(r.id)] { setTab(Tab::Model), addShape(id); }, {},
            r.starter ? "starter model" : "shape primitive");
    add("add.text", "Add text (3D letters)...", "Add", "", [this] { setTab(Tab::Model), showText_ = true; }, {}, "font letters words label name");
    add("add.draw", "Draw a shape", "Add", "D", [this] { if (mode_ == Mode::Edit) leaveEdit(); setTool(Tool::Draw); }, {}, "sketch outline polygon");
    add("add.picture", "Add a reference picture...", "Add", "", [this] { dialog(4); }, {}, "image background blueprint");
    add("object.goto", "Go to it (the camera)", "Object", "F", [this] { goTo(selected_); }, picked, "frame focus");
    add("object.copies", "Make copies (in a row, round, along a path)...", "Object", "", [this] { showCopies_ = true; }, picked, "array repeat pattern circular curve");
    add("object.group", "Group into a folder", "Object", "Ctrl+G", [this] { groupSelected(); }, picked);
    add("object.add", "Add them together", "Object", "Ctrl+Shift++", [this] { combineSelected(Combine::Add); }, many, "boolean union");
    add("object.subtract", "Take the others out of it", "Object", "Ctrl+Shift+-", [this] { combineSelected(Combine::Subtract); }, many, "boolean difference cut");
    add("object.overlap", "Keep only where they overlap", "Object", "", [this] { combineSelected(Combine::Overlap); }, many, "boolean intersect");
    add("object.thicken", "Make it thick (or hollow)...", "Object", "", [this] { showThicken_ = true; }, picked, "shell solidify wall");
    add("object.cleanflat", "Join flat faces side by side", "Object", "", [this] { cleanFlatFaces(); }, picked, "dissolve tidy");
    add("object.spin", "Spin (turn its outline round an axis)...", "Object", "", [this] { showSpin_ = true; }, [this] { return !selected_.empty(); }, "lathe screw revolve");
    add("tools.measure", "Measure between two points", "Tools", "", [this] {
        if (tab_ != Tab::Model && tab_ != Tab::Print) setTab(Tab::Model);
        if (mode_ == Mode::Edit) leaveEdit();
        printTool_ = PrintTool::Measure, measure_.clear();
        say("Measure: click two points (corners catch the mouse); Esc: done");
    }, {}, "ruler distance length dimension how far");
    add("object.repair", "Repair for printing (close holes, fix faces)", "Print", "", [this] { repairSelected(); }, picked, "fix manifold watertight holes");
    add("object.scale", "Print size: scale, fit the plate, fix units...", "Print", "", [this] { setTab(Tab::Print), showPrintSize_ = true; }, picked, "resize fit dimensions percent scaler");
    for (const ToolInfo& t : kTools)
        add(std::string("tool.") + t.name, std::string("Tool: ") + t.name, "Tools", t.key, [this, tool = t.tool, obj = t.objectMode] {
            if (tab_ != Tab::Model) setTab(Tab::Model);
            if (!obj && mode_ != Mode::Edit) enterEdit();
            setTool(tool);
        });
    add("shape.subdivide", "Cut every face in four", "Shape", "", [this] { subdivideNow(false); }, editing, "subdivide");
    add("shape.smoothall", "Smooth it all (cut and round)", "Shape", "", [this] { subdivideNow(true); }, editing, "subdivision surface");
    add("shape.merge", "Merge points", "Shape", "M", [this] { mergeNow(); }, editing, "weld collapse");
    add("shape.face", "Make a face", "Shape", "", [this] { makeFaceNow(); }, editing, "fill");
    add("shape.flip", "Flip faces", "Shape", "", [this] { flipNow(); }, editing, "normals reverse");
    add("shape.smooth", "Smooth points", "Shape", "", [this] { smoothPointsNow(); }, editing, "relax");
    add("shape.push", "Push out / pull in...", "Shape", "Alt+S", [this] { startPush(); }, editing, "shrink fatten inflate");
    add("shape.bridge", "Bridge two faces", "Shape", "", [this] { bridgeNow(); }, editing, "tunnel connect");
    add("shape.soft", "Soft selection on / off", "Shape", "Shift+O", [this] { softSel_ = !softSel_; }, {}, "proportional falloff");
    add("shape.separate", "Make into its own object", "Shape", "P", [this] { separateSelected(); }, editing, "separate split");
    add("paint.mirror", "Paint both sides (mirrored)", "Paint", "", [this] { paintMirror_ = !paintMirror_, say(paintMirror_ ? "Painting both sides" : "Painting one side"); }, {}, "symmetry symmetric");
    add("paint.stencil", "Draw a paint stencil (paint only inside it)", "Paint", "", [this] { setTab(Tab::Paint), drawingStencil_ = true, stencil_.clear(); }, {}, "mask masking tape area");
    add("object.cutEditable", "Cut out, editable (the cutters stay, the hole follows them)", "Object", "", [this] { cutEditable(); }, many, "boolean subtract difference hole non-destructive live");
    add("shape.crease", "Crease the picked edges (stay sharp when smoothed)", "Shape", "Shift+E", [this] { creaseSelected(); }, editing, "sharp hard edge subdivision smooth");
    add("shape.slide", "Slide the points along their edges...", "Shape", "Alt+G", [this] { startSlide(); }, editing, "edge slide vertex slide");
    add("shape.mirror", "Mirror editing (both sides change together)", "Shape", "", [this] { liveMirror_ = !liveMirror_, say(liveMirror_ ? "Mirror editing on: both sides change together" : "Mirror editing off"); }, {}, "symmetry symmetric x");
    add("view.solid", "Show: solid", "View", "Z", [this] { shading_ = Shading::Solid; });
    add("view.rendered", "Show: rendered (sky, sun, shadows)", "View", "Z", [this] { shading_ = Shading::Rendered; });
    add("view.wire", "Show: wireframe", "View", "Shift+Z", [this] { shading_ = Shading::Wire; });
    add("view.xray", "X-ray on / off", "View", "Alt+Z", [this] { xray_ = !xray_; }, {}, "see through");
    add("view.grid", "Grid on / off", "View", "", [this] { showGrid_ = !showGrid_; });
    add("view.persp", "View: perspective", "View", "Numpad 5", [this] { leaveFlatView(); });
    for (const auto& [v, n, k] : std::initializer_list<std::tuple<View, const char*, const char*>>{
             {View::Front, "front", "Numpad 1"}, {View::Back, "back", "Shift+Numpad 1"}, {View::Left, "left side", "Numpad 3"}, {View::Right, "right side", "Shift+Numpad 3"},
             {View::Top, "top", "Numpad 7"}, {View::Bottom, "bottom", "Shift+Numpad 7"}})
        add(std::string("view.") + n, std::string("View: ") + n, "View", k, [this, v] { setView(v); }, {}, "orthographic camera");
    add("view.frame", "Frame the selected", "View", "F", [this] { frameSelected(); }, {}, "zoom focus");
    add("view.all", "Frame everything", "View", "Home", [this] { frameAll(); }, {}, "zoom extents");
    add("view.only", "Show only the selected (again: all)", "View", "/", [this] { showOnly(selected_); }, picked, "isolate local view hide");
    add("view.unhide", "Show everything", "View", "Alt+H", [this] { for (Object& o : scene_.objects) o.visible = true; }, {}, "unhide");
    add("view.layout", "Put the panels back as they were", "View", "", [this] { resetLayout(); }, {}, "reset layout workspace default panels");
    add("view.camera", "Camera: fly or orbit (switch)", "View", "Shift+C", [this] {
        keel::execute(std::format("fry_camera {}", fry_camera.i() == 1 ? 0 : 1));
        say(fry_camera.i() == 1 ? "Orbit camera: right drag turns round what you look at, the wheel zooms (Shift+C: back to flying)"
                                : "Fly camera: right drag turns it where it stands, WASD moves, the wheel goes forward and back (Shift+C: orbit)");
    }, {}, "orbit fly navigation blender game turntable");
    add("view.workspace", "Workspaces (save the panels as they are)...", "View", "", [this] { showWorkspaces_ = true; }, {}, "layout save panels");
    for (int i = 0; i < 3; ++i)
        add(std::string("view.look") + char('0' + i), std::string("Appearance: ") + (i == 0 ? "like the desktop" : i == 1 ? "dark" : "light"), "View", "",
            [i] { keel::execute(std::format("fry_theme {}", i)); }, {}, "theme colours");
    const char* tabs[] = {"Model", "Paint", "Layout", "Render", "Print"};
    for (int i = 0; i < 5; ++i)
        add(std::string("tab.") + tabs[i], std::string("Go to ") + tabs[i], "Tabs", std::format("Ctrl+{}", i + 1), [this, i] { setTab(Tab(i)); }, {}, i == 1 ? "sculpt texture" : "");
    add("script.panel", "The Script panel", "Script", "", [this] { showScript_ = !showScript_; }, {}, "lua code");
    add("script.run", "Run the script", "Script", "F5", [this] { runScriptNow(); }, [this] { return currentScript() != nullptr; });
    add("script.new", "New script", "Script", "", [this] { addScript("Script", kFirstScript); });
    add("script.plugins", "Plug-ins...", "Script", "", [this] { showPlugins_ = true; }, {}, "addons extensions");
    add("help.keys", "Keys and mouse (and your own keys)", "Help", "F1", [this] { showHelp_ = true; }, {}, "shortcuts hotkeys keymap bind");
    add("help.about", "About Fry3DX", "Help", "", [this] { showAbout_ = true; }, {}, "version");
    add("help.search", "Search for a command", "Help", "F3", [this] { openSearch(); }, {}, "find palette");
    for (size_t i = 0; i < plugins_.size(); ++i)
        if (pluginUsable(i))
            for (size_t c = 0; c < plugins_[i].p->commands.size(); ++c) {
                const fry::PluginCommand& cmd = plugins_[i].p->commands[c];
                add("plugin." + plugins_[i].file + "." + cmd.name, cmd.name + (cmd.settings.empty() ? "" : "..."), "Plug-in: " + plugins_[i].p->name, "", [this, i, c] {
                    if (i >= plugins_.size() || !pluginUsable(i) || c >= plugins_[i].p->commands.size()) return;
                    const fry::PluginCommand& cm = plugins_[i].p->commands[c];
                    if (cm.settings.empty()) return runPluginCommand(i, c, {});
                    pendingCommand_ = PendingCommand{i, c, {}};
                    for (const auto& [n, v] : cm.settings) pendingCommand_->values.push_back(v);
                }, {}, cmd.help);
            }
    (void)model;
}

auto App::openSearch() -> void {
    buildCommands();
    search_.show();
}

auto App::runCommand(const std::string& id) -> void {
    for (const fry::Command& c : commands_)
        if (c.id == id) {
            if (!c.usable()) return say(c.name + ": not now (" + (c.group == "Shape" ? "edit a shape first, Tab" : "pick something first") + ")");
            std::erase(recentCommands_, id);
            recentCommands_.insert(recentCommands_.begin(), id);
            if (recentCommands_.size() > 8) recentCommands_.pop_back();
            return c.run();
        }
}

// A menu item for a command: its name, its key (your own if you gave it one), greyed when it can't be done now.
auto App::commandItem(const char* id) -> void {
    for (const fry::Command& c : commands_)
        if (c.id == id) {
            const auto mine = myKeys_.find(c.id);
            const std::string keys = mine != myKeys_.end() ? fry::chordName(mine->second) : c.keys;
            if (ImGui::MenuItem(c.name.c_str(), keys.empty() ? nullptr : keys.c_str(), false, c.usable())) runCommand(c.id);
            return;
        }
}

auto App::commandSearch() -> void {
    if (!search_.open) return;
    if (const int i = search_.draw(commands_, recentCommands_); i >= 0) runCommand(commands_[size_t(i)].id);
}

// The pies: the tools (Space), or the views (Shift+Space), round the mouse while it's held.
auto App::pieFrame(ImGuiIO& io) -> void {
    const bool released = !pieStays_ && !ImGui::IsKeyDown(ImGuiKey_Space);
    std::vector<fry::PieItem> items;
    std::vector<std::function<void()>> runs;
    if (pie_ == 1) {
        for (const ToolInfo& t : kTools)
            if (mode_ == Mode::Edit ? t.editMode : t.objectMode)
                items.push_back({t.name, t.icon, true, tool_ == t.tool}), runs.push_back([this, tool = t.tool] { setTool(tool); });
        items.push_back({mode_ == Mode::Edit ? "Done (objects)" : "Edit the shape", mode_ == Mode::Edit ? Icon::Objects : Icon::Shape, true, false});
        runs.push_back([this] { mode_ == Mode::Edit ? leaveEdit() : enterEdit(); });
    } else {
        const std::pair<View, const char*> views[] = {{View::Top, "Top"}, {View::Right, "Right"}, {View::Front, "Front"}, {View::Perspective, "3D"},
                                                      {View::Bottom, "Bottom"}, {View::Back, "Back"}, {View::Left, "Left"}};
        for (const auto& [v, n] : views) {
            items.push_back({n, Icon::Select, false, view_ == v});
            runs.push_back([this, v] { v == View::Perspective ? leaveFlatView() : setView(v); });
        }
        items.push_back({"Frame", Icon::Select, false, false}), runs.push_back([this] { frameSelected(); });
    }
    const int got = fry::pieMenu(pie_ == 1 ? "Tools" : "Views", pieAt_, items, released);
    if (got == -1) return;
    if (got >= 0) runs[size_t(got)]();
    pie_ = 0, pieStays_ = false;
    (void)io;
}

// Keys of your own: kept in keys.txt (a line each: the command's id, a tab, the key).
auto App::loadMyKeys() -> void {
    myKeys_.clear();
    std::ifstream in(keel::prefPath() + "keys.txt");
    for (std::string line; std::getline(in, line);)
        if (const size_t tab = line.find('\t'); tab != std::string::npos)
            if (const auto c = fry::parseChord(line.substr(tab + 1))) myKeys_[line.substr(0, tab)] = *c;
}

auto App::saveMyKeys() -> void {
    std::ofstream out(keel::prefPath() + "keys.txt");
    for (const auto& [id, c] : myKeys_) out << id << '\t' << fry::chordName(c) << '\n';
}

auto App::menus() -> void {
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) whenSaved(2);
        if (ImGui::MenuItem("Open...", "Ctrl+O")) whenSaved(3);
        if (ImGui::MenuItem("Save", "Ctrl+S")) path_.empty() ? dialog(2) : void(saveFile(path_));
        if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S")) dialog(2);
        ImGui::Separator();
        if (ImGui::MenuItem("Import a model (STL, OBJ, 3MF, glTF)...", "Ctrl+Shift+O")) dialog(12);
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
        commandItem("edit.history");
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
        commandItem("select.similar");
        commandItem("select.loop");
        commandItem("select.ring");
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
        if (ImGui::MenuItem("Cut out, editable", nullptr, false, many)) cutEditable();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The others cut into the last picked but stay objects: move or change them and the hole follows (Apply makes it for good)");
        if (ImGui::MenuItem("Keep only where they overlap", nullptr, false, many)) combineSelected(Combine::Overlap);
        if (ImGui::MenuItem("Join (just as they are)", "Ctrl+J", false, many)) joinSelected();
        ImGui::Separator();
        if (ImGui::MenuItem("Make it thick (or hollow)...", nullptr, false, any)) showThicken_ = true;
        commandItem("object.spin");
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
        ImGui::EndDisabled();
        commandItem("shape.crease");
        commandItem("shape.slide");
        commandItem("shape.mirror");
        ImGui::BeginDisabled(!ed);
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
        ImGui::Separator();
        if (ImGui::MenuItem("Camera: fly (as in games)", nullptr, fry_camera.i() == 0)) keel::execute("fry_camera 0");
        if (ImGui::MenuItem("Camera: orbit (as in Blender)", "Shift+C", fry_camera.i() == 1)) keel::execute("fry_camera 1");
        ImGui::Separator();
        commandItem("tools.measure");
        commandItem("view.workspace");
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
        commandItem("help.search");
        if (ImGui::MenuItem("About Fry3DX")) showAbout_ = true;
        ImGui::EndMenu();
    }
}

auto App::menuBar() -> void {
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

auto App::addMenuItems() -> void {
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
    commandItem("add.text");
}

// How the last item's key looks: filled when it's the one in use, sunk while held.
auto App::keyState(bool on) -> keel::ui::KeyState {
    using keel::ui::KeyState;
    return on ? KeyState::On : ImGui::IsItemActive() ? KeyState::Pressed : ImGui::IsItemHovered() ? KeyState::Hovered : KeyState::Normal;
}

// A square button with a picture on it (the accent when it's the one in use).
auto App::iconButton(const char* id, Icon icon, bool on, const char* tip, float size) -> bool {
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
auto App::wideButton(const char* id, Icon icon, const char* label, bool on, float width, const char* tip, float h) -> bool {
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

auto App::toolbox() -> void {
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
    // Mouse tools from plug-ins (fry.tool): by name, as plug-ins load again.
    if (mode_ == Mode::Object) {
        bool first = true;
        for (size_t i = 0; i < plugins_.size(); ++i)
            if (pluginUsable(i))
                for (const fry::PluginTool& t : plugins_[i].p->tools) {
                    if (std::exchange(first, false)) ImGui::SeparatorText("Plug-in tools");
                    const bool on = pluginTool_ == std::pair(plugins_[i].file, t.name);
                    if (ImGui::Selectable(std::format("{}##{}", t.name, plugins_[i].file).c_str(), on, 0, ImVec2(w, touch() ? 34.0f : 0.0f)))
                        on ? setTool(Tool::Select) : void((pluginTool_ = {plugins_[i].file, t.name}, tool_ = Tool::Select, say(t.name + ": click in the view (Esc stops)")));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s(%s)", t.help.c_str(), t.help.empty() ? "" : "\n", plugins_[i].p->name.c_str());
                }
    }
    ImGui::Spacing();
    if (pluginTool_.first.empty()) // (a plug-in tool: its row shows it's on)
        for (const ToolInfo& t : kTools)
            if (t.tool == tool_) ImGui::TextColored(accentText(), "%s", t.name);
    ImGui::End();
}

// The plug-in tool that's on: which plug-in and which of its tools (none: -1).
auto App::pluginToolAt() const -> std::pair<int, int> {
    for (size_t i = 0; i < plugins_.size(); ++i)
        if (plugins_[i].file == pluginTool_.first && pluginUsable(i))
            for (size_t k = 0; k < plugins_[i].p->tools.size(); ++k)
                if (plugins_[i].p->tools[k].name == pluginTool_.second) return {int(i), int(k)};
    return {-1, -1};
}

// A plug-in tool's click: the spot on what's under the mouse, or on the ground.
auto App::clickPluginTool(ImVec2 mouse) -> void {
    const auto [i, k] = pluginToolAt();
    if (i < 0) return pluginTool_ = {}, say("That plug-in tool isn't loaded any more");
    const Ray ray = mouseRay(mouse);
    glm::vec3 at, n(0, 0, 1);
    uint32_t onto = 0;
    const Hit h = pickObject(ray, 0, true);
    if (const Object* o = h.object ? find(h.object) : nullptr) {
        at = ray.origin + ray.dir * h.t, onto = o->id;
        n = glm::normalize(glm::transpose(glm::inverse(glm::mat3(objectMatrix(*o)))) * faceNormal(o->mesh, h.face));
    } else if (auto g = hitPlane(ray, glm::vec3(0.0f), n)) at = *g;
    else return;
    pushUndo();
    bool frame = false;
    fry::ScriptHost host{&scene_, &selected_, &active_, [this](const std::string& t) { scriptLine(t); }, [&] { frame = true; }};
    const fry::ScriptResult r = plugins_[size_t(i)].p->click(host, size_t(k), at, n, onto);
    finishRun(r, pluginTool_.second, frame);
}

auto App::paintToolbox(float w) -> void {
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

auto App::sculptOptions(float w) -> void {
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

auto App::paintOptions(float w) -> void {
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
        ImGui::Checkbox("Both sides (mirrored, X)", &paintMirror_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("What you paint on one side goes on the other too, mirrored (left and right: the object's own Y)");
        ImGui::SeparatorText("Stencil");
        if (stencil_.size() >= 3) {
            ImGui::Checkbox("Paint only inside it", &stencilOn_);
            if (ImGui::Button("Draw it again", ImVec2(w * 0.5f - 2, 0))) drawingStencil_ = true, stencil_.clear();
            ImGui::SameLine(0, 4);
            if (ImGui::Button("Take it off", ImVec2(w * 0.5f - 2, 0))) stencil_.clear(), stencilOn_ = false;
        } else {
            if (ImGui::Button(drawingStencil_ ? "Drawing: drag round the area..." : "Draw a stencil", ImVec2(w, 0))) drawingStencil_ = !drawingStencil_, stencil_.clear();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag round an area on the screen: paint goes only inside it (it stays put on the screen as you turn the view)");
        }
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

auto App::softChoice(float w) -> void {
    if (mode_ != Mode::Edit) return;
    ImGui::Checkbox("Soft selection (Shift+O)", &softSel_);
    ImGui::Checkbox("Mirror editing (across X)", &liveMirror_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A symmetric shape stays so: move a point, its twin on the other side moves too");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Points near the selection follow it, less the further they are");
    if (!softSel_) return;
    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##reach", &softReach_, 0.002f, 0.001f, 1000.0f, "reaches %.3f m");
    ImGui::TextDisabled("(the wheel while dragging too)");
}

auto App::toolOptions() -> void {
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
    if ((tab_ == Tab::Print && printTool_ != PrintTool::None) || printTool_ == PrintTool::Measure) {
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
        if (angle) return void(ImGui::DragFloat("##snapangle", &snapAngle_, 0.5f, 1.0f, 90.0f, "every %.0f deg"));
        if (tool_ == Tool::Move) {
            ImGui::SetNextItemWidth(w);
            ImGui::Combo("##snapkind", &snapKind_, "To steps\0To points\0To edges' middles\0To faces' middles\0");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Points, middles: the arrows' point goes to the one nearest the mouse (on other shapes, or what isn't picked)");
            if (snapKind_) return;
            ImGui::SetNextItemWidth(w);
        }
        ImGui::DragFloat("##snapmove", &snapMove_, 0.001f, 0.001f, 10.0f, "every %.3f m");
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

auto App::layersPanel() -> void {
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

auto App::paintProperties(float w) -> void {
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
            ImGui::SetNextItemWidth(180);
            undoable([&] { return ImGui::SliderFloat("##op", &l.opacity, 0.0f, 1.0f, "see-through %.2f"); });
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

auto App::renderProperties(float w) -> void {
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
auto App::picturePanel() -> void {
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

auto App::propertiesPanel() -> void {
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
        if (ImGui::Checkbox("Only in its own view", &ref->ownViewOnly)) dirty_ = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shown only looking straight at it (front, side or top): out of the way in the 3D view and the others");
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
    // Its size itself, typed (the scale worked out from it): 0.25 makes it 25 cm, whatever it was.
    glm::vec3 dims = hi - lo;
    const glm::vec3 was = dims;
    if (undoable([&] { return ImGui::DragFloat3("Dimensions (m)", &dims.x, 0.002f, 0.0001f, 1e5f, "%.4g"); })) {
        int k = -1;
        for (int i = 0; i < 3; ++i)
            if (std::abs(dims[i] - was[i]) > 1e-9f) k = i;
        if (k >= 0 && was[k] > 1e-7f && dims[k] > 1e-7f) {
            const float f = dims[k] / was[k];
            if (keepProportions_) o->scale *= f;
            else o->scale[k] *= f;
            touched(o->id);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Across, deep, high: type a size and it's made that big");
    ImGui::Checkbox("Keep its proportions", &keepProportions_);
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
    if (const Object* t = fry::cutTarget(scene_, *o)) { // (an editable cutter)
        ImGui::SeparatorText("Cut");
        ImGui::TextColored(accentText(), "Cuts into %s", t->name.c_str());
        ImGui::TextDisabled("Move it, turn it, change its shape: the hole follows.");
        const uint32_t target = t->id, me = o->id;
        if (ImGui::Button("Apply: make the cut for good", ImVec2(w, 0))) return applyCuts(target, me);
        if (ImGui::Button("Stop cutting (keep it as an object)", ImVec2(w, 0))) pushUndo(), o->cuts = 0, dirty_ = true;
    } else if (cutKeys_.count(o->id)) {
        int n = 0;
        for (const Object& c : scene_.objects) n += c.cuts == o->id && isCutter(c);
        ImGui::SeparatorText("Cut");
        ImGui::TextDisabled("%d editable cut%s in it", n, n == 1 ? "" : "s");
        if (ImGui::Button(n == 1 ? "Apply: make the cut for good" : "Apply: make them all for good", ImVec2(w, 0))) return applyCuts(o->id);
        if (ImGui::Button("Pick the cutters", ImVec2(w, 0))) {
            selected_.clear();
            for (const Object& c : scene_.objects)
                if (c.cuts == o->id) selected_.insert(c.id), active_ = c.id;
        }
    }
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
auto App::materialPicker(Object& o) -> void {
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

auto App::editProperties(Object& o, float w) -> void {
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

auto App::statusBar() -> void {
    ImGui::Begin("Status", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
    if (now() - messageAt_ < 4.0) ImGui::TextColored(accentText(), "%s", message_.c_str());
    else if (tab_ == Tab::Layout) {
        for (const LayToolInfo& t : kLayTools)
            if (t.tool == layTool_)
                ImGui::TextDisabled("%s:  %s   ·   Tab: frames or corners   Right drag: look (pan in the Picture panel)   F1: all keys", t.name, t.hint);
    } else if ((tab_ == Tab::Print && printTool_ != PrintTool::None) || printTool_ == PrintTool::Measure) {
        ImGui::TextDisabled(printTool_ == PrintTool::Measure ? "Measure:  click two points on the model (corners catch the mouse); a third click starts again   ·   M (Print) or Esc: done"
                                                             : "Lay flat:  click the face it should stand on: it turns so that face is down, on the bed   ·   L: done");
    } else if (tab_ == Tab::Paint && sculpting_) {
        for (const SculptToolInfo& t : kSculptTools)
            if (t.brush == sculptBrush_) ImGui::TextDisabled("%s:  %s   ·   Shift: smooth   X: both sides   Tab: colour   Right drag: look   F1: all keys", t.name, t.hint);
    } else if (tab_ == Tab::Paint) {
        for (const PaintToolInfo& t : kPaintTools)
            if (t.tool == paintTool_) ImGui::TextDisabled("%s:  %s   ·   Right drag: look   Wheel: forward / back (zoom in the Picture panel)   F1: all keys", t.name, t.hint);
    } else
        for (const ToolInfo& t : kTools)
            if (t.tool == tool_)
                ImGui::TextDisabled("%s:  %s   ·   %s   F1: all keys", t.name, t.hint,
                                    fry_camera.i() == 1 ? "Right drag: orbit   Middle drag: pan   Wheel: zoom   Shift+C: fly"
                                                        : "Right drag + WASD: look and move   Alt+right: orbit   Wheel: forward / back   Shift+C: orbit");
    ImGui::End();
}

// The view's corner: which way you're looking, buttons to look straight at a side, and
// handles to drag to orbit, pan and zoom (for a touchpad or a pen).
auto App::viewButtons() -> void {
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

} // namespace fryapp
