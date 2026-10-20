#pragma once
// Scripts: Lua that builds and changes the scene (the Script panel, and scripts/*.lua). They get
// the `fry` library (shapes, objects, combining, the look) and Lua's own string, table, math
// and utf8; no io, os or loading modules: a script changes the scene, not the computer.
// The whole API is listed in scriptHelp().
#include "scene.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

// Plug-ins: a script in the plug-ins folder, run once when Fry3DX starts and kept loaded. What
// it adds stays until it's turned off or reloaded: commands in the menus (fry.command), materials
// (fry.material, as any script can).
struct PluginCommand {
    std::string name, help;
    std::string menu;                                      // "add", "object", or "script" (Script > Plug-ins)
    std::vector<std::pair<std::string, double>> settings;  // asked for first: (name, starting value)
    int ref = 0;                                           // (its run function, in the plug-in's Lua)
};

class Plugin {
public:
    explicit Plugin(std::string file); // "stairs.lua": shows in errors, and as its name until fry.plugin{name=} says
    ~Plugin();
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;
    // Runs the plug-in's file (it registers what it adds). False, with the error, if it stopped.
    bool load(ScriptHost& host, const std::string& code, std::string* error);
    // One of its commands, with the settings asked for (in order). Like a script: one run.
    ScriptResult run(ScriptHost& host, size_t command, const std::vector<double>& settings);

    std::string file, name, about;
    std::vector<PluginCommand> commands;

private:
    struct State;
    std::unique_ptr<State> s_;
};

// Every fry.* function and object property, one per line, for the Script panel's help.
const char* scriptHelp();

} // namespace fry
