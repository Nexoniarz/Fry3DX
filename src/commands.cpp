#include "commands.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>

namespace fry {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The keys a chord can be made of (letters, numbers, F keys, the keypad and the rest).
bool chordKey(ImGuiKey k) {
    return (k >= ImGuiKey_Tab && k <= ImGuiKey_KeypadEqual) && !(k >= ImGuiKey_LeftCtrl && k <= ImGuiKey_Menu) && k != ImGuiKey_Escape;
}

} // namespace

std::string chordName(const Chord& c) {
    if (!c) return "";
    std::string s;
    if (c.ctrl) s += "Ctrl+";
    if (c.shift) s += "Shift+";
    if (c.alt) s += "Alt+";
    return s + ImGui::GetKeyName(c.key);
}

std::optional<Chord> parseChord(const std::string& text) {
    Chord c;
    std::string rest = text;
    for (;;) {
        if (rest.starts_with("Ctrl+")) c.ctrl = true, rest.erase(0, 5);
        else if (rest.starts_with("Shift+")) c.shift = true, rest.erase(0, 6);
        else if (rest.starts_with("Alt+")) c.alt = true, rest.erase(0, 4);
        else break;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        if (chordKey(ImGuiKey(k)) && rest == ImGui::GetKeyName(ImGuiKey(k))) {
            c.key = ImGuiKey(k);
            return c;
        }
    return std::nullopt;
}

std::optional<Chord> pressedChord() {
    const ImGuiIO& io = ImGui::GetIO();
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        if (chordKey(ImGuiKey(k)) && ImGui::IsKeyPressed(ImGuiKey(k), false)) return Chord{ImGuiKey(k), io.KeyCtrl, io.KeyShift, io.KeyAlt};
    return std::nullopt;
}

int commandScore(const Command& c, const std::string& query) {
    const std::string name = lower(c.name), all = name + " " + lower(c.group) + " " + lower(c.words) + " " + lower(c.keys);
    int score = 1;
    size_t from = 0;
    bool any = false;
    const std::string q = lower(query);
    while (from < q.size()) {
        const size_t space = q.find(' ', from);
        const std::string word = q.substr(from, space == std::string::npos ? std::string::npos : space - from);
        from = space == std::string::npos ? q.size() : space + 1;
        if (word.empty()) continue;
        any = true;
        const size_t at = all.find(word);
        if (at == std::string::npos) return 0;
        const bool wordStart = at == 0 || !std::isalnum(static_cast<unsigned char>(all[at - 1]));
        score += (at < name.size() ? 20 : 6) + (wordStart ? 10 : 0) + (at == 0 ? 15 : 0);
    }
    if (any && name == q) score += 50;
    return score;
}

int CommandSearch::draw(const std::vector<Command>& all, const std::vector<std::string>& recent) {
    if (!open) return -1;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(560.0f, vp->WorkSize.x - 32.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.12f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    if (!ImGui::IsPopupOpen("##commands")) ImGui::OpenPopup("##commands");
    int run = -1;
    if (!ImGui::BeginPopup("##commands", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
        open = false;
        return -1;
    }
    // F3 or Ctrl+Space again: closed (not the press that opened it)
    if (!focus && (ImGui::IsKeyPressed(ImGuiKey_F3, false) || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)))) open = false;
    if (focus) ImGui::SetKeyboardFocusHere(), focus = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##find", "Type what you want to do: round edges, hole, export STL...", text, sizeof text);
    // What fits, best first (nothing typed: the ones used lately, then the rest A to Z).
    std::vector<std::pair<int, int>> found; // (score, index)
    const std::string q = text;
    for (size_t i = 0; i < all.size(); ++i) {
        int s = q.empty() ? 1 : commandScore(all[i], q);
        if (!s) continue;
        if (const auto r = std::find(recent.begin(), recent.end(), all[i].id); r != recent.end()) s += 40 - int(r - recent.begin()); // (used lately: higher)
        if (!all[i].usable()) s -= 30;
        found.push_back({s, int(i)});
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (found.size() > 60) found.resize(60);
    at = std::clamp(at, 0, std::max(0, int(found.size()) - 1));
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) at = std::min(at + 1, int(found.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) at = std::max(at - 1, 0);
    if (ImGui::IsItemEdited()) at = 0;
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    const float rowH = ImGui::GetTextLineHeightWithSpacing() + 4;
    ImGui::BeginChild("##list", ImVec2(0, std::min(float(found.size()), 12.0f) * rowH + 6), ImGuiChildFlags_None);
    for (int n = 0; n < int(found.size()); ++n) {
        const Command& c = all[size_t(found[size_t(n)].second)];
        ImGui::PushID(n);
        ImGui::BeginDisabled(!c.usable());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##c", n == at, ImGuiSelectableFlags_None, ImVec2(0, rowH - 4))) run = found[size_t(n)].second;
        if (n == at && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))) ImGui::SetScrollHereY();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float ty = p.y + (rowH - 4 - ImGui::GetFontSize()) * 0.5f;
        dl->AddText(ImVec2(p.x + 6, ty), ImGui::GetColorU32(ImGuiCol_Text), c.name.c_str());
        const std::string right = c.keys.empty() ? c.group : c.group + "   " + c.keys;
        const float rw = ImGui::CalcTextSize(right.c_str()).x;
        dl->AddText(ImVec2(p.x + ImGui::GetContentRegionAvail().x - rw - 6, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), right.c_str());
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (found.empty()) ImGui::TextDisabled("Nothing called that");
    ImGui::EndChild();
    if (enter && !found.empty()) run = found[size_t(at)].second;
    if (run >= 0 && !all[size_t(run)].usable()) run = -1;
    if (run >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;
    if (!open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return run;
}

int pieMenu(const char* title, ImVec2 c, const std::vector<PieItem>& items, bool released) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const int n = int(items.size());
    if (!n) return -2;
    const float radius = std::max(110.0f, ImGui::GetFontSize() * 8.5f), dead = 26.0f;
    const ImVec2 d(io.MousePos.x - c.x, io.MousePos.y - c.y);
    const float far = std::hypot(d.x, d.y);
    int hot = -1;
    if (far > dead) { // (the slice the mouse points into; the first at the top, going round clockwise)
        const float a = std::atan2(d.x, -d.y), slice = 2 * std::numbers::pi_v<float> / float(n);
        hot = int(std::floor((a + slice * 0.5f + 2 * std::numbers::pi_v<float>) / slice)) % n;
    }
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_PopupBg), text = ImGui::GetColorU32(ImGuiCol_Text), hi = ImGui::GetColorU32(ImGuiCol_ButtonActive),
                line = ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddCircleFilled(c, dead, bg, 32);
    dl->AddCircle(c, dead, line, 32, 1.5f);
    if (far > dead) dl->AddLine(c, ImVec2(c.x + d.x / far * dead, c.y + d.y / far * dead), hi, 3.0f);
    const ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + dead + 6), ImGui::GetColorU32(ImGuiCol_TextDisabled), title);
    const float h = ImGui::GetFrameHeight() + 6;
    for (int i = 0; i < n; ++i) {
        const float a = 2 * std::numbers::pi_v<float> * float(i) / float(n);
        const ImVec2 at(c.x + std::sin(a) * radius, c.y - std::cos(a) * radius);
        const float tw = ImGui::CalcTextSize(items[size_t(i)].name.c_str()).x, w = tw + (items[size_t(i)].hasIcon ? h : 0) + 20;
        const ImVec2 lo(at.x - w * 0.5f, at.y - h * 0.5f), up(at.x + w * 0.5f, at.y + h * 0.5f);
        const bool hover = i == hot || ImGui::IsMouseHoveringRect(lo, up, false);
        if (hover) hot = i;
        dl->AddRectFilled(lo, up, i == hot ? hi : bg, h * 0.5f);
        dl->AddRect(lo, up, items[size_t(i)].on ? hi : line, h * 0.5f, 0, items[size_t(i)].on ? 2.0f : 1.0f);
        float x = lo.x + 10;
        if (items[size_t(i)].hasIcon) drawIcon(dl, items[size_t(i)].icon, ImVec2(x, lo.y + 4), h - 8, text), x += h - 2;
        dl->AddText(ImVec2(x, at.y - ImGui::GetFontSize() * 0.5f), text, items[size_t(i)].name.c_str());
    }
    if (released || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return hot >= 0 ? hot : -2;
    return -1;
}

} // namespace fry
