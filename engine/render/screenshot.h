#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace keel {
// Writes RGBA8 pixels (row 0 at the top) to a PNG on a background thread; alpha is forced opaque.
void saveScreenshot(std::string path, std::vector<uint8_t> rgba, uint32_t width, uint32_t height);
// Blocks until screenshots still being written are done (before the process exits).
void waitForScreenshots();
} // namespace keel
