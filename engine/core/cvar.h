#pragma once
// Quake-style console variables and commands.
//
//   static keel::Cvar r_vsync("r_vsync", "1", keel::CVAR_ARCHIVE, "Wait for vertical sync");
//   static keel::Cmd  quit("quit", [](const keel::Args&) { ... }, "Quit");
//
// Both register themselves at static-init time and are reachable from the console.
#include <functional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace keel {

enum CvarFlags : unsigned {
    CVAR_NONE      = 0,
    CVAR_ARCHIVE   = 1 << 0, // saved to config.cfg
    CVAR_CHEAT     = 1 << 1, // needs sv_cheats 1
    CVAR_REPLICATE = 1 << 2, // server value is sent to clients
};

class Cvar {
public:
    Cvar(const char* name, const char* default_value, unsigned flags, const char* help);
    ~Cvar(); // unregisters (scripts create and destroy cvars at runtime)
    Cvar(const Cvar&) = delete;
    Cvar& operator=(const Cvar&) = delete;

    const char* name() const { return name_; }
    const char* help() const { return help_; }
    const std::string& defaultValue() const { return default_; }
    unsigned flags() const { return flags_; }

    const std::string& str() const { return value_; }
    float f() const { return float_; }
    int i() const { return int(float_); }
    bool b() const { return float_ != 0.0f; }

    void set(std::string_view value);
    // True once after the value changed; lets systems react (e.g. recreate the swapchain).
    bool consumeModified();

    static Cvar* find(std::string_view name);
    static const std::vector<Cvar*>& all();

private:
    const char* name_;
    const char* help_;
    std::string default_;
    std::string value_;
    float float_ = 0.0f;
    unsigned flags_;
    bool modified_ = true;
};

using Args = std::vector<std::string>; // args[0] is the command name

class Cmd {
public:
    using Fn = std::function<void(const Args&)>;
    // Suggestions for the argument being typed (the console's list): given what's typed of
    // it and which argument it is (1 = the first), the values it could be. Set with complete().
    using Completer = std::function<std::vector<std::string>(std::string_view partial, size_t arg)>;
    Cmd(const char* name, Fn fn, const char* help);
    ~Cmd();
    Cmd(const Cmd&) = delete;
    Cmd& operator=(const Cmd&) = delete;

    const char* name() const { return name_; }
    const char* help() const { return help_; }
    void run(const Args& args) const { fn_(args); }
    Cmd& complete(Completer c) { completer_ = std::move(c); return *this; }
    const Completer& completer() const { return completer_; }

    static Cmd* find(std::string_view name);
    static const std::vector<Cmd*>& all();

private:
    const char* name_;
    const char* help_;
    Fn fn_;
    Completer completer_;
};

// alias name "commands": a new command that runs them (Quake/Source).
const std::vector<std::pair<std::string, std::string>>& aliases();
// Every command, cvar and alias name, sorted (for completion).
std::vector<std::string> allNames();
// The names of things in `names` that start with `partial` (completers use it).
std::vector<std::string> matching(const std::vector<std::string>& names, std::string_view partial);

// Runs a line of console text: "cmd args; name value; name". "wait [frames]" defers
// the rest of the line (Quake style), e.g. "map test; wait 120; screenshot".
void execute(std::string_view text);
// Runs text deferred by "wait" whose frames have passed. Called once per frame.
void executeDeferred();
// Splits a line into arguments, honouring "quoted strings".
Args tokenize(std::string_view line);

} // namespace keel
