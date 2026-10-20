#include "ui/ui.h"
#include "core/log.h"
#include "fs/vfs.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// ImGui's RenderFrame asks this first (patched in by cmake/Dependencies.cmake).
extern bool (*GImRenderFrameHook)(ImDrawList*, ImVec2, ImVec2, ImU32, bool, float);

namespace keel::ui {
namespace {
ImFont* g_fonts[4] = {};
float g_scale = 1.0f;

ImFont* loadFont(const char* path, float size, const char* fallback = nullptr) {
    auto data = fs::read(path);
    if (!data) {
        log::warn("Font {} not found, using the built-in one", path);
        return nullptr;
    }
    void* owned = IM_ALLOC(data->size()); // the atlas frees it
    std::memcpy(owned, data->data(), data->size());
    ImFont* f = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(owned, int(data->size()), size);
    if (auto more = fallback ? fs::read(fallback) : std::nullopt) { // what the face hasn't got (arrows, symbols): from another
        ImFontConfig cfg;
        cfg.MergeMode = true;
        void* o = IM_ALLOC(more->size());
        std::memcpy(o, more->data(), more->size());
        ImGui::GetIO().Fonts->AddFontFromMemoryTTF(o, int(more->size()), size, &cfg);
    }
    return f;
}

// ── Colour: Niveus's derivation (tokens/color.css, themes/*.css), in OKLab ──
struct Lab {
    float L, a, b, alpha = 1.0f;
};
float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float toSrgb(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
Lab lab(uint32_t rgb, float alpha = 1.0f) {
    const float r = toLinear(float((rgb >> 16) & 255) / 255.0f), g = toLinear(float((rgb >> 8) & 255) / 255.0f), b = toLinear(float(rgb & 255) / 255.0f);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s, 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s, alpha};
}
ImVec4 rgba(const Lab& c) {
    const float l = std::pow(c.L + 0.3963377774f * c.a + 0.2158037573f * c.b, 3.0f);
    const float m = std::pow(c.L - 0.1055613458f * c.a - 0.0638541728f * c.b, 3.0f);
    const float s = std::pow(c.L - 0.0894841775f * c.a - 1.2914855480f * c.b, 3.0f);
    auto out = [](float v) { return std::clamp(toSrgb(std::max(v, 0.0f)), 0.0f, 1.0f); };
    return ImVec4(out(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s), out(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s),
                  out(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s), c.alpha);
}
Lab mix(const Lab& x, const Lab& y, float t) { return {x.L + (y.L - x.L) * t, x.a + (y.a - x.a) * t, x.b + (y.b - x.b) * t, 1.0f}; }
float chroma(const Lab& c) { return std::hypot(c.a, c.b); }
// The same hue with lightness L and chroma C (oklch(from c L C h)).
Lab lch(const Lab& c, float L, float C, float alpha = 1.0f) {
    const float k = chroma(c) > 1e-6f ? C / chroma(c) : 0.0f;
    return {L, c.a * k, c.b * k, alpha};
}
// A fill pushed out of the mid-tones until its label reads (--nv-contrast-*).
Lab fill(const Lab& c) { return lch(c, c.L < 0.7f ? std::min(c.L, 0.53f) : std::max(c.L, 0.78f), std::clamp(chroma(c), 0.02f, 0.25f)); }
Lab onFill(const Lab& f) { return lch(f, 0.14f + std::clamp((0.68f - f.L) * 100.0f, 0.0f, 1.0f) * 0.84f, chroma(f) * 0.2f); }

uint32_t g_brand = 0xff7828; // Fry3DX's hot orange
Appearance g_appearance = Appearance::System;
bool g_dark = true, g_styled = false;
ImVec4 g_role[size_t(Role::Count)];

void derive(bool darkLook) {
    const Lab brand = lab(g_brand);
    auto tint = [&](uint32_t grey, float t = 0.04f) { return rgba(mix(lab(grey), brand, t)); };
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    auto set = [](Role r, ImVec4 v) { g_role[size_t(r)] = v; };
    const Lab accent = fill(brand);
    const Lab success = fill(mix(lab(0x189a60), brand, 0.12f)), warning = fill(mix(lab(0xeda520), brand, 0.12f)), danger = fill(mix(lab(0xe2334a), brand, 0.12f));
    set(Role::OnAccent, rgba(onFill(accent)));
    set(Role::OnDanger, rgba(onFill(danger)));
    set(Role::Success, rgba(success)), set(Role::Warning, rgba(warning)), set(Role::Danger, rgba(danger));
    if (darkLook) {
        set(Role::Background, tint(0x121214)), set(Role::BackgroundSubtle, tint(0x18181b)), set(Role::Surface, tint(0x1f1f22));
        set(Role::SurfaceElevated, tint(0x2a2a2e)), set(Role::SurfaceInset, tint(0x111113)), set(Role::SurfaceHover, tint(0x313136));
        set(Role::SurfaceActive, tint(0x39393f)), set(Role::Scrim, rgb(6, 6, 8, 0.62f));
        set(Role::Text, rgb(0xec, 0xeb, 0xe8)), set(Role::TextSecondary, tint(0xa9a8a3, 0.08f)), set(Role::TextTertiary, tint(0x7d7c77, 0.08f));
        set(Role::TextDisabled, rgb(0x58, 0x58, 0x5c));
        set(Role::BorderSubtle, rgb(255, 255, 255, 0.06f)), set(Role::Border, rgb(255, 255, 255, 0.1f)), set(Role::BorderStrong, rgb(255, 255, 255, 0.2f));
        set(Role::Accent, rgba(accent)), set(Role::AccentHover, rgba(lch(accent, accent.L + 0.05f, chroma(accent))));
        set(Role::AccentActive, rgba(lch(accent, accent.L + 0.09f, chroma(accent))));
        set(Role::AccentSubtle, rgba({accent.L, accent.a, accent.b, 0.2f})), set(Role::AccentSubtleHover, rgba({accent.L, accent.a, accent.b, 0.28f}));
        set(Role::AccentText, rgba(lch(accent, std::clamp(accent.L + 0.2f, 0.74f, 0.9f), chroma(accent) * 0.9f)));
        auto text = [](const Lab& c) { return rgba(lch(c, std::clamp(c.L + 0.2f, 0.76f, 0.9f), chroma(c) * 0.9f)); };
        set(Role::SuccessText, text(success)), set(Role::WarningText, text(warning)), set(Role::DangerText, text(danger));
        set(Role::Highlight, rgb(255, 255, 255, 0.11f)), set(Role::Lowlight, rgb(0, 0, 0, 0.42f)), set(Role::Outline, rgb(0, 0, 0, 0.72f));
        set(Role::Shadow, rgb(0, 0, 0, 0.5f)), set(Role::ShadowInset, rgb(0, 0, 0, 0.65f));
        set(Role::SheenTop, rgb(255, 255, 255, 0.06f)), set(Role::SheenBottom, rgb(0, 0, 0, 0.09f));
        set(Role::SheenStrongTop, rgb(255, 255, 255, 0.2f)), set(Role::SheenStrongBottom, rgb(0, 0, 0, 0.12f));
    } else {
        set(Role::Background, tint(0xe8e5de)), set(Role::BackgroundSubtle, tint(0xeeece6)), set(Role::Surface, rgb(0xfb, 0xfb, 0xfa));
        set(Role::SurfaceElevated, rgb(0xf8, 0xf8, 0xf6)), set(Role::SurfaceInset, tint(0xe3e2dd)), set(Role::SurfaceHover, tint(0xf1f0ec));
        set(Role::SurfaceActive, tint(0xe6e5e0)), set(Role::Scrim, rgb(28, 24, 18, 0.34f));
        set(Role::Text, rgb(0x1d, 0x1d, 0x20)), set(Role::TextSecondary, tint(0x63625d, 0.08f)), set(Role::TextTertiary, tint(0x87867f, 0.08f));
        set(Role::TextDisabled, rgb(0xae, 0xad, 0xa7));
        set(Role::BorderSubtle, rgb(28, 24, 18, 0.07f)), set(Role::Border, rgb(28, 24, 18, 0.13f)), set(Role::BorderStrong, rgb(28, 24, 18, 0.22f));
        set(Role::Accent, rgba(accent)), set(Role::AccentHover, rgba(lch(accent, accent.L - 0.045f, chroma(accent))));
        set(Role::AccentActive, rgba(lch(accent, accent.L - 0.09f, chroma(accent))));
        set(Role::AccentSubtle, rgba(lch(accent, 0.965f, chroma(accent) * 0.16f))), set(Role::AccentSubtleHover, rgba(lch(accent, 0.93f, chroma(accent) * 0.22f)));
        set(Role::AccentText, rgba(lch(accent, std::clamp(accent.L - 0.12f, 0.3f, 0.5f), chroma(accent))));
        auto text = [](const Lab& c) { return rgba(lch(c, std::clamp(c.L - 0.1f, 0.3f, 0.48f), chroma(c))); };
        set(Role::SuccessText, text(success)), set(Role::WarningText, text(warning)), set(Role::DangerText, text(danger));
        set(Role::Highlight, rgb(255, 255, 255, 0.9f)), set(Role::Lowlight, rgb(40, 32, 20, 0.07f)), set(Role::Outline, rgb(40, 32, 20, 0.05f));
        set(Role::Shadow, rgb(40, 32, 20, 0.12f)), set(Role::ShadowInset, rgb(40, 32, 20, 0.17f));
        set(Role::SheenTop, rgb(255, 255, 255, 0.75f)), set(Role::SheenBottom, rgb(40, 32, 20, 0.04f));
        set(Role::SheenStrongTop, rgb(255, 255, 255, 0.24f)), set(Role::SheenStrongBottom, rgb(20, 16, 10, 0.1f));
    }
}
ImVec4 role(Role r) { return g_role[size_t(r)]; }
ImVec4 withAlpha(ImVec4 c, float a) { return c.w = a, c; }

void theme() {
    derive(g_dark);
    ImGuiStyle& s = ImGui::GetStyle();
    const float fontScale = s.FontScaleMain, fontDpi = s.FontScaleDpi;
    s = ImGuiStyle();
    s.FontScaleMain = fontScale, s.FontScaleDpi = fontDpi;
    // Niveus's radii and 4px rhythm. Frames have no border of ImGui's: their edges are the
    // keys' and wells' own (drawn by frameHook).
    s.WindowRounding = 8, s.ChildRounding = 6, s.PopupRounding = 8, s.FrameRounding = 6, s.GrabRounding = 5, s.TabRounding = 6;
    s.ScrollbarRounding = 8, s.ScrollbarSize = 11, s.GrabMinSize = 12;
    s.WindowBorderSize = 1, s.PopupBorderSize = 1, s.FrameBorderSize = 0, s.TabBorderSize = 0, s.DockingSeparatorSize = 2;
    s.WindowPadding = ImVec2(10, 10), s.FramePadding = ImVec2(8, 5), s.ItemSpacing = ImVec2(8, 6), s.ItemInnerSpacing = ImVec2(6, 4);
    s.SeparatorTextBorderSize = 1, s.SeparatorTextPadding = ImVec2(10, 3);
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = role(Role::Text);
    c[ImGuiCol_TextDisabled] = role(Role::TextTertiary);
    c[ImGuiCol_WindowBg] = role(Role::Surface);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = role(Role::SurfaceElevated);
    c[ImGuiCol_Border] = role(Role::Border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = role(Role::SurfaceInset);
    c[ImGuiCol_FrameBgHovered] = g_dark ? role(Role::Background) : role(Role::SurfaceActive);
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_FrameBgHovered];
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = role(Role::BackgroundSubtle);
    c[ImGuiCol_MenuBarBg] = role(Role::BackgroundSubtle);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = role(Role::BorderStrong);
    c[ImGuiCol_ScrollbarGrabHovered] = role(Role::TextTertiary);
    c[ImGuiCol_ScrollbarGrabActive] = role(Role::TextSecondary);
    c[ImGuiCol_CheckMark] = role(Role::Accent);
    c[ImGuiCol_SliderGrab] = role(Role::Accent);
    c[ImGuiCol_SliderGrabActive] = role(Role::AccentHover);
    c[ImGuiCol_Button] = role(Role::SurfaceElevated);
    c[ImGuiCol_ButtonHovered] = role(Role::SurfaceHover);
    c[ImGuiCol_ButtonActive] = role(Role::SurfaceActive);
    c[ImGuiCol_Header] = role(Role::AccentSubtle);
    c[ImGuiCol_HeaderHovered] = role(Role::SurfaceHover);
    c[ImGuiCol_HeaderActive] = role(Role::AccentSubtleHover);
    c[ImGuiCol_Separator] = role(Role::BorderSubtle);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = role(Role::Accent);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = role(Role::AccentSubtleHover);
    c[ImGuiCol_InputTextCursor] = role(Role::Accent);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = role(Role::BackgroundSubtle);
    c[ImGuiCol_TabHovered] = role(Role::SurfaceHover);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = role(Role::Surface);
    c[ImGuiCol_TabSelectedOverline] = role(Role::Accent);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_DockingPreview] = withAlpha(role(Role::Accent), 0.4f);
    c[ImGuiCol_DockingEmptyBg] = role(Role::Background);
    c[ImGuiCol_PlotLines] = c[ImGuiCol_PlotHistogram] = role(Role::Accent);
    c[ImGuiCol_PlotLinesHovered] = c[ImGuiCol_PlotHistogramHovered] = role(Role::AccentHover);
    c[ImGuiCol_TableHeaderBg] = role(Role::BackgroundSubtle);
    c[ImGuiCol_TableBorderStrong] = role(Role::Border);
    c[ImGuiCol_TableBorderLight] = role(Role::BorderSubtle);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = withAlpha(role(Role::Text), 0.03f);
    c[ImGuiCol_TextLink] = role(Role::AccentText);
    c[ImGuiCol_TextSelectedBg] = withAlpha(role(Role::Accent), 0.3f);
    c[ImGuiCol_TreeLines] = role(Role::BorderSubtle);
    c[ImGuiCol_DragDropTarget] = role(Role::Accent);
    c[ImGuiCol_NavCursor] = withAlpha(role(Role::Accent), 0.6f);
    c[ImGuiCol_NavWindowingHighlight] = role(Role::Accent);
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = withAlpha(role(Role::Scrim), role(Role::Scrim).w * 0.5f); // (an editor: the work stays readable behind)
}

ImU32 u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImVec4 over(ImVec4 base, ImVec4 top) { // `top` laid over `base`
    const float a = top.w;
    return ImVec4(base.x + (top.x - base.x) * a, base.y + (top.y - base.y) * a, base.z + (top.z - base.z) * a, base.w);
}
bool saturated(ImVec4 c) { return std::max({c.x, c.y, c.z}) - std::min({c.x, c.y, c.z}) > 0.3f; }

// A key with `fillCol` as its face: contact shadow, the face with light falling down it, a lit top
// edge and a shaded bottom one; pressed, it sinks: flat, with the shade inside along its top.
void key(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImVec4 face, bool pressed) {
    const bool filled = saturated(face);
    if (pressed) {
        dl->AddRectFilled(a, b, u32(face), r);
        dl->AddRect(a, b, u32(role(Role::Border)), r);
        dl->PushClipRect(a, b, true);
        dl->AddRect(ImVec2(a.x, a.y), ImVec2(b.x, b.y + 4), u32(withAlpha(role(Role::ShadowInset), role(Role::ShadowInset).w * 0.6f)), r, 0, 2.0f);
        dl->PopClipRect();
        return;
    }
    dl->AddRectFilled(ImVec2(a.x, a.y + 1), ImVec2(b.x, b.y + 2), u32(withAlpha(role(Role::Shadow), role(Role::Shadow).w * 0.7f)), r); // its shadow
    if (g_dark) dl->AddRect(ImVec2(a.x - 0.5f, a.y - 0.5f), ImVec2(b.x + 0.5f, b.y + 0.5f), u32(role(Role::Outline)), r + 0.5f); // the contact line
    const ImVec4 top = over(face, role(filled ? Role::SheenStrongTop : Role::SheenTop)), bottom = over(face, role(filled ? Role::SheenStrongBottom : Role::SheenBottom));
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, u32(face), r);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), u32(top), u32(bottom));
    if (!filled) dl->AddRect(a, b, u32(role(Role::Border)), r);
    const float in = std::min(r * 0.7f, (b.x - a.x) * 0.3f);
    dl->AddLine(ImVec2(a.x + in, a.y + 1.0f), ImVec2(b.x - in, a.y + 1.0f), u32(filled ? withAlpha(role(Role::Highlight), 0.35f) : role(Role::Highlight)));
    dl->AddLine(ImVec2(a.x + in, b.y - 1.0f), ImVec2(b.x - in, b.y - 1.0f), u32(role(Role::Lowlight)));
}

