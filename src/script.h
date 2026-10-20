#pragma once
// Scripts: Lua that builds and changes the scene (the Script panel, and scripts/*.lua). They get
// the `fry` library (shapes, objects, combining, the look) and Lua's own string, table, math
// and utf8; no io, os or loading modules: a script changes the scene, not the computer.
// The whole API is listed in scriptHelp().
#include "scene.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>

namespace fry {

// What a script works on: the app's scene and its selection (the last picked: `active`).
struct ScriptHost {
    Scene* scene = nullptr;
    std::set<uint32_t>* selected = nullptr;
    uint32_t* active = nullptr;
    std::function<void(const std::string&)> print; // print(), fry.say
    std::function<void()> frame;                   // fry.frame(): show everything (may be empty)
    double timeLimit = 20.0;                       // seconds before a script's stopped
};

struct ScriptResult {
    bool ok = true;
    std::string error;   // what went wrong, with the line
    int objectsMade = 0; // (counted for the message afterwards)
};

// Runs `code` once (a fresh Lua each time); `name` shows in errors ("hello.lua").
ScriptResult runScript(ScriptHost& host, const std::string& code, const std::string& name = "script");

// Every fry.* function and object property, one per line, for the Script panel's help.
const char* scriptHelp();

} // namespace fry
