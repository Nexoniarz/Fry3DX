#include "render/screenshot.h"
#include "core/log.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION // (stb_image for the whole program: the window icon, and the app's pictures)
#include <stb_image.h>
#include <stb_image_write.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace keel {
namespace {
std::atomic<int> g_pending{0};
}

void saveScreenshot(std::string path, std::vector<uint8_t> rgba, uint32_t width, uint32_t height) {
    // PNG compression is slow (very slow in debug builds); keep it off the render thread.
    ++g_pending;
    std::thread([px = std::move(rgba), path = std::move(path), width, height]() mutable {
        for (size_t i = 0; i < size_t(width) * height; ++i) px[i * 4 + 3] = 255;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        stbi_write_png_compression_level = 4;
        if (stbi_write_png(path.c_str(), int(width), int(height), 4, px.data(), int(width * 4))) log::info("Saved {}", path);
        else log::warn("Couldn't write {}", path);
        --g_pending;
    }).detach();
}

void waitForScreenshots() {
    // Quitting right after a screenshot would cut it off half-written.
    for (int i = 0; g_pending > 0 && i < 1000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

} // namespace keel
