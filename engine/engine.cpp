#include "engine.h"
#include "crash.h"
#include "audio/audio.h"
#include "core/console.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/time.h"
#include "fs/vfs.h"
#include "platform/input.h"
#include "platform/touch.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "render/screenshot.h"
#include "ui/ui.h"

#include <keel_version.h>

#include <yapf.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stb_image.h>

#include <algorithm>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <process.h>
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void*, char*, unsigned long);
#else
#include <unistd.h>
#endif

namespace keel {
namespace {
volatile std::sig_atomic_t g_quit = false;
bool g_dedicated = false;
// -test: for automated visual checks while someone uses the machine. The window doesn't
// take focus, ignores mouse and keyboard, plays no sound and never saves the config.
bool g_testMode = false;
std::string g_configPath;

#ifdef NDEBUG
Cvar r_validation("r_validation", "0", CVAR_ARCHIVE, "Enable Vulkan validation layers (restart needed)");
#else
Cvar r_validation("r_validation", "1", CVAR_ARCHIVE, "Enable Vulkan validation layers (restart needed)");
#endif
Cvar vid_mode("vid_mode", "0", CVAR_ARCHIVE, "0 = window, 1 = borderless fullscreen, 2 = fullscreen");
Cvar vid_width("vid_width", "1600", CVAR_ARCHIVE, "Window / fullscreen width");
Cvar vid_height("vid_height", "900", CVAR_ARCHIVE, "Window / fullscreen height");
Cvar r_renderer("r_renderer", "vulkan", CVAR_ARCHIVE,
               "Graphics API: vulkan, opengl (4.6), or the cut-down fallbacks for old GPUs: opengl40, opengl31 (restart needed)");
Cvar hotreload("hotreload", "1", CVAR_ARCHIVE,
                "Reload textures, models, sounds, scripts and the map when their files change (with tools/watch.py compiling as you save)");
Cvar fps_max_menu("fps_max_menu", "120", CVAR_ARCHIVE, "Frame rate limit in menus and while paused (0 = same as fps_max)");
Cvar fps_max_nofocus("fps_max_nofocus", "30", CVAR_ARCHIVE, "Frame rate limit while another window has focus (10 when minimized; 0 = no limit)");
Cvar fps_max("fps_max", "0", CVAR_ARCHIVE, "Frame rate limit (0 = none; vsync also limits it)");
Window* g_window = nullptr;
Cmd cmd_vid_modes("vid_modes", [](const Args&) {
    if (!g_window) return;
    for (auto [w, h] : g_window->resolutions()) log::info("  {}x{}", w, h);
}, "List the display's fullscreen resolutions");
Cvar r_speeds("r_speeds", "0", CVAR_NONE, "Show what the renderer draws each frame (draws, culled, triangles, shadows)");
Cvar cl_showfps("cl_showfps", "0", CVAR_ARCHIVE, "Show frame rate in the corner");

Cmd cmd_quit("quit", [](const Args&) { g_quit = true; }, "Quit");
Cmd cmd_exit("exit", [](const Args&) { g_quit = true; }, "Quit");
Renderer* g_renderer = nullptr;
std::string g_prefPath;
Cmd cmd_version("version", [](const Args&) {
    log::info("Fry3DX, build {} from {}, {} renderer", KEEL_COMMIT, KEEL_COMMIT_DATE, g_renderer ? g_renderer->apiName() : "no");
}, "Engine version and build date");
Cmd cmd_screenshot("screenshot", [](const Args& a) {
    if (!g_renderer) return;
    std::string name = a.size() > 1 ? a[1] : "";
    if (name.empty()) {
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&now));
        name = std::string("shot_") + stamp + ".png";
    }
    if (!name.ends_with(".png")) name += ".png";
    g_renderer->requestScreenshot(std::filesystem::path(name).is_absolute() ? name : g_prefPath + "screenshots/" + name);
}, "Save the screen as a PNG: screenshot [name or absolute path]");
Cmd cmd_exec("exec", [](const Args& a) {
    if (a.size() < 2) return log::info("usage: exec <file>");
    // Game files first (cfg/autoexec.cfg), then a plain path on disk.
    std::optional<std::string> text = fs::readText(a[1]);
    if (!text) text = fs::readText("cfg/" + a[1]);
    if (!text) {
        std::ifstream in(a[1]);
        if (!in) return log::warn("Couldn't exec {}", a[1]);
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    execute(*text);
}, "Run commands from a file");

void saveConfig() {
    if (g_testMode) return; // (automated runs mustn't change the player's settings)
    std::ofstream out(g_configPath);
    out << "// Written on exit - edit autoexec.cfg instead.\n";
    if (!g_dedicated) out << input::bindingsConfig();
    for (Cvar* c : Cvar::all())
        if ((c->flags() & CVAR_ARCHIVE) && c->str() != c->defaultValue())
            out << c->name() << " \"" << c->str() << "\"\n";
}

// Typing, for automated tests of text boxes: real key and text events through the event loop.
// sdl_type "hello" or keys in braces: {enter} {esc} {y}.
int g_typedEvents = 0; // sdl_type's, let through in -test

Cmd cmd_sdl_type("sdl_type", [](const Args& a) {
    static std::vector<std::string> texts; // (SDL keeps a pointer to the text until it's handled)
    std::string s;
    for (size_t i = 1; i < a.size(); ++i) s += (i > 1 ? " " : "") + a[i];
    auto key = [](SDL_Scancode sc, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = sc;
        e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        e.key.down = down;
        e.key.windowID = g_window ? SDL_GetWindowID(g_window->handle()) : 0;
        if (SDL_PushEvent(&e)) ++g_typedEvents;
    };
    { // (tests run unfocused, and text boxes ignore an unfocused window)
        SDL_Event f{};
        f.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        f.window.windowID = g_window ? SDL_GetWindowID(g_window->handle()) : 0;
        if (SDL_PushEvent(&f)) ++g_typedEvents;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{') {
            const size_t end = s.find('}', i);
            const std::string name = s.substr(i + 1, end - i - 1);
            const SDL_Scancode sc = name == "enter" ? SDL_SCANCODE_RETURN : name == "esc" ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(name.c_str());
            key(sc, true), key(sc, false);
            i = end;
            continue;
        }
        const SDL_Scancode sc = SDL_GetScancodeFromKey(SDL_Keycode(std::tolower(static_cast<unsigned char>(s[i]))), nullptr);
        key(sc, true);
        texts.push_back(std::string(1, s[i]));
        SDL_Event t{};
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.text = texts.back().c_str();
        t.text.windowID = g_window ? SDL_GetWindowID(g_window->handle()) : 0;
        if (SDL_PushEvent(&t)) ++g_typedEvents;
        key(sc, false);
    }
}, "(tests) Type text in as key presses: sdl_type \"hi{enter}\"");

Cmd cmd_writeconfig("host_writeconfig", [](const Args&) {
    saveConfig();
    log::info("Wrote {}", g_configPath);
}, "Save settings and key bindings now (they're also saved on exit)");

// "+map foo +name bob" -> {"map foo", "name \"bob\""}
std::vector<std::string> commandLine(int argc, char** argv) {
    std::vector<std::string> out;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.starts_with('+')) out.push_back(arg.substr(1));
        else if (arg == "-dedicated") g_dedicated = true;
        else if (arg == "-test") g_testMode = true;
        else if (arg == "-dev") out.push_back("developer 1");
        else if (!out.empty()) out.back() += " \"" + arg + "\"";
    }
    return out;
}

