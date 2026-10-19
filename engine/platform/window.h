#pragma once
#include <cstdint>

#include <cstdint>
#include <utility>
#include <vector>

struct SDL_Window;

namespace keel {

class Window {
public:
    // opengl: a window for an OpenGL context (the backend creates it); otherwise Vulkan.
    bool init(const char* title, int width, int height, bool opengl = false);
    void shutdown();
    SDL_Window* handle() const { return window_; }
    // Size in pixels (not points); 0×0 while minimized.
    void pixelSize(uint32_t& w, uint32_t& h) const;
    // mode: 0 = windowed (width × height), 1 = borderless fullscreen (desktop size),
    // 2 = exclusive fullscreen at the display mode closest to width × height.
    void setMode(int mode, int width, int height);
    void setIcon(const uint8_t* rgba, int width, int height);
    // Fullscreen resolutions the display offers, largest first, no duplicates.
    std::vector<std::pair<int, int>> resolutions() const;

private:
    SDL_Window* window_ = nullptr;
};

} // namespace keel
