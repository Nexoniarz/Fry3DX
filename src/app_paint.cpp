// Fry3DX: painting and sculpting, picture layers, and the Layout tab.
#include "app.h"

using namespace fry;
namespace stdfs = std::filesystem;

namespace fryapp {

// ───────────────────────────── painting ─────────────────────────────

// The object ready to be painted: its rounding and smoothing made part of it, its surface laid
// out on a picture (made the plain colour of it at first).
auto App::readyToPaint(uint32_t id) -> Object* {
    Object* o = find(id);
    if (!o) return nullptr;
    if (o->locked) return say("That object is locked"), nullptr;
    if (o->picture && o->uvsFit() && !o->hasFinish()) return o;
    pushUndo();
    if (o->hasFinish()) {
        o->mesh = finishedMesh(*o);
        o->mirror = 0, o->round = 0, o->smoothLevels = 0;
        handMade(*o);
    }
    o->uvs = autoUnwrap(o->mesh);
    if (!o->picture) o->picture = std::make_shared<Image>(makeImage(pictureSize_, pictureSize_, glm::vec4(o->color, 1.0f)));
    touched(o->id);
    picZoom_ = 0;
    say(std::format("{} is ready to paint: its surface is laid out flat on a {}x{} picture", o->name, o->picture->w, o->picture->h));
    return o;
}

// The object's picture, copied so this change can be undone; the copy is now the object's.
auto App::changePicture(Object& o) -> Image& {
    pushUndo();
    auto img = std::make_shared<Image>(*o.picture);
    o.picture = img;
    paint_.image = img;
    uploaded_[o.id].dirty = true;
    return *img;
}

auto App::beginStroke(Object& o, bool in2D) -> void {
    Image& img = changePicture(o);
    paint_.active = true, paint_.in2D = in2D, paint_.haveLast = false, paint_.object = o.id;
    paint_.stroke.begin(img, paintTool_ == PaintTool::Eraser ? glm::vec4(o.color, 1.0f) : paintColor_);
}

auto App::endStroke() -> void {
    if (!paint_.active) return;
    paint_.active = false;
    uploaded_[paint_.object].dirty = true;
    paint_.image.reset();
    paint_.stroke = {};
}

// Where on its picture a ray hits an object (0..1), or nothing.
auto App::uvUnder(const Object& o, const Ray& ray) -> std::optional<glm::vec2> {
    if (!o.uvsFit()) return std::nullopt;
    const glm::mat4 m = objectMatrix(o);
    float best = 1e30f;
    std::optional<glm::vec2> out;
    for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
        const auto& fv = o.mesh.faces[f];
        for (const auto& t : triangulate(o.mesh, int(f))) {
            glm::vec3 w[3];
            glm::vec2 uv[3];
            for (int k = 0; k < 3; ++k) {
                w[k] = glm::vec3(m * glm::vec4(o.mesh.verts[size_t(t[size_t(k)])], 1.0f));
                uv[k] = o.uvs[f][size_t(std::find(fv.begin(), fv.end(), t[size_t(k)]) - fv.begin())];
            }
            const glm::vec3 e1 = w[1] - w[0], e2 = w[2] - w[0], p = glm::cross(ray.dir, e2);
            const float det = glm::dot(e1, p);
            if (std::abs(det) < 1e-12f) continue;
            const glm::vec3 sv = ray.origin - w[0], q = glm::cross(sv, e1);
            const float u = glm::dot(sv, p) / det, v = glm::dot(ray.dir, q) / det, tt = glm::dot(e2, q) / det;
            if (u < 0 || v < 0 || u + v > 1 || tt <= 0 || tt >= best) continue;
            best = tt, out = uv[0] * (1 - u - v) + uv[1] * u + uv[2] * v;
        }
    }
    return out;
}

