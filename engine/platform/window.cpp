#include "platform/window.h"
#include "core/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
// Hybrid laptops: ask the NVIDIA / AMD drivers for the dedicated GPU (read from the executable).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace keel {
namespace {
// Hybrid laptops on Linux: OpenGL goes to whatever GPU runs the desktop (often the integrated
// one) unless asked. Vulkan picks the discrete GPU itself; this makes OpenGL do the same. Set
// before SDL loads the GL library; anything the user already set wins. KEEL_IGPU=1 opts out.
void preferDedicatedGpu() {
#ifndef _WIN32
    if (std::getenv("KEEL_IGPU")) return;
    if (std::filesystem::exists("/proc/driver/nvidia")) { // NVIDIA's own driver: PRIME render offload
        setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
        setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
        setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 0);
    } else {
        setenv("DRI_PRIME", "1", 0); // Mesa: the other GPU if there is one, else the same
    }
#endif
}
} // namespace

bool Window::init(const char* title, int width, int height, bool opengl) {
    preferDedicatedGpu();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        log::error("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    SDL_WindowFlags flags = (opengl ? SDL_WINDOW_OPENGL : SDL_WINDOW_VULKAN) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__ANDROID__)
    flags |= SDL_WINDOW_FULLSCREEN; // (the whole screen: no status or navigation bar over the edges)
#endif
    window_ = SDL_CreateWindow(title, width, height, flags);
    if (!window_) {
        log::error("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    return true;
}

void Window::shutdown() {
    if (window_) SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_Quit();
}

void Window::setIcon(const uint8_t* rgba, int width, int height) {
    if (SDL_Surface* s = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ABGR8888, const_cast<uint8_t*>(rgba), width * 4)) {
        SDL_SetWindowIcon(window_, s);
        SDL_DestroySurface(s);
    }
}

void Window::setMode(int mode, int width, int height) {
    if (!window_) return;
    if (mode == 0) {
        SDL_SetWindowFullscreen(window_, false);
        SDL_SetWindowSize(window_, width, height);
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        return;
    }
    SDL_DisplayMode closest{};
    const bool exclusive = mode == 2 && SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(window_), width, height, 0.0f, true, &closest);
    SDL_SetWindowFullscreenMode(window_, exclusive ? &closest : nullptr); // nullptr = borderless desktop
    SDL_SetWindowFullscreen(window_, true);
}

std::vector<std::pair<int, int>> Window::resolutions() const {
    std::vector<std::pair<int, int>> out;
    int count = 0;
    if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window_), &count)) {
        for (int i = 0; i < count; ++i) {
            const std::pair<int, int> r{modes[i]->w, modes[i]->h};
            if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
        }
        SDL_free(modes);
    }
    return out;
}

void Window::pixelSize(uint32_t& w, uint32_t& h) const {
    int iw = 0, ih = 0;
    if (!(SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) SDL_GetWindowSizeInPixels(window_, &iw, &ih);
    w = uint32_t(iw);
    h = uint32_t(ih);
}

} // namespace keel