// ImGui's RenderFrame (patched in, see cmake/Dependencies.cmake): buttons become keys and fields wells.
bool frameHook(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, bool, float r) {
    if (!g_styled) return false;
    const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Button), hovered = ImGui::GetColorU32(ImGuiCol_ButtonHovered), held = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    if (col == normal || col == hovered || col == held) {
        key(dl, a, b, r, ImGui::ColorConvertU32ToFloat4(col), col == held && held != normal);
        return true;
    }
    if (col == ImGui::GetColorU32(ImGuiCol_FrameBg) || col == ImGui::GetColorU32(ImGuiCol_FrameBgHovered) || col == ImGui::GetColorU32(ImGuiCol_FrameBgActive)) {
        drawWell(dl, a, b, r, col != ImGui::GetColorU32(ImGuiCol_FrameBg));
        return true;
    }
    return false;
}

Appearance fromSystem() { return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT ? Appearance::Light : Appearance::Dark; }
void restyle() {
    g_dark = (g_appearance == Appearance::System ? fromSystem() : g_appearance) != Appearance::Light;
    theme();
    g_styled = true;
}
} // namespace

void init() {
    ImGuiIO& io = ImGui::GetIO();
    g_fonts[0] = loadFont("fonts/InstrumentSans-Regular.ttf", 16.0f, "fonts/DejaVuSans.ttf");
    g_fonts[1] = loadFont("fonts/InstrumentSans-SemiBold.ttf", 16.0f, "fonts/DejaVuSans-Bold.ttf");
    g_fonts[2] = loadFont("fonts/InstrumentSans-Bold.ttf", 48.0f);
    g_fonts[3] = loadFont("fonts/JetBrainsMono-Regular.ttf", 15.0f, "fonts/DejaVuSans.ttf");
    if (!g_fonts[0]) g_fonts[0] = loadFont("fonts/DejaVuSans.ttf", 16.0f); // (an older content folder)
    if (g_fonts[0]) io.FontDefault = g_fonts[0];
    GImRenderFrameHook = frameHook;
    restyle();
}