// One dab of the brush on the model: on every face near it, into the picture where that face
// lies (so it carries on across the seams between pieces of the layout).
// One dab of paint at a point of the object (the world), on the faces towards `eye` (the
// mirrored dab: towards the eye mirrored). The stencil: only where the surface shows inside
// it on the screen (the mirrored dab: where its mirror image does, `back` taking it there).
auto App::dab3D(Object& o, const glm::vec3& at, float radius, const glm::vec3* eyeAt, const glm::mat4* back) -> void {
    Image& im = *paint_.image;
    const glm::mat4 m = objectMatrix(o);
    const glm::vec3 eye = eyeAt ? *eyeAt : eye_;
    const bool stencil = stencilOn_ && stencil_.size() >= 3;
    const glm::vec2 size(float(im.w), float(im.h));
    const float inner = std::clamp(hardness_, 0.0f, 0.999f) * radius;
    for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
        const auto& fv = o.mesh.faces[f];
        for (const auto& t : triangulate(o.mesh, int(f))) {
            glm::vec3 w[3];
            glm::vec2 px[3];
            for (int k = 0; k < 3; ++k) {
                w[k] = glm::vec3(m * glm::vec4(o.mesh.verts[size_t(t[size_t(k)])], 1.0f));
                px[k] = o.uvs[f][size_t(std::find(fv.begin(), fv.end(), t[size_t(k)]) - fv.begin())] * size;
            }
            const glm::vec3 lo = glm::min(w[0], glm::min(w[1], w[2])) - radius, hi = glm::max(w[0], glm::max(w[1], w[2])) + radius;
            if (glm::any(glm::lessThan(at, lo)) || glm::any(glm::greaterThan(at, hi))) continue;
            const glm::vec3 n = glm::cross(w[1] - w[0], w[2] - w[0]);
            const float area3 = glm::length(n) * 0.5f;
            if (area3 < 1e-12f || glm::dot(n, eye - w[0]) <= 0 || std::abs(glm::dot(glm::normalize(n), at - w[0])) > radius) continue;
            const float areaPx = std::abs((px[1].x - px[0].x) * (px[2].y - px[0].y) - (px[2].x - px[0].x) * (px[1].y - px[0].y)) * 0.5f;
            if (areaPx < 1e-6f) continue;
            // The dab's middle on the picture, and how far it reaches there.
            const glm::vec3 nn = glm::normalize(n), c = at - nn * glm::dot(nn, at - w[0]);
            const glm::vec3 v0 = w[1] - w[0], v1 = w[2] - w[0], v2 = c - w[0];
            const float d00 = glm::dot(v0, v0), d01 = glm::dot(v0, v1), d11 = glm::dot(v1, v1), d20 = glm::dot(v2, v0), d21 = glm::dot(v2, v1), den = d00 * d11 - d01 * d01;
            const float bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den;
            const glm::vec2 mid = px[0] * (1 - bv - bw) + px[1] * bv + px[2] * bw;
            const float reach = radius * std::sqrt(areaPx / area3) * 1.5f + 2.0f;
            glm::vec2 plo = glm::max(glm::min(px[0], glm::min(px[1], px[2])) - 2.0f, mid - reach), phi = glm::min(glm::max(px[0], glm::max(px[1], px[2])) + 2.0f, mid + reach);
            plo = glm::max(plo, glm::vec2(0.0f)), phi = glm::min(phi, size - 1.0f);
            const float twice = areaPx * 2.0f * ((px[1].x - px[0].x) * (px[2].y - px[0].y) - (px[2].x - px[0].x) * (px[1].y - px[0].y) > 0 ? 1.0f : -1.0f);
            for (int y = int(plo.y); y <= int(phi.y); ++y)
                for (int x = int(plo.x); x <= int(phi.x); ++x) {
                    const glm::vec2 q(float(x) + 0.5f, float(y) + 0.5f);
                    float l[3];
                    bool ok = true;
                    for (int k = 0; k < 3; ++k) {
                        const glm::vec2 a = px[(k + 1) % 3], b = px[(k + 2) % 3];
                        l[k] = ((b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y)) / twice;
                        ok &= l[k] * std::abs(twice) / std::max(glm::length(b - a), 1e-6f) >= -1.5f; // (a pixel and a half past the edge: no seams)
                    }
                    if (!ok) continue;
                    const glm::vec3 p = w[0] * l[0] + w[1] * l[1] + w[2] * l[2];
                    const float d = glm::length(p - at);
                    if (d >= radius) continue;
                    if (stencil) {
                        ImVec2 sp;
                        if (!project(back ? glm::vec3(*back * glm::vec4(p, 1.0f)) : p, sp) || !insidePolygon(sp, stencil_)) continue;
                    }
                    const float s = d <= inner ? 1.0f : 1.0f - (d - inner) / (radius - inner);
                    paint_.stroke.put(im, x, y, s * s * (3 - 2 * s));
                }
        }
    }
    uploaded_[o.id].dirty = true;
}

// The brush at a point of the view: starts a stroke on what's there, or carries one on.
auto App::paintAt(ImVec2 mouse, bool start) -> void {
    const Ray ray = mouseRay(mouse);
    if (start) {
        const Hit h = pickObject(ray, 0, true);
        if (!h.object) return;
        selected_ = {h.object}, active_ = h.object;
        Object* o = readyToPaint(h.object);
        if (!o) return;
        beginStroke(*o, false);
    }
    if (!paint_.active || paint_.in2D) return;
    Object* o = find(paint_.object);
    if (!o) return;
    const Hit h = pickObject(ray, o->id, true);
    if (!h.object) {
        paint_.haveLast = false;
        return;
    }
    const glm::vec3 at = ray.origin + ray.dir * h.t;
    const float radius = brushPx_ * pixelSize(at);
    const float gap = glm::distance(at, paint_.last);
    // (painting both sides: the same dab mirrored across the object's own Y, its left and right)
    const glm::mat4 m = objectMatrix(*o), mirror = m * glm::scale(glm::mat4(1.0f), glm::vec3(1, -1, 1)) * glm::inverse(m);
    const glm::vec3 eyeMirrored = glm::vec3(mirror * glm::vec4(eye_, 1.0f));
    auto dab = [&](const glm::vec3& p) {
        dab3D(*o, p, radius);
        if (paintMirror_) dab3D(*o, glm::vec3(mirror * glm::vec4(p, 1.0f)), radius, &eyeMirrored, &mirror);
    };
    if (paint_.haveLast && gap < radius * 8) {
        const int steps = std::max(1, int(std::ceil(gap / (radius * 0.3f))));
        for (int i = 1; i <= steps; ++i) dab(glm::mix(paint_.last, at, float(i) / float(steps)));
    } else
        dab(at);
    paint_.last = at, paint_.haveLast = true;
}

auto App::pickColor(const Object& o, glm::vec2 uv) -> void {
    const glm::vec4 c = sample(*o.picture, uv * glm::vec2(o.picture->w, o.picture->h));
    paintColor_ = glm::vec4(glm::vec3(c), paintColor_.a);
}

