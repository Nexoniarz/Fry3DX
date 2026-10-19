#include "core/log.h"

#include <cstdio>
#include <mutex>
#include <string>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace keel::log {
namespace {
constexpr size_t kMaxLines = 4096;
std::mutex g_mutex;
std::vector<Line> g_lines;
} // namespace

void write(Level level, std::string_view text) {
    static const char* const prefix[] = {"", "WARN: ", "ERROR: "};
    std::lock_guard lock(g_mutex);
    std::fprintf(level == Level::Info ? stdout : stderr, "%s%.*s\n", prefix[int(level)], int(text.size()), text.data());
    std::fflush(level == Level::Info ? stdout : stderr);
#if defined(__ANDROID__) // (a phone has no console: adb logcat -s Fry3DX shows it)
    static const int priority[] = {ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_print(priority[int(level)], "Fry3DX", "%.*s", int(text.size()), text.data());
#endif
    if (g_lines.size() >= kMaxLines) g_lines.erase(g_lines.begin(), g_lines.begin() + kMaxLines / 4);
    g_lines.push_back({level, std::string(text)});
}

const std::vector<Line>& history() { return g_lines; }
void clearHistory() { g_lines.clear(); }

} // namespace keel::log
