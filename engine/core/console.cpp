// The developer console, Source-style: it drops down from the top; as you type, a list under
// the input shows the commands and cvars that match (cvars with their values) and, once a
// command is typed, what its argument can be (map names for "map", files for "exec", ...).
// Arrows walk the list (writing the pick into the input, like Source), Tab completes, Enter
// runs. History survives restarts.
#include "core/console.h"
#include "core/cvar.h"
#include "core/log.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace keel::console {
namespace {
bool g_open = false;
bool g_focus = false;
float g_slide = 0.0f; // 0 = up, 1 = down
char g_input[512] = {};
std::vector<std::string> g_history;
int g_historyPos = -1;
std::string g_historyFile;

struct Suggestion {
    std::string text;  // the whole input it stands for
    std::string label; // what's shown: the name or argument
    std::string value; // cvars: their value
    std::string help;
    bool pick = true;  // false: an info row (a cvar's description)
};
std::vector<Suggestion> g_list;
int g_sel = -1;
std::string g_typed;             // what the user typed (the list is built from it, not from picks)
std::string g_pending;           // replace the input with this on the next callback (a click)
bool g_hasPending = false;
constexpr size_t kMaxSuggestions = 14;

bool containsNoCase(std::string_view hay, std::string_view needle) {
    auto low = [](char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); };
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [&](char a, char b) { return low(a) == low(b); }) != hay.end();
}

void describe(Suggestion& s, const std::string& name) {
    if (Cmd* c = Cmd::find(name)) s.help = c->help();
    else if (Cvar* v = Cvar::find(name)) s.value = v->str(), s.help = v->help();
    else
        for (const auto& [n, text] : aliases())
            if (n == name) s.help = "alias: " + text;
}

void buildList() {
    g_list.clear();
    g_sel = -1;
    const std::string& in = g_typed;
    // Only the last command of a "a; b; c" line.
    const size_t semi = in.rfind(';');
    std::string head = semi == std::string::npos ? "" : in.substr(0, semi + 1) + " ";
    std::string seg = semi == std::string::npos ? in : in.substr(semi + 1);
    seg.erase(0, seg.find_first_not_of(' '));
    if (semi != std::string::npos) head = in.substr(0, in.size() - seg.size());
    if (seg.empty()) return;
    const size_t space = seg.find(' ');
    if (space == std::string::npos) {
        // A name: those starting with it first, then those containing it.
        const std::vector<std::string> names = allNames();
        for (int pass = 0; pass < 2 && g_list.size() < kMaxSuggestions; ++pass)
            for (const std::string& n : names) {
                const bool prefix = n.starts_with(seg);
                if (pass == 0 ? !prefix : (prefix || !containsNoCase(n, seg))) continue;
                Suggestion s{head + n + " ", n};
                describe(s, n);
                g_list.push_back(std::move(s));
                if (g_list.size() >= kMaxSuggestions) break;
            }
        return;
    }
    // An argument: the command's own suggestions, or what a cvar is.
    const std::string name = seg.substr(0, space);
    const size_t lastSpace = seg.rfind(' ');
    const std::string partial = seg.substr(lastSpace + 1);
    const std::string before = head + seg.substr(0, lastSpace + 1);
    if (Cmd* c = Cmd::find(name); c && c->completer()) {
        const size_t arg = tokenize(seg.substr(0, lastSpace)).size(); // the command is token 0
        for (const std::string& v : c->completer()(partial, arg)) {
            Suggestion s{before + v + " ", v};
            if (name == "help" || name == "reset" || name == "toggle" || name == "set" || name == "incrementvar") describe(s, v);
            g_list.push_back(std::move(s));
            if (g_list.size() >= kMaxSuggestions) break;
        }
    } else if (Cvar* v = Cvar::find(name)) {
        Suggestion s{"", name, v->str(), std::string(v->help()) + "  (default \"" + v->defaultValue() + "\")"};
        s.pick = false;
        g_list.push_back(std::move(s));
    } else if (Cmd* c2 = Cmd::find(name)) {
        Suggestion s{"", name, "", c2->help()};
        s.pick = false;
        g_list.push_back(std::move(s));
    }
}