auto App::fillAt(Object& o, glm::vec2 uv) -> void {
    Image& img = changePicture(o);
    floodFill(img, glm::ivec2(uv * glm::vec2(img.w, img.h)), glm::vec4(glm::vec3(paintColor_), 1.0f), fillTolerance_);
    paint_.image.reset();
}

// ── Sculpting ──

// The object ready to sculpt: its finishing touches made part of it, and made finer (the same
// shape) if it has too few points to push about.
auto App::readyToSculpt(uint32_t id) -> Object* {
    Object* o = find(id);
    if (!o) return nullptr;
    if (o->locked) return say("That object is locked"), nullptr;
    pushUndo();
    if (o->hasFinish()) {
        o->mesh = finishedMesh(*o);
        o->mirror = 0, o->round = 0, o->smoothLevels = 0;
    }
    handMade(*o);
    if (finerForSculpting(o->mesh)) {
        o->smooth = true;
        say(std::format("{} was made finer to sculpt: {} points now, the same shape", o->name, o->mesh.verts.size()));
    }
    touched(o->id);
    return o;
}

auto App::endSculpt() -> void {
    if (!sculpt_.active) return;
    sculpt_ = {};
}

// The brush at a point of the view: starts a stroke on what's there, or carries one on.
auto App::sculptAt(ImVec2 mouse, bool start, bool ctrl, bool shift) -> void {
    const Ray ray = mouseRay(mouse);
    const auto toLocal = [](const Object& o, float radius) { return radius * 3.0f / glm::max(glm::abs(o.scale.x) + glm::abs(o.scale.y) + glm::abs(o.scale.z), 1e-6f); };
    const glm::vec3 other(1, -1, 1); // (the other side: the object's Y turned round)
    if (start) {
        const Hit h = pickObject(ray, 0, true);
        if (!h.object) return;
        selected_ = {h.object}, active_ = h.object;
        Object* o = readyToSculpt(h.object);
        if (!o) return;
        sculpt_.active = true, sculpt_.object = o->id;
        sculpt_.mesh = std::make_unique<SculptMesh>(o->mesh);
        if (sculptBrush_ == SculptBrush::Grab && !shift) { // (made finer: the same shape, so the same spot)
            sculpt_.start = ray.origin + ray.dir * h.t;
            const glm::vec3 at = glm::vec3(glm::inverse(objectMatrix(*o)) * glm::vec4(sculpt_.start, 1.0f));
            const float radius = toLocal(*o, sculptPx_ * pixelSize(sculpt_.start));
            for (int side = 0; side < (sculptMirror_ ? 2 : 1); ++side) {
                sculpt_.held[side] = grabbed(o->mesh, side ? at * other : at, radius);
                for (const auto& [i, w] : sculpt_.held[side]) sculpt_.from[side].push_back(o->mesh.verts[size_t(i)]);
            }
        }
    }
    if (!sculpt_.active) return;
    Object* o = find(sculpt_.object);
    if (!o) return endSculpt();
    const glm::mat4 inv = glm::inverse(objectMatrix(*o));
    if (!sculpt_.from[0].empty()) { // grabbing: it follows the mouse across the view
        const auto p = hitPlane(ray, sculpt_.start, forwardOf(yaw_, pitch_));
        if (!p) return;
        const glm::vec3 d = glm::vec3(inv * glm::vec4(*p - sculpt_.start, 0.0f));
        moveGrabbed(o->mesh, sculpt_.held[0], sculpt_.from[0], d);
        if (sculptMirror_) moveGrabbed(o->mesh, sculpt_.held[1], sculpt_.from[1], d * other);
        return touched(o->id);
    }
    const Hit h = pickObject(ray, o->id, true);
    if (!h.object) {
        sculpt_.haveLast = false;
        return;
    }
    const glm::vec3 at = ray.origin + ray.dir * h.t;
    const float radius = sculptPx_ * pixelSize(at);
    const SculptBrush brush = shift ? SculptBrush::Smooth : sculptBrush_;
    const auto dab = [&](const glm::vec3& world) {
        Dab d{glm::vec3(inv * glm::vec4(world, 1.0f)), toLocal(*o, radius), sculptStrength_, ctrl};
        sculptDab(o->mesh, *sculpt_.mesh, brush, d);
        if (sculptMirror_) d.at *= other, sculptDab(o->mesh, *sculpt_.mesh, brush, d);
    };
    // A dab every quarter of the brush along the way, not one a frame (fast strokes stay even).
    const float gap = glm::distance(at, sculpt_.last);
    if (!sculpt_.haveLast) dab(at);
    else if (gap >= radius * 0.25f && gap < radius * 8) {
        const int steps = int(gap / (radius * 0.25f));
        for (int i = 1; i <= steps; ++i) dab(glm::mix(sculpt_.last, at, float(i) / float(steps)));
    } else if (gap < radius * 0.25f)
        return; // (not far enough for the next dab yet)
    else
        dab(at);
    sculpt_.last = at, sculpt_.haveLast = true;
    touched(o->id);
}

auto App::sculptViewport(const ImGuiIO& io) -> void {
    if (sculpt_.waiting) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) sculpt_.waiting = false;
        else if (pickObject(mouseRay(io.MousePos), 0, true).object) sculpt_.waiting = false, sculptAt(io.MousePos, true, io.KeyCtrl, io.KeyShift);
        return;
    }
    if (sculpt_.active) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return endSculpt();
        return sculptAt(io.MousePos, false, io.KeyCtrl, io.KeyShift);
    }
    if (!mouseInView(io) || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (pickObject(mouseRay(io.MousePos), 0, true).object) sculptAt(io.MousePos, true, io.KeyCtrl, io.KeyShift);
    else sculpt_.waiting = sculptBrush_ != SculptBrush::Grab;
}

