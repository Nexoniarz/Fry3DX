#include "platform/input.h"
#include "platform/touch.h"
#include "core/cvar.h"
#include "core/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <map>
#include <set>

namespace keel::input {
namespace {
float g_dx = 0, g_dy = 0;
bool g_captured = false;
bool g_noCapture = false;
std::map<std::string, std::string> g_binds; // key name → command
std::set<std::string> g_held;               // keys whose "+" command is active
void (*g_capture)(const char*) = nullptr;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string keyName(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "ctrl";
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "shift";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "alt";
    default: return lower(SDL_GetScancodeName(sc));
    }
}

void press(const std::string& key, bool down) {
    if (down && g_capture) {
        auto cb = g_capture;
        g_capture = nullptr;
        cb(key.c_str());
        return;
    }
    auto it = g_binds.find(key);
    if (it == g_binds.end()) return;
    const std::string& cmd = it->second;
    if (cmd.starts_with('+')) {
        if (down && g_held.insert(key).second) execute(cmd);
        else if (!down && g_held.erase(key)) execute("-" + cmd.substr(1));
    } else if (down) {
        execute(cmd);
    }
}

Cmd cmd_bind("bind", [](const Args& a) {
    if (a.size() < 2) return log::info("usage: bind <key> [command]");
    if (a.size() == 2) {
        auto it = g_binds.find(lower(a[1]));
        return log::info("\"{}\" = \"{}\"", a[1], it == g_binds.end() ? "" : it->second);
    }
    std::string cmd;
    for (size_t i = 2; i < a.size(); ++i) cmd += (i > 2 ? " " : "") + a[i];
    bind(a[1].c_str(), cmd.c_str());
}, "Bind a key to a command");
Cmd cmd_unbind("unbind", [](const Args& a) {
    if (a.size() > 1) g_binds.erase(lower(a[1]));
}, "Remove a key binding");
Cmd cmd_unbindall("unbindall", [](const Args&) { g_binds.clear(); }, "Remove all key bindings");
Cmd cmd_bindlist("key_listboundkeys", [](const Args&) {
    for (auto& [k, v] : g_binds) log::info("  \"{}\" = \"{}\"", k, v);
    log::info("{} keys bound", g_binds.size());
}, "List every key binding");
Cmd cmd_bindlist2("bindlist", [](const Args&) { execute("key_listboundkeys"); }, "List every key binding");
// Key names for the console's suggestions.
std::vector<std::string> keyNames() {
    std::vector<std::string> v;
    for (char c = 'a'; c <= 'z'; ++c) v.emplace_back(1, c);
    for (char c = '0'; c <= '9'; ++c) v.emplace_back(1, c);
    for (int f = 1; f <= 12; ++f) v.push_back("f" + std::to_string(f));
    for (const char* k : {"space", "tab", "escape", "return", "backspace", "ctrl", "shift", "alt", "left ctrl", "left shift", "left alt", "up", "down",
                          "left", "right", "insert", "delete", "home", "end", "pageup", "pagedown", "mouse1", "mouse2", "mouse3", "mouse4", "mouse5",
                          "mwheelup", "mwheeldown", "`", "-", "=", "[", "]", ";", "'", ",", ".", "/"})
        v.emplace_back(k);
    return v;
}
[[maybe_unused]] const bool g_keyCompleters = [] {
    cmd_bind.complete([](std::string_view p, size_t arg) { return matching(arg == 1 ? keyNames() : allNames(), p); });
    cmd_unbind.complete([](std::string_view p, size_t) {
        std::vector<std::string> v;
        for (auto& [k, _] : g_binds) v.push_back(k);
        return matching(v, p);
    });
    return true;
}();
} // namespace

void bind(const char* key, const char* command) {
    if (!*command) g_binds.erase(lower(key));
    else g_binds[lower(key)] = command;
}

std::string bindingsConfig() {
    std::string out = "unbindall\n";
    for (auto& [k, v] : g_binds) out += "bind \"" + k + "\" \"" + v + "\"\n";
    return out;
}

std::string keyFor(const char* command) {
    for (auto& [k, v] : g_binds)
        if (v == command) return k;
    return {};
}

void captureNextKey(void (*callback)(const char*)) { g_capture = callback; }

void releaseAll() {
    for (const std::string& key : std::set<std::string>(g_held)) press(key, false);
}

bool keyDown(int scancode) {
    if (!g_captured) return false;
    int n = 0;
    const bool* keys = SDL_GetKeyboardState(&n);
    return scancode >= 0 && scancode < n && keys[scancode];
}

bool mouseDown(int button) { return g_captured && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(button)); }

void takeMouseDelta(float& dx, float& dy) {
    float tx, ty; // looking with a finger counts as the mouse
    touch::takeLook(tx, ty);
    dx = g_dx + tx;
    dy = g_dy + ty;
    g_dx = g_dy = 0;
}

bool captured() { return g_captured; }
void disableCapture() { g_noCapture = true; }

void setCaptured(bool on) {
    if (on && g_noCapture) return;
    if (on == g_captured) return;
    g_captured = on;
    if (!on) releaseAll(), touch::releaseAll();
    if (SDL_Window* w = SDL_GetKeyboardFocus()) SDL_SetWindowRelativeMouseMode(w, on);
    g_dx = g_dy = 0;
}

void handleEvent(const SDL_Event& ev) {
    // While playing, touches are the touch controls (platform/touch.cpp), not a mouse.
    if ((ev.type == SDL_EVENT_MOUSE_MOTION && ev.motion.which == SDL_TOUCH_MOUSEID) ||
        ((ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) && ev.button.which == SDL_TOUCH_MOUSEID)) {
        if (g_captured) return;
    }
    if (ev.type == SDL_EVENT_MOUSE_MOTION && g_captured) {
        g_dx += ev.motion.xrel;
        g_dy += ev.motion.yrel;
    }
    if (!g_captured && !g_capture) return;
    if ((ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) && !ev.key.repeat)
        press(keyName(ev.key.scancode), ev.type == SDL_EVENT_KEY_DOWN);
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP)
        // Source's names: mouse1 left, mouse2 right, mouse3 middle (SDL has middle = 2, right = 3).
        press("mouse" + std::to_string(ev.button.button == SDL_BUTTON_RIGHT ? 2 : ev.button.button == SDL_BUTTON_MIDDLE ? 3 : ev.button.button),
              ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    if (ev.type == SDL_EVENT_MOUSE_WHEEL && ev.wheel.y != 0) {
        const std::string key = ev.wheel.y > 0 ? "mwheelup" : "mwheeldown";
        press(key, true);
        press(key, false);
    }
}

} // namespace keel::input