void setInput(ImGuiInputTextCallbackData* data, const std::string& text) {
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, text.c_str());
}

int inputCallback(ImGuiInputTextCallbackData* data) {
    switch (data->EventFlag) {
    case ImGuiInputTextFlags_CallbackAlways:
        if (g_hasPending) {
            setInput(data, g_pending);
            g_hasPending = false;
        }
        break;
    case ImGuiInputTextFlags_CallbackEdit:
        g_typed.assign(data->Buf, size_t(data->BufTextLen));
        g_historyPos = -1;
        buildList();
        break;
    case ImGuiInputTextFlags_CallbackHistory: {
        const bool up = data->EventKey == ImGuiKey_UpArrow;
        int picks = 0;
        for (const auto& s : g_list) picks += s.pick;
        if (picks > 0 && !g_typed.empty()) { // walk the suggestions
            int i = g_sel;
            do i = up ? (i <= 0 ? int(g_list.size()) - 1 : i - 1) : (i + 1) % int(g_list.size());
            while (!g_list[size_t(i)].pick);
            g_sel = i;
            setInput(data, g_list[size_t(i)].text);
        } else if (!g_history.empty()) { // or the history
            if (up) g_historyPos = g_historyPos < 0 ? int(g_history.size()) - 1 : std::max(0, g_historyPos - 1);
            else if (g_historyPos >= 0 && ++g_historyPos >= int(g_history.size())) g_historyPos = -1;
            setInput(data, g_historyPos >= 0 ? g_history[size_t(g_historyPos)] : "");
        }
        break;
    }
    case ImGuiInputTextFlags_CallbackCompletion: {
        // Tab: the picked suggestion, else the first, else the longest common start.
        std::vector<const Suggestion*> picks;
        for (const auto& s : g_list)
            if (s.pick) picks.push_back(&s);
        if (picks.empty()) break;
        const Suggestion* s = g_sel >= 0 && g_list[size_t(g_sel)].pick ? &g_list[size_t(g_sel)] : picks.front();
        setInput(data, s->text);
        g_typed = s->text;
        buildList();
        break;
    }
    default:
        break;
    }
    return 0;
}

void remember(const std::string& line) {
    if (g_history.empty() || g_history.back() != line) g_history.push_back(line);
    if (g_history.size() > 200) g_history.erase(g_history.begin());
    if (g_historyFile.empty()) return;
    std::ofstream out(g_historyFile);
    for (const auto& h : g_history) out << h << '\n';
}
} // namespace

namespace {
Cmd cmd_toggleconsole("toggleconsole", [](const Args&) { toggle(); }, "Open or close the console");
Cmd cmd_con_input("con_input", [](const Args& a) {
    std::string text;
    for (size_t i = 1; i < a.size(); ++i) text += (i > 1 ? " " : "") + a[i];
    if (a.size() > 1 && a.back().empty()) text += " ";
    std::snprintf(g_input, sizeof g_input, "%s", text.c_str());
    g_pending = text; // the input box keeps its own copy while focused
    g_hasPending = true;
    g_typed = text;
    buildList();
    if (!g_open) toggle();
}, "con_input <text>: open the console with this typed (scripts, tests)");
} // namespace

void toggle() {
    g_open = !g_open;
    g_focus = g_open;
}
bool isOpen() { return g_open; }

void setHistoryFile(const std::string& path) {
    g_historyFile = path;
    g_history.clear();
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) g_history.push_back(line);
}