// ── Picture layers ──

auto App::editLayer() -> PictureLayer* {
    Object* o = find(ledit_.object);
    return o && ledit_.index >= 0 && ledit_.index < int(o->layers.size()) ? &o->layers[size_t(ledit_.index)] : nullptr;
}

// A picture put on the selected object as a new layer, unlocked to be moved: a sticker on the
// surface in the middle of the view, or pinned onto the flat picture.
auto App::startFloating(Image img) -> void {
    Object* o = selected_.empty() ? nullptr : readyToPaint(active_);
    if (!o) return say("Select the object to put the picture on first");
    if (img.empty()) return;
    lockLayer();
    pushUndo();
    PictureLayer l;
    l.name = std::format("Picture {}", o->layers.size() + 1);
    l.image = std::make_shared<Image>(std::move(img));
    l.sticker = pasteOnSurface_;
    const float aspect = float(l.image->h) / float(std::max(l.image->w, 1));
    if (l.sticker) {
        const glm::mat4 m = objectMatrix(*o), inv = glm::inverse(m);
        const Ray ray = mouseRay(ImVec2((viewMin_.x + viewMax_.x) * 0.5f, (viewMin_.y + viewMax_.y) * 0.5f));
        const Hit h = pickObject(ray, o->id, true);
        if (h.object) l.face = h.face, l.centre = glm::vec3(inv * glm::vec4(ray.origin + ray.dir * h.t, 1.0f));
        else { // (the face looking most at you)
            float best = -2;
            const glm::vec3 toEye = glm::normalize(glm::vec3(inv * glm::vec4(-viewDir(), 0.0f)));
            for (size_t f = 0; f < o->mesh.faces.size(); ++f)
                if (const float d = glm::dot(faceNormal(o->mesh, int(f)), toEye); d > best) best = d, l.face = int(f), l.centre = faceCenter(o->mesh, int(f));
        }
        // Upright as you see it, a third as big as the object.
        const glm::vec3 f = viewDir(), right = glm::normalize(glm::cross(f, std::abs(f.z) > 0.99f ? glm::vec3(std::cos(glm::radians(yaw_)), std::sin(glm::radians(yaw_)), 0) : glm::vec3(0, 0, 1)));
        l.up = glm::normalize(glm::vec3(inv * glm::vec4(glm::cross(right, f), 0.0f)));
        const glm::vec3 d = boundsMax(o->mesh) - boundsMin(o->mesh);
        l.width = std::max({d.x, d.y, d.z}) * 0.35f, l.height = l.width * aspect;
    } else {
        const float W = float(o->picture->w), H = float(o->picture->h);
        const float k = std::min(W * 0.6f / float(l.image->w), H * 0.6f / float(l.image->h));
        const glm::vec2 half(float(l.image->w) * k * 0.5f, float(l.image->h) * k * 0.5f), c(W * 0.5f, H * 0.5f);
        l.corners = {c - half, glm::vec2(c.x + half.x, c.y - half.y), c + half, glm::vec2(c.x - half.x, c.y + half.y)};
    }
    o->layers.push_back(std::move(l));
    ledit_ = {};
    ledit_.object = o->id, ledit_.index = int(o->layers.size()) - 1, ledit_.fresh = true;
    paintTool_ = PaintTool::Picture;
    if (tab_ != Tab::Paint) setTab(Tab::Paint);
    say(pasteOnSurface_ ? "Drag it round the model (it bends over edges); corners size it (Shift: stretch), the round handle turns it. Enter locks it."
                        : "Drag its corners in the Picture panel to fit (Shift: keep its shape), inside to move it. Enter locks it.");
}

auto App::floatFromFile(const std::string& path) -> void {
    Image img;
    if (!loadImageFile(path, img)) return say("Couldn't load " + path);
    startFloating(std::move(img));
}

auto App::pasteFromClipboard() -> void {
    for (const char* mime : {"image/png", "image/bmp", "image/jpeg"}) {
        if (!SDL_HasClipboardData(mime)) continue;
        size_t n = 0;
        void* data = SDL_GetClipboardData(mime, &n);
        if (!data) continue;
        int w = 0, h = 0, c = 0;
        stbi_uc* px = stbi_load_from_memory(static_cast<const stbi_uc*>(data), int(n), &w, &h, &c, 4);
        SDL_free(data);
        if (!px) continue;
        Image img;
        img.w = w, img.h = h, img.px.assign(px, px + size_t(w) * size_t(h) * 4);
        stbi_image_free(px);
        return startFloating(std::move(img));
    }
    say("Nothing to paste: copy a picture in another app first (or use Add a picture from a file)");
}

// Enter: it stays where it is, a layer (click it in the Picture panel to move it again).
auto App::lockLayer() -> void {
    if (!editLayer()) return void(ledit_ = {});
    ledit_ = {};
    say("Locked in place, as a layer (see Properties). Click it in the Picture panel to move it again.");
}

auto App::unlockLayer(uint32_t object, int index) -> void {
    lockLayer();
    ledit_.object = object, ledit_.index = index;
    paintTool_ = PaintTool::Picture;
    if (const PictureLayer* l = editLayer()) say(std::format("{} unlocked: {}. Enter locks it again.", l->name, l->sticker ? "drag it on the model" : "drag its corners in the Picture panel"));
}

