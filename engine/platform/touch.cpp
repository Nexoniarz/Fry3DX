#include "platform/touch.h"
#include "core/cvar.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace keel::touch {
namespace {
#if defined(__ANDROID__)
Cvar touch_controls("touch_controls", "1", CVAR_ARCHIVE, "On-screen touch controls (0 off, 1 on)");
#else
Cvar touch_controls("touch_controls", "1", CVAR_ARCHIVE, "On-screen touch controls once the screen is touched (0 never)");
#endif
Cvar touch_sensitivity("touch_sensitivity", "1.6", CVAR_ARCHIVE, "How far looking turns for a finger's drag");
Cvar touch_size("touch_size", "1", CVAR_ARCHIVE, "Size of the touch buttons and stick");

// A button: its command ("+x" held, else run once), label, and where (from the bottom-right
// corner, in shorter-screen-sides), how big.
struct Button {
    const char* cmd;
    const char* label;
    float x, y, r;
};
constexpr Button kButtons[] = {
    {"+attack", "FIRE", 0.15f, 0.34f, 0.085f}, {"+jump", "JUMP", 0.33f, 0.14f, 0.07f}, {"+duck", "DUCK", 0.13f, 0.13f, 0.06f},
    {"+use", "USE", 0.34f, 0.36f, 0.055f},     {"+attack2", "ALT", 0.13f, 0.57f, 0.055f}, {"+sprint", "RUN", 0.50f, 0.12f, 0.055f},
    {"flashlight", "LIGHT", 0.32f, 0.57f, 0.045f}, {"backpack", "BAG", 0.47f, 0.32f, 0.045f},
};
constexpr int kButtonCount = int(sizeof kButtons / sizeof kButtons[0]);

struct Finger {
    enum Role { Look, Stick, ButtonHeld, Pause } role;
    float x, y;    // where it is now (pixels)
    float ox, oy;  // the stick's centre
    int button = -1;
};
std::unordered_map<SDL_FingerID, Finger> g_fingers;
bool g_used = false, g_escape = false;
float g_lookX = 0, g_lookY = 0;
bool g_held[kButtonCount] = {};

#if defined(__ANDROID__)
constexpr bool kAlways = true;
#else
constexpr bool kAlways = false;
#endif

ImVec2 screen() {
    const ImVec2 s = ImGui::GetIO().DisplaySize;
    return s.x > 0 ? s : ImVec2(1280, 720);
}
float unit() { // the shorter side, scaled
    const ImVec2 s = screen();
    return std::min(s.x, s.y) * std::clamp(touch_size.f(), 0.5f, 2.0f);
}
ImVec2 buttonAt(const Button& b) {
    const ImVec2 s = screen();
    const float m = unit();
    return ImVec2(s.x - b.x * m, s.y - b.y * m);
}
ImVec2 pauseAt() { return ImVec2(unit() * 0.07f, unit() * 0.07f); }
void press(int i, bool down) {
    const Button& b = kButtons[i];
    if (b.cmd[0] == '+') {
        if (down == g_held[i]) return;
        g_held[i] = down;
        execute(down ? b.cmd : ("-" + std::string(b.cmd + 1)));
    } else if (down) {
        execute(b.cmd);
    }
}
} // namespace

bool active() { return touch_controls.b() && (kAlways || g_used); }

