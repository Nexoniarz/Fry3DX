#pragma once
// The UI's look: Niveus (github.com/Nexoniarz/niveus) on top of Dear ImGui. Buttons are keys,
// bevelled with light falling across them; fields are wells cut into the surface; warm
// graphite in dark, soft paper in light, and one brand colour that every accent is made from.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImFont;
struct ImDrawList;
struct ImVec2;

namespace keel::ui {

enum class Font { Regular, Bold, Title, Mono };

// After the renderer (ImGui context) and the VFS are up.
void init();
ImFont* font(Font f);
// Sizes in pixels; scaled with the window height so 1080p looks like Source at 1080p.
float scale();
void beginFrame(float windowHeight);
// The UI laid out `k` times smaller than the screen's pixels and drawn scaled up, everything
// alike (a phone: its pixels are tiny). 1: as it is.
void setPixelScale(float k);
float pixelScale();
// The backends call this between the platform's NewFrame and ImGui::NewFrame.
void applyPixelScale();

// ── Appearance ──
enum class Appearance { System, Dark, Light };
void setAppearance(Appearance a); // System: follows the desktop's (and changes with it)
Appearance appearance();
bool dark(); // what's showing now
// The brand colour (sRGB): the accent and its states, the text on it and the tint in the greys
// are all made from it, in both appearances.
void setBrand(uint8_t r, uint8_t g, uint8_t b);

// Niveus's semantic roles.
enum class Role {
    Background, BackgroundSubtle, Surface, SurfaceElevated, SurfaceInset, SurfaceHover, SurfaceActive, Scrim,
    Text, TextSecondary, TextTertiary, TextDisabled,
    BorderSubtle, Border, BorderStrong,
    Accent, AccentHover, AccentActive, AccentSubtle, AccentSubtleHover, AccentText, OnAccent,
    Success, SuccessText, Warning, WarningText, Danger, DangerText, OnDanger,
    Highlight, Lowlight, Outline, Shadow, ShadowInset, SheenTop, SheenBottom, SheenStrongTop, SheenStrongBottom,
    Count
};
uint32_t color(Role r, float alpha = 1.0f); // ImU32
// The accent (alpha, and brighter or darker by `brightness`).
uint32_t accent(float alpha = 1.0f, float brightness = 1.0f); // ImU32

// The materials, for widgets drawn by hand.
enum class KeyState { Normal, Hovered, Pressed, On };
// A raised key; `on` keys are filled with the accent.
void drawKey(ImDrawList* dl, const ImVec2& a, const ImVec2& b, float rounding, KeyState state);
// A well cut into the surface (fields, tracks).
void drawWell(ImDrawList* dl, const ImVec2& a, const ImVec2& b, float rounding, bool hovered = false);
// The text/icon colour that goes on a key in that state.
uint32_t keyText(KeyState state);

// ── Popups: modal messages over everything (disconnects, errors, questions) ──
struct PopupButton {
    std::string label;
    std::function<void()> action; // may be empty: just closes
};
// Queued; shown one at a time. No buttons = a single "OK".
void popup(std::string title, std::string message, std::vector<PopupButton> buttons = {});
bool popupOpen();
void closePopup(); // Escape: like pressing the last button
// The engine calls this each frame after the game's UI.
void drawPopups();

} // namespace keel::ui