// Esc: a picture just pasted goes again; one unlocked is locked again.
auto App::cancelFloating() -> void {
    Object* o = find(ledit_.object);
    if (o && editLayer() && ledit_.fresh) {
        o->layers.erase(o->layers.begin() + ledit_.index);
        ledit_ = {};
        return say("Taken away");
    }
    lockLayer();
}

auto App::deleteLayer(Object& o, int i) -> void {
    if (i < 0 || i >= int(o.layers.size())) return;
    pushUndo();
    o.layers.erase(o.layers.begin() + i);
    ledit_ = {};
}

// The layer painted into the painting for good (it can't be moved after).
auto App::mergeLayer(Object& o, int i) -> void {
    if (i < 0 || i >= int(o.layers.size()) || !o.picture) return;
    pushUndo();
    Object only = o;
    only.layers = {o.layers[size_t(i)]};
    o.picture = std::make_shared<Image>(composePicture(only));
    o.layers.erase(o.layers.begin() + i);
    ledit_ = {};
}

// The handles of a sticker being moved, in the world: its corners (top-left, top-right,
// bottom-right, bottom-left), its turning handle, its middle — on its face's plane.
auto App::stickerHandles(const Object& o, const PictureLayer& l, std::array<glm::vec3, 6>& out) -> bool {
    glm::vec3 right, up, n;
    if (!stickerFrame(o.mesh, l, right, up, n)) return false;
    const int f = stickerFace(o.mesh, l);
    const glm::vec3 c = l.centre - n * glm::dot(l.centre - faceCenter(o.mesh, f), n);
    const float hw = l.width * 0.5f, hh = l.height * 0.5f;
    const glm::mat4 m = objectMatrix(o);
    auto w = [&](const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); };
    out = {w(c - right * hw + up * hh), w(c + right * hw + up * hh), w(c + right * hw - up * hh), w(c - right * hw - up * hh), w(c + up * (hh + std::max(hw, hh) * 0.4f)), w(c)};
    return true;
}

auto App::stickerHandle(const Object& o, const PictureLayer& l, ImVec2 mouse) -> int {
    std::array<glm::vec3, 6> h;
    if (!stickerHandles(o, l, h)) return -1;
    ImVec2 s[6];
    for (int i = 0; i < 6; ++i)
        if (!project(h[size_t(i)], s[i])) return -1;
    for (int i = 0; i < 4; ++i)
        if (std::abs(mouse.x - s[i].x) < 10 && std::abs(mouse.y - s[i].y) < 10) return i;
    if (std::hypot(mouse.x - s[4].x, mouse.y - s[4].y) < 11) return 5;
    return insidePolygon(mouse, {s[0], s[1], s[2], s[3]}) ? 4 : -1;
}

// Moving a sticker on the model: it slides over the surface (onto other faces too), its
// corners size it, its handle turns it.
auto App::stickerMouse(const ImGuiIO& io) -> void {
    Object* o = find(ledit_.object);
    PictureLayer* l = editLayer();
    if (!o || !l) return;
    const glm::mat4 inv = glm::inverse(objectMatrix(*o));
    const Ray ray = mouseRay(io.MousePos);
    if (ledit_.drag >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return void(ledit_.drag = -1);
        const PictureLayer& s = ledit_.start;
        glm::vec3 right, up, n;
        stickerFrame(o->mesh, s, right, up, n);
        if (ledit_.drag == 4) {
            const Hit h = pickObject(ray, o->id, true);
            if (!h.object) return;
            const glm::vec3 p(inv * glm::vec4(ray.origin + ray.dir * h.t, 1.0f));
            l->face = h.face, l->up = s.up;
            glm::vec3 r2, u2, n2;
            stickerFrame(o->mesh, *l, r2, u2, n2);
            l->up = u2; // (its top carried over onto the new face)
            l->centre = p - r2 * ledit_.offset.x - u2 * ledit_.offset.y;
            settleSticker(o->mesh, *l);
            return;
        }
        const Ray local{glm::vec3(inv * glm::vec4(ray.origin, 1.0f)), glm::normalize(glm::vec3(inv * glm::vec4(ray.dir, 0.0f)))};
        const auto hp = hitPlane(local, s.centre, n);
        if (!hp) return;
        const glm::vec3 d = *hp - s.centre;
        const float dx = glm::dot(d, right), dy = glm::dot(d, up);
        if (ledit_.drag < 4) {
            if (io.KeyShift) l->width = std::max(2 * std::abs(dx), 0.002f), l->height = std::max(2 * std::abs(dy), 0.002f);
            else {
                const float k = std::max(std::abs(dx) / (s.width * 0.5f), std::abs(dy) / (s.height * 0.5f));
                l->width = std::max(s.width * k, 0.002f), l->height = std::max(s.height * k, 0.002f);
            }
            message_ = std::format("{:.0f} x {:.0f} mm", l->width * 1000, l->height * 1000), messageAt_ = now();
        } else {
            float a = std::atan2(dx, dy);
            if (snap_ != io.KeyCtrl) a = glm::radians(snapTo(glm::degrees(a), 15.0f));
            l->up = up * std::cos(a) + right * std::sin(a);
            message_ = std::format("Turned {:.0f} degrees", glm::degrees(a)), messageAt_ = now();
        }
        return;
    }
    if (!mouseInView(io) || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    const int h = stickerHandle(*o, *l, io.MousePos);
    if (h < 0 && !pickObject(ray, o->id, true).object) return;
    pushUndo();
    ledit_.start = *l, ledit_.drag = h < 0 ? 4 : h, ledit_.offset = {0, 0};
    if (h == 4) { // (held where it was grabbed)
        glm::vec3 right, up, n;
        stickerFrame(o->mesh, *l, right, up, n);
        const Ray local{glm::vec3(inv * glm::vec4(ray.origin, 1.0f)), glm::normalize(glm::vec3(inv * glm::vec4(ray.dir, 0.0f)))};
        if (auto hp = hitPlane(local, l->centre, n)) ledit_.offset = {glm::dot(*hp - l->centre, right), glm::dot(*hp - l->centre, up)};
    }
}

// A cheap fingerprint of what the object's picture shows (its painting, its layers, its shape).
auto App::pictureStamp(const Object& o) -> uint64_t {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    };
    const void* base = o.picture.get();
    mix(&base, sizeof base);
    const uint64_t version = versions_[o.id];
    mix(&version, sizeof version);
    for (const PictureLayer& l : o.layers) {
        const void* im = l.image.get();
        mix(&im, sizeof im), mix(&l.sticker, 1), mix(&l.visible, 1), mix(&l.opacity, 4), mix(l.corners.data(), 32);
        mix(&l.face, 4), mix(&l.centre, 12), mix(&l.up, 12), mix(&l.width, 4), mix(&l.height, 4);
    }
    return h;
}

