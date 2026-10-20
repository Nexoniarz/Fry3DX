#include "text.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace fry {
namespace {

// The next character of UTF-8 text (a bad byte: itself).
uint32_t nextChar(const std::string& s, size_t& i) {
    const auto b = [&](size_t k) { return k < s.size() ? uint32_t(uint8_t(s[k])) : 0u; };
    const uint32_t c = b(i);
    if (c < 0x80) return ++i, c;
    if ((c & 0xE0) == 0xC0) return i += 2, ((c & 0x1F) << 6) | (b(i - 1) & 0x3F);
    if ((c & 0xF0) == 0xE0) return i += 3, ((c & 0x0F) << 12) | ((b(i - 2) & 0x3F) << 6) | (b(i - 1) & 0x3F);
    if ((c & 0xF8) == 0xF0) return i += 4, ((c & 0x07) << 18) | ((b(i - 3) & 0x3F) << 12) | ((b(i - 2) & 0x3F) << 6) | (b(i - 1) & 0x3F);
    return ++i, c;
}

using Contour = std::vector<glm::vec2>;

// A glyph's outlines (font units), its curves cut into `detail` straight pieces each.
std::vector<Contour> glyphContours(const stbtt_fontinfo& f, int glyph, int detail) {
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetGlyphShape(&f, glyph, &v);
    std::vector<Contour> out;
    glm::vec2 at(0.0f);
    for (int i = 0; i < n; ++i) {
        const glm::vec2 to(v[i].x, v[i].y);
        switch (v[i].type) {
        case STBTT_vmove:
            out.emplace_back();
            out.back().push_back(to);
            break;
        case STBTT_vline:
            if (!out.empty()) out.back().push_back(to);
            break;
        case STBTT_vcurve: {
            const glm::vec2 c(v[i].cx, v[i].cy);
            for (int k = 1; k <= detail && !out.empty(); ++k) {
                const float t = float(k) / float(detail);
                out.back().push_back((1 - t) * (1 - t) * at + 2 * (1 - t) * t * c + t * t * to);
            }
            break;
        }
        case STBTT_vcubic: {
            const glm::vec2 c1(v[i].cx, v[i].cy), c2(v[i].cx1, v[i].cy1);
            for (int k = 1; k <= detail + 2 && !out.empty(); ++k) {
                const float t = float(k) / float(detail + 2), u = 1 - t;
                out.back().push_back(u * u * u * at + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * to);
            }
            break;
        }
        default: break;
        }
        at = to;
    }
    stbtt_FreeShape(&f, v);
    for (Contour& c : out) {
        while (c.size() > 1 && glm::distance(c.front(), c.back()) < 1e-3f) c.pop_back();
        Contour clean; // (no point twice in a row, none in a straight line between its neighbours)
        for (const glm::vec2& p : c)
            if (clean.empty() || glm::distance(clean.back(), p) > 1e-3f) clean.push_back(p);
        c = clean;
    }
    std::erase_if(out, [](const Contour& c) { return c.size() < 3; });
    return out;
}

// Its area: positive when it turns counter-clockwise.
float area(const Contour& c) {
    float a = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        const glm::vec2 p = c[i], q = c[(i + 1) % c.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a * 0.5f;
}

// A glyph made solid. Fonts tell holes by the way an outline turns: the other way from the
// letter's body. Pieces that turn the body's way are joined (a letter can be drawn as overlapping
// strokes), then the holes cut out of what they're in.
Mesh solidGlyph(const std::vector<Contour>& cs, float scale, glm::vec2 at, float depth) {
    if (cs.empty()) return {};
    size_t biggest = 0;
    for (size_t i = 1; i < cs.size(); ++i)
        if (std::abs(area(cs[i])) > std::abs(area(cs[biggest]))) biggest = i;
    const bool bodyTurn = area(cs[biggest]) > 0;
    auto prism = [&](const Contour& c, float thick) {
        std::vector<glm::vec3> pts;
        for (const glm::vec2& p : c) pts.push_back(glm::vec3(at + p * scale, 0.0f));
        Mesh m = extrudeOutline(pts, glm::vec3(0, 0, 1), thick);
        transform(m, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, depth * 0.5f)));
        return m;
    };
    struct Box {
        glm::vec2 lo{1e30f}, hi{-1e30f};
        bool meets(const Box& o) const { return lo.x <= o.hi.x && o.lo.x <= hi.x && lo.y <= o.hi.y && o.lo.y <= hi.y; }
    };
    std::vector<Box> box(cs.size());
    for (size_t i = 0; i < cs.size(); ++i)
        for (const glm::vec2& p : cs[i]) box[i].lo = glm::min(box[i].lo, p), box[i].hi = glm::max(box[i].hi, p);
    // The body's pieces in groups that touch (each group one solid).
    std::vector<size_t> body, holes;
    for (size_t i = 0; i < cs.size(); ++i) (area(cs[i]) > 0) == bodyTurn ? body.push_back(i) : holes.push_back(i);
    std::vector<int> group(cs.size(), -1);
    int groups = 0;
    for (size_t i : body) {
        if (group[i] >= 0) continue;
        std::vector<size_t> todo{i};
        group[i] = groups;
        while (!todo.empty()) {
            const size_t k = todo.back();
            todo.pop_back();
            for (size_t j : body)
                if (group[j] < 0 && box[k].meets(box[j])) group[j] = groups, todo.push_back(j);
        }
        ++groups;
    }
    Mesh out;
    for (int g = 0; g < groups; ++g) {
        Mesh solid;
        Box gb;
        for (size_t i : body) {
            if (group[i] != g) continue;
            gb.lo = glm::min(gb.lo, box[i].lo), gb.hi = glm::max(gb.hi, box[i].hi);
            if (solid.faces.empty()) solid = prism(cs[i], depth);
            else if (Mesh joined = solid; combine(joined, prism(cs[i], depth), Combine::Add)) solid = std::move(joined);
            else append(solid, prism(cs[i], depth)); // (couldn't join: overlapping, but there)
        }
        for (size_t h : holes)
            if (gb.meets(box[h]))
                if (Mesh cut = solid; combine(cut, prism(cs[h], depth * 1.5f), Combine::Subtract)) solid = std::move(cut);
        append(out, solid);
    }
    return out;
}

} // namespace