// Like Source: cvar settings on the command line apply before any commands, so
// "+map test +name bob" joins as bob. Pass 0 = settings, pass 1 = commands.
// Commands run as one line so "+map test +wait 60 +screenshot" chains through wait.
void executeCommandLine(const std::vector<std::string>& lines, int pass) {
    std::string commands;
    for (const std::string& line : lines) {
        const Args args = tokenize(line);
        const bool isSetting = !args.empty() && (Cvar::find(args[0]) || args[0] == "set");
        if (isSetting != (pass == 0)) continue;
        if (pass == 0) execute(line);
        else commands += line + ";";
    }
    if (!commands.empty()) execute(commands);
}

Cvar fs_mods("fs_mods", "", CVAR_ARCHIVE, "Mods to load from the mods folder: \"all\" or \"name1,name2\" (restart needed)");

Cvar r_profile("r_profile", "0", CVAR_ARCHIVE, "Where each frame's time goes: the app, the CPU's rendering work, each GPU pass (ms)");
float g_gameMs = 0, g_drawMs = 0;

// r_profile 1: a bar per part of the frame (like Source's VProf / showbudget).
void drawProfile() {
    if (!r_profile.b() || !g_renderer) return;
    const Renderer::Timings& t = g_renderer->timings();
    static float smooth[32] = {};
    std::vector<std::pair<std::string, float>> rows = {{"game (logic, network, sound)", g_gameMs}, {"game draws (UI, scene list)", g_drawMs},
                                                       {"render: build draw list", t.build}, {"render: record + submit", t.record},
                                                       {"waiting for the GPU", t.wait}};
    float gpu = 0;
    for (const auto& [name, ms] : t.gpu) rows.push_back({std::string("gpu: ") + name, ms}), gpu += ms;
    rows.push_back({"gpu total", gpu});
    ImGui::SetNextWindowPos(ImVec2(10, ImGui::GetMainViewport()->WorkSize.y * 0.3f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("r_profile", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("%.2f ms  %.0f fps", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < rows.size() && i < 32; ++i) {
        smooth[i] += (rows[i].second - smooth[i]) * 0.1f; // steady numbers
        const float ms = smooth[i];
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = std::min(ms / 16.67f, 1.0f) * 200.0f; // the bar: a 60 fps frame is full width
        const ImU32 col = rows[i].first.starts_with("gpu") ? IM_COL32(90, 170, 255, 200) : IM_COL32(255, 180, 70, 200);
        dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + w, p.y + 14), col);
        ImGui::Dummy(ImVec2(205, 16));
        ImGui::SameLine();
        ImGui::Text("%6.2f  %s", ms, rows[i].first.c_str());
    }
    ImGui::End();
}

void drawFps() {
    drawProfile();
    if (r_speeds.b() && g_renderer) { // Quake's r_speeds: what the renderer did last frame
        const Renderer::DrawStats& d = g_renderer->drawStats();
        const Renderer::LightStats& l = g_renderer->lightStats();
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 10, 60), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("r_speeds", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("%.2f ms  %.0f fps", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
        ImGui::Text("draws %u (+%u translucent), culled %u (%u hidden behind things)", d.drawn, d.translucent, d.culled, d.occluded);
        ImGui::Text("triangles %u", d.triangles);
        ImGui::Text("sun shadow draws %u, light shadow draws %u", d.sunShadow, d.lightShadow);
        ImGui::Text("lights %u (%u shadowed)", l.lights, l.shadowed);
        ImGui::End();
    }
    if (!cl_showfps.b()) return;
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 10, 10), 0, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGui::Begin("fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);
    const float fps = ImGui::GetIO().Framerate;
    ImGui::Text("%.0f fps  %.2f ms", fps, 1000.0f / fps);
    ImGui::End();
}

int runDedicated(Game& game) {
    // No SDL here, so handle Ctrl+C / kill ourselves to shut down cleanly.
    std::signal(SIGINT, [](int) { g_quit = true; });
    std::signal(SIGTERM, [](int) { g_quit = true; });
    double last = now();
    while (!g_quit) {
        crash::heartbeat();
        const double t = now();
        game.frame(t - last);
        executeDeferred();
        last = t;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

int runClient(Game& game) {
    Window window;
    Renderer renderer;
    g_renderer = &renderer;
    if (g_testMode) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        input::disableCapture();
    }
    g_window = &window;
    const int w = std::max(vid_width.i(), 640), h = std::max(vid_height.i(), 480);
    // The API asked for, then the others: the modern pair, then the fallbacks for old GPUs.
    const std::string want = r_renderer.str();
    const RenderApi first = want == "opengl" ? RenderApi::OpenGL : want == "opengl40" ? RenderApi::OpenGL40 : want == "opengl31" ? RenderApi::OpenGL31 : RenderApi::Vulkan;
    std::vector<RenderApi> order{first};
    for (RenderApi a : {RenderApi::Vulkan, RenderApi::OpenGL, RenderApi::OpenGL40, RenderApi::OpenGL31})
        if (std::find(order.begin(), order.end(), a) == order.end() && (first != RenderApi::OpenGL31 || a == RenderApi::OpenGL31)) order.push_back(a);
    bool ok = false;
    for (RenderApi api : order) {
        ok = window.init(game.title(), w, h, api != RenderApi::Vulkan) && renderer.init(window, r_validation.b(), api);
        if (ok) {
            if (api != first)
                ui::popup("Renderer changed", std::string("Your graphics driver couldn't start the ") + (want.empty() ? "vulkan" : want) +
                                                  " renderer, so it's using " + renderer.apiName() + " instead.");
            break;
        }
        log::warn("Renderer {} unavailable, trying the next", int(api));
        renderer.shutdown();
        window.shutdown();
    }
    if (!ok) {
        renderer.shutdown();
        window.shutdown();
        return 1;
    }
    if (game.dockingUi()) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (auto png = fs::read("branding/icon.png")) { // the window/taskbar icon
        int w = 0, h = 0, n = 0;
        if (stbi_uc* px = stbi_load_from_memory(png->data(), int(png->size()), &w, &h, &n, 4)) {
            window.setIcon(px, w, h);
            stbi_image_free(px);
        }
    }
    if (!g_testMode) audio::init();
    ui::init();
    log::info("Press ` (tilde key) to open the console");

    double last = now();
    while (!g_quit) {
        crash::heartbeat();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            const bool typed = g_typedEvents > 0 && (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP || ev.type == SDL_EVENT_TEXT_INPUT ||
                                                     ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
            if (typed) --g_typedEvents;
            if (g_testMode && !typed) { // nobody's playing: only window events (and sdl_type's)
                if (ev.type == SDL_EVENT_QUIT) g_quit = true;
                if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) renderer.onResize();
                continue;
            }
            const bool keyDown = ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat;
            if (keyDown && ev.key.scancode == SDL_SCANCODE_GRAVE) {
                console::toggle();
                continue;
            }
            touch::handleEvent(ev, input::captured());
            // Escape, or a phone's back button.
            if (keyDown && (ev.key.scancode == SDL_SCANCODE_ESCAPE || ev.key.scancode == SDL_SCANCODE_AC_BACK)) {
                if (console::isOpen()) console::toggle();
                else if (ui::popupOpen()) ui::closePopup();
                else game.onEscape();
                continue;
            }
            // Don't let the toggle key's character land in the console input.
            if (ev.type == SDL_EVENT_TEXT_INPUT && ev.text.text[0] == '`') continue;
            input::handleEvent(ev);
            if (!input::captured()) ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT && game.allowQuit()) g_quit = true;
            if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) renderer.onResize();
        }
        const bool focused = SDL_GetWindowFlags(window.handle()) & SDL_WINDOW_INPUT_FOCUS;
        audio::setFocused(focused);
        if (vid_mode.consumeModified() | vid_width.consumeModified() | vid_height.consumeModified())
            window.setMode(vid_mode.i(), std::max(vid_width.i(), 640), std::max(vid_height.i(), 480));
        // Menus need no more than fps_max_menu (like Source's): high frame rates there only heat
        // the GPU and make coil whine (often heard in headphones on the onboard jack).
        int limit = fps_max.i();
        if (game.wantsCursor() && fps_max_menu.i() > 0 && !g_testMode) limit = limit > 0 ? std::min(limit, fps_max_menu.i()) : fps_max_menu.i();
        // In the background (like Source's engine_no_focus_sleep): another window has the
        // keyboard, or it's minimized; no need to draw flat out (and take the GPU from the game
        // you're looking at).
        if (!focused && !g_testMode && fps_max_nofocus.i() > 0) {
            const bool minimized = SDL_GetWindowFlags(window.handle()) & SDL_WINDOW_MINIMIZED;
            const int cap = minimized ? 10 : fps_max_nofocus.i();
            limit = limit > 0 ? std::min(limit, cap) : cap;
        }
        if (limit > 0) { // sleep off the rest of the frame
            static double nextFrame = 0;
            const double frame = 1.0 / std::max(limit, 10);
            nextFrame = std::max(nextFrame + frame, now());
            while (now() < nextFrame - 0.0015) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            while (now() < nextFrame) {}
        }
        const bool capture = focused && !console::isOpen() && !game.wantsCursor() && !ui::popupOpen();
        // Handing the keyboard between the game and the menus: the menus forget what was held.
        // (Keys let go while the game had them never reach the menus: Enter that sent a chat line
        // stayed "held" and repeated, closing the chat the moment it opened again.)
        if (capture != input::captured()) ImGui::GetIO().ClearInputKeys();
        input::setCaptured(capture);
        if (touch::takeEscape()) { // the touch controls' pause button
            if (console::isOpen()) console::toggle();
            else if (ui::popupOpen()) ui::closePopup();
            else game.onEscape();
        }

        const double t = now();
        game.frame(t - last);
        g_gameMs = float((now() - t) * 1000.0);
        last = t;

        // Hot reload (Source 2's resource system, in spirit): what changed on disk comes back in.
        static double nextCheck = 0.0;
        if (hotreload.i() >= (g_testMode ? 2 : 1) && now() >= nextCheck) { // (tests: hotreload 2)
            nextCheck = now() + 0.5;
            std::vector<std::string> changed = renderer.reloadChanged();
            for (std::string& s : audio::reloadChanged()) changed.push_back(std::move(s));
            for (const std::string& s : changed) log::info("Reloaded {}", s);
            game.hotReload(changed);
        }
        renderer.beginUi();
        int winW = 0, winH = 0;
        SDL_GetWindowSizeInPixels(window.handle(), &winW, &winH);
        ui::beginFrame(float(winH));
        const double drawStart = now();
        game.render(renderer);
        g_drawMs = float((now() - drawStart) * 1000.0);
        touch::draw(input::captured());
        ui::drawPopups();
        console::draw();
        drawFps();
        renderer.drawFrame();
        executeDeferred();
    }
    input::setCaptured(false);
    audio::shutdown();
    waitForScreenshots();
    renderer.shutdown();
    window.shutdown();
    return 0;
}
} // namespace