// The object's picture on the graphics card (its painting with its layers on top): sent again
// when it changes — while painting or dragging, at most 20 times a second.
auto App::uploadPicture(const Object& o, bool now_) -> void {
    if (!o.picture) return;
    Uploaded& u = uploaded_[o.id];
    const bool checker = tab_ == Tab::Layout && layChecker_;
    const uint64_t stamp = pictureStamp(o) ^ (checker ? 0x9E3779B97F4A7C15ull : 0);
    const bool first = !paintTex_.count(o.id);
    if (!first && u.stamp == stamp && !u.dirty) return;
    if (!first && !now_ && now() - u.at < 0.05) return;
    if (checker) { // (squares that show where the layout stretches or squashes the picture)
        static Image board;
        if (board.empty()) {
            board = makeImage(512, 512, {1, 1, 1, 1});
            for (int y = 0; y < 512; ++y)
                for (int x = 0; x < 512; ++x) {
                    const bool dark = ((x / 32) + (y / 32)) % 2;
                    const glm::vec3 c = glm::mix(glm::vec3(0.95f, 0.55f, 0.25f), glm::vec3(0.3f, 0.55f, 0.9f), float(y) / 511.0f) * (dark ? 0.55f : 1.0f);
                    uint8_t* p = board.at(x, y);
                    p[0] = uint8_t(c.r * 255), p[1] = uint8_t(c.g * 255), p[2] = uint8_t(c.b * 255);
                }
        }
        paintTex_[o.id] = r_->createTexture(std::format("fry:paint:{}", o.id), 512, 512, board.px.data());
    } else if (o.layers.empty()) {
        owner_.erase(o.id);
        paintTex_[o.id] = r_->createTexture(std::format("fry:paint:{}", o.id), uint32_t(o.picture->w), uint32_t(o.picture->h), o.picture->px.data());
    } else {
        const Image shown = composePicture(o, &owner_[o.id]);
        paintTex_[o.id] = r_->createTexture(std::format("fry:paint:{}", o.id), uint32_t(shown.w), uint32_t(shown.h), shown.px.data());
    }
    u = {stamp, now(), false};
}

// ───────────────────────────── the Layout tab ─────────────────────────────

// The frames an object's faces are laid out in on its picture, moved, turned and sized by
// hand (whole frames, or their corners); what's painted goes with them unless you say not.

// What's picked, as (face, corner) pairs of its layout.
auto App::layoutPicked(const Object& o, const std::vector<std::vector<int>>& islands) const -> std::vector<std::pair<int, int>> {
    std::vector<std::pair<int, int>> out;
    if (lay_.object != o.id) return out;
    if (lay_.corners) return {lay_.points.begin(), lay_.points.end()};
    for (int i : lay_.frames)
        if (i < int(islands.size()))
            for (int f : islands[size_t(i)])
                for (size_t c = 0; c < o.mesh.faces[size_t(f)].size(); ++c) out.push_back({f, int(c)});
    return out;
}

// A corner and every other on the same point of the model at the same place on the picture.
auto App::layoutCorner(const Object& o, int f, int c) const -> std::set<std::pair<int, int>> {
    std::set<std::pair<int, int>> out;
    const int v = o.mesh.faces[size_t(f)][size_t(c)];
    const glm::vec2 at = o.uvs[size_t(f)][size_t(c)];
    for (size_t g = 0; g < o.mesh.faces.size(); ++g)
        for (size_t k = 0; k < o.mesh.faces[g].size(); ++k)
            if (o.mesh.faces[g][k] == v && glm::distance(o.uvs[g][k], at) < 1e-5f) out.insert({int(g), int(k)});
    return out;
}