Mesh makeText(const std::vector<uint8_t>& font, const std::string& text, const TextStyle& style, std::string* error) {
    stbtt_fontinfo f;
    if (font.empty() || !stbtt_InitFont(&f, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0))) {
        if (error) *error = "That isn't a font that can be read (TrueType or OpenType)";
        return {};
    }
    // The size: a capital H as tall as asked.
    int x0, y0, x1, y1;
    float capital = 0;
    if (stbtt_GetCodepointBox(&f, 'H', &x0, &y0, &x1, &y1)) capital = float(y1);
    if (capital <= 0) {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&f, &ascent, &descent, &gap);
        capital = float(ascent) * 0.7f;
    }
    const float scale = style.height / std::max(capital, 1.0f);
    // The lines, each as wide as its letters.
    std::vector<std::string> lines(1);
    for (char c : text) c == '\n' ? (void)lines.emplace_back() : lines.back().push_back(c);
    auto width = [&](const std::string& line) {
        float x = 0;
        int prev = 0;
        for (size_t i = 0; i < line.size();) {
            const int g = stbtt_FindGlyphIndex(&f, int(nextChar(line, i)));
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&f, g, &adv, &lsb);
            if (prev) x += float(stbtt_GetGlyphKernAdvance(&f, prev, g)) * scale;
            x += float(adv) * scale + style.spacing * style.height;
            prev = g;
        }
        return x;
    };
    float widest = 0;
    for (const std::string& l : lines) widest = std::max(widest, width(l));
    Mesh out;
    float y = 0;
    for (const std::string& line : lines) {
        const float w = width(line);
        float x = style.align == 1 ? (widest - w) * 0.5f : style.align == 2 ? widest - w : 0.0f;
        int prev = 0;
        for (size_t i = 0; i < line.size();) {
            const int g = stbtt_FindGlyphIndex(&f, int(nextChar(line, i)));
            if (prev) x += float(stbtt_GetGlyphKernAdvance(&f, prev, g)) * scale;
            append(out, solidGlyph(glyphContours(f, g, std::max(1, style.detail)), scale, glm::vec2(x, y), style.depth));
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&f, g, &adv, &lsb);
            x += float(adv) * scale + style.spacing * style.height;
            prev = g;
        }
        y -= style.height * (1.0f + std::max(0.0f, style.lineGap) + 0.4f); // (a capital, what hangs below, and the gap)
    }
    if (out.faces.empty() && error) *error = "No letters to make (only spaces?)";
    return out;
}

} // namespace fry
