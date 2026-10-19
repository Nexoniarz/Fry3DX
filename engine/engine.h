#pragma once
#include <utility>
#include <string>
#include <vector>

namespace keel {
// Where this game keeps its config and saves (SDL_GetPrefPath), "" before it starts.
const std::string& prefPath();


class Renderer;

// What a game plugs into the engine. Like Source, the game owns its server and
// client code; the engine owns the window, renderer, console and main loop.
class Game {
public:
    virtual ~Game() = default;
    virtual const char* name() const = 0;  // folder for config/saves, e.g. "fry3dx"
    virtual const char* title() const = 0; // window title

    virtual void init() {}
    virtual void shutdown() {}
    // Every frame, before rendering. dt in seconds.
    virtual void frame(double dt) = 0;
    // Queue draws and UI. Not called on a dedicated server.
    virtual void render(Renderer&) {}
    // True while the game shows a menu: the mouse is released and binds don't run.
    virtual bool wantsCursor() const { return false; }
    // Dear ImGui docking (windows that dock into each other and the screen): tools like this one.
    virtual bool dockingUi() const { return false; }
    // Escape was pressed (the console has its own key).
    virtual void onEscape() {}
    // The window's close button (or the OS asking it to close): false keeps it open (to ask
    // about saving first; the game quits later itself). The quit command always quits.
    virtual bool allowQuit() { return true; }
    // Hot reload: called twice a second (hotreload 1) after the engine reloaded what changed
    // (`changed`: textures, models, sounds); the game reloads its own (scripts, the map).
    virtual void hotReload(const std::vector<std::string>& changed) { (void)changed; }
};

// Runs until quit. Command line works like Quake/Source:
//   fry3dx file.fry +set r_vsync 0
int run(Game& game, int argc, char** argv);

bool isDedicated();
// Started with -test (automated runs: no input, no audio, no splash).
bool isTestMode();
// Fullscreen resolutions of the window's display, largest first (empty when dedicated).
std::vector<std::pair<int, int>> displayResolutions();
// The graphics API in use ("Vulkan 1.3", "OpenGL 4.6").
const char* rendererName();
// This build: its git commit and date ("1a2b3c4, 2026-10-11 02:14").
const char* buildVersion();
// The game's mods folder (<exe dir>/mods/<game>/).
const std::string& modsDirectory();
// Takes the mods down and mounts the ones fs_mods names now (scripts and new files apply from
// the next map; textures/models already loaded keep their old look until a restart).
void applyMods();
// Starts the game again with the same command line (after the config is saved).
void restart();

} // namespace keel