auto App::layoutSelectAll() -> void {
    Object* o = selected_.empty() ? nullptr : find(active_);
    if (!o || !o->uvsFit()) return;
    if (lay_.object != o->id) lay_ = {}, lay_.object = o->id;
    lay_.frames.clear(), lay_.points.clear();
    if (lay_.corners)
        for (size_t f = 0; f < o->uvs.size(); ++f)
            for (size_t c = 0; c < o->uvs[f].size(); ++c) lay_.points.insert({int(f), int(c)});
    else
        for (size_t i = 0; i < uvIslands(o->mesh, o->uvs).size(); ++i) lay_.frames.insert(int(i));
}

// The picked corners moved by `to` (picture pixels), painting carried along if wanted.
auto App::layoutChange(Object& o, const std::vector<std::vector<glm::vec2>>& from, const std::shared_ptr<const Image>& picture, const std::vector<std::pair<int, int>>& picked, const std::function<glm::vec2(glm::vec2)>& to, bool final) -> void {
    const glm::vec2 size(float(o.picture->w), float(o.picture->h));
    o.uvs = from;
    std::set<int> faces;
    for (auto [f, c] : picked) o.uvs[size_t(f)][size_t(c)] = to(from[size_t(f)][size_t(c)] * size) / size, faces.insert(f);
    touched(o.id);
    if (layKeepPaint_ && picture && (final || now() - lay_.remapAt > 0.06)) {
        Image img = *picture;
        remapPicture(img, o.mesh, from, o.uvs, {faces.begin(), faces.end()});
        o.picture = std::make_shared<Image>(std::move(img));
        lay_.remapAt = now();
    }
}

// Buttons' changes (turn, flip, straighten): about the middle of what's picked.
auto App::layoutApply(const std::function<glm::vec2(glm::vec2, glm::vec2)>& to) -> void {
    Object* o = selected_.empty() ? nullptr : find(active_);
    if (!o || !o->picture || !o->uvsFit()) return;
    const auto picked = layoutPicked(*o, uvIslands(o->mesh, o->uvs));
    if (picked.empty()) return say("Pick some frames first (click them in the Picture panel or on the model)");
    pushUndo();
    const glm::vec2 size(float(o->picture->w), float(o->picture->h));
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (auto [f, c] : picked) lo = glm::min(lo, o->uvs[size_t(f)][size_t(c)] * size), hi = glm::max(hi, o->uvs[size_t(f)][size_t(c)] * size);
    const glm::vec2 mid = (lo + hi) * 0.5f;
    layoutChange(*o, o->uvs, o->picture, picked, [&](glm::vec2 p) { return to(p, mid); }, true);
}

auto App::layoutStraighten() -> void {
    Object* o = selected_.empty() ? nullptr : find(active_);
    if (!o || !o->picture || !o->uvsFit()) return;
    const auto picked = layoutPicked(*o, uvIslands(o->mesh, o->uvs));
    std::set<int> faces;
    for (auto [f, c] : picked) faces.insert(f);
    float best = -1, angle = 0; // (its longest edge, laid level or upright)
    for (int f : faces)
        for (size_t i = 0; i < o->uvs[size_t(f)].size(); ++i) {
            const glm::vec2 d = (o->uvs[size_t(f)][(i + 1) % o->uvs[size_t(f)].size()] - o->uvs[size_t(f)][i]) * glm::vec2(o->picture->w, o->picture->h);
            if (const float l = glm::length(d); l > best) best = l, angle = std::atan2(d.y, d.x);
        }
    const float quarter = glm::half_pi<float>(), turn = std::round(angle / quarter) * quarter - angle;
    layoutApply([&](glm::vec2 p, glm::vec2 m) { return m + glm::vec2(std::cos(turn) * (p - m).x - std::sin(turn) * (p - m).y, std::sin(turn) * (p - m).x + std::cos(turn) * (p - m).y); });
}

// Laid out again by itself, what's painted carried over onto the new layout.
auto App::layoutAgain(Object& o) -> void {
    pushUndo();
    const auto before = o.uvs;
    o.uvs = autoUnwrap(o.mesh);
    if (layKeepPaint_ && o.picture && before.size() == o.uvs.size()) {
        Image img = *o.picture;
        std::vector<int> all(o.mesh.faces.size());
        for (size_t f = 0; f < all.size(); ++f) all[f] = int(f);
        Image fresh = makeImage(img.w, img.h, glm::vec4(glm::vec3(o.color), 1.0f));
        fresh.px = img.px;
        remapPicture(fresh, o.mesh, before, o.uvs, all);
        o.picture = std::make_shared<Image>(std::move(fresh));
    }
    lay_.frames.clear(), lay_.points.clear();
    touched(o.id);
    say("Laid out again" + std::string(layKeepPaint_ ? ", what's painted carried over" : ""));
}

