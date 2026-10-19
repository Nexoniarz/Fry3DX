#pragma once
#include <chrono>

namespace keel {

// Seconds since program start, monotonic.
inline double now() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

} // namespace keel
