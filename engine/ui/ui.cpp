#include "ui/ui.h"
#include "core/log.h"
#include "fs/vfs.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace keel::ui {
namespace {
ImFont* g_fonts[3] = {};
float g_scale = 1.0f;

ImFont* loadFont(const char* path, float size) {
    auto data = fs::read(path);
    if (!data) {
        log::warn("Font {} not found, using the built-in one", path);
        return nullptr;
    }
    void* owned = IM_ALLOC(data->size()); // the atlas frees it
    std::memcpy(owned, data->data(), data->size());
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(owned, int(data->size()), size);
}

void theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 2;
    s.FrameRounding = 1;
    s.GrabRounding = 1;
    s.TabRounding = 1;
    s.ScrollbarRounding = 1;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 1;
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(10, 8);
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    // Source 2013 GameUI's charcoal panels, grey frames and near-white text, with a red
    // where Source has its orange.
    const ImVec4 red = rgb(kAccent[0], kAccent[1], kAccent[2]), redLight = rgb(245, 70, 76), redDark = rgb(150, 24, 32);
    c[ImGuiCol_Text] = rgb(235, 230, 220);
    c[ImGuiCol_TextDisabled] = rgb(140, 136, 128);
    c[ImGuiCol_WindowBg] = rgb(40, 40, 38, 0.94f);
    c[ImGuiCol_ChildBg] = rgb(30, 30, 29, 0.6f);
    c[ImGuiCol_PopupBg] = rgb(36, 36, 34, 0.98f);
    c[ImGuiCol_Border] = rgb(90, 88, 82, 0.8f);
    c[ImGuiCol_FrameBg] = rgb(28, 28, 27);
    c[ImGuiCol_FrameBgHovered] = rgb(50, 49, 46);
    c[ImGuiCol_FrameBgActive] = rgb(60, 58, 54);
    c[ImGuiCol_TitleBg] = rgb(52, 51, 48);
    c[ImGuiCol_TitleBgActive] = rgb(70, 30, 32);
    c[ImGuiCol_Button] = rgb(70, 68, 63);
    c[ImGuiCol_ButtonHovered] = rgb(95, 92, 84);
    c[ImGuiCol_ButtonActive] = red;
    c[ImGuiCol_Header] = rgb(70, 68, 63);
    c[ImGuiCol_HeaderHovered] = rgb(95, 92, 84);
    c[ImGuiCol_HeaderActive] = red;
    c[ImGuiCol_Tab] = rgb(55, 54, 50);
    c[ImGuiCol_TabHovered] = rgb(95, 92, 84);
    c[ImGuiCol_TabSelected] = redDark;
    c[ImGuiCol_TabSelectedOverline] = red;
    c[ImGuiCol_SliderGrab] = red;
    c[ImGuiCol_SliderGrabActive] = redLight;
    c[ImGuiCol_CheckMark] = redLight;
    c[ImGuiCol_TextSelectedBg] = rgb(kAccent[0], kAccent[1], kAccent[2], 0.45f);
    c[ImGuiCol_NavCursor] = red;
    c[ImGuiCol_Separator] = rgb(90, 88, 82, 0.6f);
    c[ImGuiCol_ScrollbarBg] = rgb(28, 28, 27, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = rgb(80, 78, 72);
}
} // namespace

void init() {
    ImGuiIO& io = ImGui::GetIO();
    g_fonts[0] = loadFont("fonts/DejaVuSans.ttf", 16.0f);
    g_fonts[1] = loadFont("fonts/DejaVuSans-Bold.ttf", 16.0f);
    g_fonts[2] = loadFont("fonts/DejaVuSans-Bold.ttf", 48.0f);
    if (g_fonts[0]) io.FontDefault = g_fonts[0];
    theme();
}

uint32_t accent(float alpha, float brightness) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(kAccent[0] / 255.0f * brightness, 1.0f), std::min(kAccent[1] / 255.0f * brightness, 1.0f),
                                                 std::min(kAccent[2] / 255.0f * brightness, 1.0f), alpha));
}

ImFont* font(Font f) {
    ImFont* r = g_fonts[int(f)];
    return r ? r : ImGui::GetFont();
}

float scale() { return g_scale; }

void beginFrame(float windowHeight) {
    g_scale = std::clamp(windowHeight / 1080.0f, 0.75f, 2.5f);
    ImGui::GetStyle().FontScaleMain = g_scale;
}

namespace {
struct Popup {
    std::string title, message;
    std::vector<PopupButton> buttons;
};
std::vector<Popup> g_popups;

void finish(size_t button) {
    if (g_popups.empty()) return;
    Popup p = std::move(g_popups.front()); // the action may queue another popup
    g_popups.erase(g_popups.begin());
    if (button < p.buttons.size() && p.buttons[button].action) p.buttons[button].action();
}
} // namespace

void popup(std::string title, std::string message, std::vector<PopupButton> buttons) {
    if (buttons.empty()) buttons.push_back({"OK", {}});
    g_popups.push_back({std::move(title), std::move(message), std::move(buttons)});
}

bool popupOpen() { return !g_popups.empty(); }

void closePopup() {
    if (!g_popups.empty()) finish(g_popups.front().buttons.size() - 1);
}

void drawPopups() {
    if (g_popups.empty()) return;
    const Popup& p = g_popups.front();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = scale();
    // Dim everything behind it, and swallow clicks meant for it.
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    // Keep the dim layer and then the popup on top — but never while a button is held: moving
    // focus cancels the press, so OK would never click.
    const bool refocus = !ImGui::IsAnyMouseDown();
    if (refocus) ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 120));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##popupdim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460 * s, 0));
    if (refocus) ImGui::SetNextWindowFocus();
    ImGui::Begin("##popup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    const ImVec2 pos = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + ImGui::GetWindowWidth(), pos.y + 4 * s), accent());
    ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::PushFont(font(Font::Bold), 0.0f);
    ImGui::TextUnformatted(p.title.c_str());
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(p.message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 6 * s));
    // Buttons on the right; the first is the default (Enter).
    float width = 0;
    for (const PopupButton& b : p.buttons) width += ImGui::CalcTextSize(b.label.c_str()).x + 40 * s;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - width - ImGui::GetStyle().WindowPadding.x));
    size_t pressed = SIZE_MAX;
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(p.buttons[i].label.c_str(), ImVec2(ImGui::CalcTextSize(p.buttons[i].label.c_str()).x + 32 * s, 0))) pressed = i;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) pressed = 0;
    ImGui::End();
    if (pressed != SIZE_MAX) finish(pressed);
}

} // namespace keel::ui