void setAppearance(Appearance a) {
    g_appearance = a;
    restyle();
}
Appearance appearance() { return g_appearance; }
bool dark() { return g_dark; }
void setBrand(uint8_t r, uint8_t g, uint8_t b) {
    g_brand = uint32_t(r) << 16 | uint32_t(g) << 8 | b;
    restyle();
}

uint32_t color(Role r, float alpha) {
    ImVec4 c = role(r);
    c.w *= alpha;
    return u32(c);
}

uint32_t accent(float alpha, float brightness) {
    const ImVec4 c = role(Role::Accent);
    return u32(ImVec4(std::min(c.x * brightness, 1.0f), std::min(c.y * brightness, 1.0f), std::min(c.z * brightness, 1.0f), alpha));
}

void drawKey(ImDrawList* dl, const ImVec2& a, const ImVec2& b, float rounding, KeyState state) {
    switch (state) {
    case KeyState::Normal: return key(dl, a, b, rounding, role(Role::SurfaceElevated), false);
    case KeyState::Hovered: return key(dl, a, b, rounding, role(Role::SurfaceHover), false);
    case KeyState::Pressed: return key(dl, a, b, rounding, role(Role::SurfaceActive), true);
    case KeyState::On: return key(dl, a, b, rounding, role(Role::Accent), false);
    }
}

