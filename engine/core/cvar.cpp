#include "core/cvar.h"
#include "core/log.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace keel {
namespace {
// Function-local statics: safe to use from other files' static initializers.
std::vector<Cvar*>& cvars() { static std::vector<Cvar*> v; return v; }
std::vector<Cmd*>& cmds() { static std::vector<Cmd*> v; return v; }
std::vector<std::pair<std::string, std::string>>& aliasList() { static std::vector<std::pair<std::string, std::string>> v; return v; }
int g_aliasDepth = 0; // an alias that runs itself stops here

template <class T> T* findByName(std::vector<T*>& list, std::string_view name) {
    auto it = std::find_if(list.begin(), list.end(), [&](T* x) { return name == x->name(); });
    return it == list.end() ? nullptr : *it;
}
} // namespace

Cvar::Cvar(const char* name, const char* default_value, unsigned flags, const char* help)
    : name_(name), help_(help), default_(default_value), flags_(flags) {
    set(default_value);
    cvars().push_back(this);
}

Cvar::~Cvar() { std::erase(cvars(), this); }

void Cvar::set(std::string_view value) {
    value_ = value;
// (The NDK's library, and Apple's before macOS 26, have no float from_chars. strtof reads the
// "C" locale's way: the program never sets another.)
#if defined(__ANDROID__) || defined(__APPLE__)
    const std::string text(value);
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    float_ = end != text.c_str() ? parsed : 0.0f;
#else
    float parsed = 0.0f;
    auto [_, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    float_ = ec == std::errc() ? parsed : 0.0f;
#endif
    modified_ = true;
}

bool Cvar::consumeModified() { return std::exchange(modified_, false); }
Cvar* Cvar::find(std::string_view name) { return findByName(cvars(), name); }
const std::vector<Cvar*>& Cvar::all() { return cvars(); }

Cmd::Cmd(const char* name, Fn fn, const char* help) : name_(name), help_(help), fn_(std::move(fn)) {
    cmds().push_back(this);
}

Cmd::~Cmd() { std::erase(cmds(), this); }

Cmd* Cmd::find(std::string_view name) { return findByName(cmds(), name); }
const std::vector<Cmd*>& Cmd::all() { return cmds(); }

Args tokenize(std::string_view line) {
    Args out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] <= ' ') ++i;
        if (i >= line.size()) break;
        if (line[i] == '"') {
            size_t end = line.find('"', ++i);
            if (end == std::string_view::npos) end = line.size();
            out.emplace_back(line.substr(i, end - i));
            i = end + 1;
        } else {
            size_t start = i;
            while (i < line.size() && line[i] > ' ') ++i;
            out.emplace_back(line.substr(start, i - start));
        }
    }
    return out;
}

namespace {
struct Deferred {
    int frames;
    std::string text;
};
std::vector<Deferred> g_deferred;
} // namespace

void executeDeferred() {
    std::vector<Deferred> due;
    for (auto it = g_deferred.begin(); it != g_deferred.end();)
        if (--it->frames <= 0) {
            due.push_back(std::move(*it));
            it = g_deferred.erase(it);
        } else {
            ++it;
        }
    for (const Deferred& d : due) execute(d.text);
}

void execute(std::string_view text) {
    // Split on ';' and newlines outside quotes.
    bool quoted = false;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] == '"') quoted = !quoted;
        if (i < text.size() && (quoted || (text[i] != ';' && text[i] != '\n'))) continue;

        Args args = tokenize(text.substr(start, i - start));
        start = i + 1;
        if (args.empty() || args[0].starts_with("//")) continue;
        if (args[0] == "wait") {
            const int frames = args.size() > 1 ? std::max(std::atoi(args[1].c_str()), 1) : 1;
            if (i < text.size()) g_deferred.push_back({frames, std::string(text.substr(i + 1))});
            return;
        }

        auto alias = std::find_if(aliasList().begin(), aliasList().end(), [&](const auto& a) { return a.first == args[0]; });
        if (Cmd* cmd = Cmd::find(args[0])) {
            cmd->run(args);
        } else if (alias != aliasList().end()) {
            if (g_aliasDepth > 32) {
                log::warn("alias {} runs itself too deeply", args[0]);
                continue;
            }
            ++g_aliasDepth;
            execute(alias->second);
            --g_aliasDepth;
        } else if (Cvar* cvar = Cvar::find(args[0])) {
            if (args.size() == 1) {
                log::info("\"{}\" = \"{}\" (default \"{}\"){}", cvar->name(), cvar->str(), cvar->defaultValue(),
                          cvar->flags() & CVAR_CHEAT ? " cheat" : "");
                log::info("  {}", cvar->help());
            } else if ((cvar->flags() & CVAR_CHEAT) && !Cvar::find("sv_cheats")->b()) {
                log::warn("{} is cheat protected", cvar->name());
            } else {
                cvar->set(args[1]);
            }
        } else {
            log::warn("Unknown command \"{}\"", args[0]);
        }
    }
}

