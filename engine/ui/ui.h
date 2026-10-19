#pragma once
// Game UI look: fonts from the game folder and a Source-like theme (dark
// translucent panels, flat buttons, light text, red highlights) on top of Dear ImGui.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImFont;

namespace keel::ui {

enum class Font { Regular, Bold, Title };

// After the renderer (ImGui context) and the VFS are up.
void init();
ImFont* font(Font f);
// Sizes in pixels; scaled with the window height so 1080p looks like Source at 1080p.
float scale();
void beginFrame(float windowHeight);

// The highlight colour: highlights, the logo.
inline constexpr uint8_t kAccent[3] = {227, 34, 45};
uint32_t accent(float alpha = 1.0f, float brightness = 1.0f); // ImU32

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