void drawWell(ImDrawList* dl, const ImVec2& a, const ImVec2& b, float r, bool hovered) {
    dl->AddRectFilled(a, b, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), r);
    // Shade inside along the top (the light's blocked by the rim), light along the bottom lip.
    dl->PushClipRect(a, b, true);
    const ImVec4 sh = role(Role::ShadowInset);
    dl->AddRect(a, ImVec2(b.x, b.y + 6), u32(withAlpha(sh, sh.w * 0.55f)), r, 0, 1.5f);
    dl->AddRect(ImVec2(a.x, a.y + 1), ImVec2(b.x, b.y + 6), u32(withAlpha(sh, sh.w * 0.2f)), r, 0, 1.5f);
    dl->PopClipRect();
    dl->AddRect(a, b, u32(role(Role::BorderSubtle)), r);
    const float in = std::min(r, (b.x - a.x) * 0.3f);
    dl->AddLine(ImVec2(a.x + in, b.y + 0.5f), ImVec2(b.x - in, b.y + 0.5f), u32(withAlpha(role(Role::Highlight), role(Role::Highlight).w * 0.6f)));
}

uint32_t keyText(KeyState state) { return u32(role(state == KeyState::On ? Role::OnAccent : Role::Text)); }

ImFont* font(Font f) {
    ImFont* r = g_fonts[int(f)];
    return r ? r : ImGui::GetFont();
}

