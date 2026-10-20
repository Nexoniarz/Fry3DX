#pragma once
// Commands: everything the app can do, by name. Found by typing (the command search, F3 or
// Ctrl+Space), put on keys of your own (Keys and mouse), and offered round the mouse (pie menus).
#include "icons.h"

#include <imgui.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fry {

struct Command {
    std::string id;    // (stays the same: what keys of your own are kept against)
    std::string name;  // what it's called ("Round edges")
    std::string group; // where it lives ("Shape", "View")
    std::string keys;  // its own key, as shown ("B")
    std::function<void()> run;
    std::function<bool()> ok; // (can it be done now; none: always)
    std::string words;        // more to find it by ("bevel chamfer")
    bool usable() const { return !ok || ok(); }
};

// A key with what's held with it.
struct Chord {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false, shift = false, alt = false;
    bool operator==(const Chord&) const = default;
    explicit operator bool() const { return key != ImGuiKey_None; }
};
std::string chordName(const Chord& c);
std::optional<Chord> parseChord(const std::string& text);
// The chord pressed this frame (a key that isn't only Ctrl / Shift / Alt), or none.
std::optional<Chord> pressedChord();

// How well `query` fits a command (0: not at all): every word of it in the name, group or
// words; the start of a word, and the name, count most.
int commandScore(const Command& c, const std::string& query);

// The command search: a box to type in and the commands that fit, best first; Enter or a click
// runs one. `recent`: ids run lately (first when nothing's typed yet). True while it's open.
struct CommandSearch {
    bool open = false;
    char text[128] = {};
    int at = 0;
    bool focus = false;
    void show() { open = true, focus = true, text[0] = 0, at = 0; }
    // Draws it; the command to run (its index), or -1.
    int draw(const std::vector<Command>& all, const std::vector<std::string>& recent);
};

// A pie menu: the choices round the mouse, picked by moving towards one and letting go of the
// key (or clicking). Draws it; the index picked, -1 while choosing, -2 when let go on nothing.
struct PieItem {
    std::string name;
    Icon icon = Icon::Select;
    bool hasIcon = true;
    bool on = false;
};
int pieMenu(const char* title, ImVec2 centre, const std::vector<PieItem>& items, bool released);

} // namespace fry
