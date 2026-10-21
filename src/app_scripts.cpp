// Fry3DX: scripts, your own materials, plug-ins, the windows that go with them.
#include "app.h"

using namespace fry;
namespace stdfs = std::filesystem;

namespace fryapp {

// ───────────────────────────── scripts ─────────────────────────────

auto App::scriptLine(std::string text, bool error) -> void {
    size_t at = 0;
    for (size_t nl; (nl = text.find('\n', at)) != std::string::npos; at = nl + 1) scriptLog_.push_back({text.substr(at, nl - at), error});
    scriptLog_.push_back({text.substr(at), error});
    if (scriptLog_.size() > 2000) scriptLog_.erase(scriptLog_.begin(), scriptLog_.begin() + 500);
    scriptLogToBottom_ = true;
}

// "little_town.lua" -> "little town" (a script's name in the list).
auto App::scriptName(const std::string& path) -> std::string {
    std::string n = stdfs::path(fileName(path)).stem().string();
    std::replace(n.begin(), n.end(), '_', ' ');
    return n.empty() ? "Script" : n;
}

// Adds a script to the file's list and shows it (the same one again: just shows it).
auto App::addScript(std::string name, std::string code) -> void {
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
auto App::runScriptNow() -> void {
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
auto App::finishRun(const fry::ScriptResult& r, const std::string& name, bool frame) -> void {
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

auto App::openScript(const std::string& path, const std::string& shownName) -> void {
    std::ifstream in(path, std::ios::binary);
    if (!in) return say("Couldn't open " + shownName);
    addScript(scriptName(shownName), std::string(std::istreambuf_iterator<char>(in), {}));
    scriptPath_ = isUri(path) ? std::string() : path;
    say("Opened " + shownName + ": it's kept in this file now. F5 runs it");
}

auto App::saveScript(std::string file) -> void {
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
auto App::exampleScripts() -> void {
    if (!ImGui::BeginMenu("Examples")) return;
    exampleItems(false);
    ImGui::EndMenu();
}

auto App::exampleItems(bool selectable) -> void {
    for (const std::string& f : keel::fs::list("scripts", ".lua")) {
        const std::string name = scriptName(f);
        if (selectable ? ImGui::Selectable(name.c_str()) : ImGui::MenuItem(name.c_str()))
            if (auto text = keel::fs::readText(f)) addScript(name, *text);
    }
}

auto App::loadOwnMaterials() -> void {
    std::ifstream in(ownMaterialsPath(), std::ios::binary);
    if (in) parseOwnMaterials(std::string(std::istreambuf_iterator<char>(in), {}));
}

auto App::saveOwnMaterials() -> void {
    const std::string text = ownMaterialsText();
    std::string error;
    if (!writeFileSafely(ownMaterialsPath(), text.data(), text.size(), &error)) say("Couldn't keep your materials: " + error);
}

auto App::pluginsOff() const -> std::set<std::string> {
    std::set<std::string> off;
    std::istringstream in(fry_pluginsoff.str());
    for (std::string f; std::getline(in, f, '|');)
        if (!f.empty()) off.insert(f);
    return off;
}

auto App::setPluginOn(const std::string& file, bool on) -> void {
    std::set<std::string> off = pluginsOff();
    if (on) off.erase(file);
    else off.insert(file);
    std::string text;
    for (const std::string& f : off) text += (text.empty() ? "" : "|") + f;
    fry_pluginsoff.set(text);
    loadPlugins();
}

auto App::loadPlugins() -> void {
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

auto App::copyPlugin(const PluginEntry& e) -> void {
    const auto text = keel::fs::readText(e.path);
    if (!text) return say("Couldn't read " + e.file);
    std::error_code ec;
    stdfs::create_directories(pluginsFolder(), ec);
    const stdfs::path to = stdfs::path(pluginsFolder()) / ("my_" + e.file);
    if (!stdfs::exists(to)) std::ofstream(to, std::ios::binary) << *text;
    setPluginOn(e.file, false); // (loads them again)
    say("Your copy: " + to.string() + " (Open the plug-ins folder, change it, Load them again)");
}

auto App::openPluginsFolder() -> void {
    std::error_code ec;
    stdfs::create_directories(pluginsFolder(), ec);
    SDL_OpenURL(("file://" + pluginsFolder()).c_str());
}

auto App::pluginMenuItems(const char* menu) -> void {
    bool first = true;
    for (size_t i = 0; i < plugins_.size(); ++i)
        if (pluginUsable(i))
            for (size_t c = 0; c < plugins_[i].p->commands.size(); ++c)
                if (plugins_[i].p->commands[c].menu == menu) {
                    if (std::exchange(first, false)) ImGui::SeparatorText("Plug-ins");
                    pluginMenuItem(i, c);
                }
}

auto App::pluginMenuItem(size_t i, size_t c) -> void {
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

auto App::runPluginCommand(size_t i, size_t c, const std::vector<double>& values) -> void {
    if (!pluginUsable(i) || c >= plugins_[i].p->commands.size()) return;
    if (mode_ == Mode::Edit) leaveEdit();
    pushUndo();
    bool frame = false;
    fry::ScriptHost host{&scene_, &selected_, &active_, [this](const std::string& t) { scriptLine(t); }, [&] { frame = true; }};
    const fry::ScriptResult r = plugins_[i].p->run(host, c, values);
    finishRun(r, plugins_[i].p->commands[c].name, frame);
}

// A plug-in command's settings, asked for before it runs.
auto App::commandForm() -> void {
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

auto App::pluginsWindow() -> void {
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
    else {
        ImGui::SetNextItemWidth(-1);
        inputString("##filter", pluginFilter_, "Look for a plug-in...");
    }
    std::optional<std::pair<std::string, bool>> toggle;
    for (size_t i = 0; i < plugins_.size(); ++i) {
        const PluginEntry& e = plugins_[i];
        if (!pluginFilter_.empty()) {
            std::string all = lower(e.file + " " + (e.p ? e.p->name + " " + e.p->about : std::string()));
            if (e.p)
                for (const fry::PluginCommand& c : e.p->commands) all += " " + lower(c.name + " " + c.help);
            if (e.p)
                for (const fry::PluginTool& t : e.p->tools) all += " " + lower(t.name + " " + t.help);
            if (all.find(lower(pluginFilter_)) == std::string::npos) continue;
        }
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
            for (const fry::PluginTool& t : e.p->tools) adds += std::format("{}{} (a tool in the Toolbox)", adds.empty() ? "" : ", ", t.name);
            size_t mats = 0;
            for (const MaterialPreset& m : ownMaterials()) mats += m.from == e.file;
            if (mats) adds += std::format("{}{} material{}", adds.empty() ? "" : ", ", mats, mats == 1 ? "" : "s");
            ImGui::TextColored(roleColor(Role::TextTertiary), "Adds: %s", adds.empty() ? "nothing yet" : adds.c_str());
        } else ImGui::TextDisabled("Turned off");
        if (e.bundled) { // (to change one that comes with Fry3DX: your own copy, in your folder)
            if (ImGui::SmallButton("Make my own copy to change")) copyPlugin(e);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copies it into your plug-ins folder (as my_%s) and turns this one off", e.file.c_str());
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::End();
    if (toggle) setPluginOn(toggle->first, toggle->second);
}

auto App::growString(ImGuiInputTextCallbackData* d) -> int {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(d->UserData);
        str->resize(size_t(d->BufTextLen));
        d->Buf = str->data();
    }
    return 0;
}

auto App::scriptPanel() -> void {
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

auto App::helpWindow() -> void {
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
    rows("Finding things", {{"F3 or Ctrl+Space", "Search for any command by name and run it"}, {"Space (held)", "The tools round the mouse: point at one, let go"},
                            {"Shift+Space (held)", "The views round the mouse"}});
    myKeysTable();
    rows("Looking around", {{"Right drag", "Turn the camera where it stands"}, {"Right held + W A S D", "Move where you look (Q / E: down and up)"},
                            {"Arrow keys", "Move where you look, any time (Page Up / Down: down and up)"}, {"Wheel", "Forward and back"},
                            {"Shift + wheel", "How fast every way of moving goes"}, {"Shift (while moving)", "Faster"},
                            {"Alt + right drag", "Orbit round what's picked (or what you look at)"},
                            {"Shift+C", "Switch the camera: fly / orbit (right drag round what you look at, the wheel zooms)"}, {"Middle drag / Shift+Right drag", "Pan"},
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

// Keys of your own: any command on any key (they come before the usual ones).
auto App::myKeysTable() -> void {
    static char filter[64] = {};
    if (!ImGui::CollapsingHeader("Your own keys")) return;
    if (commands_.empty()) buildCommands();
    ImGui::TextWrapped("Give any command a key: click Set, then press the key (with Ctrl, Shift or Alt if you like). Your keys come before the usual ones.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##kf", "Find a command", filter, sizeof filter);
    if (!settingKey_.empty()) {
        ImGui::TextColored(accentText(), "Press the key for it now (Esc: never mind)");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) settingKey_.clear();
        else if (const auto c = fry::pressedChord()) {
            std::erase_if(myKeys_, [&](const auto& kv) { return kv.second == *c; }); // (a key does one thing)
            myKeys_[settingKey_] = *c;
            saveMyKeys();
            settingKey_.clear();
        }
    }
    if (!ImGui::BeginTable("mykeys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 260))) return;
    ImGui::TableSetupColumn("what");
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 130);
    ImGui::TableSetupColumn("do", ImGuiTableColumnFlags_WidthFixed, 96);
    for (const fry::Command& c : commands_) {
        if (filter[0] && !fry::commandScore(c, filter)) continue;
        ImGui::PushID(c.id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(c.name.c_str());
        ImGui::TableNextColumn();
        const auto mine = myKeys_.find(c.id);
        if (mine != myKeys_.end()) ImGui::TextColored(accentText(), "%s", fry::chordName(mine->second).c_str());
        else ImGui::TextDisabled("%s", c.keys.c_str());
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(settingKey_ == c.id ? "..." : "Set")) settingKey_ = c.id;
        if (mine != myKeys_.end()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) myKeys_.erase(c.id), saveMyKeys();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// Each picked object (or all, in the Print tab with none picked) made ready to print.
auto App::repairSelected() -> void {
    std::vector<Object*> targets;
    if (tab_ == Tab::Print) targets = printTargets();
    else
        for (Object& o : scene_.objects)
            if (selected_.count(o.id)) targets.push_back(&o);
    std::erase_if(targets, [](const Object* o) { return o->locked; });
    if (targets.empty()) return say("Pick what to repair first");
    if (mode_ == Mode::Edit) leaveEdit();
    pushUndo();
    fry::Repair all;
    int still = 0;
    for (Object* o : targets) {
        const fry::Repair r = fry::repairForPrint(o->mesh);
        all.joinedPoints += r.joinedPoints, all.emptyFaces += r.emptyFaces, all.doubledFaces += r.doubledFaces, all.turnedFaces += r.turnedFaces, all.closedHoles += r.closedHoles;
        if (r.total()) handMade(*o), touched(o->id);
        still += !checkForPrint(o->mesh).closed();
    }
    if (!all.total()) return takeBack(), say(still ? "Nothing it can repair by itself: look at what the Print tab says" : "Nothing to repair: ready to print");
    std::vector<std::string> did;
    if (all.joinedPoints) did.push_back(std::format("{} points joined", all.joinedPoints));
    if (all.emptyFaces) did.push_back(std::format("{} empty faces gone", all.emptyFaces));
    if (all.doubledFaces) did.push_back(std::format("{} doubled faces gone", all.doubledFaces));
    if (all.turnedFaces) did.push_back(std::format("{} faces turned out", all.turnedFaces));
    if (all.closedHoles) did.push_back(std::format("{} holes closed", all.closedHoles));
    std::string text;
    for (const std::string& d : did) text += (text.empty() ? "" : ", ") + d;
    say("Repaired: " + text + (still ? std::format(". {} still not closed (see the Print tab)", still) : ". Ready to print"));
}

// Crease (Shift+E): the picked edges (or those between picked points / round picked faces)
// kept sharp when the shape's smoothed; again: not.
auto App::creaseSelected() -> void {
    Object* o = editing();
    if (!o) return say("Crease works on a shape being edited (Tab)");
    std::set<std::pair<int, int>> picked;
    if (elem_ == Elem::Edge)
        for (auto [a, b] : selEdges_) picked.insert({std::min(a, b), std::max(a, b)});
    else {
        const std::set<int> pts = selectedPoints();
        for (const auto& e : adjacencyOf(*o, o->mesh).edges)
            if (pts.count(e[0]) && pts.count(e[1])) picked.insert({e[0], e[1]});
    }
    if (picked.empty()) return say("Pick the edges to crease first (Edges, 2)");
    pushUndo();
    const bool all = std::all_of(picked.begin(), picked.end(), [&](const auto& e) { return o->creases.count(e) > 0; });
    for (const auto& e : picked) all ? (void)o->creases.erase(e) : (void)o->creases.insert(e);
    touched(o->id);
    say(all ? std::format("{} edge{} smooth again", picked.size(), picked.size() == 1 ? "" : "s")
            : std::format("{} edge{} creased: smoothing keeps {} sharp{}", picked.size(), picked.size() == 1 ? "" : "s", picked.size() == 1 ? "it" : "them",
                          o->smoothLevels ? "" : " (turn on Smooth out to see)"));
}

auto App::startSlide() -> void {
    Object* o = editing();
    if (!o) return say("Slide works on a shape being edited (Tab)");
    const std::set<int> pts = selectedPoints();
    if (pts.empty()) return say("Pick points, edges or faces to slide first");
    std::vector<std::set<int>> near(o->mesh.verts.size());
    for (const auto& e : adjacencyOf(*o, o->mesh).edges)
        if (pts.count(e[0]) != pts.count(e[1])) near[size_t(e[0])].insert(e[1]), near[size_t(e[1])].insert(e[0]);
    slide_ = {};
    for (int v : pts)
        if (!near[size_t(v)].empty()) slide_.pts.push_back(v), slide_.rails.emplace_back(near[size_t(v)].begin(), near[size_t(v)].end());
    if (slide_.pts.empty()) return say("Nothing for those to slide along (every edge of theirs is picked too)");
    pushUndo();
    slide_.active = true, slide_.object = o->id, slide_.from = o->mesh, slide_.start = ImGui::GetIO().MousePos;
    say("Sliding: move the mouse along the edges, click to keep it (Esc: as it was)");
}

auto App::cancelSlide() -> void {
    if (!slide_.active) return;
    slide_.active = false;
    if (Object* o = find(slide_.object)) o->mesh = slide_.from, touched(o->id);
    takeBack();
    say("Slide put back");
}

auto App::slideFrame(const ImGuiIO& io) -> void {
    Object* o = find(slide_.object);
    if (!o || mode_ != Mode::Edit) {
        if (o) o->mesh = slide_.from, touched(o->id);
        return void(slide_.active = false);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) return cancelSlide(); // (Esc: onEscape)
    const glm::mat4 m = objectMatrix(*o);
    const ImVec2 d(io.MousePos.x - slide_.start.x, io.MousePos.y - slide_.start.y);
    const float dl = std::hypot(d.x, d.y);
    float shown = 0;
    o->mesh.verts = slide_.from.verts;
    if (dl > 1) {
        for (size_t i = 0; i < slide_.pts.size(); ++i) {
            const glm::vec3 p = slide_.from.verts[size_t(slide_.pts[i])];
            ImVec2 sp;
            if (!project(glm::vec3(m * glm::vec4(p, 1.0f)), sp)) continue;
            // (the edge whose way on the screen is most the mouse's, and how far along it the mouse has gone)
            float best = 0.2f, t = 0;
            int to = -1;
            for (int r : slide_.rails[i]) {
                ImVec2 sr;
                if (!project(glm::vec3(m * glm::vec4(slide_.from.verts[size_t(r)], 1.0f)), sr)) continue;
                const ImVec2 e(sr.x - sp.x, sr.y - sp.y);
                const float el = std::hypot(e.x, e.y);
                if (el < 1) continue;
                const float along = (e.x * d.x + e.y * d.y) / el; // (pixels the mouse went that way)
                if (const float k = along / dl; k > best) best = k, to = r, t = std::clamp(along / el, 0.0f, 0.98f);
            }
            if (to >= 0) o->mesh.verts[size_t(slide_.pts[i])] = glm::mix(p, slide_.from.verts[size_t(to)], t), shown = std::max(shown, t);
        }
    }
    if (liveMirror_) mirrorEdits(*o, slide_.from);
    touched(o->id);
    message_ = std::format("Sliding {:.0f}% of the way along (click: keep, Esc: as it was)", shown * 100), messageAt_ = now();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        slide_.active = false;
        changedShape(*o, true);
        say(std::format("Slid {} point{}", slide_.pts.size(), slide_.pts.size() == 1 ? "" : "s"));
    }
}

// Looking straight at a reference picture's plane (the front picture from the front or the back...).
auto App::lookingAt(Axis a) const -> bool {
    switch (a) {
    case Axis::Front: return view_ == View::Front || view_ == View::Back;
    case Axis::Top: return view_ == View::Top || view_ == View::Bottom;
    default: return view_ == View::Left || view_ == View::Right;
    }
}

auto App::resetLayout() -> void {
    layoutAgain_ = true, layoutDone_ = false;
    say("The panels are back where they started");
}

auto App::workspacesWindow() -> void {
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Workspaces", &showWorkspaces_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) return ImGui::End();
    ImGui::TextWrapped("The panels as they are now, kept under a name: one for modelling, one for painting, one for a small screen...");
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##wname", workspaceName_, sizeof workspaceName_);
    ImGui::SameLine();
    if (ImGui::Button("Keep") && workspaceName_[0]) {
        std::error_code ec;
        stdfs::create_directories(workspaceDir(), ec);
        size_t size = 0;
        const char* ini = ImGui::SaveIniSettingsToMemory(&size);
        std::ofstream(workspaceDir() + fileSafe(workspaceName_) + ".ini", std::ios::binary).write(ini, std::streamsize(size));
        say(std::format("Workspace \"{}\" kept", workspaceName_));
    }
    ImGui::SeparatorText("Kept");
    if (ImGui::Selectable("As it started (the usual)")) resetLayout();
    std::error_code ec;
    std::vector<stdfs::path> kept;
    for (const auto& e : stdfs::directory_iterator(workspaceDir(), ec))
        if (e.path().extension() == ".ini") kept.push_back(e.path());
    std::sort(kept.begin(), kept.end());
    for (const stdfs::path& f : kept) {
        ImGui::PushID(f.string().c_str());
        if (ImGui::Selectable(f.stem().string().c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(250, 0))) {
            std::ifstream in(f, std::ios::binary);
            pendingLayout_.assign(std::istreambuf_iterator<char>(in), {});
            say("Workspace: " + f.stem().string());
        }
        ImGui::SameLine(270);
        if (ImGui::SmallButton("Delete")) stdfs::remove(f, ec);
        ImGui::PopID();
    }
    ImGui::End();
}

auto App::fileSafe(std::string n) -> std::string {
    for (char& c : n)
        if (std::strchr("\\/:*?\"<>|", c)) c = '_';
    return n;
}

// The steps that can be undone, oldest first: a click goes back (or on) to just after it.
auto App::historyWindow() -> void {
    ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Undo history", &showHistory_)) return ImGui::End();
    int back = 0, on = 0; // (how many undos / redos the click asks for)
    if (ImGui::Selectable("The start", undo_.empty())) back = int(undo_.size());
    for (size_t i = 0; i < undo_.size(); ++i) {
        const std::string& l = undo_[i].label;
        ImGui::PushID(int(i));
        if (ImGui::Selectable(l.empty() ? "A change" : l.c_str(), i + 1 == undo_.size())) back = int(undo_.size() - 1 - i);
        ImGui::PopID();
    }
    for (size_t i = redo_.size(); i-- > 0;) { // (what redo would bring back, greyed)
        ImGui::PushID(int(10000 + i));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const std::string& l = redo_[i].label;
        if (ImGui::Selectable(l.empty() ? "A change (undone)" : (l + "  (undone)").c_str())) on = int(redo_.size() - i);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (undo_.size() >= 200) ImGui::TextDisabled("(the 200 latest steps are kept)");
    ImGui::End();
    for (int i = 0; i < back; ++i) undo();
    for (int i = 0; i < on; ++i) redo();
}

auto App::aboutWindow() -> void {
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

} // namespace fryapp
