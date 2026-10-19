#pragma once
#include <string>

namespace keel::console {

void toggle();
bool isOpen();
// Draws the console window with Dear ImGui. Call between ImGui::NewFrame and ImGui::Render.
void draw();
// Where typed lines are kept between runs (loaded now, saved as you type).
void setHistoryFile(const std::string& path);

} // namespace keel::console