bool isDedicated() { return g_dedicated; }
bool isTestMode() { return g_testMode; }
const char* rendererName() { return g_renderer ? g_renderer->apiName() : "none"; }
const char* buildVersion() { return KEEL_COMMIT ", " KEEL_COMMIT_DATE; }

// Replaces this process with a fresh one (Windows: starts it, then this one exits).
void relaunch(const std::vector<std::string>& args) {
    std::vector<std::string> a = args;
#ifdef _WIN32
    char exe[1024] = {};
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    for (std::string& x : a)
        if (x.find(' ') != std::string::npos) x = "\"" + x + "\"";
#else
    const std::string exe = std::filesystem::read_symlink("/proc/self/exe").string();
#endif
    std::vector<char*> argv;
    for (std::string& x : a) argv.push_back(x.data());
    argv.push_back(nullptr);
    log::info("Restarting");
#ifdef _WIN32
    _spawnv(_P_NOWAIT, exe, argv.data());
#else
    execv(exe.c_str(), argv.data());
    log::error("Restart failed");
#endif
}

std::string g_modsDir;
std::vector<std::string> g_argv;
bool g_restart = false;
const std::string& modsDirectory() { return g_modsDir; }
void applyMods() {
    fs::unmountMods();
    fs::mountMods(g_modsDir, fs_mods.str());
}
void restart() {
    g_restart = true;
    execute("quit");
}
std::vector<std::pair<int, int>> displayResolutions() { return g_window ? g_window->resolutions() : std::vector<std::pair<int, int>>{}; }