void handleEvent(const SDL_Event& ev, bool inGame) {
    if (ev.type != SDL_EVENT_FINGER_DOWN && ev.type != SDL_EVENT_FINGER_MOTION && ev.type != SDL_EVENT_FINGER_UP) return;
    g_used = true;
    const ImVec2 s = screen();
    const float x = ev.tfinger.x * s.x, y = ev.tfinger.y * s.y;
    if (!inGame || !active()) { // menus: SDL's mouse emulation does the taps
        if (ev.type == SDL_EVENT_FINGER_UP) g_fingers.erase(ev.tfinger.fingerID);
        return;
    }
    const float m = unit();
    if (ev.type == SDL_EVENT_FINGER_DOWN) {
        Finger f{Finger::Look, x, y, x, y};
        const ImVec2 p = pauseAt();
        if (std::hypot(x - p.x, y - p.y) < m * 0.06f) {
            f.role = Finger::Pause;
            g_escape = true;
        } else {
            for (int i = 0; i < kButtonCount; ++i) {
                const ImVec2 c = buttonAt(kButtons[i]);
                if (std::hypot(x - c.x, y - c.y) < kButtons[i].r * m * 1.25f) {
                    f.role = Finger::ButtonHeld, f.button = i;
                    press(i, true);
                    break;
                }
            }
            if (f.role == Finger::Look && x < s.x * 0.4f && y > s.y * 0.3f) f.role = Finger::Stick; // the left thumb's area
        }
        g_fingers[ev.tfinger.fingerID] = f;
    } else if (ev.type == SDL_EVENT_FINGER_MOTION) {
        auto it = g_fingers.find(ev.tfinger.fingerID);
        if (it == g_fingers.end()) return;
        Finger& f = it->second;
        if (f.role == Finger::Look) {
            g_lookX += (x - f.x) * touch_sensitivity.f();
            g_lookY += (y - f.y) * touch_sensitivity.f();
        }
        f.x = x, f.y = y;
        if (f.role == Finger::Stick) { // the stick's centre follows a thumb that goes past its edge
            const float r = m * 0.11f, dx = x - f.ox, dy = y - f.oy, d = std::hypot(dx, dy);
            if (d > r) f.ox = x - dx / d * r, f.oy = y - dy / d * r;
        }
    } else {
        auto it = g_fingers.find(ev.tfinger.fingerID);
        if (it == g_fingers.end()) return;
        if (it->second.role == Finger::ButtonHeld) press(it->second.button, false);
        g_fingers.erase(it);
    }
}

void move(float& forward, float& side) {
    forward = side = 0.0f;
    const float r = unit() * 0.11f;
    for (const auto& [id, f] : g_fingers)
        if (f.role == Finger::Stick) {
            side = std::clamp((f.x - f.ox) / r, -1.0f, 1.0f);
            forward = std::clamp(-(f.y - f.oy) / r, -1.0f, 1.0f);
        }
}

void takeLook(float& dx, float& dy) {
    dx = g_lookX, dy = g_lookY;
    g_lookX = g_lookY = 0;
}

bool takeEscape() { return std::exchange(g_escape, false); }

void releaseAll() {
    for (int i = 0; i < kButtonCount; ++i)
        if (g_held[i]) press(i, false);
    g_fingers.clear();
}

void draw(bool inGame) {
    if (!inGame || !active()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float m = unit();
    const ImU32 ring = IM_COL32(255, 255, 255, 110), fill = IM_COL32(20, 20, 24, 70), hot = IM_COL32(220, 40, 40, 150), text = IM_COL32(255, 255, 255, 200);
    auto label = [&](ImVec2 c, const char* t) {
        const ImVec2 sz = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f), text, t);
    };
    for (int i = 0; i < kButtonCount; ++i) {
        const ImVec2 c = buttonAt(kButtons[i]);
        const float r = kButtons[i].r * m;
        dl->AddCircleFilled(c, r, g_held[i] ? hot : fill, 40);
        dl->AddCircle(c, r, ring, 40, 2.0f);
        label(c, kButtons[i].label);
    }
    const ImVec2 p = pauseAt();
    dl->AddCircleFilled(p, m * 0.045f, fill, 32);
    dl->AddCircle(p, m * 0.045f, ring, 32, 2.0f);
    dl->AddRectFilled(ImVec2(p.x - m * 0.016f, p.y - m * 0.02f), ImVec2(p.x - m * 0.006f, p.y + m * 0.02f), text);
    dl->AddRectFilled(ImVec2(p.x + m * 0.006f, p.y - m * 0.02f), ImVec2(p.x + m * 0.016f, p.y + m * 0.02f), text);
    // The stick, where the thumb is (or a hint of where it goes).
    const ImVec2 s = screen();
    bool stick = false;
    for (const auto& [id, f] : g_fingers)
        if (f.role == Finger::Stick) {
            stick = true;
            dl->AddCircleFilled(ImVec2(f.ox, f.oy), m * 0.11f, fill, 48);
            dl->AddCircle(ImVec2(f.ox, f.oy), m * 0.11f, ring, 48, 2.0f);
            const float dx = f.x - f.ox, dy = f.y - f.oy, d = std::hypot(dx, dy), k = d > m * 0.11f ? m * 0.11f / d : 1.0f;
            dl->AddCircleFilled(ImVec2(f.ox + dx * k, f.oy + dy * k), m * 0.045f, hot, 32);
        }
    if (!stick) {
        const ImVec2 c(m * 0.22f, s.y - m * 0.22f);
        dl->AddCircle(c, m * 0.11f, IM_COL32(255, 255, 255, 50), 48, 2.0f);
    }
}
} // namespace keel::touch
