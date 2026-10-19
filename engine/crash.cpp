#include "crash.h"

#include <core/log.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#if !defined(__ANDROID__) // (bionic: backtrace() from Android 13 only)
#include <execinfo.h>
#endif
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace keel::crash {
namespace {
std::string g_report, g_exe;
std::vector<std::string> g_args;      // reporter's command line (exe -crashreport file -- args)
std::vector<char*> g_argv;            // ...as pointers, made before anything goes wrong
std::atomic<double> g_beat{0.0};
std::atomic<int> g_busy{0};
std::atomic<bool> g_fired{false};
#ifndef _WIN32
pthread_t g_main;
#endif

double seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// The report: a headline, then the backtrace and the end of the log.
void writeReport(const char* what) {
#ifdef _WIN32
    FILE* f = std::fopen(g_report.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%s\n\n", what);
    const auto& h = log::history();
    std::fprintf(f, "--- last log lines ---\n");
    for (size_t i = h.size() > 80 ? h.size() - 80 : 0; i < h.size(); ++i) std::fprintf(f, "%s\n", h[i].text.c_str());
    std::fclose(f);
#else
    const int fd = open(g_report.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    auto put = [&](const char* s) { (void)!::write(fd, s, std::strlen(s)); };
    put(what);
    put("\n\n--- backtrace ---\n");
#if defined(__ANDROID__)
    put("(none on Android: see logcat)\n");
#else
    void* frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), fd);
#endif
    put("\n--- last log lines ---\n");
    const auto& h = log::history(); // best effort: we're crashing anyway
    for (size_t i = h.size() > 80 ? h.size() - 80 : 0; i < h.size(); ++i) put(h[i].text.c_str()), put("\n");
    close(fd);
#endif
}

void launchReporter() {
#ifdef _WIN32
    _spawnv(_P_NOWAIT, g_exe.c_str(), g_argv.data());
#elif defined(__ANDROID__)
    // (an app is a library in the Java runtime: no second process to report with; the report
    // file and logcat have it)
#else
    if (fork() == 0) {
        setsid(); // on its own: whatever waited on the app (a terminal, a pipe) isn't kept waiting
        if (const int null = open("/dev/null", O_RDWR); null >= 0) dup2(null, 0), dup2(null, 1), dup2(null, 2);
        execv(g_exe.c_str(), g_argv.data());
        _exit(127);
    }
#endif
}

[[noreturn]] void fail(const char* what) {
    if (!g_fired.exchange(true)) {
        writeReport(what);
        launchReporter();
    }
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 3);
#endif
    _exit(3);
}

#ifdef _WIN32
LONG WINAPI onException(EXCEPTION_POINTERS* e) {
    char what[128];
    std::snprintf(what, sizeof what, "Fry3DX crashed (exception 0x%08lX).", e->ExceptionRecord->ExceptionCode);
    fail(what);
}
#else
void onSignal(int sig) {
    const char* what = sig == SIGSEGV ? "Fry3DX crashed: it touched memory it shouldn't have (SIGSEGV)."
                     : sig == SIGBUS  ? "Fry3DX crashed: a bad memory access (SIGBUS)."
                     : sig == SIGFPE  ? "Fry3DX crashed: a maths error, like dividing by zero (SIGFPE)."
                     : sig == SIGILL  ? "Fry3DX crashed: an illegal instruction (SIGILL)."
                     : sig == SIGABRT ? "Fry3DX crashed: it gave up on an error it couldn't handle (abort)."
                     : sig == SIGUSR2 ? "Fry3DX froze: no frame for a long time (the watchdog stopped it). Backtrace of the stuck thread:"
                                      : "Fry3DX crashed.";
    fail(what);
}
#endif

bool debuggerAttached() {
#ifdef _WIN32
    return IsDebuggerPresent();
#else
    std::ifstream f("/proc/self/status");
    for (std::string line; std::getline(f, line);)
        if (line.rfind("TracerPid:", 0) == 0) return std::atoi(line.c_str() + 10) != 0;
    return false;
#endif
}

void watchdog() {
    constexpr double kTimeout = 45.0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_busy > 0 || g_beat == 0.0) {
            g_beat = seconds();
            continue;
        }
        if (seconds() - g_beat < kTimeout || debuggerAttached()) continue;
#ifndef _WIN32
        pthread_kill(g_main, SIGUSR2); // its handler reports from the stuck thread (its backtrace)
        std::this_thread::sleep_for(std::chrono::seconds(3));
#endif
        fail("Fry3DX froze: no frame for a long time (the watchdog stopped it).");
    }
}
} // namespace