// Tries the crash screen: "crash" crashes, "crash freeze" hangs (the watchdog steps in), "crash throw" throws.
Cmd cmd_crash("crash", [](const Args& a) {
    const std::string how = a.size() > 1 ? a[1] : "";
    if (how == "freeze") for (volatile bool stuck = true; stuck;) {}
    if (how == "throw") throw std::runtime_error("crash throw (from the console)");
    volatile int* p = nullptr;
    *p = 1;
}, "crash [freeze|throw]: crash on purpose, to see the crash screen");

#if defined(__ANDROID__)
// On a phone the content ships inside the APK (assets/, listed in content.txt with a version on
// its first line). It's copied out to the app's own storage the first time, and after an
// update, where it's read like any game folder. Returns that folder.
std::string unpackAndroidContent() {
    const char* internal = SDL_GetAndroidInternalStoragePath();
    const std::string dir = std::string(internal ? internal : ".") + "/";
    size_t size = 0;
    void* list = SDL_LoadFile("content.txt", &size); // (relative paths read from the APK's assets)
    if (!list) {
        log::error("No content list in the APK");
        return dir;
    }
    std::istringstream in(std::string(static_cast<const char*>(list), size));
    SDL_free(list);
    std::string version, have;
    std::getline(in, version);
    if (std::ifstream v(dir + "content.version"); v) std::getline(v, have);
    if (have == version) return dir;
    log::info("Unpacking the content ({})", version);
    for (std::string rel; std::getline(in, rel);) {
        if (rel.empty()) continue;
        size_t n = 0;
        void* data = SDL_LoadFile(rel.c_str(), &n);
        if (!data) {
            log::warn("Not in the APK: {}", rel);
            continue;
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir + rel).parent_path(), ec);
        std::ofstream(dir + rel, std::ios::binary).write(static_cast<const char*>(data), std::streamsize(n));
        SDL_free(data);
    }
    std::ofstream(dir + "content.version") << version << "\n";
    return dir;
}
#endif