float scale() { return g_scale; }

namespace {
float g_pixelScale = 1.0f;
ImU32 g_lastScaledEvent = 0;
}
void setPixelScale(float k) { g_pixelScale = std::clamp(k, 0.5f, 6.0f); }
float pixelScale() { return g_pixelScale; }
void applyPixelScale() {
    const float k = g_pixelScale;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x /= k, io.DisplaySize.y /= k; // (set again each frame by the platform)
    io.DisplayFramebufferScale.x *= k, io.DisplayFramebufferScale.y *= k;
    // The mouse's positions queued since last frame, in the screen's pixels: into the UI's.
    // (Events left over for the next frame were done already: by their number.)
    ImU32 last = g_lastScaledEvent;
    for (ImGuiInputEvent& e : GImGui->InputEventsQueue) {
        if (e.EventId <= g_lastScaledEvent) continue;
        last = std::max(last, e.EventId);
        if (e.Type == ImGuiInputEventType_MousePos && e.MousePos.PosX > -FLT_MAX)
            e.MousePos.PosX /= k, e.MousePos.PosY /= k;
    }
    g_lastScaledEvent = last;
}

void beginFrame(float windowHeight) {
    g_scale = std::clamp(windowHeight / 1080.0f, 0.75f, 2.5f);
    ImGui::GetStyle().FontScaleMain = g_scale;
    if (g_appearance == Appearance::System && g_styled && (fromSystem() != Appearance::Light) != g_dark) restyle(); // (the desktop changed)
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, color(Role::Scrim));
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
