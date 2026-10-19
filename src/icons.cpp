#include "icons.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace fry {

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 o, float s, ImU32 c) {
    auto P = [&](float x, float y) { return ImVec2(o.x + x * s, o.y + y * s); };
    const float t = std::max(1.5f, s * 0.07f);
    const ImU32 dim = (c & 0x00FFFFFF) | 0x60000000;
    auto line = [&](float x0, float y0, float x1, float y1, float w = 0) { dl->AddLine(P(x0, y0), P(x1, y1), c, w > 0 ? w : t); };
    auto tri = [&](float x0, float y0, float x1, float y1, float x2, float y2) { dl->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), c); };
    auto quad = [&](float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, bool fill, ImU32 col) {
        if (fill) dl->AddQuadFilled(P(x0, y0), P(x1, y1), P(x2, y2), P(x3, y3), col);
        else dl->AddQuad(P(x0, y0), P(x1, y1), P(x2, y2), P(x3, y3), col, t);
    };
    auto dot = [&](float x, float y, float r) { dl->AddCircleFilled(P(x, y), r * s, c); };
    switch (icon) {
    case Icon::Select: {
        const ImVec2 pts[] = {P(0.28f, 0.14f), P(0.28f, 0.80f), P(0.44f, 0.65f), P(0.56f, 0.88f), P(0.66f, 0.83f), P(0.55f, 0.60f), P(0.76f, 0.60f)};
        dl->AddConcavePolyFilled(pts, 7, c);
        break;
    }
    case Icon::Move:
        line(0.5f, 0.2f, 0.5f, 0.8f), line(0.2f, 0.5f, 0.8f, 0.5f);
        tri(0.5f, 0.08f, 0.38f, 0.24f, 0.62f, 0.24f), tri(0.5f, 0.92f, 0.38f, 0.76f, 0.62f, 0.76f);
        tri(0.08f, 0.5f, 0.24f, 0.38f, 0.24f, 0.62f), tri(0.92f, 0.5f, 0.76f, 0.38f, 0.76f, 0.62f);
        break;
    case Icon::Rotate:
        dl->PathArcTo(P(0.5f, 0.52f), 0.30f * s, 3.14159f * 0.15f, 3.14159f * 1.75f, 24);
        dl->PathStroke(c, 0, t * 1.2f);
        tri(0.80f, 0.18f, 0.86f, 0.46f, 0.60f, 0.36f);
        break;
    case Icon::Scale:
        dl->AddRect(P(0.16f, 0.16f), P(0.84f, 0.84f), dim, 0, 0, t);
        dl->AddRectFilled(P(0.16f, 0.56f), P(0.44f, 0.84f), c);
        line(0.44f, 0.56f, 0.76f, 0.24f);
        tri(0.84f, 0.16f, 0.62f, 0.22f, 0.78f, 0.38f);
        break;
    case Icon::Draw: {
        const ImVec2 pts[] = {P(0.18f, 0.78f), P(0.30f, 0.26f), P(0.62f, 0.18f), P(0.82f, 0.50f), P(0.60f, 0.82f)};
        dl->AddPolyline(pts, 5, c, ImDrawFlags_Closed, t);
        for (const ImVec2& p : pts) dl->AddCircleFilled(p, 0.07f * s, c);
        break;
    }
    case Icon::Extrude:
        quad(0.14f, 0.70f, 0.56f, 0.70f, 0.86f, 0.52f, 0.44f, 0.52f, true, dim);
        quad(0.14f, 0.42f, 0.56f, 0.42f, 0.86f, 0.24f, 0.44f, 0.24f, false, c);
        line(0.50f, 0.62f, 0.50f, 0.36f), tri(0.50f, 0.10f, 0.40f, 0.30f, 0.60f, 0.30f);
        break;
    case Icon::Inset:
        dl->AddRect(P(0.14f, 0.14f), P(0.86f, 0.86f), c, 0, 0, t);
        dl->AddRectFilled(P(0.34f, 0.34f), P(0.66f, 0.66f), c);
        line(0.14f, 0.14f, 0.34f, 0.34f, t * 0.6f), line(0.86f, 0.14f, 0.66f, 0.34f, t * 0.6f);
        line(0.14f, 0.86f, 0.34f, 0.66f, t * 0.6f), line(0.86f, 0.86f, 0.66f, 0.66f, t * 0.6f);
        break;
    case Icon::Bevel:
        dl->PathLineTo(P(0.16f, 0.86f));
        dl->PathLineTo(P(0.16f, 0.16f));
        dl->PathLineTo(P(0.40f, 0.16f));
        dl->PathArcTo(P(0.40f, 0.60f), 0.44f * s, -3.14159f * 0.5f, 0.0f, 16);
        dl->PathLineTo(P(0.84f, 0.86f));
        dl->PathFillConvex(dim);
        dl->PathLineTo(P(0.40f, 0.16f));
        dl->PathArcTo(P(0.40f, 0.60f), 0.44f * s, -3.14159f * 0.5f, 0.0f, 16);
        dl->PathStroke(c, 0, t * 1.4f);
        break;
    case Icon::LoopCut:
        dl->AddRect(P(0.12f, 0.22f), P(0.88f, 0.78f), dim, 0, 0, t);
        line(0.12f, 0.50f, 0.88f, 0.50f, t * 0.6f);
        line(0.50f, 0.10f, 0.50f, 0.90f, t * 1.4f);
        break;
    case Icon::Objects:
    case Icon::Shape:
        quad(0.5f, 0.12f, 0.86f, 0.30f, 0.5f, 0.48f, 0.14f, 0.30f, icon == Icon::Objects, icon == Icon::Objects ? c : dim);
        quad(0.14f, 0.30f, 0.5f, 0.48f, 0.5f, 0.88f, 0.14f, 0.70f, icon == Icon::Objects, icon == Icon::Objects ? dim : dim);
        quad(0.86f, 0.30f, 0.5f, 0.48f, 0.5f, 0.88f, 0.86f, 0.70f, false, c);
        if (icon == Icon::Shape)
            for (auto [x, y] : {std::pair{0.5f, 0.12f}, {0.86f, 0.30f}, {0.5f, 0.48f}, {0.14f, 0.30f}, {0.14f, 0.70f}, {0.5f, 0.88f}, {0.86f, 0.70f}}) dot(x, y, 0.07f);
        break;
    case Icon::Points:
    case Icon::Edges:
    case Icon::Faces:
        if (icon == Icon::Faces) dl->AddRectFilled(P(0.2f, 0.2f), P(0.8f, 0.8f), c);
        dl->AddRect(P(0.2f, 0.2f), P(0.8f, 0.8f), icon == Icon::Faces ? c : dim, 0, 0, t);
        if (icon == Icon::Edges) line(0.2f, 0.2f, 0.8f, 0.2f, t * 2.2f), line(0.8f, 0.2f, 0.8f, 0.8f, t * 2.2f);
        if (icon == Icon::Points)
            for (auto [x, y] : {std::pair{0.2f, 0.2f}, {0.8f, 0.2f}, {0.8f, 0.8f}, {0.2f, 0.8f}}) dot(x, y, 0.11f);
        break;
    case Icon::Box:
        for (int i = 0; i < 4; ++i) { // dashed
            const float a = 0.16f + float(i) * 0.18f, b = a + 0.1f;
            line(a, 0.18f, b, 0.18f), line(a, 0.82f, b, 0.82f), line(0.16f, a + 0.02f, 0.16f, b + 0.02f), line(0.84f, a + 0.02f, 0.84f, b + 0.02f);
        }
        break;
    case Icon::Brush:
        dl->AddCircle(P(0.5f, 0.5f), 0.32f * s, c, 24, t);
        dot(0.5f, 0.5f, 0.07f);
        break;
    case Icon::Lasso: {
        dl->PathArcTo(P(0.52f, 0.40f), 0.30f * s, 0.0f, 6.2832f, 24);
        dl->PathStroke(c, ImDrawFlags_Closed, t);
        line(0.32f, 0.62f, 0.26f, 0.88f);
        break;
    }
    case Icon::Add:
        line(0.5f, 0.18f, 0.5f, 0.82f, t * 1.6f), line(0.18f, 0.5f, 0.82f, 0.5f, t * 1.6f);
        break;
    case Icon::Knife: {
        const ImVec2 blade[] = {P(0.10f, 0.90f), P(0.56f, 0.30f), P(0.70f, 0.44f)};
        dl->AddTriangleFilled(blade[0], blade[1], blade[2], c);
        line(0.66f, 0.38f, 0.88f, 0.14f, t * 2.4f);
        line(0.14f, 0.62f, 0.34f, 0.62f, t * 0.6f), line(0.06f, 0.74f, 0.22f, 0.74f, t * 0.6f); // (the cut)
        break;
    }
    case Icon::Hole:
        dl->AddRect(P(0.12f, 0.12f), P(0.88f, 0.88f), dim, 0, 0, t);
        dl->AddCircleFilled(P(0.5f, 0.5f), 0.26f * s, (c & 0x00FFFFFF) | 0x40000000, 24);
        dl->AddCircle(P(0.5f, 0.5f), 0.26f * s, c, 24, t * 1.2f);
        dl->PathArcTo(P(0.5f, 0.5f), 0.18f * s, 3.14159f * 0.15f, 3.14159f * 0.85f, 12);
        dl->PathStroke(c, 0, t * 0.7f);
        break;
    case Icon::PaintBrush:
        line(0.84f, 0.14f, 0.46f, 0.52f, t * 1.6f);
        dl->AddQuadFilled(P(0.40f, 0.48f), P(0.52f, 0.60f), P(0.36f, 0.80f), P(0.22f, 0.66f), c);
        dl->AddTriangleFilled(P(0.22f, 0.66f), P(0.36f, 0.80f), P(0.10f, 0.90f), c);
        break;
    case Icon::Eraser:
        quad(0.16f, 0.62f, 0.50f, 0.28f, 0.82f, 0.60f, 0.48f, 0.94f, false, c);
        quad(0.16f, 0.62f, 0.33f, 0.45f, 0.65f, 0.77f, 0.48f, 0.94f, true, c);
        break;
    case Icon::Fill:
        quad(0.16f, 0.46f, 0.46f, 0.16f, 0.76f, 0.46f, 0.46f, 0.76f, false, c);
        quad(0.16f, 0.46f, 0.76f, 0.46f, 0.61f, 0.61f, 0.31f, 0.61f, true, dim);
        dot(0.84f, 0.74f, 0.08f), tri(0.84f, 0.52f, 0.77f, 0.72f, 0.91f, 0.72f);
        break;
    case Icon::Picker:
        line(0.18f, 0.82f, 0.60f, 0.40f, t * 1.6f);
        line(0.50f, 0.36f, 0.64f, 0.50f, t * 1.6f);
        dot(0.72f, 0.28f, 0.13f);
        dot(0.16f, 0.84f, 0.05f);
        break;
    case Icon::Picture:
        dl->AddRect(P(0.10f, 0.18f), P(0.90f, 0.82f), c, 0, 0, t);
        tri(0.16f, 0.76f, 0.40f, 0.40f, 0.62f, 0.76f), tri(0.46f, 0.76f, 0.66f, 0.52f, 0.84f, 0.76f);
        dot(0.70f, 0.34f, 0.07f);
        break;
    case Icon::Render:
        dl->AddRect(P(0.10f, 0.32f), P(0.90f, 0.82f), c, 0.08f * s, 0, t);
        dl->AddRectFilled(P(0.30f, 0.20f), P(0.54f, 0.33f), c);
        dl->AddCircle(P(0.5f, 0.57f), 0.16f * s, c, 20, t);
        dot(0.78f, 0.42f, 0.04f);
        break;
    case Icon::Layout: // (a frame unfolded: a cross of squares)
        dl->AddRect(P(0.38f, 0.10f), P(0.62f, 0.34f), c, 0, 0, t);
        dl->AddRect(P(0.14f, 0.34f), P(0.86f, 0.58f), c, 0, 0, t);
        dl->AddRect(P(0.38f, 0.58f), P(0.62f, 0.90f), c, 0, 0, t);
        line(0.38f, 0.34f, 0.38f, 0.58f), line(0.62f, 0.34f, 0.62f, 0.58f);
        break;
    case Icon::Print: // (a nozzle over what it's printed)
        quad(0.34f, 0.10f, 0.66f, 0.10f, 0.66f, 0.30f, 0.34f, 0.30f, true, c);
        tri(0.40f, 0.30f, 0.60f, 0.30f, 0.50f, 0.44f);
        for (int i = 0; i < 3; ++i) dl->AddRectFilled(P(0.22f + 0.04f * float(i), 0.56f + 0.11f * float(i)), P(0.78f - 0.04f * float(i), 0.64f + 0.11f * float(i)), i ? dim : c);
        line(0.10f, 0.90f, 0.90f, 0.90f);
        break;
    case Icon::Ruler: {
        quad(0.08f, 0.66f, 0.66f, 0.08f, 0.92f, 0.34f, 0.34f, 0.92f, false, c);
        for (int i = 1; i < 6; ++i) {
            const float k = float(i) / 6.0f, x = 0.08f + 0.58f * k, y = 0.66f - 0.58f * k, l = i % 2 ? 0.09f : 0.15f;
            line(x, y, x + l, y + l, t * 0.8f);
        }
        break;
    }
    case Icon::LayFlat:
        quad(0.22f, 0.40f, 0.62f, 0.22f, 0.86f, 0.50f, 0.46f, 0.68f, false, c);
        line(0.10f, 0.88f, 0.90f, 0.88f);
        line(0.54f, 0.06f, 0.54f, 0.30f), tri(0.54f, 0.36f, 0.46f, 0.24f, 0.62f, 0.24f);
        break;
    }
}

} // namespace fry
