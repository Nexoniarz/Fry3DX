// Pictures on top of the painting: stickers that wrap over edges, and pictures pinned by corners.
#include "scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

namespace fry {

int stickerFace(const Mesh& m, const PictureLayer& l) {
    if (l.face >= 0 && l.face < int(m.faces.size())) return l.face;
    int best = -1;
    float d = 1e30f;
    for (size_t f = 0; f < m.faces.size(); ++f)
        if (const float e = glm::distance(faceCenter(m, int(f)), l.centre); e < d) d = e, best = int(f);
    return best;
}

bool stickerFrame(const Mesh& m, const PictureLayer& l, glm::vec3& right, glm::vec3& up, glm::vec3& normal) {
    const int f = stickerFace(m, l);
    if (f < 0) return false;
    normal = faceNormal(m, f);
    up = l.up - normal * glm::dot(l.up, normal);
    if (glm::length(up) < 1e-6f) up = glm::cross(normal, std::abs(normal.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
    up = glm::normalize(up);
    right = glm::cross(up, normal);
    return true;
}

void stampSticker(Image& dst, const Mesh& m, const std::vector<std::vector<glm::vec2>>& uvs, const PictureLayer& l, std::vector<uint8_t>* owner, uint8_t id) {
    if (!l.image || l.image->empty() || uvs.size() != m.faces.size() || l.width <= 0 || l.height <= 0) return;
    glm::vec3 right, up, n;
    if (!stickerFrame(m, l, right, up, n)) return;
    const int root = stickerFace(m, l);
    const glm::vec3 c = l.centre - n * glm::dot(l.centre - faceCenter(m, root), n); // (on its face's plane)
    const float hw = l.width * 0.5f, hh = l.height * 0.5f;
    const Image& src = *l.image;
    const glm::vec2 size(float(dst.w), float(dst.h));
    std::map<std::pair<int, int>, std::vector<int>> byEdge;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) byEdge[{std::min(fv[i], fv[(i + 1) % fv.size()]), std::max(fv[i], fv[(i + 1) % fv.size()])}].push_back(int(f));
    }
    std::vector<char> seen(m.faces.size(), 0), done(size_t(dst.w) * size_t(dst.h), 0);
    // Each face reached: how to fold it flat into the sticker's plane (turned about the edges
    // between it and the sticker's face, as a sticker bends over them). Nearest the sticker's
    // middle first, so a face round a corner gets the part of it that reaches it straight, not
    // what's left after going the long way round over the faces beside it.
    struct Todo {
        float near;
        int face;
        glm::mat4 fold;
        bool operator<(const Todo& o) const { return near > o.near; }
    };
    std::priority_queue<Todo> todo;
    todo.push({0.0f, root, glm::mat4(1.0f)});
    while (!todo.empty()) {
        const Todo next = todo.top();
        todo.pop();
        const int f = next.face;
        const glm::mat4 fold = next.fold;
        if (seen[size_t(f)]) continue;
        seen[size_t(f)] = 1;
        const auto& fv = m.faces[size_t(f)];
        auto flat = [&](const glm::vec3& p) {
            const glm::vec3 q = glm::vec3(fold * glm::vec4(p, 1.0f)) - c;
            return glm::vec2(glm::dot(q, right), glm::dot(q, up));
        };
        glm::vec2 lo(1e30f), hi(-1e30f);
        for (int v : fv) {
            const glm::vec2 q = flat(m.verts[size_t(v)]);
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        }
        if (hi.x < -hw || lo.x > hw || hi.y < -hh || lo.y > hh) continue; // (the sticker doesn't reach it, nor past it)
        for (const auto& t : triangulate(m, f)) {
            glm::vec3 w[3];
            glm::vec2 px[3];
            for (int k = 0; k < 3; ++k) {
                w[k] = m.verts[size_t(t[size_t(k)])];
                px[k] = uvs[size_t(f)][size_t(std::find(fv.begin(), fv.end(), t[size_t(k)]) - fv.begin())] * size;
            }
            const float twice = (px[1].x - px[0].x) * (px[2].y - px[0].y) - (px[2].x - px[0].x) * (px[1].y - px[0].y);
            if (std::abs(twice) < 1e-9f) continue;
            const glm::vec2 plo = glm::max(glm::min(px[0], glm::min(px[1], px[2])) - 2.0f, glm::vec2(0.0f));
            const glm::vec2 phi = glm::min(glm::max(px[0], glm::max(px[1], px[2])) + 2.0f, size - 1.0f);
            for (int y = int(plo.y); y <= int(phi.y); ++y)
                for (int x = int(plo.x); x <= int(phi.x); ++x) {
                    char& d = done[size_t(y) * size_t(dst.w) + size_t(x)];
                    if (d) continue;
                    const glm::vec2 q(float(x) + 0.5f, float(y) + 0.5f);
                    float b[3];
                    bool in = true;
                    for (int k = 0; k < 3; ++k) {
                        const glm::vec2 a = px[(k + 1) % 3], e = px[(k + 2) % 3];
                        b[k] = ((e.x - a.x) * (q.y - a.y) - (q.x - a.x) * (e.y - a.y)) / twice;
                        in &= b[k] * std::abs(twice) / std::max(glm::length(e - a), 1e-6f) >= -1.5f; // (and a little past: no seams)
                    }
                    if (!in) continue;
                    const glm::vec2 s = flat(w[0] * b[0] + w[1] * b[1] + w[2] * b[2]);
                    const glm::vec2 uv(s.x / l.width + 0.5f, 0.5f - s.y / l.height);
                    if (uv.x < 0 || uv.y < 0 || uv.x > 1 || uv.y > 1) continue;
                    d = 1;
                    const glm::vec4 col = sample(src, uv * glm::vec2(src.w, src.h));
                    const float a = col.a * l.opacity;
                    if (owner && a > 0.05f) (*owner)[size_t(y) * size_t(dst.w) + size_t(x)] = id;
                    uint8_t* p = dst.at(x, y);
                    for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(float(p[k]) + (col[k] * 255.0f - float(p[k])) * a));
                }
        }
        // On to the faces next to it, folded about the edge between.
        const glm::vec3 nf = faceNormal(m, f);
        for (size_t i = 0; i < fv.size(); ++i) {
            const int a = fv[i], b = fv[(i + 1) % fv.size()];
            // (how near the sticker's middle the edge comes, flat)
            const glm::vec2 ea = flat(m.verts[size_t(a)]), eb = flat(m.verts[size_t(b)]);
            const float along = glm::clamp(glm::dot(-ea, eb - ea) / std::max(glm::dot(eb - ea, eb - ea), 1e-12f), 0.0f, 1.0f);
            const float near = std::max(next.near, glm::length(ea + (eb - ea) * along));
            for (int g : byEdge[{std::min(a, b), std::max(a, b)}]) {
                if (seen[size_t(g)]) continue;
                const glm::vec3 ng = faceNormal(m, g), pa = m.verts[size_t(a)], e = glm::normalize(m.verts[size_t(b)] - pa);
                const float angle = std::atan2(glm::dot(glm::cross(ng, nf), e), glm::dot(ng, nf));
                todo.push({near, g, fold * glm::translate(glm::mat4(1.0f), pa) * glm::rotate(glm::mat4(1.0f), angle, e) * glm::translate(glm::mat4(1.0f), -pa)});
            }
        }
    }
}

