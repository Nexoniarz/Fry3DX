#pragma once
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace keel::log {

enum class Level { Info, Warn, Error };

struct Line {
    Level level;
    std::string text;
};

void write(Level level, std::string_view text);
// The last few thousand lines, for the console.
const std::vector<Line>& history();
void clearHistory();

template <class... A> void info(std::format_string<A...> f, A&&... a)  { write(Level::Info,  std::format(f, std::forward<A>(a)...)); }
template <class... A> void warn(std::format_string<A...> f, A&&... a)  { write(Level::Warn,  std::format(f, std::forward<A>(a)...)); }
template <class... A> void error(std::format_string<A...> f, A&&... a) { write(Level::Error, std::format(f, std::forward<A>(a)...)); }

} // namespace keel::log
