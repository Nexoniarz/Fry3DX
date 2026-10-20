#pragma once
// The toolbox's pictures, drawn as lines and shapes (sharp at any size, no files).
#include <imgui.h>

namespace fry {

enum class Icon { Select, Move, Rotate, Scale, Draw, Extrude, Inset, Bevel, LoopCut, Objects, Shape, Points, Edges, Faces, Box, Brush, Lasso, Add, Knife, Hole, PaintBrush, Eraser, Fill, Picker, Picture, Render, Layout, Print, Ruler, LayFlat, Sculpt, Inflate, Smooth, Flatten, Pinch, Grab };

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 topLeft, float size, ImU32 color);

} // namespace fry