static Cvar sv_cheats("sv_cheats", "0", CVAR_REPLICATE, "Allow cheat-protected cvars");

static Cmd cmd_echo("echo", [](const Args& a) {
    std::string s;
    for (size_t i = 1; i < a.size(); ++i) s += (i > 1 ? " " : "") + a[i];
    log::info("{}", s);
}, "Print text to the console");

static Cmd cmd_set("set", [](const Args& a) {
    if (a.size() < 3) return log::info("usage: set <cvar> <value>");
    if (Cvar* c = Cvar::find(a[1])) c->set(a[2]);
    else log::warn("Unknown cvar \"{}\"", a[1]);
}, "Set a cvar");

const std::vector<std::pair<std::string, std::string>>& aliases() { return aliasList(); }

std::vector<std::string> allNames() {
    std::vector<std::string> out;
    for (Cmd* c : Cmd::all()) out.emplace_back(c->name());
    for (Cvar* c : Cvar::all()) out.emplace_back(c->name());
    for (const auto& a : aliasList()) out.push_back(a.first);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> matching(const std::vector<std::string>& names, std::string_view partial) {
    std::vector<std::string> out;
    for (const std::string& n : names)
        if (n.starts_with(partial)) out.push_back(n);
    return out;
}

namespace {
std::string flagText(unsigned f) {
    std::string s;
    if (f & CVAR_ARCHIVE) s += " saved";
    if (f & CVAR_CHEAT) s += " cheat";
    if (f & CVAR_REPLICATE) s += " replicated";
    return s;
}
bool contains(std::string_view hay, std::string_view needle) {
    auto low = [](char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); };
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [&](char a, char b) { return low(a) == low(b); }) != hay.end();
}
Cmd::Completer names() {
    return [](std::string_view p, size_t) { return matching(allNames(), p); };
}
Cmd::Completer cvarNames() {
    return [](std::string_view p, size_t) {
        std::vector<std::string> v;
        for (Cvar* c : Cvar::all()) v.emplace_back(c->name());
        std::sort(v.begin(), v.end());
        return matching(v, p);
    };
}
} // namespace

// toggle <cvar> [values...]: 0 ↔ 1, or the next of the values given (Source).
static Cmd cmd_toggle("toggle", [](const Args& a) {
    if (a.size() < 2) return log::info("usage: toggle <cvar> [value1 value2 ...]");
    Cvar* c = Cvar::find(a[1]);
    if (!c) return log::warn("Unknown cvar \"{}\"", a[1]);
    if (a.size() < 4) return execute(a[1] + (c->b() ? " 0" : " 1"));
    size_t next = 2;
    for (size_t i = 2; i < a.size(); ++i)
        if (a[i] == c->str()) next = i + 1 < a.size() ? i + 1 : 2;
    execute(a[1] + " \"" + a[next] + "\"");
}, "toggle <cvar> [values...]: flip between 0 and 1, or step through the values given");

static Cmd cmd_incrementvar("incrementvar", [](const Args& a) {
    if (a.size() < 5) return log::info("usage: incrementvar <cvar> <min> <max> <step>");
    Cvar* c = Cvar::find(a[1]);
    if (!c) return log::warn("Unknown cvar \"{}\"", a[1]);
    const float lo = float(std::atof(a[2].c_str())), hi = float(std::atof(a[3].c_str())), step = float(std::atof(a[4].c_str()));
    float v = c->f() + step;
    if (v > hi) v = lo;
    if (v < lo) v = hi;
    execute(std::string(a[1]) + " " + std::to_string(v));
}, "incrementvar <cvar> <min> <max> <step>: add step, wrapping round (bind it to keys)");

static Cmd cmd_find("find", [](const Args& a) {
    if (a.size() < 2) return log::info("usage: find <text>  (searches names and descriptions)");
    int n = 0;
    for (const std::string& name : allNames()) {
        Cmd* cmd = Cmd::find(name);
        Cvar* cv = cmd ? nullptr : Cvar::find(name);
        const char* help = cmd ? cmd->help() : cv ? cv->help() : "";
        if (!contains(name, a[1]) && !contains(help, a[1])) continue;
        if (cv) log::info("  {} = \"{}\"  - {}", name, cv->str(), help);
        else log::info("  {}  - {}", name, cmd ? help : "alias");
        ++n;
    }
    log::info("{} found", n);
}, "find <text>: commands and cvars whose name or description mentions it");