void draw() {
    const float dt = ImGui::GetIO().DeltaTime;
    g_slide = std::clamp(g_slide + (g_open ? dt : -dt) * 7.0f, 0.0f, 1.0f);
    if (g_slide <= 0.0f) return;
    const float k = 1.0f - (1.0f - g_slide) * (1.0f - g_slide); // ease out
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float fullH = vp->WorkSize.y * 0.46f;
    const ImU32 accent = IM_COL32(226, 54, 54, 255);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y - fullH * (1.0f - k)));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, fullH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(14, 13, 15, 238));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
    ImGui::Begin("Console", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(wp.x, wp.y + ws.y - 2), ImVec2(wp.x + ws.x, wp.y + ws.y), accent);

    const float footer = ImGui::GetFrameHeightWithSpacing() + 4;
    ImGui::BeginChild("lines", ImVec2(0, -footer));
    for (const log::Line& line : log::history()) {
        const bool echo = line.text.starts_with("] ");
        const ImVec4 color = line.level == log::Level::Error ? ImVec4(1, .42f, .42f, 1)
                           : line.level == log::Level::Warn  ? ImVec4(1, .78f, .35f, 1)
                           : echo                            ? ImVec4(.95f, .55f, .5f, 1)
                                                             : ImVec4(.86f, .85f, .83f, 1);
        ImGui::TextColored(color, "%s", line.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // The input, with a prompt.
    ImGui::TextColored(ImVec4(.89f, .21f, .21f, 1), ">");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (g_focus) {
        ImGui::SetKeyboardFocusHere();
        g_focus = false;
    }
    const ImVec2 inputPos = ImGui::GetCursorScreenPos();
    const auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion |
                       ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways;
    if (ImGui::InputText("##input", g_input, sizeof g_input, flags, inputCallback)) {
        std::string line = g_input;
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!line.empty()) {
            log::info("] {}", line);
            remember(line);
            execute(line);
        }
        g_input[0] = 0;
        g_typed.clear();
        g_list.clear();
        g_sel = -1;
        g_historyPos = -1;
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_L) && ImGui::GetIO().KeyCtrl) log::clearHistory();
    const float inputH = ImGui::GetItemRectSize().y;
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    // The suggestions, under the console like Source's.
    if (g_list.empty() || g_slide < 1.0f || !g_open) return;
    ImGui::SetNextWindowPos(ImVec2(inputPos.x, inputPos.y + inputH + 6));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(22, 20, 23, 245));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(226, 54, 54, 120));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::Begin("##suggestions", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
    float nameW = 0, valueW = 0;
    for (const auto& s : g_list) {
        nameW = std::max(nameW, ImGui::CalcTextSize(s.label.c_str()).x);
        valueW = std::max(valueW, s.value.empty() ? 0.0f : ImGui::CalcTextSize(("\"" + s.value + "\"").c_str()).x);
    }
    const float maxW = vp->WorkSize.x - (inputPos.x - vp->WorkPos.x) - 40;
    for (int i = 0; i < int(g_list.size()); ++i) {
        const Suggestion& s = g_list[size_t(i)];
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float rowW = std::min(maxW, nameW + valueW + 520);
        if (s.pick) {
            if (ImGui::Selectable("##row", g_sel == i, 0, ImVec2(rowW, 0))) {
                g_pending = s.text;
                g_hasPending = true;
                g_typed = s.text;
                buildList();
                g_focus = true;
            }
        } else {
            ImGui::Dummy(ImVec2(rowW, ImGui::GetTextLineHeight()));
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (g_sel == i) dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + ImGui::GetTextLineHeight()), accent);
        dl->AddText(ImVec2(p.x + 8, p.y), IM_COL32(240, 236, 228, 255), s.label.c_str());
        float x = p.x + 8 + nameW + 16;
        if (!s.value.empty()) dl->AddText(ImVec2(x, p.y), IM_COL32(240, 120, 110, 255), ("\"" + s.value + "\"").c_str());
        x += valueW + (valueW > 0 ? 16 : 0);
        if (!s.help.empty()) {
            ImGui::PushClipRect(ImVec2(x, p.y), ImVec2(p.x + rowW, p.y + 40), true);
            dl->AddText(ImVec2(x, p.y), IM_COL32(150, 145, 138, 255), s.help.c_str());
            ImGui::PopClipRect();
        }
        ImGui::PopID();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

} // namespace keel::console