void install(const std::string& prefPath, const std::vector<std::string>& args, bool dog) {
    g_report = prefPath + "crash.txt";
#ifdef _WIN32
    char exe[1024] = {};
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    g_exe = exe;
#else
    g_exe = std::filesystem::read_symlink("/proc/self/exe").string();
    g_main = pthread_self();
#endif
    g_args = {g_exe, "-crashreport", g_report, "--"};
    for (size_t i = 1; i < args.size(); ++i) g_args.push_back(args[i]);
#ifdef _WIN32
    for (std::string& x : g_args)
        if (x.find(' ') != std::string::npos) x = "\"" + x + "\"";
#endif
    for (std::string& a : g_args) g_argv.push_back(a.data());
    g_argv.push_back(nullptr);
#ifdef _WIN32
    SetUnhandledExceptionFilter(onException);
#else
    // On a stack of its own, so a stack overflow can still be reported.
    static std::vector<char> altStack(1 << 16);
    stack_t ss{};
    ss.ss_sp = altStack.data(), ss.ss_size = altStack.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sa.sa_flags = SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGUSR2}) sigaction(sig, &sa, nullptr);
#endif
    std::set_terminate([] {
        std::string what = "Fry3DX crashed: an error nobody caught";
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
        } catch (const std::exception& e) {
            what += std::string(" (") + e.what() + ")";
        } catch (...) {
        }
        what += ".";
        fail(what.c_str());
    });
    if (dog) std::thread(watchdog).detach();
}

void heartbeat() { g_beat = seconds(); }
Busy::Busy(const char*) { ++g_busy; }
Busy::~Busy() {
    heartbeat();
    --g_busy;
}