static Cmd cmd_help("help", [](const Args& a) {
    if (a.size() < 2) {
        log::info("Type a command or cvar name; a list of matches shows as you type (Tab completes, arrows pick).");
        log::info("  help <name>     what it does        find <text>   search names and descriptions");
        log::info("  cvarlist [x]    all cvars           cmdlist [x]   all commands");
        log::info("  differences     cvars you changed   reset <cvar>  back to its default");
        log::info("  alias, bind, exec, toggle, incrementvar, echo, clear, condump, wait");
        return;
    }
    if (Cmd* c = Cmd::find(a[1])) return log::info("{} (command) - {}", c->name(), c->help());
    if (Cvar* c = Cvar::find(a[1])) {
        log::info("\"{}\" = \"{}\" (default \"{}\"){}", c->name(), c->str(), c->defaultValue(), flagText(c->flags()));
        return log::info("  {}", c->help());
    }
    for (const auto& [name, text] : aliasList())
        if (name == a[1]) return log::info("{} (alias) = \"{}\"", name, text);
    log::warn("Nothing called \"{}\". Try: find {}", a[1], a[1]);
}, "help [name]: how the console works, or what a command/cvar does");

static Cmd cmd_cvarlist("cvarlist", [](const Args& a) {
    std::vector<Cvar*> v(Cvar::all().begin(), Cvar::all().end());
    std::sort(v.begin(), v.end(), [](Cvar* x, Cvar* y) { return std::string_view(x->name()) < y->name(); });
    int n = 0;
    for (Cvar* c : v)
        if (a.size() < 2 || std::string_view(c->name()).starts_with(a[1]))
            log::info("  {:<24} {:<10}{:<9} {}", c->name(), "\"" + c->str() + "\"", flagText(c->flags()), c->help()), ++n;
    log::info("{} cvars", n);
}, "cvarlist [prefix]: every cvar, its value and what it does");

static Cmd cmd_cmdlist("cmdlist", [](const Args& a) {
    std::vector<Cmd*> v(Cmd::all().begin(), Cmd::all().end());
    std::sort(v.begin(), v.end(), [](Cmd* x, Cmd* y) { return std::string_view(x->name()) < y->name(); });
    int n = 0;
    for (Cmd* c : v)
        if (a.size() < 2 || std::string_view(c->name()).starts_with(a[1])) log::info("  {:<20} {}", c->name(), c->help()), ++n;
    log::info("{} commands", n);
}, "cmdlist [prefix]: every command and what it does");

static Cmd cmd_differences("differences", [](const Args&) {
    int n = 0;
    for (Cvar* c : Cvar::all())
        if (c->str() != c->defaultValue()) log::info("  {} = \"{}\" (default \"{}\")", c->name(), c->str(), c->defaultValue()), ++n;
    log::info("{} changed from their defaults", n);
}, "Cvars that aren't at their default value");

static Cmd cmd_reset("reset", [](const Args& a) {
    if (a.size() < 2) return log::info("usage: reset <cvar>");
    if (Cvar* c = Cvar::find(a[1])) c->set(c->defaultValue()), log::info("{} = \"{}\"", c->name(), c->str());
    else log::warn("Unknown cvar \"{}\"", a[1]);
}, "reset <cvar>: back to its default value");

static Cmd cmd_alias("alias", [](const Args& a) {
    auto& list = aliasList();
    if (a.size() < 2) {
        for (const auto& [name, text] : list) log::info("  {} = \"{}\"", name, text);
        return log::info("{} aliases", list.size());
    }
    std::erase_if(list, [&](const auto& x) { return x.first == a[1]; });
    if (a.size() < 3) return;
    std::string text;
    for (size_t i = 2; i < a.size(); ++i) text += (i > 2 ? " " : "") + a[i];
    list.emplace_back(a[1], text);
}, "alias <name> \"commands; more commands\": make a new command (alias <name> alone removes it)");

static Cmd cmd_clear("clear", [](const Args&) { log::clearHistory(); }, "Clear the console");

static Cmd cmd_condump("condump", [](const Args& a) {
    const std::string path = a.size() > 1 ? a[1] : "condump.txt";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return log::warn("Can't write {}", path);
    for (const log::Line& l : log::history()) std::fprintf(f, "%s\n", l.text.c_str());
    std::fclose(f);
    log::info("Console written to {}", path);
}, "condump [file]: save the console's text to a file");

// What the console suggests for their arguments.
[[maybe_unused]] static const bool g_completers = [] {
    cmd_help.complete(names());
    for (Cmd* c : {&cmd_reset, &cmd_toggle, &cmd_incrementvar, &cmd_set}) c->complete(cvarNames());
    return true;
}();

} // namespace keel