// In the 3D view: clicking the model picks the frame its face is in (Shift: add).
auto App::layoutViewport(const ImGuiIO& io) -> void {
    if (!mouseInView(io) || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    const Hit h = pickObject(mouseRay(io.MousePos), 0, true);
    if (!h.object) {
        if (!io.KeyShift) lay_.frames.clear(), lay_.points.clear();
        return;
    }
    if (h.object != active_ || selected_.empty()) selected_ = {h.object}, active_ = h.object, lay_ = {}, lay_.object = h.object;
    Object* o = find(h.object);
    if (!o || !o->uvsFit()) return;
    const auto islands = uvIslands(o->mesh, o->uvs);
    for (size_t i = 0; i < islands.size(); ++i)
        if (std::find(islands[i].begin(), islands[i].end(), h.face) != islands[i].end()) {
            if (lay_.corners) lay_.corners = false, lay_.points.clear();
            if (!io.KeyShift) lay_.frames.clear();
            if (io.KeyShift && lay_.frames.count(int(i))) lay_.frames.erase(int(i));
            else lay_.frames.insert(int(i));
        }
}

auto App::layoutOverlay(ImDrawList* dl) -> void {
    Object* o = selected_.empty() ? nullptr : find(active_);
    if (!o || !o->uvsFit() || lay_.object != o->id) return;
    const auto picked = layoutPicked(*o, uvIslands(o->mesh, o->uvs));
    std::set<int> faces;
    for (auto [f, c] : picked) faces.insert(f);
    const glm::mat4 m = objectMatrix(*o);
    for (int f : faces) {
        std::vector<ImVec2> pts;
        bool ok = true;
        for (int v : o->mesh.faces[size_t(f)]) {
            ImVec2 q;
            ok &= project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(v)], 1.0f)), q);
            pts.push_back(q);
        }
        if (ok) dl->AddPolyline(pts.data(), int(pts.size()), kAccent, ImDrawFlags_Closed, 2.0f);
    }
}

auto App::layoutToolbox(float w) -> void {
    grid(int(std::size(kLayTools)), 38.0f, [&](int i) {
        const LayToolInfo& t = kLayTools[i];
        const std::string id = std::format("##{}", t.name), tip = std::format("{}  ({})\n{}", t.name, t.key, t.hint);
        if (iconButton(id.c_str(), t.icon, layTool_ == t.tool, tip.c_str())) layTool_ = t.tool;
    });
    ImGui::Spacing();
    ImGui::TextDisabled("Pick");
    if (wideButton("##frames", Icon::Faces, "Frames", !lay_.corners, w, "Whole frames: faces laid out together (Tab)")) lay_.corners = false, lay_.points.clear();
    if (wideButton("##corners", Icon::Points, "Corners", lay_.corners, w, "Corners of the frames, to bend them (Tab)")) lay_.corners = true, lay_.frames.clear();
}

auto App::layoutOptions(float w) -> void {
    for (const LayToolInfo& t : kLayTools)
        if (t.tool == layTool_) ImGui::SeparatorText(t.name);
    ImGui::TextDisabled("What's painted");
    int keep = layKeepPaint_ ? 0 : 1;
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("##keep", &keep, "Stays on the model (moves too)\0Stays on the picture\0")) layKeepPaint_ = keep == 0;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stays on the model: the model looks the same, frames just get more or less of the picture.\n"
                          "Stays on the picture: the model shows whatever the frame is moved over (to fit a frame onto something drawn).");
    ImGui::Checkbox("Show squares on the model", &layChecker_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Squares instead of the painting: where they're stretched or squashed, so is the picture");
    ImGui::SeparatorText("What's picked");
    const float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    auto turn = [&](float deg) {
        const float a = glm::radians(deg);
        layoutApply([a](glm::vec2 p, glm::vec2 m) { return m + glm::vec2(std::cos(a) * (p - m).x - std::sin(a) * (p - m).y, std::sin(a) * (p - m).x + std::cos(a) * (p - m).y); });
    };
    if (ImGui::Button("Turn left", ImVec2(half, 0))) turn(-90);
    ImGui::SameLine();
    if (ImGui::Button("Turn right", ImVec2(half, 0))) turn(90);
    if (ImGui::Button("Flip across", ImVec2(half, 0))) layoutApply([](glm::vec2 p, glm::vec2 m) { return glm::vec2(2 * m.x - p.x, p.y); });
    ImGui::SameLine();
    if (ImGui::Button("Flip up-down", ImVec2(half, 0))) layoutApply([](glm::vec2 p, glm::vec2 m) { return glm::vec2(p.x, 2 * m.y - p.y); });
    if (ImGui::Button("Bigger", ImVec2(half, 0))) layoutApply([](glm::vec2 p, glm::vec2 m) { return m + (p - m) * 1.25f; });
    ImGui::SameLine();
    if (ImGui::Button("Smaller", ImVec2(half, 0))) layoutApply([](glm::vec2 p, glm::vec2 m) { return m + (p - m) * 0.8f; });
    if (ImGui::Button("Straighten", ImVec2(w, 0))) layoutStraighten();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns it so its longest edge is level or upright");
}

auto App::layoutProperties(float w) -> void {
    Object* o = selected_.empty() ? nullptr : find(active_);
    if (!o) return void(ImGui::TextWrapped("Click an object: its picture's layout shows in the Picture panel."));
    ImGui::TextColored(accentText(), "%s", o->name.c_str());
    if (!o->picture || !o->uvsFit()) {
        if (ImGui::Button("Lay it out", ImVec2(w, 0))) readyToPaint(o->id);
        return;
    }
    const auto islands = uvIslands(o->mesh, o->uvs);
    ImGui::TextDisabled("%zu frames, %zu faces, picture %d x %d", islands.size(), o->mesh.faces.size(), o->picture->w, o->picture->h);
    if (ImGui::Button("Pick all (Ctrl+A)", ImVec2(w, 0))) layoutSelectAll();
    if (ImGui::Button("Lay it all out again", ImVec2(w, 0))) layoutAgain(*o);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fry3DX lays it out by itself again (what's painted carried over, if it stays on the model)");
    ImGui::Spacing();
    ImGui::TextWrapped("Each frame is a part of the model laid flat on its picture. Make a frame bigger to give that part more detail, "
                       "turn it to run a pattern the way you want, or move corners to bend it.");
}

} // namespace fryapp
