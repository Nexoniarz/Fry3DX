#pragma once
#include <string>

union SDL_Event;

namespace keel::input {

bool keyDown(int scancode); // SDL_SCANCODE_*
bool mouseDown(int button); // SDL_BUTTON_LEFT etc.
// Mouse movement since the last call, in pixels.
void takeMouseDelta(float& dx, float& dy);
// While captured the cursor is hidden and the game owns the mouse and keyboard.
bool captured();

// Key bindings, Quake/Source style: "bind w +forward" runs "+forward" on press
// and "-forward" on release; other commands run on press. Key names: letters,
// digits, "space", "ctrl", "shift", "escape", "f1".., "mouse1".."mouse5",
// "mwheelup", "mwheeldown".
void bind(const char* key, const char* command);
// Lines "unbindall" + "bind ..." for config.cfg.
std::string bindingsConfig();
// The key bound to `command` ("" if none), for menus.
std::string keyFor(const char* command);
// The next key pressed is reported here instead of running its binding (options menu).
void captureNextKey(void (*callback)(const char* keyName));

// Engine side.
void setCaptured(bool on);
// Never grab the mouse (test mode).
void disableCapture();
void handleEvent(const SDL_Event& ev);
// Releases every held "+" action (when a menu opens mid-press).
void releaseAll();

} // namespace keel::input