void settleSticker(const Mesh& m, PictureLayer& l) {
    glm::vec3 right, up, n;
    if (!stickerFrame(m, l, right, up, n)) return;
    const int root = stickerFace(m, l);
    const glm::vec3 c = l.centre - n * glm::dot(l.centre - faceCenter(m, root), n);
    std::map<std::pair<int, int>, std::vector<int>> byEdge;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) byEdge[{std::min(fv[i], fv[(i + 1) % fv.size()]), std::max(fv[i], fv[(i + 1) % fv.size()])}].push_back(int(f));
    }
    // Unfolded nearest-first (as stampSticker does) until the face its middle lies on.
    struct Todo {
        float near;
        int face;
        glm::mat4 fold;
        bool operator<(const Todo& o) const { return near > o.near; }
    };
    std::priority_queue<Todo> todo;
    todo.push({0.0f, root, glm::mat4(1.0f)});
    std::vector<char> seen(m.faces.size(), 0);
    const float reach = std::max(l.width, l.height);
    while (!todo.empty()) {
        const Todo next = todo.top();
        todo.pop();
        if (seen[size_t(next.face)] || next.near > reach) continue;
        seen[size_t(next.face)] = 1;
        const auto& fv = m.faces[size_t(next.face)];
        std::vector<glm::vec2> poly;
        for (int v : fv) {
            const glm::vec3 q = glm::vec3(next.fold * glm::vec4(m.verts[size_t(v)], 1.0f)) - c;
            poly.push_back({glm::dot(q, right), glm::dot(q, up)});
        }
        bool inside = false; // (the middle, 0 0, in the face as unfolded)
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
            if ((poly[i].y > 0) != (poly[j].y > 0) && 0 < (poly[j].x - poly[i].x) * (0 - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) inside = !inside;
        if (inside) {
            if (next.face == root) return;
            const glm::mat4 back = glm::inverse(next.fold); // (the sticker's plane folded onto that face)
            l.face = next.face;
            l.centre = glm::vec3(back * glm::vec4(c, 1.0f));
            l.up = glm::normalize(glm::vec3(back * glm::vec4(up, 0.0f)));
            return;
        }
        const glm::vec3 nf = faceNormal(m, next.face);
        for (size_t i = 0; i < fv.size(); ++i) {
            const int a = fv[i], b = fv[(i + 1) % fv.size()];
            const glm::vec2 ea = poly[i], eb = poly[(i + 1) % fv.size()];
            const float along = glm::clamp(glm::dot(-ea, eb - ea) / std::max(glm::dot(eb - ea, eb - ea), 1e-12f), 0.0f, 1.0f);
            const float near = std::max(next.near, glm::length(ea + (eb - ea) * along));
            for (int g : byEdge[{std::min(a, b), std::max(a, b)}]) {
                if (seen[size_t(g)]) continue;
                const glm::vec3 ng = faceNormal(m, g), pa = m.verts[size_t(a)], e = glm::normalize(m.verts[size_t(b)] - pa);
                const float angle = std::atan2(glm::dot(glm::cross(ng, nf), e), glm::dot(ng, nf));
                todo.push({near, g, next.fold * glm::translate(glm::mat4(1.0f), pa) * glm::rotate(glm::mat4(1.0f), angle, e) * glm::translate(glm::mat4(1.0f), -pa)});
            }
        }
    }
}

Image composePicture(const Object& o, std::vector<uint8_t>* owner) {
    Image img = *o.picture;
    if (owner) owner->assign(size_t(img.w) * size_t(img.h), 0);
    for (size_t i = 0; i < o.layers.size() && i < 255; ++i) {
        const PictureLayer& l = o.layers[i];
        if (!l.visible || !l.image) continue;
        if (l.sticker) stampSticker(img, o.mesh, o.uvs, l, owner, uint8_t(i + 1));
        else stampQuad(img, *l.image, l.corners, l.opacity, owner, uint8_t(i + 1));
    }
    return img;
}

} // namespace fry