// ── The crash screen: SDL's 2D renderer and its built-in 8×8 font, scaled up ──
int runReporter(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "-crashreport") != 0) return -1;
    std::string report;
    {
        std::ifstream f(argv[2]);
        std::stringstream ss;
        ss << f.rdbuf();
        report = ss.str();
    }
    std::vector<std::string> restart;
    for (int i = 3; i < argc; ++i)
        if (std::strcmp(argv[i], "--") != 0) restart.emplace_back(argv[i]);
    std::vector<std::string> lines;
    {
        std::stringstream ss(report);
        for (std::string l; std::getline(ss, l);) lines.push_back(l);
    }
    const std::string headline = lines.empty() ? "Fry3DX crashed." : lines[0];

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Fry3DX - something got burnt", 1000, 620, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)) return 1;
    SDL_SetRenderVSync(ren, 1);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    auto text = [&](float x, float y, float scale, const std::string& s, SDL_Color c) {
        SDL_SetRenderScale(ren, scale, scale);
        SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
        SDL_RenderDebugText(ren, x / scale, y / scale, s.c_str());
        SDL_SetRenderScale(ren, 1, 1);
    };
    // Shapes for the picture: an ellipse (filled), a box.
    auto ellipse = [&](float cx, float cy, float rx, float ry, SDL_FColor c) {
        std::vector<SDL_Vertex> v;
        const int n = 48;
        for (int i = 0; i < n; ++i) {
            const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
            v.push_back({{cx, cy}, c, {}});
            v.push_back({{cx + std::cos(a0) * rx, cy + std::sin(a0) * ry}, c, {}});
            v.push_back({{cx + std::cos(a1) * rx, cy + std::sin(a1) * ry}, c, {}});
        }
        SDL_RenderGeometry(ren, nullptr, v.data(), int(v.size()), nullptr, 0);
    };
    auto quad = [&](SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_FColor col) {
        const SDL_Vertex v[6] = {{a, col, {}}, {b, col, {}}, {c, col, {}}, {a, col, {}}, {c, col, {}}, {d, col, {}}};
        SDL_RenderGeometry(ren, nullptr, v, 6, nullptr, 0);
    };
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return SDL_FColor{r / 255.0f, g / 255.0f, b / 255.0f, a}; };
    // Word wrap for the 8-pixel font.
    auto wrap = [](const std::string& s, size_t cols) {
        std::vector<std::string> out;
        std::string line;
        std::stringstream ss(s);
        for (std::string w; ss >> w;) {
            if (!line.empty() && line.size() + 1 + w.size() > cols) out.push_back(line), line.clear();
            line += (line.empty() ? "" : " ") + w;
        }
        if (!line.empty()) out.push_back(line);
        return out;
    };
    const SDL_Color ink{236, 232, 226, 255}, dim{150, 146, 140, 255}, orange{255, 140, 60, 255};
    int scroll = 0, result = 0;
    std::string toast;
    double toastUntil = 0;
    for (bool running = true; running;) {
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(ren, &w, &h);
        const float s = std::min(h / 620.0f, w / 1000.0f);
        const double t = SDL_GetTicks() / 1000.0;
        struct Button { const char* label; float x; } buttons[] = {{"OPEN IT AGAIN", 60 * s}, {"COPY REPORT", 290 * s}, {"REPORT ON GITHUB", 520 * s}, {"CLOSE", 750 * s}};
        const float by = h - 90 * s, bw = 210 * s, bh = 50 * s;
        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        float px = 1, py = 1; // window → pixels
        {
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            px = float(w) / ww, py = float(h) / wh;
        }
        mx *= px, my *= py;
        int clicked = -1;
        for (SDL_Event e; SDL_PollEvent(&e);) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_Q) running = false;
                if (e.key.key == SDLK_R) clicked = 0;
                if (e.key.key == SDLK_C) clicked = 1;
            }
            if (e.type == SDL_EVENT_MOUSE_WHEEL) scroll = std::max(0, scroll - int(e.wheel.y * 3));
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
                for (int i = 0; i < 4; ++i)
                    if (mx >= buttons[i].x && mx < buttons[i].x + bw && my >= by && my < by + bh) clicked = i;
        }
        if (clicked == 0) { // opened again as it was started (it offers the autosave back)
            std::vector<char*> a;
            std::string exe = argv[0];
            a.push_back(exe.data());
            for (std::string& r : restart) a.push_back(r.data());
            a.push_back(nullptr);
            SDL_DestroyRenderer(ren);
            SDL_DestroyWindow(win);
            SDL_Quit();
#ifdef _WIN32
            _spawnv(_P_NOWAIT, exe.c_str(), a.data());
            return 0;
#else
            execv(std::filesystem::read_symlink("/proc/self/exe").c_str(), a.data());
            return 1;
#endif
        }
        if (clicked == 1 || clicked == 2) {
            SDL_SetClipboardText(report.c_str());
            toast = clicked == 1 ? "Copied: paste it into a bug report." : "Copied: paste it into the new issue.";
            toastUntil = t + 3.0;
            if (clicked == 2) SDL_OpenURL("https://github.com/Nexoniarz/Fry3DX/issues/new");
        }
        if (clicked == 3) running = false;

        SDL_SetRenderDrawColor(ren, 22, 22, 25, 255);
        SDL_RenderClear(ren);
        // The picture, top right: a pan, a cube on it burnt black, smoke going up.
        {
            const float cx = w - 230 * s, cy = 170 * s;
            quad({cx + 80 * s, cy - 6 * s}, {cx + 200 * s, cy - 30 * s}, {cx + 204 * s, cy - 16 * s}, {cx + 84 * s, cy + 8 * s}, rgb(58, 58, 64)); // handle
            ellipse(cx, cy + 10 * s, 105 * s, 34 * s, rgb(34, 34, 38));
            ellipse(cx, cy, 105 * s, 34 * s, rgb(70, 70, 78));
            ellipse(cx, cy + 2 * s, 88 * s, 26 * s, rgb(28, 28, 32));
            for (int i = 0; i < 3; ++i) { // flames under it
                const float f = float(std::sin(t * 7 + i * 2.1) * 0.5 + 0.5);
                const float fx = cx + (i - 1) * 50 * s;
                ellipse(fx, cy + 46 * s, (14 + 4 * f) * s, (12 + 6 * f) * s, rgb(255, 74, 28, 0.85f));
                ellipse(fx, cy + 48 * s, 7 * s, (6 + 4 * f) * s, rgb(255, 194, 74, 0.9f));
            }
            const float k = 26 * s; // the cube (seen from a corner), charred
            quad({cx, cy - 2 * k}, {cx + k, cy - 1.5f * k}, {cx, cy - k}, {cx - k, cy - 1.5f * k}, rgb(64, 52, 46));
            quad({cx - k, cy - 1.5f * k}, {cx, cy - k}, {cx, cy}, {cx - k, cy - 0.5f * k}, rgb(40, 32, 28));
            quad({cx, cy - k}, {cx + k, cy - 1.5f * k}, {cx + k, cy - 0.5f * k}, {cx, cy}, rgb(26, 21, 19));
            for (int i = 0; i < 6; ++i) { // smoke, rising and fading
                const double life = std::fmod(t * 0.45 + i / 6.0, 1.0);
                const float sx = cx + float(std::sin(life * 6 + i) * 18 * s), sy = cy - 2.2f * k - float(life * 80 * s);
                ellipse(sx, sy, float(10 + life * 22) * s, float(8 + life * 16) * s, rgb(150, 150, 158, float(0.35 * (1 - life))));
            }
        }
        text(60 * s, 60 * s, 4 * s, "Fry3DX", orange);
        text(60 * s, 110 * s, 2.5f * s, "Something got burnt.", ink);
        float y = 160 * s;
        for (const std::string& l : wrap(headline, size_t((w - 400 * s) / (16 * s))))
            text(60 * s, y, 2 * s, l, ink), y += 24 * s;
        y += 6 * s;
        for (const std::string& l : wrap("Your work isn't lost: Open it again and Fry3DX offers back the last autosave.", size_t((w - 400 * s) / (12 * s))))
            text(60 * s, y, 1.5f * s, l, {130, 210, 120, 255}), y += 18 * s;
        text(60 * s, y + 4 * s, 1.25f * s, "Report saved to " + std::string(argv[2]), dim);
        y += 30 * s;
        // The report's tail, in a panel, scrollable.
        const SDL_FRect panel{50 * s, y - 8 * s, w - 100 * s, by - 20 * s - (y - 8 * s)};
        SDL_SetRenderDrawColor(ren, 15, 15, 17, 255);
        SDL_RenderFillRect(ren, &panel);
        SDL_SetRenderDrawColor(ren, 255, 120, 40, 255);
        const SDL_FRect edge{panel.x, panel.y, 3 * s, panel.h};
        SDL_RenderFillRect(ren, &edge);
        const int visible = std::max(1, int((panel.h - 10 * s) / (13 * s)));
        const int start = std::clamp(int(lines.size()) - visible - scroll, 1, std::max(1, int(lines.size()) - 1));
        for (int i = start; i < int(lines.size()) && y < by - 30 * s; ++i, y += 13 * s)
            text(64 * s, y, 1.25f * s, lines[size_t(i)].substr(0, size_t((w - 140 * s) / (10 * s))), {165, 160, 152, 255});
        for (int i = 0; i < 4; ++i) {
            const bool hover = mx >= buttons[i].x && mx < buttons[i].x + bw && my >= by && my < by + bh;
            SDL_FRect r{buttons[i].x, by, bw, bh};
            if (i == 0 || hover) SDL_SetRenderDrawColor(ren, hover ? 255 : 220, hover ? 140 : 100, hover ? 60 : 30, 255);
            else SDL_SetRenderDrawColor(ren, 46, 46, 52, 255);
            SDL_RenderFillRect(ren, &r);
            const float n = float(std::strlen(buttons[i].label)), ts = std::min(1.75f * s, (bw - 24 * s) / (n * 8)); // (fits its button)
            text(buttons[i].x + (bw - n * 8 * ts) / 2, by + bh / 2 - 4 * ts, ts, buttons[i].label, ink);
        }
        text(60 * s, by + bh + 12 * s, 1.25f * s, "R open again   C copy   Esc close", dim);
        if (t < toastUntil) text(60 * s, by - 16 * s, 1.5f * s, toast, orange);
        if (const char* shot = std::getenv("KEEL_CRASHSHOT")) { // for checking the screen: one frame to a file, then quit
            if (SDL_Surface* px = SDL_RenderReadPixels(ren, nullptr)) SDL_SaveBMP(px, shot), SDL_DestroySurface(px);
            running = false;
        }
        SDL_RenderPresent(ren);
    }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return result;
}

} // namespace keel::crash