int run(Game& game, int argc, char** argv) {
    if (const int r = crash::runReporter(argc, argv); r >= 0) return r; // the crash screen
    log::info("{}", game.title());

    const std::vector<std::string> startup = commandLine(argc, argv); // also detects -dedicated
    char* pref = SDL_GetPrefPath("Nexoniarz", game.name());
    g_prefPath = pref ? pref : "";
    g_configPath = g_prefPath + (g_dedicated ? "server.cfg" : "config.cfg");
    if (!g_testMode) console::setHistoryFile(g_prefPath + "console_history.txt");
    cmd_exec.complete([](std::string_view p, size_t) {
        std::vector<std::string> v;
        for (const std::string& f : fs::list("cfg", ".cfg")) v.push_back(f.substr(4)); // "cfg/x.cfg" → "x.cfg"
        return matching(v, p);
    });
    SDL_free(pref);
    {
        std::vector<std::string> args(argv, argv + argc);
        bool dog = !g_testMode;
        for (const std::string& a : args) dog = a == "-watchdog" ? true : a == "-nowatchdog" ? false : dog;
        crash::install(g_prefPath, args, dog);
    }

    // Content: the engine's own (<exe dir>/engine/), then the game's (<exe dir>/<game>/) over it,
    // each with its packs — like Source's hl2/ under a mod.
#if defined(__ANDROID__)
    const std::string exeDir = unpackAndroidContent();
    if (const char* cache = SDL_GetAndroidCachePath()) setenv("TMPDIR", cache, 1); // temp files go there
#else
    const char* base = SDL_GetBasePath();
    const std::string exeDir = base ? base : "./";
#endif
    if (!fs::mountGameDir(exeDir + "engine")) log::warn("Engine folder {}engine not found; fonts and UI won't load", exeDir);
    const std::string gameDir = exeDir + game.name();
    if (!fs::mountGameDir(gameDir)) log::warn("Game folder {} not found; no content will load", gameDir);

    game.init();
    // Order matters: saved config first, then the user's autoexec, then the command line.
    // Like Source's config_default.cfg: default binds/settings, then the player's own.
    if (!g_dedicated && fs::exists("cfg/default.cfg")) execute("exec default.cfg");
    if (std::filesystem::exists(g_configPath)) execute("exec \"" + g_configPath + "\"");
    if (fs::exists("cfg/autoexec.cfg")) execute("exec autoexec.cfg");
    executeCommandLine(startup, 0);
    g_modsDir = exeDir + "mods/" + game.name();
    for (int i = 0; i < argc; ++i) g_argv.emplace_back(argv[i]);
    fs::mountMods(g_modsDir, fs_mods.str());
    executeCommandLine(startup, 1);

    const int result = g_dedicated ? runDedicated(game) : runClient(game);
    game.shutdown();
    if (!g_testMode) saveConfig();
    if (g_restart) relaunch(g_argv);
    return result;
}

const std::string& prefPath() { return g_prefPath; }

} // namespace keel
