#include "crash.h"

#include <core/log.h>
#include <core/sysinfo.h>
#include <fs/vfs.h>
#include <ui/ui.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <stb_image.h>

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

void watchdog() {
    constexpr double kTimeout = 45.0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_busy > 0 || g_beat == 0.0) {
            g_beat = seconds();
            continue;
        }
        if (seconds() - g_beat < kTimeout || sys::debuggerAttached()) continue;
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
    g_exe = sys::executablePath();
#ifndef _WIN32
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
// The crash screen: its own small program (this one, started again), drawn with SDL's own
// renderer rather than Vulkan or OpenGL (what crashed may have been the graphics driver), in the
// app's look: the same fonts, colours and keys.
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
    std::string details; // (the report without its headline)
    for (size_t i = 1; i < lines.size(); ++i) details += lines[i] + "\n";
    while (!details.empty() && details.front() == '\n') details.erase(0, 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Fry3DX - something got burnt", 940, 640, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)) return 1;
    SDL_SetRenderVSync(ren, 1);
    const char* base = SDL_GetBasePath();
    const std::string exeDir = base ? base : "./";
    fs::mountGameDir(exeDir + "engine"); // (the fonts)
    fs::mountGameDir(exeDir + "Fry3DX"); // (the logo)

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    ui::init();
    const float density = SDL_GetWindowDisplayScale(win);
    ImGui::GetStyle().ScaleAllSizes(density);
    ImGui::GetStyle().FontScaleDpi = density;

    SDL_Texture* logo = nullptr;
    if (auto png = fs::read("branding/logo.png")) {
        int w = 0, h = 0, n = 0;
        if (stbi_uc* px = stbi_load_from_memory(png->data(), int(png->size()), &w, &h, &n, 4)) {
            if ((logo = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h))) {
                SDL_UpdateTexture(logo, nullptr, px, w * 4);
                SDL_SetTextureBlendMode(logo, SDL_BLENDMODE_BLEND);
            }
            stbi_image_free(px);
        }
    }

    using ui::Role;
    auto col = [](Role r, float a = 1.0f) { return ImGui::ColorConvertU32ToFloat4(ui::color(r, a)); };
    std::string toast;
    double toastUntil = 0;
    int frame = 0, choice = -1;
    for (bool running = true; running;) {
        for (SDL_Event e; SDL_PollEvent(&e);) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ui::beginFrame(1080.0f); // (sizes as they are: the window's density is in FontScaleDpi)
        ImGui::NewFrame();
        const double t = SDL_GetTicks() / 1000.0;
        const float u = density; // (one point)

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, col(Role::Background));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * u, 44 * u));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::Begin("crash", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // The logo, gone dark at the edges, with smoke curling up off it.
        const float size = 92 * u;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (logo) dl->AddImage(ImTextureRef(ImTextureID(logo)), at, ImVec2(at.x + size, at.y + size));
        for (int i = 0; i < 7; ++i) {
            const double life = std::fmod(t * 0.35 + i / 7.0, 1.0);
            const float x = at.x + size * (0.45f + 0.18f * float(std::sin(life * 5.0 + i * 1.7))), y = at.y + size * 0.3f - float(life) * 52 * u;
            dl->AddCircleFilled(ImVec2(x, y), (4 + float(life) * 12) * u, ui::color(Role::TextTertiary, 0.28f * float(1.0 - life)), 24);
        }
        ImGui::Dummy(ImVec2(size, size));
        ImGui::SameLine(0, 24 * u);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, 4 * u));
        ImGui::PushStyleColor(ImGuiCol_Text, col(Role::AccentText));
        ImGui::PushFont(ui::font(ui::Font::Bold), 13.0f);
        ImGui::TextUnformatted("FRY3DX");
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::PushFont(ui::font(ui::Font::Title), 34.0f);
        ImGui::TextUnformatted("Something got burnt.");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, col(Role::TextSecondary));
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
        ImGui::TextUnformatted(headline.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, 14 * u));

        // The good news, on a card of its own.
        {
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight() + 26 * u;
            dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), ui::color(Role::Success, 0.14f), 8 * u);
            dl->AddRect(a, ImVec2(a.x + w, a.y + h), ui::color(Role::Success, 0.4f), 8 * u);
            dl->AddCircleFilled(ImVec2(a.x + 18 * u, a.y + h / 2), 4 * u, ui::color(Role::Success));
            ImGui::SetCursorScreenPos(ImVec2(a.x + 32 * u, a.y + 13 * u));
            ImGui::PushStyleColor(ImGuiCol_Text, col(Role::SuccessText));
            ImGui::TextUnformatted("Your work isn't lost: open it again and Fry3DX offers back the last autosave.");
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + 18 * u));
        }

        // The report: what was going on when it happened, for a bug report.
        ImGui::PushStyleColor(ImGuiCol_Text, col(Role::TextTertiary));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("The report, saved to %s", argv[2]);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        const float buttonsH = ImGui::GetFrameHeight() + 26 * u + ImGui::GetTextLineHeight();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * u, 12 * u));
        ImGui::BeginChild("report", ImVec2(0, std::max(80 * u, ImGui::GetContentRegionAvail().y - buttonsH)), ImGuiChildFlags_FrameStyle | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(ui::font(ui::Font::Mono), 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col(Role::TextSecondary));
        ImGui::TextUnformatted(details.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (frame == 1) ImGui::SetScrollHereY(1.0f); // (the newest lines: the end)
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::Dummy(ImVec2(0, 10 * u));
        const ImVec2 button(170 * u, ImGui::GetFrameHeight() + 10 * u);
        ImGui::PushStyleColor(ImGuiCol_Button, col(Role::Accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(Role::AccentHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(Role::AccentActive));
        ImGui::PushStyleColor(ImGuiCol_Text, col(Role::OnAccent));
        if (ImGui::Button("Open it again", button)) choice = 0;
        ImGui::PopStyleColor(4);
        ImGui::SameLine(0, 10 * u);
        if (ImGui::Button("Copy the report", button)) choice = 1;
        ImGui::SameLine(0, 10 * u);
        if (ImGui::Button("Report it on GitHub", button)) choice = 2;
        ImGui::SameLine(0, 10 * u);
        if (ImGui::Button("Close", ImVec2(110 * u, button.y))) running = false;
        if (!ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) choice = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) choice = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)) running = false;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col(t < toastUntil ? Role::AccentText : Role::TextTertiary));
        ImGui::TextUnformatted(t < toastUntil ? toast.c_str() : "R open it again    C copy    Esc close");
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        if (choice == 1 || choice == 2) {
            SDL_SetClipboardText(report.c_str());
            toast = choice == 1 ? "Copied: paste it into a bug report." : "Copied: paste it into the new issue.";
            toastUntil = t + 3.0;
            if (choice == 2) SDL_OpenURL("https://github.com/Nexoniarz/Fry3DX/issues/new");
            choice = -1;
        }

        ImGui::Render();
        const ImVec4 bg = col(Role::Background);
        SDL_SetRenderDrawColorFloat(ren, bg.x, bg.y, bg.z, 1.0f);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (const char* shot = std::getenv("KEEL_CRASHSHOT"); shot && frame == 4) { // for checking the screen: one frame to a file, then quit
            if (SDL_Surface* px = SDL_RenderReadPixels(ren, nullptr)) SDL_SaveBMP(px, shot), SDL_DestroySurface(px);
            running = false;
        }
        SDL_RenderPresent(ren);
        ++frame;
        if (choice == 0) break;
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (logo) SDL_DestroyTexture(logo);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (choice == 0) { // opened again as it was started (it offers the autosave back)
        const std::string exe = sys::executablePath();
        std::vector<char*> a;
        std::string self = argv[0];
        a.push_back(self.data());
        for (std::string& r : restart) a.push_back(r.data());
        a.push_back(nullptr);
#ifdef _WIN32
        _spawnv(_P_NOWAIT, exe.c_str(), a.data());
        return 0;
#else
        execv(exe.c_str(), a.data());
        return 1;
#endif
    }
    return 0;
}

} // namespace keel::crash
