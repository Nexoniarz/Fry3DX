// Fry3DX: the view: the camera, touch, what's shown (and kept), picking, the gizmo, drawing it.
#include "app.h"

using namespace fry;
namespace stdfs = std::filesystem;

namespace fryapp {

// ───────────────────────────── touch: phones and tablets ─────────────────────────────

// How much bigger than its pixels the UI is drawn.
auto App::touchScale() const -> float {
    if (fry_uiscale.f() > 0) return fry_uiscale.f();
    if (!touch()) return 1.0f;
    const float density = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    return std::clamp((density > 0 ? density : 2.0f) * 0.9f, 1.0f, 4.0f) * std::clamp(fry_uisize.f(), 0.5f, 2.0f);
}

auto App::touchFrame(ImGuiIO& io) -> void {
    if (shiftLatch_) io.KeyShift = true;
    if (ctrlLatch_) io.KeyCtrl = true;
    ImGui::GetStyle().FontScaleMain = 1.0f; // (the sizes come from touchScale)
    if (gest_.rightUp) { // (the long press's right-click: let go the frame after)
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        gest_.rightUp = false;
    }
    // The fingers on the screen now (where, in the UI's units).
    std::vector<ImVec2> fingers;
    int devices = 0;
    if (SDL_TouchID* ids = SDL_GetTouchDevices(&devices)) {
        for (int d = 0; d < devices; ++d) {
            int n = 0;
            if (SDL_Finger** f = SDL_GetTouchFingers(ids[d], &n)) {
                for (int i = 0; i < n; ++i) fingers.push_back(ImVec2(f[i]->x * io.DisplaySize.x, f[i]->y * io.DisplaySize.y));
                SDL_free(f);
            }
        }
        SDL_free(ids);
    }
    auto inView = [&](ImVec2 p) { return p.x >= viewMin_.x && p.y >= viewMin_.y && p.x < viewMax_.x && p.y < viewMax_.y; };
    if (fingers.size() >= 2) {
        const ImVec2 mid((fingers[0].x + fingers[1].x) * 0.5f, (fingers[0].y + fingers[1].y) * 0.5f);
        const float spread = std::max(std::hypot(fingers[0].x - fingers[1].x, fingers[0].y - fingers[1].y), 1.0f);
        if (!gest_.two && !drag_.active && inView(mid) && !ImGui::IsAnyItemActive()) { // two fingers: the first's tap or drag is off
            gest_.two = true, gest_.mid = mid, gest_.spread = spread;
            boxing_ = lassoing_ = touchOrbit_ = tapDraw_ = false;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        }
        if (gest_.two) {
            panBy(ImVec2(mid.x - gest_.mid.x, mid.y - gest_.mid.y));
            dist_ = std::clamp(dist_ * gest_.spread / spread, 0.01f, 5000.0f);
            gest_.mid = mid, gest_.spread = spread;
        }
        gest_.downAt = -1;
        return;
    }
    if (fingers.empty()) gest_.two = false;
    if (fingers.size() != 1 || gest_.two) return void(gest_.downAt = -1);
    // One finger held still: a right-click there (menus in the panels, orbit in the view).
    const ImVec2 p = fingers[0];
    if (gest_.downAt < 0) gest_.downAt = now(), gest_.downPos = p, gest_.longDone = false;
    if (std::hypot(p.x - gest_.downPos.x, p.y - gest_.downPos.y) > 12) gest_.downAt = now() + 1e9; // (moved: not a long press)
    if (!gest_.longDone && now() - gest_.downAt > 0.55 && !drag_.active) {
        gest_.longDone = true;
        boxing_ = lassoing_ = touchOrbit_ = tapDraw_ = false;
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        gest_.rightUp = true;
    }
}

// The bar along the bottom of the view: big buttons, in rows if the view's narrow.
auto App::touchBar() -> void {
    const float maxW = std::max(viewMax_.x - viewMin_.x - 16, 120.0f);
    ImGui::SetNextWindowPos(ImVec2((viewMin_.x + viewMax_.x) * 0.5f, viewMax_.y - 6), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("##touchbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
    if (now() - messageAt_ < 4.0) ImGui::PushTextWrapPos(maxW), ImGui::TextColored(accentText(), "%s", message_.c_str()), ImGui::PopTextWrapPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
    const ImGuiStyle& st = ImGui::GetStyle();
    float x = 0; // (where the row's got to)
    auto button = [&](const char* label, bool on = false, const char* tip = nullptr) {
        const float w = ImGui::CalcTextSize(label).x + st.FramePadding.x * 2;
        if (x > 0 && x + st.ItemSpacing.x + w <= maxW) ImGui::SameLine(), x += st.ItemSpacing.x;
        else x = 0;
        x += w;
        if (on) pushAccent();
        const bool pressed = ImGui::Button(label);
        if (on) popAccent();
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return pressed;
    };
    if (button("Undo")) undo();
    if (button("Redo")) redo();
    if (button("Shift", shiftLatch_, "Held down until tapped again: adds to the selection, picks loops, scales both sides")) shiftLatch_ = !shiftLatch_;
    if (button("Ctrl", ctrlLatch_, "Held down until tapped again: takes from the selection, snaps")) ctrlLatch_ = !ctrlLatch_;
    if (tab_ == Tab::Model) {
        if (button(mode_ == Mode::Edit ? "Done" : "Edit shape", mode_ == Mode::Edit)) mode_ == Mode::Edit ? leaveEdit() : enterEdit();
        if (button("Delete")) deleteSelected();
    }
    if (button("Frame")) frameSelected();
    if (button("Views", viewsOpen_)) viewsOpen_ = !viewsOpen_;
    if (button("Panels", panelsOpen_, "Layers, Properties (and the Picture when painting)")) panelsOpen_ = !panelsOpen_;
    ImGui::PopStyleVar();
    ImGui::End();
}

// ───────────────────────────── the camera ─────────────────────────────

// metres across the view's height at the target
auto App::setView(View v) -> void {
    const float size = viewHeight();
    view_ = v;
    switch (v) {
    case View::Front: yaw_ = 180, pitch_ = 0; break; // from +X, looking back at the front
    case View::Back: yaw_ = 0, pitch_ = 0; break;
    case View::Left: yaw_ = -90, pitch_ = 0; break; // from +Y (the model's left side)
    case View::Right: yaw_ = 90, pitch_ = 0; break;
    case View::Top: yaw_ = 0, pitch_ = 89.9f; break; // front up the screen
    case View::Bottom: yaw_ = 0, pitch_ = -89.9f; break;
    default: break;
    }
    // Side views: nearly flat (a narrow lens from far away), the same size on screen.
    fov_ = v == View::Perspective ? 50.0f : 2.0f;
    dist_ = size / (2.0f * std::tan(glm::radians(fov_) * 0.5f));
}

auto App::leaveFlatView() -> void {
    if (view_ == View::Perspective) return;
    const float size = viewHeight();
    view_ = View::Perspective, fov_ = 50.0f;
    dist_ = size / (2.0f * std::tan(glm::radians(fov_) * 0.5f));
}

auto App::frameBox(glm::vec3 lo, glm::vec3 hi) -> void {
    if (lo.x > hi.x) return;
    target_ = (lo + hi) * 0.5f;
    const float radius = std::max(glm::length(hi - lo) * 0.5f, 0.05f);
    dist_ = radius * 1.4f / std::tan(glm::radians(fov_) * 0.5f);
}

auto App::frameAll() -> void {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const Object& o : scene_.objects) {
        if (!o.visible) continue;
        const glm::mat4 m = objectMatrix(o);
        for (const glm::vec3& v : o.mesh.verts) {
            const glm::vec3 w(m * glm::vec4(v, 1.0f));
            lo = glm::min(lo, w), hi = glm::max(hi, w);
        }
    }
    frameBox(lo, hi);
}

auto App::frameSelected() -> void {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const glm::vec3& w : selectionPoints()) lo = glm::min(lo, w), hi = glm::max(hi, w);
    if (lo.x > hi.x) return frameAll();
    frameBox(lo, hi);
}

auto App::mouseInView(const ImGuiIO& io) const -> bool {
    const ImVec2 m = io.MousePos;
    return m.x >= viewMin_.x && m.y >= viewMin_.y && m.x < viewMax_.x && m.y < viewMax_.y && !io.WantCaptureMouse;
}

auto App::orbitBy(ImVec2 d) -> void {
    if (std::abs(d.x) + std::abs(d.y) > 0.5f) leaveFlatView();
    yaw_ -= d.x * 0.3f;
    pitch_ = std::clamp(pitch_ + d.y * 0.3f, -89.9f, 89.9f);
}

// Turning the camera where it stands (right drag): what it looks at moves, the eye doesn't.
auto App::lookBy(ImVec2 d) -> void {
    if (std::abs(d.x) + std::abs(d.y) > 0.5f) leaveFlatView();
    const glm::vec3 eye = target_ - viewDir() * dist_;
    yaw_ -= d.x * 0.2f;
    pitch_ = std::clamp(pitch_ + d.y * 0.2f, -89.9f, 89.9f);
    target_ = eye + viewDir() * dist_;
}

// What the camera looks at now: the object or the ground straight ahead (not further than the
// scene reaches). The point it turns round, how fast it moves and how near it draws follow it.
auto App::refocus() -> void {
    const glm::vec3 f = viewDir(), eye = target_ - f * dist_;
    const Ray ray{eye, f};
    float t = -1.0f;
    if (const Hit h = pickObject(ray); h.object) t = h.t;
    else if (auto g = hitPlane(ray, glm::vec3(0.0f), glm::vec3(0, 0, 1))) t = std::min(glm::distance(eye, *g), std::max(glm::length(eye) * 2.0f, 50.0f));
    if (t > 0.0f) dist_ = std::clamp(t, 0.01f, 5000.0f), target_ = eye + f * dist_;
}

// How far an object's mesh reaches (worked out again when its shape changes).
auto App::reachOf(const Object& o) -> const Reach& {
    Reach& r = radii_[o.id];
    if (r.version == shapeKey(o) && r.version) return r;
    r = {shapeKey(o)};
    if (!o.mesh.verts.empty()) r.lo = boundsMin(o.mesh), r.hi = boundsMax(o.mesh);
    for (int a = 0; a < 3; ++a)
        if (o.mirror & (1 << a)) r.lo[a] = std::min(r.lo[a], -r.hi[a]), r.hi[a] = std::max(r.hi[a], -r.lo[a]);
    for (const glm::vec3& v : o.mesh.verts) r.radius = std::max(r.radius, glm::length(v));
    return r;
}

// How far the nearest thing (an object, the ground) is from the eye: the near plane and the
// shade in corners go by it, so looking past something close doesn't break them. To each
// object's box (not a ball round it: inside a long flat one's ball, it would come to nothing).
auto App::nearestThing() -> float {
    float best = std::abs(eye_.z); // (the ground)
    for (const Object& o : scene_.objects) {
        if (!o.visible) continue;
        const Reach& r = reachOf(o);
        const glm::mat4 m = objectMatrix(o);
        const glm::vec3 local = glm::inverse(m) * glm::vec4(eye_, 1.0f);
        best = std::min(best, glm::distance(eye_, glm::vec3(m * glm::vec4(glm::clamp(local, r.lo, r.hi), 1.0f))));
    }
    for (const Reference& ref : scene_.references) {
        if (!ref.visible || (ref.ownViewOnly && !lookingAt(ref.axis))) continue;
        const Picture& pic = picture(ref);
        if (!pic.ok) continue;
        const auto c = referenceCorners(ref, pic.aspect);
        const glm::vec3 u = c[1] - c[0], v = c[3] - c[0], d = eye_ - c[0];
        const float s = std::clamp(glm::dot(d, u) / std::max(glm::dot(u, u), 1e-12f), 0.0f, 1.0f), t = std::clamp(glm::dot(d, v) / std::max(glm::dot(v, v), 1e-12f), 0.0f, 1.0f);
        best = std::min(best, glm::distance(eye_, c[0] + u * s + v * t));
    }
    return std::max(best, 0.001f);
}

auto App::panBy(ImVec2 d) -> void {
    const glm::vec3 f = viewDir(), right = glm::normalize(glm::cross(f, std::abs(f.z) > 0.99f ? glm::vec3(std::cos(glm::radians(yaw_)), std::sin(glm::radians(yaw_)), 0) : glm::vec3(0, 0, 1)));
    const glm::vec3 up = glm::cross(right, f);
    const float perPixel = viewHeight() / std::max(ImGui::GetIO().DisplaySize.y, 1.0f);
    target_ += (-right * d.x + up * d.y) * perPixel * fry_movespeed.f();
}

auto App::updateCamera(ImGuiIO& io) -> void {
    const bool inView = mouseInView(io);
    // Orbit: right drag. Pan: middle drag (or Shift + right). Zoom: the wheel.
    const bool rightDown = ImGui::IsMouseDown(ImGuiMouseButton_Right), midDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    // Right drag turns the camera where it stands (as in games, Unreal, Unity); Alt + right drag
    // turns it round what's picked (or what it looks at). A finger on a phone: round, still.
    const bool orbitCamera = fry_camera.i() == 1; // (View > Camera: round what you look at, as in Blender)
    if (inView && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !io.KeyShift) {
        if (io.KeyAlt || touch() || orbitCamera) {
            orbiting_ = true;
            if (!selected_.empty() && !touch() && !orbitCamera) { // (round the middle of what's picked)
                glm::vec3 lo(1e30f), hi(-1e30f);
                for (const Object& o : scene_.objects)
                    if (selected_.count(o.id)) lo = glm::min(lo, o.position), hi = glm::max(hi, o.position);
                const glm::vec3 eye = target_ - viewDir() * dist_, pivot = (lo + hi) * 0.5f;
                if (glm::distance(eye, pivot) > 1e-4f) {
                    const glm::vec3 f = glm::normalize(pivot - eye);
                    yaw_ = glm::degrees(std::atan2(f.y, f.x)), pitch_ = glm::degrees(std::asin(std::clamp(-f.z, -1.0f, 1.0f)));
                    target_ = pivot, dist_ = glm::distance(eye, pivot);
                }
            }
        } else looking_ = true;
    }
    if (inView && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right)))) panning_ = true, orbiting_ = looking_ = false;
    if (!rightDown) {
        if (looking_) refocus();
        orbiting_ = looking_ = false;
    }
    if (!midDown && !rightDown) panning_ = false;
    const ImVec2 d = io.MouseDelta;
    if (orbiting_ && (d.x != 0 || d.y != 0)) orbitBy(d);
    if (looking_ && (d.x != 0 || d.y != 0)) lookBy(d);
    if (panning_ && (d.x != 0 || d.y != 0)) panBy(d);
    if (inView && io.MouseWheel != 0 && drag_.active && softSel_ && mode_ == Mode::Edit) { // the soft selection's reach, not zoom
        softReach_ = std::clamp(softReach_ * std::pow(1.15f, io.MouseWheel), 0.001f, 1000.0f);
        findSoft();
        message_ = std::format("Soft selection reaches {:.3f} m (wheel)", softReach_), messageAt_ = now();
    } else if (inView && io.MouseWheel != 0 && io.KeyShift) { // Shift + wheel: how fast every way of moving goes
        const float speed = std::clamp(fry_movespeed.f() * std::pow(1.25f, io.MouseWheel), 0.05f, 20.0f);
        keel::execute(std::format("fry_movespeed {:.3g}", speed));
        message_ = std::format("Moving speed: x{:.2g} (Shift + wheel)", speed), messageAt_ = now();
    } else if (inView && io.MouseWheel != 0) {
        // The wheel moves the camera straight forward or back (where it looks), a fifth of the
        // way to what's ahead each notch, but never less than a little of the scene's size: so it
        // doesn't creep to a stop in front of a surface (it goes through). In a flat view: closer.
        // The orbiting camera: closer to (or further from) what it turns round.
        if (view_ != View::Perspective || orbitCamera) dist_ = std::clamp(dist_ * std::pow(0.85f, io.MouseWheel), 0.01f, 5000.0f);
        else {
            float size = 0.0f; // (the biggest object's size: the least step is a little of it)
            for (const Object& o : scene_.objects)
                size = std::max(size, reachOf(o).radius * std::max({std::abs(o.scale.x), std::abs(o.scale.y), std::abs(o.scale.z)}));
            const float step = std::clamp(std::max(dist_ * 0.2f, size * 0.05f), 0.001f, 500.0f) * fry_movespeed.f();
            target_ += viewDir() * step * io.MouseWheel;
            refocus();
        }
    }
    // Moving: the arrow keys any time, and WASD (Q / E down and up) while the right button turns
    // the camera; forward is where it looks (in a flat view: up the screen). Page Up / Down: up
    // and down. Shift: faster. As fast as what's near is far.
    if (!io.WantTextInput && !search_.open && !ImGui::IsAnyItemActive() && !io.KeyCtrl && !io.KeyAlt) {
        auto down = [](ImGuiKey k) { return float(ImGui::IsKeyDown(k)); };
        const float wasd = looking_ ? 1.0f : 0.0f;
        const float x = down(ImGuiKey_RightArrow) - down(ImGuiKey_LeftArrow) + wasd * (down(ImGuiKey_D) - down(ImGuiKey_A));
        const float y = down(ImGuiKey_UpArrow) - down(ImGuiKey_DownArrow) + wasd * (down(ImGuiKey_W) - down(ImGuiKey_S));
        const float z = down(ImGuiKey_PageUp) - down(ImGuiKey_PageDown) + wasd * (down(ImGuiKey_E) - down(ImGuiKey_Q));
        if (x != 0 || y != 0 || z != 0) {
            const glm::vec3 f = viewDir(), right = glm::normalize(glm::cross(f, std::abs(f.z) > 0.99f ? glm::vec3(std::cos(glm::radians(yaw_)), std::sin(glm::radians(yaw_)), 0) : glm::vec3(0, 0, 1)));
            const glm::vec3 ahead = view_ == View::Perspective ? f : glm::cross(right, f);
            const float dt = std::min(io.DeltaTime, 0.1f), speed = std::max(viewScale() * 1.5f, 0.3f) * fry_movespeed.f() * (io.KeyShift ? 3.0f : 1.0f) * dt;
            target_ += (right * x + ahead * y + (view_ == View::Perspective ? glm::vec3(0, 0, z) : glm::vec3(0.0f))) * speed;
            if (view_ != View::Perspective && z != 0) dist_ = std::clamp(dist_ * std::pow(io.KeyShift ? 0.1f : 0.4f, z * dt), 0.01f, 5000.0f); // (flat: closer or further)
            if (view_ == View::Perspective && !looking_) refocus(); // (while turning: when the button's let go)
        }
    }
    settleView();
}

// Where the eye is and how the view maps to the screen, from the camera as it is now: again
// just before drawing, so the model and the lines over it agree (a touch can turn it between).
auto App::settleView() -> void {
    const ImGuiIO& io = ImGui::GetIO();
    const float aspect = io.DisplaySize.x / std::max(io.DisplaySize.y, 1.0f);
    shift_ = glm::vec2((viewMin_.x + viewMax_.x) / std::max(io.DisplaySize.x, 1.0f) - 1.0f, (viewMin_.y + viewMax_.y) / std::max(io.DisplaySize.y, 1.0f) - 1.0f);
    eye_ = target_ - viewDir() * dist_;
    nearest_ = nearestThing();
    nearFar_ = nearFar();
    vp_ = viewProjection(eye_, yaw_, pitch_, fov_, aspect, shift_, nearFar_);
}

// How near and far the view draws (units), going by how far away it looks: close up to a
// 5 mm part, or out to see a whole town, the same share of the depth's precision.
auto App::nearFar() -> glm::vec2 {
    float reach = 1.0f; // (how far from the middle the furthest object reaches, m)
    for (const Object& o : scene_.objects) {
        reach = std::max(reach, glm::length(o.position) + reachOf(o).radius * std::max({std::abs(o.scale.x), std::abs(o.scale.y), std::abs(o.scale.z)}));
    }
    // (the near plane as far out as it can be: the depth's precision is spread between them, so
    // the nearer it is, the more surfaces flicker through each other. Nothing is nearer than the
    // nearest thing, and a quarter of the way to it keeps even the screen's corners clear of it)
    const float nearM = std::clamp(std::max(viewScale() * 0.005f, nearest_ * 0.25f), 1e-5f, 50.0f);
    const float farM = std::max({dist_ * 60.0f, (glm::length(eye_) + reach) * 2.0f, 100.0f});
    return glm::vec2(nearM, farM) * kUnits;
}

auto App::project(const glm::vec3& p, ImVec2& out) const -> bool {
    const glm::vec4 c = vp_ * glm::vec4(p, 1.0f);
    if (c.w < 1e-4f) return false;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    out = ImVec2((c.x / c.w * 0.5f + 0.5f) * size.x, (c.y / c.w * 0.5f + 0.5f) * size.y);
    return true;
}

auto App::mouseRay(ImVec2 m) const -> Ray {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const glm::vec2 ndc(m.x / size.x * 2.0f - 1.0f, m.y / size.y * 2.0f - 1.0f);
    const glm::mat4 inv = glm::inverse(vp_);
    glm::vec4 a = inv * glm::vec4(ndc, 0.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
    a /= a.w, b /= b.w;
    return {glm::vec3(a), glm::normalize(glm::vec3(b - a))};
}

// How many metres a pixel is, at a point.
auto App::pixelSize(const glm::vec3& at) const -> float {
    const float d = std::max(glm::dot(at - eye_, viewDir()), 0.001f);
    return 2.0f * d * std::tan(glm::radians(fov_) * 0.5f) / std::max(ImGui::GetIO().DisplaySize.y, 1.0f);
}

// ───────────────────────────── what's shown ─────────────────────────────

// The mesh as seen: with its rounding, smoothing and mirror — but while its shape is being
// edited, only the mirror (the rest would hide the points you're moving).
// Editable cuts show (but not while its own shape's being edited: the points you move).
// A big one (smoothed into many faces, cut) is made on another thread: what was shown stays
// till it's ready, so the window doesn't stop.
auto App::shownMesh(const Object& o) -> const Mesh& {
    const bool editingThis = mode_ == Mode::Edit && o.id == active_;
    const bool cut = !editingThis && cutKeys_.count(o.id);
    if ((!o.hasFinish() && !cut) || (editingThis && !o.mirror)) return o.mesh;
    Shown& s = shown_[o.id];
    const uint64_t v = shapeKey(o);
    if (s.coming.valid() && s.coming.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [got, tree] = s.coming.get();
        if (s.comingVersion == v && s.comingMirror == editingThis) s.mesh = std::move(got), s.tree = std::move(tree), s.version = v, s.mirrorOnly = editingThis;
    }
    if (s.version == v && v && s.mirrorOnly == editingThis) return s.mesh;
    // (what it'll come to: its faces four times over for each level of smoothing, more for each
    // step of rounding; past a few thousand it's made on another thread, so dragging a setting
    // in Properties doesn't hold up every frame)
    const size_t rounded = !editingThis && o.round > 0.0f ? size_t(std::max(o.roundSteps, 1)) + 1 : 1;
    const size_t faces = o.mesh.faces.size() * rounded * (size_t(1) << (2 * std::clamp(editingThis ? 0 : o.smoothLevels, 0, 6))) * (o.mirror ? 2 : 1);
    const bool big = faces > (editingThis ? 40000u : 6000u) || (cut && o.mesh.faces.size() > 2000); // (the mirror while editing: with each drag, so only when huge)
    if (!big || s.mesh.verts.empty()) { // (small: now; the first time: now too, there's nothing to show yet)
        s.mesh = cut ? fry::cutMesh(scene_, o) : finishedMesh(o, editingThis), s.tree.reset(), s.version = v, s.mirrorOnly = editingThis;
        return s.mesh;
    }
    if (!s.coming.valid()) { // (one at a time for each object: while one's being made, the newest wanted waits for it to land)
        Scene part; // (just it and what cuts it: the thread has its own copies)
        part.objects.push_back(o);
        if (cut)
            for (const Object& c : scene_.objects)
                if (c.cuts == o.id && c.id != o.id) part.objects.push_back(c);
        s.comingVersion = v, s.comingMirror = editingThis;
        s.coming = std::async(std::launch::async, [part = std::move(part), cut, editingThis] {
            Mesh m = cut ? fry::cutMesh(part, part.objects[0]) : finishedMesh(part.objects[0], editingThis);
            auto tree = std::make_shared<const fry::TriangleTree>(m, glm::mat4(1.0f)); // (its picking tree too, while it's here)
            return std::pair{std::move(m), std::move(tree)};
        });
        message_ = "Working it out...", messageAt_ = now();
    }
    return s.mesh;
}

// The key of the mesh actually shown (the one before, while a new one's being made).
auto App::meshKey(const Object& o, const Mesh& mesh) -> uint64_t {
    if (&mesh == &o.mesh) return shapeKey(o);
    auto it = shown_.find(o.id);
    return it != shown_.end() && &it->second.mesh == &mesh ? it->second.version * 2 + it->second.mirrorOnly : shapeKey(o);
}

// (where each cut object and cutter was last frame)
auto App::followCuts() -> void {
    // What's cut moved, turned or sized (not by a drag: that takes them itself), and its cutter
    // didn't: the cutter goes the same way, so the hole stays where it was on it.
    std::map<uint32_t, glm::mat4> at;
    for (Object& c : scene_.objects) {
        const Object* t = fry::cutTarget(scene_, c);
        if (!t) continue;
        const glm::mat4 mt = objectMatrix(*t);
        const auto was = followAt_.find(t->id), cWas = followAt_.find(c.id);
        if (!drag_.active && was != followAt_.end() && cWas != followAt_.end() && was->second != mt && cWas->second == objectMatrix(c) && !c.locked) {
            const glm::mat4 m = mt * glm::inverse(was->second) * objectMatrix(c);
            glm::vec3 skew;
            glm::vec4 persp;
            glm::vec3 scale, pos;
            glm::quat rot;
            if (glm::decompose(m, scale, rot, pos, skew, persp)) c.position = pos, c.rotation = glm::normalize(rot), c.scale = scale, touched(c.id);
        }
        at[t->id] = mt, at[c.id] = objectMatrix(c);
    }
    followAt_ = std::move(at);
    std::map<uint32_t, uint64_t> now;
    for (const Object& c : scene_.objects) {
        const Object* t = fry::cutTarget(scene_, c);
        if (!t || c.mesh.faces.empty()) continue;
        uint64_t& key = now.emplace(t->id, 1469598103934665603ull).first->second;
        key = (key ^ versions_[c.id] ^ (uint64_t(c.id) << 40)) * 1099511628211ull;
        const glm::mat4 m = objectMatrix(c);
        for (int i = 0; i < 16; ++i) {
            uint32_t b;
            std::memcpy(&b, &m[i / 4][i % 4], 4);
            key = (key ^ b) * 1099511628211ull;
        }
    }
    for (const auto& [id, key] : now)
        if (auto it = cutKeys_.find(id); it == cutKeys_.end() || it->second != key) versions_[id] = ++versionClock_;
    for (const auto& [id, key] : cutKeys_)
        if (!now.count(id)) versions_[id] = ++versionClock_;
    cutKeys_ = std::move(now);
}

// Editable cut: the last picked has the others cut out of it, and they stay objects to move.
auto App::cutEditable() -> void {
    if (mode_ == Mode::Edit) leaveEdit();
    Object* a = find(active_);
    if (!a || selected_.size() < 2) return say("Pick the cutters, then (Shift) what they cut last: they stay, to move and change, and the hole follows");
    pushUndo();
    int n = 0;
    for (Object& o : scene_.objects)
        if (selected_.count(o.id) && o.id != a->id) o.cuts = a->id, ++n;
    for (Object& o : scene_.objects) // (one that was cutting the cutters: not a loop of cuts)
        if (o.id == a->id && o.cuts && selected_.count(o.cuts)) o.cuts = 0;
    selected_.erase(a->id), active_ = *selected_.rbegin();
    dirty_ = true;
    say(std::format("{} cut out of {} (editable: move or change {}; Apply makes it for good)", n == 1 ? "1 object" : std::format("{} objects", n), find(a->id)->name, n == 1 ? "it" : "them"));
}

// The cut made for good: one cutter (or all of what's cut out of it) into the shape, gone.
auto App::applyCuts(uint32_t target, uint32_t only) -> void {
    Object* t = find(target);
    if (!t) return;
    pushUndo();
    Scene part = scene_; // (just the cutters wanted)
    for (Object& o : part.objects)
        if (o.cuts == target && only && o.id != only) o.cuts = 0;
    Object* pt = &*std::find_if(part.objects.begin(), part.objects.end(), [&](const Object& o) { return o.id == target; });
    t->mesh = fry::cutMesh(part, *pt);
    t->mirror = 0, t->round = 0, t->smoothLevels = 0, t->creases.clear();
    std::set<uint32_t> gone;
    for (const Object& o : scene_.objects)
        if (o.cuts == target && (!only || o.id == only)) gone.insert(o.id);
    std::erase_if(scene_.objects, [&](const Object& o) { return gone.count(o.id) > 0; });
    for (uint32_t id : gone) selected_.erase(id);
    selected_.insert(target), active_ = target;
    changedShape(*find(target));
    say(std::format("Cut made for good in {} ({} faces)", find(target)->name, find(target)->mesh.faces.size()));
}

// ───────────────────────────── picking ─────────────────────────────

// base: against its own mesh (face numbers of that), not the rounded or smoothed one shown.
auto App::pickObject(const Ray& ray, uint32_t only, bool base) -> Hit {
    Hit best;
    for (const Object& o : scene_.objects) {
        if (!o.visible || (only && o.id != only)) continue;
        const Mesh& mesh = base || (mode_ == Mode::Edit && o.id == active_) ? o.mesh : shownMesh(o);
        // (the ray into the object's own space, where its tree is: t is the same there)
        const glm::mat4 m = objectMatrix(o);
        if (std::abs(glm::determinant(m)) < 1e-12f) continue; // (squashed flat to nothing)
        const glm::mat4 inv = glm::inverse(m);
        const glm::vec3 from(inv * glm::vec4(ray.origin, 1.0f)), dir(inv * glm::vec4(ray.dir, 0.0f));
        if (auto h = treeOf(o, mesh).nearest(from, dir, best.t)) best = {o.id, h->face, h->t};
    }
    return best;
}

// (an object's own mesh, or the one shown)
auto App::treeOf(const Object& o, const Mesh& mesh) -> const fry::TriangleTree& {
    if (auto s = shown_.find(o.id); s != shown_.end() && &s->second.mesh == &mesh && s->second.tree) return *s->second.tree; // (made with it)
    Tree& t = trees_[{o.id, &mesh != &o.mesh}];
    const uint64_t version = meshKey(o, mesh);
    if (!t.tree || t.version != version || !version || t.faces != mesh.faces.size() || t.tree->points().size() != mesh.verts.size())
        t.version = version, t.faces = mesh.faces.size(), t.tree = std::make_shared<const fry::TriangleTree>(mesh, glm::mat4(1.0f));
    return *t.tree;
}

// Can the eye see this point of the object (nothing of it in the way)?
auto App::visible(const Object& o, const glm::vec3& world) -> bool {
    if (xray_) return true;
    const glm::vec3 d = world - eye_;
    const float len = glm::length(d);
    const Hit h = pickObject({eye_, d / len}, o.id);
    return h.t > len - std::max(len * 0.002f, 0.0005f);
}

// (an object's own mesh, or the one shown)
auto App::seen(const Object& o, const Mesh& mesh, int i) -> bool {
    Visibility& v = visibility_[{o.id, &mesh != &o.mesh}];
    const uint64_t version = versions_[o.id];
    const glm::mat4 m = objectMatrix(o);
    if (v.version != version || glm::distance(v.eye, eye_) >= 1e-6f || v.vp != vp_ || v.matrix != m || v.point.size() != mesh.verts.size()) {
        v.version = version, v.eye = eye_, v.vp = vp_, v.matrix = m, v.point.assign(mesh.verts.size(), 0);
        v.eyeIn = std::abs(glm::determinant(m)) < 1e-12f ? eye_ : glm::vec3(glm::inverse(m) * glm::vec4(eye_, 1.0f));
    }
    char& got = v.point[size_t(i)];
    if (got) return got == 1;
    const fry::TriangleTree& tree = treeOf(o, mesh);
    const glm::vec3 p = tree.points()[size_t(i)], w(m * glm::vec4(p, 1.0f));
    const glm::vec4 c = vp_ * glm::vec4(w, 1.0f);
    const float len = glm::length(w - eye_);
    if (c.w < 1e-5f || std::abs(c.x) > c.w || std::abs(c.y) > c.w || len < 1e-6f) return (got = 1); // (off the screen: not drawn anyway)
    // (from the eye to the point in the object's own space: 1 is the point, a hair short of it in the world)
    got = tree.blocked(v.eyeIn, p - v.eyeIn, 1.0f - std::max(0.002f, 0.0005f / len), i) ? 2 : 1;
    return got == 1;
}

auto App::adjacencyOf(const Object& o, const Mesh& mesh) -> const Adjacency& {
    Adjacency& a = adjacency_[{o.id, &mesh != &o.mesh}];
    const uint64_t version = meshKey(o, mesh);
    if (a.version == version && version && a.verts == mesh.verts.size() && a.faces == mesh.faces.size()) return a;
    a.version = version, a.verts = mesh.verts.size(), a.faces = mesh.faces.size();
    a.edges.clear(), a.plane.resize(mesh.faces.size());
    std::unordered_map<uint64_t, int> at;
    at.reserve(mesh.faces.size() * 2);
    for (size_t f = 0; f < mesh.faces.size(); ++f) {
        const auto& fv = mesh.faces[f];
        const glm::vec3 n = faceNormal(mesh, int(f));
        a.plane[f] = glm::vec4(n, fv.empty() ? 0.0f : glm::dot(n, mesh.verts[size_t(fv[0])]));
        for (size_t i = 0; i < fv.size(); ++i) {
            const int p = std::min(fv[i], fv[(i + 1) % fv.size()]), q = std::max(fv[i], fv[(i + 1) % fv.size()]);
            auto [it, fresh] = at.try_emplace(uint64_t(uint32_t(p)) << 32 | uint32_t(q), int(a.edges.size()));
            if (fresh) a.edges.push_back({p, q, int(f), -1});
            else if (a.edges[size_t(it->second)][3] < 0) a.edges[size_t(it->second)][3] = int(f);
        }
    }
    return a;
}

// The edge of the edited object nearest the mouse on screen (that can be seen), or (-1, -1).
auto App::nearestEdge(Object& o, ImVec2 mouse, float within) -> std::pair<int, int> {
    const glm::mat4 m = objectMatrix(o);
    std::pair<int, int> best{-1, -1};
    for (const auto& e : adjacencyOf(o, o.mesh).edges) {
        const int a = e[0], b = e[1];
        ImVec2 sa, sb;
        if (!project(glm::vec3(m * glm::vec4(o.mesh.verts[size_t(a)], 1.0f)), sa) || !project(glm::vec3(m * glm::vec4(o.mesh.verts[size_t(b)], 1.0f)), sb)) continue;
        const float d = distToSegment(mouse, sa, sb);
        if (d < within && (pointSeen(o, a) || pointSeen(o, b))) within = d, best = {a, b};
    }
    return best;
}

auto App::clickSelect(const ImGuiIO& io) -> void {
    const Ray ray = mouseRay(io.MousePos);
    bool add = io.KeyShift, remove = io.KeyCtrl;
    if (Object* o = editing()) {
        // Shapes: Shift picks the whole loop (Shift+Ctrl on an edge: the ring across it), Ctrl
        // adds or takes away one.
        if (io.KeyShift) return loopSelect(io, io.KeyCtrl);
        add = io.KeyCtrl, remove = false;
        const glm::mat4 m = objectMatrix(*o);
        auto toggle = [&](auto& set, auto key) {
            if (remove) set.erase(key);
            else if (add && set.count(key)) set.erase(key);
            else set.insert(key);
        };
        if (!add && !remove) clearElements();
        if (elem_ == Elem::Vertex) {
            int best = -1;
            float bestD = 14.0f;
            for (size_t v = 0; v < o->mesh.verts.size(); ++v) {
                ImVec2 s;
                if (!project(glm::vec3(m * glm::vec4(o->mesh.verts[v], 1.0f)), s)) continue;
                const float d = std::hypot(s.x - io.MousePos.x, s.y - io.MousePos.y);
                if (d < bestD && pointSeen(*o, int(v))) bestD = d, best = int(v);
            }
            if (best >= 0) toggle(selVerts_, best);
        } else if (elem_ == Elem::Edge) {
            const auto e = nearestEdge(*o, io.MousePos, 10.0f);
            if (e.first >= 0) toggle(selEdges_, e);
        } else {
            const Hit h = pickObject(ray, o->id);
            if (h.face >= 0) toggle(selFaces_, h.face);
        }
        return;
    }
    const Hit h = pickObject(ray);
    if (!add && !remove) selected_.clear();
    if (h.object) {
        if (remove || (add && selected_.count(h.object))) selected_.erase(h.object);
        else selected_.insert(h.object), active_ = h.object;
        activeRef_ = 0;
    }
}

// A whole loop: of edges (points), or a ring of faces, through the edge under the mouse.
auto App::loopSelect(const ImGuiIO& io, bool ring) -> void {
    Object* o = editing();
    if (!o) return;
    if (!io.KeyShift) clearElements();
    if (elem_ == Elem::Face) {
        const Hit h = pickObject(mouseRay(io.MousePos), o->id);
        if (h.face < 0) return;
        // The face's edge nearest the mouse: the ring runs across it.
        const auto& fv = o->mesh.faces[size_t(h.face)];
        const glm::mat4 m = objectMatrix(*o);
        std::pair<int, int> best{-1, -1};
        float bestD = 1e30f;
        for (size_t i = 0; i < fv.size(); ++i) {
            ImVec2 sa, sb;
            if (project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(fv[i])], 1.0f)), sa) &&
                project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(fv[(i + 1) % fv.size()])], 1.0f)), sb))
                if (const float d = distToSegment(io.MousePos, sa, sb); d < bestD) bestD = d, best = {fv[i], fv[(i + 1) % fv.size()]};
        }
        if (best.first < 0) return;
        const auto ring = faceRing(o->mesh, best);
        selFaces_.insert(ring.begin(), ring.end());
        if (ring.empty()) selFaces_.insert(h.face);
        return;
    }
    const auto e = nearestEdge(*o, io.MousePos, 14.0f);
    if (e.first < 0) return;
    const auto loop = ring ? edgeRing(o->mesh, e) : edgeLoop(o->mesh, e);
    if (ring) say(std::format("A ring of {} edges", loop.size()));
    if (elem_ == Elem::Edge) selEdges_.insert(loop.begin(), loop.end());
    else
        for (auto [a, b] : loop) selVerts_.insert(a), selVerts_.insert(b);
}

// Selects what's on screen inside a shape (a box, a lasso, the brush).
auto App::regionSelect(const std::function<bool(ImVec2)>& inside, bool add, bool remove, bool clearFirst) -> void {
    auto in = [&](const glm::vec3& w) {
        ImVec2 s;
        return project(w, s) && inside(s);
    };
    if (Object* o = editing()) {
        if (clearFirst) clearElements();
        const glm::mat4 m = objectMatrix(*o);
        std::vector<char> hit(o->mesh.verts.size());
        for (size_t v = 0; v < hit.size(); ++v) hit[v] = in(glm::vec3(m * glm::vec4(o->mesh.verts[v], 1.0f))) && pointSeen(*o, int(v));
        auto apply = [&](auto& set, auto key, bool yes) {
            if (!yes) return;
            if (remove) set.erase(key);
            else set.insert(key);
        };
        if (elem_ == Elem::Vertex)
            for (size_t v = 0; v < hit.size(); ++v) apply(selVerts_, int(v), bool(hit[v]));
        else if (elem_ == Elem::Edge)
            for (auto e : edges(o->mesh)) {
                const glm::vec3 mid(m * glm::vec4((o->mesh.verts[size_t(e.first)] + o->mesh.verts[size_t(e.second)]) * 0.5f, 1.0f));
                apply(selEdges_, e, (hit[size_t(e.first)] && hit[size_t(e.second)]) || (in(mid) && pointSeen(*o, e.first) && pointSeen(*o, e.second)));
            }
        else
            for (size_t f = 0; f < o->mesh.faces.size(); ++f) {
                const glm::vec3 c(m * glm::vec4(faceCenter(o->mesh, int(f)), 1.0f));
                if (!in(c)) continue;
                const glm::vec3 n = glm::transpose(glm::inverse(glm::mat3(m))) * faceNormal(o->mesh, int(f));
                apply(selFaces_, int(f), xray_ || (glm::dot(n, eye_ - c) > 0 && visible(*o, c)));
            }
        return;
    }
    if (clearFirst) selected_.clear();
    (void)add;
    for (const Object& o : scene_.objects) {
        if (!o.visible) continue;
        const glm::vec3 c(objectMatrix(o) * glm::vec4((boundsMin(o.mesh) + boundsMax(o.mesh)) * 0.5f, 1.0f));
        if (!in(c)) continue;
        if (remove) selected_.erase(o.id);
        else selected_.insert(o.id), active_ = o.id;
    }
}

// ───────────────────────────── the gizmo ─────────────────────────────

auto App::hasSelection() -> bool {
    if (mode_ == Mode::Edit) return !selectedPoints().empty();
    return std::any_of(selected_.begin(), selected_.end(), [&](uint32_t id) { const Object* o = find(id); return o && !o->locked; });
}

// Every point of the selection, in the world.
auto App::selectionPoints() -> std::vector<glm::vec3> {
    std::vector<glm::vec3> out;
    if (Object* o = editing()) {
        const glm::mat4 m = objectMatrix(*o);
        for (int v : selectedPoints()) out.push_back(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(v)], 1.0f)));
        return out;
    }
    for (const Object& o : scene_.objects) {
        if (!selected_.count(o.id) || o.locked) continue;
        const glm::mat4 m = objectMatrix(o);
        for (const glm::vec3& v : shownMesh(o).verts) out.push_back(glm::vec3(m * glm::vec4(v, 1.0f)));
    }
    return out;
}

auto App::pivotOfSelection() -> glm::vec3 {
    glm::vec3 c(0.0f);
    int n = 0;
    if (Object* o = editing()) {
        const glm::mat4 m = objectMatrix(*o);
        for (int v : selectedPoints()) c += glm::vec3(m * glm::vec4(o->mesh.verts[size_t(v)], 1.0f)), ++n;
    } else {
        for (const Object& o : scene_.objects)
            if (selected_.count(o.id)) c += o.position, ++n;
    }
    return n ? c / float(n) : glm::vec3(0.0f);
}

// The selection's box, in a gizmo's axes.
auto App::selectionBox(const Axes& a) -> std::pair<glm::vec3, glm::vec3> {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const glm::vec3& p : selectionPoints()) {
        const glm::vec3 c = inFrame(a, p);
        lo = glm::min(lo, c), hi = glm::max(hi, c);
    }
    if (lo.x > hi.x) lo = hi = inFrame(a, pivotOfSelection());
    return {lo, hi};
}

auto App::selectionMiddle(const Axes& a) -> glm::vec3 {
    const auto [lo, hi] = selectionBox(a);
    return fromFrame(a, (lo + hi) * 0.5f);
}

// The gizmo's axes: the object's own (turning with it), the world's, or (shape editing) the
// way the selected faces face — so "up" on a side face is out of that face.
auto App::gizmoAxes() -> Axes {
    Orient o = orient();
    if (mode_ == Mode::Object && tool_ == Tool::Scale) o = Orient::Local; // (sizes are along an object's own axes)
    const Object* a = find(active_);
    if (!a && !selected_.empty()) a = find(*selected_.begin());
    const glm::quat q = a && o != Orient::World ? a->rotation : glm::quat(1, 0, 0, 0);
    Axes local{q * glm::vec3(1, 0, 0), q * glm::vec3(0, 1, 0), q * glm::vec3(0, 0, 1)};
    if (o != Orient::Normal || mode_ != Mode::Edit) return local;
    Object* e = editing();
    if (!e) return local;
    const glm::mat4 m = objectMatrix(*e);
    const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m)));
    const std::set<int> pts = selectedPoints();
    glm::vec3 n(0.0f), along(0.0f);
    for (size_t f = 0; f < e->mesh.faces.size(); ++f) {
        const auto& fv = e->mesh.faces[f];
        const bool whole = elem_ == Elem::Face ? selFaces_.count(int(f)) > 0 : std::any_of(fv.begin(), fv.end(), [&](int v) { return pts.count(v); });
        if (!whole) continue;
        n += nm * faceNormal(e->mesh, int(f)) * std::max(faceArea(e->mesh, int(f)), 1e-6f);
        if (glm::length(along) < 1e-9f) // the first face's longest edge: "across" it
            for (size_t i = 0; i < fv.size(); ++i) {
                const glm::vec3 d = glm::mat3(m) * (e->mesh.verts[size_t(fv[(i + 1) % fv.size()])] - e->mesh.verts[size_t(fv[i])]);
                if (glm::length(d) > glm::length(along)) along = d;
            }
    }
    if (elem_ == Elem::Edge && !selEdges_.empty()) {
        const auto [a0, b0] = *selEdges_.begin();
        along = glm::mat3(m) * (e->mesh.verts[size_t(b0)] - e->mesh.verts[size_t(a0)]);
    }
    if (glm::length(n) < 1e-9f) return local;
    const glm::vec3 z = glm::normalize(n);
    glm::vec3 x = along - z * glm::dot(along, z);
    if (glm::length(x) < 1e-6f) x = glm::cross(std::abs(z.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0), z);
    x = glm::normalize(x);
    return {x, glm::cross(z, x), z};
}

// The extrude / inset handle: the selected faces' middle, their normal.
auto App::faceHandle(glm::vec3& at, glm::vec3& normal) -> bool {
    Object* o = editing();
    if (!o || elem_ != Elem::Face || selFaces_.empty()) return false;
    const glm::mat4 m = objectMatrix(*o);
    glm::vec3 c(0.0f), n(0.0f);
    for (int f : selFaces_) {
        if (f >= int(o->mesh.faces.size())) return false;
        c += glm::vec3(m * glm::vec4(faceCenter(o->mesh, f), 1.0f));
        n += glm::normalize(glm::transpose(glm::inverse(glm::mat3(m))) * faceNormal(o->mesh, f));
    }
    at = c / float(selFaces_.size());
    normal = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0, 0, 1);
    return true;
}

// The round-edges handle: off to the right of the selection's middle.
auto App::bevelHandle(ImVec2& centre, ImVec2& handle) -> bool {
    if (!editing() || edgesToRound().empty()) return false;
    if (!project(drag_.active ? drag_.pivot : pivotOfSelection(), centre)) return false;
    handle = ImVec2(centre.x + 80.0f + (drag_.active ? ImGui::GetIO().MousePos.x - drag_.startMouse.x : 0.0f), centre.y);
    return true;
}

// The scale box's handles: 0-2 the far side along each axis, 4-6 the near side, 3 the middle.
// (Pushed out a little where the box is flat, so they don't sit on top of each other.)
auto App::scaleHandle(const Axes& a, glm::vec3 lo, glm::vec3 hi, int h) -> glm::vec3 {
    const glm::vec3 c = (lo + hi) * 0.5f;
    if (h == 3) return fromFrame(a, c);
    const int k = h % 4 == 3 ? 0 : (h < 3 ? h : h - 4);
    glm::vec3 q = c;
    const float minHalf = pixelSize(fromFrame(a, c)) * 22.0f;
    const float half = std::max((hi[k] - lo[k]) * 0.5f, minHalf);
    q[k] = c[k] + (h < 3 ? half : -half);
    return fromFrame(a, q);
}

// Which handle is under the mouse (-1 none).
auto App::hoverHandle(ImVec2 mouse) -> int {
    if (drag_.active) return drag_.handle;
    if (tool_ == Tool::Extrude || tool_ == Tool::Inset) {
        glm::vec3 at, n;
        if (!faceHandle(at, n)) return -1;
        ImVec2 a, b;
        if (!project(at, a) || !project(at + n * pixelSize(at) * 90.0f, b)) return -1;
        return distToSegment(mouse, a, b) < 10.0f ? 0 : -1;
    }
    if (tool_ == Tool::Bevel) {
        ImVec2 c, h;
        return bevelHandle(c, h) && std::hypot(mouse.x - h.x, mouse.y - h.y) < 12.0f ? 0 : -1;
    }
    if (tool_ != Tool::Move && tool_ != Tool::Rotate && tool_ != Tool::Scale) return -1;
    if (!hasSelection()) return -1;
    const Axes axes = gizmoAxes();
    if (tool_ == Tool::Scale) {
        const auto [lo, hi] = selectionBox(axes);
        for (int h : {3, 0, 1, 2, 4, 5, 6}) {
            ImVec2 s;
            if (project(scaleHandle(axes, lo, hi, h), s) && std::abs(mouse.x - s.x) < 9 && std::abs(mouse.y - s.y) < 9) return h;
        }
        return -1;
    }
    const glm::vec3 p = tool_ == Tool::Rotate ? rotatePivot(axes) : selectionMiddle(axes);
    ImVec2 c;
    if (!project(p, c)) return -1;
    const float len = pixelSize(p) * 90.0f;
    if (tool_ == Tool::Rotate) {
        int best = -1;
        float bestD = 9.0f;
        for (int k = 0; k < 3; ++k) {
            const glm::vec3 u = glm::normalize(glm::cross(axes[size_t(k)], std::abs(axes[size_t(k)].z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
            const glm::vec3 v = glm::cross(axes[size_t(k)], u);
            ImVec2 prev;
            bool havePrev = false;
            for (int i = 0; i <= 48; ++i) {
                const float a = 2 * kPi * float(i) / 48.0f;
                ImVec2 s;
                const glm::vec3 w = p + (u * std::cos(a) + v * std::sin(a)) * len;
                if (!project(w, s)) {
                    havePrev = false;
                    continue;
                }
                if (havePrev && glm::dot(w - p, eye_ - p) > -0.2f * len * glm::length(eye_ - p)) // (the front half only)
                    if (const float d = distToSegment(mouse, prev, s); d < bestD) bestD = d, best = k;
                prev = s, havePrev = true;
            }
        }
        const float r = std::hypot(mouse.x - c.x, mouse.y - c.y);
        if (best < 0 && r > 95.0f && r < 110.0f) best = 3; // the outer ring: about the view
        return best;
    }
    if (std::abs(mouse.x - c.x) < 9 && std::abs(mouse.y - c.y) < 9) return 3;
    for (int k = 0; k < 3; ++k) {
        ImVec2 e;
        if (project(p + axes[size_t(k)] * len, e) && distToSegment(mouse, c, e) < 9.0f) return k;
    }
    return -1;
}

auto App::originPoint() -> glm::vec3 {
    if (Object* o = editing()) return o->position;
    glm::vec3 c(0.0f);
    int n = 0;
    for (const Object& o : scene_.objects)
        if (selected_.count(o.id)) c += o.position, ++n;
    return n ? c / float(n) : c;
}

auto App::startDrag(const ImGuiIO& io, int handle) -> void {
    drag_ = {};
    drag_.active = true, drag_.handle = handle, drag_.tool = tool_;
    drag_.startMouse = io.MousePos;
    drag_.before = scene_;
    pushUndo();
    if (softSel_ && (tool_ == Tool::Move || tool_ == Tool::Rotate || tool_ == Tool::Scale)) findSoft();
    const Ray ray = mouseRay(io.MousePos);
    if (tool_ == Tool::Extrude || tool_ == Tool::Inset) {
        glm::vec3 at, n;
        faceHandle(at, n);
        drag_.pivot = at, drag_.startT = closestOnAxis(ray, at, n);
        drag_.startHit = n;
        return;
    }
    if (tool_ == Tool::Bevel) {
        drag_.pivot = pivotOfSelection();
        drag_.bevelEdges = edgesToRound();
        return;
    }
    drag_.axes = gizmoAxes();
    const Axes& a = drag_.axes;
    if (tool_ == Tool::Scale) {
        const auto [lo, hi] = selectionBox(a);
        const glm::vec3 c = (lo + hi) * 0.5f;
        drag_.handlePoint = scaleHandle(a, lo, hi, handle);
        const int k = handle < 3 ? handle : handle - 4;
        glm::vec3 q = c;
        switch (scalePivot_) {
        case Pivot::Opposite:
            if (handle == 3) q[2] = lo[2];
            else q[k] = handle < 3 ? lo[k] : hi[k];
            drag_.anchor = fromFrame(a, q);
            break;
        case Pivot::Bottom: q[2] = lo[2], drag_.anchor = fromFrame(a, q); break;
        case Pivot::Origin: drag_.anchor = originPoint(); break;
        default: drag_.anchor = fromFrame(a, c); break;
        }
        drag_.pivot = fromFrame(a, c);
        if (handle != 3) drag_.startDist = glm::dot(drag_.handlePoint - drag_.anchor, a[size_t(k)]), drag_.startMid = glm::dot(drag_.handlePoint - drag_.pivot, a[size_t(k)]);
        return;
    }
    drag_.pivot = tool_ == Tool::Rotate ? rotatePivot(a) : selectionMiddle(a);
    if (handle < 3) drag_.startT = closestOnAxis(ray, drag_.pivot, a[size_t(handle)]);
    if (auto hit = hitPlane(ray, drag_.pivot, -viewDir())) drag_.startHit = *hit;
}

// The points within reach of the selection (in the world: the object's size counts), each with
// how much it follows: a grid of the selected points, so a big mesh stays quick.
auto App::findSoft() -> void {
    drag_.soft.clear();
    const Object* o = editing();
    if (!o) return;
    const std::set<int> pts = selectedPoints();
    if (pts.empty()) return;
    const glm::mat4 m = objectMatrix(*o);
    const float r = std::max(softReach_, 1e-5f);
    auto cell = [&](const glm::vec3& p) { return glm::ivec3(glm::floor(p / r)); };
    auto keyOf = [](glm::ivec3 c) { return (int64_t(c.x & 0x1FFFFF) << 42) | (int64_t(c.y & 0x1FFFFF) << 21) | int64_t(c.z & 0x1FFFFF); };
    std::unordered_map<int64_t, std::vector<glm::vec3>> grid;
    for (int v : pts) {
        const glm::vec3 w(m * glm::vec4(o->mesh.verts[size_t(v)], 1.0f));
        grid[keyOf(cell(w))].push_back(w);
    }
    for (size_t v = 0; v < o->mesh.verts.size(); ++v) {
        if (pts.count(int(v))) continue;
        const glm::vec3 w(m * glm::vec4(o->mesh.verts[v], 1.0f));
        const glm::ivec3 c = cell(w);
        float best = r;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                    if (auto it = grid.find(keyOf(c + glm::ivec3(dx, dy, dz))); it != grid.end())
                        for (const glm::vec3& q : it->second) best = std::min(best, glm::distance(w, q));
        if (best < r) drag_.soft.push_back({int(v), sculptFalloff(best, r)});
    }
}

auto App::cancelDrag() -> void {
    if (!drag_.active) return;
    scene_ = drag_.before;
    dropUndo();
    touchAll();
    drag_.active = false;
}

auto App::updateDrag(const ImGuiIO& io) -> void {
    for (ImWchar c : io.InputQueueCharacters) {
        if ((c >= '0' && c <= '9') || (c == '.' && drag_.typed.find('.') == std::string::npos)) drag_.typed += char(c);
        else if (c == '-') drag_.typed = drag_.typed.starts_with('-') ? drag_.typed.substr(1) : "-" + drag_.typed;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !drag_.typed.empty()) drag_.typed.pop_back();
    dragNow(io);
    // (the number by the mouse, as it goes: what you'd read in the status line, where you look)
    if (!message_.empty()) {
        const std::string label = drag_.typed.empty() ? message_ : message_ + "   (typed)";
        const ImVec2 at(io.MousePos.x + 18, io.MousePos.y + 14), size = ImGui::CalcTextSize(label.c_str());
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(at.x - 6, at.y - 3), ImVec2(at.x + size.x + 6, at.y + size.y + 3), IM_COL32(24, 24, 28, 225), 4.0f);
        dl->AddText(at, drag_.typed.empty() ? IM_COL32(235, 235, 240, 255) : kAccent, label.c_str());
    }
    if (liveMirror_ && mode_ == Mode::Edit)
        if (Object* o = editing())
            for (const Object& b : drag_.before.objects)
                if (b.id == o->id) mirrorEdits(*o, b.mesh);
}

// Mirror editing: each point that moved, its twin across the shape's own X (the point that was
// where it is, mirrored) moved the mirrored way. Points on the middle stay on it.
auto App::mirrorEdits(Object& o, const Mesh& before) -> void {
    if (before.verts.size() != o.mesh.verts.size() || before.verts.empty()) return; // (points made or gone: nothing to match)
    const float tol = std::max(glm::length(boundsMax(before) - boundsMin(before)) * 1e-5f, 1e-7f);
    auto key = [&](const glm::vec3& p) {
        const glm::ivec3 c(glm::round(p / tol));
        return (uint64_t(uint32_t(c.x) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(c.y) & 0x1FFFFF) << 21) | uint64_t(uint32_t(c.z) & 0x1FFFFF);
    };
    std::unordered_map<uint64_t, int> at;
    for (size_t i = 0; i < before.verts.size(); ++i) at.emplace(key(before.verts[i]), int(i));
    std::vector<char> moved(before.verts.size());
    for (size_t i = 0; i < moved.size(); ++i) moved[i] = glm::distance(before.verts[i], o.mesh.verts[i]) > tol * 0.01f;
    for (size_t i = 0; i < moved.size(); ++i) {
        if (!moved[i]) continue;
        const glm::vec3 b = before.verts[i];
        if (std::abs(b.x) <= tol) { // (on the middle)
            o.mesh.verts[i].x = 0;
            continue;
        }
        const auto it = at.find(key(glm::vec3(-b.x, b.y, b.z)));
        if (it == at.end() || moved[size_t(it->second)]) continue; // (no twin, or it's moved by hand too)
        const glm::vec3 n = o.mesh.verts[i];
        o.mesh.verts[size_t(it->second)] = glm::vec3(-n.x, n.y, n.z);
    }
}

// What moving snaps to near the mouse (points, edges' or faces' middles), in the world: not
// what's being moved. Marked with a ring.
auto App::snapTarget(ImVec2 mouse, float within) -> std::optional<glm::vec3> {
    std::optional<glm::vec3> best;
    float bestD = within;
    const std::set<int> moving = mode_ == Mode::Edit ? selectedPoints() : std::set<int>{};
    for (const Object& o : scene_.objects) {
        if (!o.visible) continue;
        const bool edited = mode_ == Mode::Edit && o.id == active_;
        if (mode_ != Mode::Edit && selected_.count(o.id)) continue;
        const Mesh& mesh = edited ? o.mesh : shownMesh(o);
        const glm::mat4 m = objectMatrix(o);
        auto tryPoint = [&](const glm::vec3& local) {
            const glm::vec3 w = glm::vec3(m * glm::vec4(local, 1.0f));
            ImVec2 sp;
            if (!project(w, sp)) return;
            if (const float d = std::hypot(sp.x - mouse.x, sp.y - mouse.y); d < bestD) bestD = d, best = w;
        };
        if (snapKind_ == 1) {
            for (size_t v = 0; v < mesh.verts.size(); ++v)
                if (!(edited && moving.count(int(v)))) tryPoint(mesh.verts[v]);
        } else if (snapKind_ == 2) {
            for (const auto& e : adjacencyOf(o, mesh).edges)
                if (!(edited && (moving.count(e[0]) || moving.count(e[1])))) tryPoint((mesh.verts[size_t(e[0])] + mesh.verts[size_t(e[1])]) * 0.5f);
        } else if (snapKind_ == 3) {
            for (const auto& f : mesh.faces) {
                glm::vec3 c(0.0f);
                bool mine = false;
                for (int v : f) c += mesh.verts[size_t(v)], mine |= edited && moving.count(v) > 0;
                if (!mine && !f.empty()) tryPoint(c / float(f.size()));
            }
        }
    }
    if (best) {
        ImVec2 sp;
        if (project(*best, sp)) ImGui::GetForegroundDrawList()->AddCircle(sp, 7.0f, kAccent, 0, 2.0f);
    }
    return best;
}

// The number typed while dragging, if any (just "-" or "." is none yet).
auto App::typed() const -> std::optional<float> {
    if (drag_.typed.empty() || drag_.typed == "-" || drag_.typed == "." || drag_.typed == "-.") return std::nullopt;
    return std::strtof(drag_.typed.c_str(), nullptr);
}

auto App::dragNow(const ImGuiIO& io) -> void {
    const Ray ray = mouseRay(io.MousePos);
    const bool snapping = snap_ != io.KeyCtrl; // Ctrl flips snapping while held
    scene_ = drag_.before; // every frame from where it started: no drift
    const Axes& axes = drag_.axes;
    const glm::vec3 p = drag_.pivot;
    glm::mat4 T(1.0f); // what happens to the selection, in the world
    if (drag_.tool == Tool::Extrude || drag_.tool == Tool::Inset) {
        Object* o = editing();
        if (!o) return;
        const glm::vec3 n = drag_.startHit;
        float d = closestOnAxis(ray, p, n) - drag_.startT;
        if (drag_.tool == Tool::Extrude) {
            if (snapping) d = snapTo(d, snapMove_);
            if (auto v = typed()) d = *v;
            // (the pull in the object's own size, so it follows the mouse whatever its scale)
            const float scale = std::cbrt(std::abs(glm::determinant(glm::mat3(objectMatrix(*o)))));
            extrudeFaces(o->mesh, selFaces_, d / std::max(scale, 1e-6f));
            extrudeAmount_ = d;
            message_ = std::format("Extrude {:.3f} m", d), messageAt_ = now();
        } else {
            float t = std::clamp(-d / (pixelSize(p) * 120.0f), 0.0f, 0.99f);
            if (snapping) t = snapTo(t, 0.05f);
            if (auto v = typed()) t = std::clamp(*v / 100.0f, 0.0f, 0.99f);
            insetFaces(o->mesh, selFaces_, t);
            insetAmount_ = t;
            message_ = std::format("Inset {:.0f}%", t * 100), messageAt_ = now();
        }
        changedShape(*o, true);
        return;
    }
    if (drag_.tool == Tool::Bevel) {
        Object* o = editing();
        if (!o) return;
        float w = std::max(0.0f, io.MousePos.x - drag_.startMouse.x) * pixelSize(p) * 0.5f;
        if (snapping) w = snapTo(w, 0.005f);
        if (auto v = typed()) w = std::max(0.0f, *v);
        if (w > 0) bevelEdges(o->mesh, drag_.bevelEdges, w, bevelSteps_);
        bevelWidth_ = w;
        handMade(*o);
        touched(o->id);
        message_ = std::format("Round edges {:.3f} m, {} step{}", w, bevelSteps_, bevelSteps_ == 1 ? "" : "s"), messageAt_ = now();
        return;
    }
    glm::vec3 objectScale(1.0f);
    float even = 1.0f;
    if (drag_.tool == Tool::Move) {
        glm::vec3 delta(0.0f);
        if (drag_.handle < 3) {
            float t = closestOnAxis(ray, p, axes[size_t(drag_.handle)]) - drag_.startT;
            if (snapping && !snapKind_) t = snapTo(t, snapMove_);
            if (snapping && snapKind_)
                if (auto at = snapTarget(io.MousePos)) t = glm::dot(*at - p, axes[size_t(drag_.handle)]); // (as far along the arrow as it is)
            if (auto v = typed()) t = *v;
            delta = axes[size_t(drag_.handle)] * t;
        } else if (auto hit = hitPlane(ray, p, -viewDir())) {
            delta = *hit - drag_.startHit;
            if (snapping && !snapKind_) delta = glm::vec3(snapTo(delta.x, snapMove_), snapTo(delta.y, snapMove_), snapTo(delta.z, snapMove_));
            if (snapping && snapKind_)
                if (auto at = snapTarget(io.MousePos)) delta = *at - p; // (the arrows' point onto it)
        }
        T = glm::translate(glm::mat4(1.0f), delta);
        message_ = std::format("Move {:.3f}, {:.3f}, {:.3f} m", delta.x, delta.y, delta.z), messageAt_ = now();
    } else if (drag_.tool == Tool::Rotate) {
        ImVec2 c;
        if (!project(p, c)) return;
        const float a0 = std::atan2(drag_.startMouse.y - c.y, drag_.startMouse.x - c.x), a1 = std::atan2(io.MousePos.y - c.y, io.MousePos.x - c.x);
        float deg = glm::degrees(std::remainder(a1 - a0, 2 * kPi));
        const glm::vec3 axis = drag_.handle < 3 ? axes[size_t(drag_.handle)] : -viewDir();
        if (glm::dot(axis, viewDir()) < 0) deg = -deg; // (turning the way the mouse goes round, as seen)
        if (snapping) deg = snapTo(deg, snapAngle_);
        if (auto v = typed()) deg = *v;
        T = glm::translate(glm::mat4(1.0f), p) * glm::rotate(glm::mat4(1.0f), glm::radians(deg), axis) * glm::translate(glm::mat4(1.0f), -p);
        message_ = std::format("Rotate {:.1f} degrees", deg), messageAt_ = now();
    } else if (drag_.tool == Tool::Scale) {
        glm::vec3 s(1.0f);
        // Shift (or Alt) held: from both sides, the middle staying put.
        const bool both = (io.KeyShift || io.KeyAlt) && drag_.handle != 3;
        if (drag_.handle != 3) {
            const int k = drag_.handle < 3 ? drag_.handle : drag_.handle - 4;
            const float from = both ? drag_.startMid : drag_.startDist;
            float dist = from + closestOnAxis(ray, drag_.handlePoint, axes[size_t(k)]);
            if (snapping) dist = both ? snapTo(dist * 2, snapMove_) / 2 : snapTo(dist, snapMove_);
            if (auto v = typed()) dist = (from < 0 ? -1.0f : 1.0f) * std::abs(*v) / (both ? 2.0f : 1.0f); // (the size it's to be, m)
            const float f = std::abs(from) > 1e-6f ? dist / from : 1.0f;
            s[k] = f;
            message_ = std::format("Scale {:.2f}  ({:.3f} m){}", f, std::abs(dist) * (both ? 2 : 1), both ? "  from both sides" : "   (Shift: from both sides)"),
            messageAt_ = now();
        } else {
            even = std::max(0.01f, 1.0f + ((io.MousePos.x - drag_.startMouse.x) - (io.MousePos.y - drag_.startMouse.y)) / 200.0f);
            if (snapping) even = std::max(snapScale_, snapTo(even, snapScale_));
            if (auto v = typed()) even = std::max(0.001f, *v);
            s = glm::vec3(even);
            message_ = std::format("Scale {:.2f} (drag right or up to grow)", even), messageAt_ = now();
        }
        const glm::mat3 basis(axes[0], axes[1], axes[2]);
        const glm::vec3 a = both ? drag_.pivot : drag_.anchor;
        T = glm::translate(glm::mat4(1.0f), a) * glm::mat4(basis) * glm::scale(glm::mat4(1.0f), s) * glm::mat4(glm::transpose(basis)) * glm::translate(glm::mat4(1.0f), -a);
        objectScale = s;
    }
    if (Object* o = editing()) {
        const glm::mat4 m = objectMatrix(*o), local = glm::inverse(m) * T * m;
        for (int v : selectedPoints()) o->mesh.verts[size_t(v)] = glm::vec3(local * glm::vec4(o->mesh.verts[size_t(v)], 1.0f));
        for (const auto& [v, w] : drag_.soft) { // (part of the way, by how near)
            glm::vec3& p = o->mesh.verts[size_t(v)];
            p = glm::mix(p, glm::vec3(local * glm::vec4(p, 1.0f)), w);
        }
        handMade(*o);
        touched(o->id);
        return;
    }
    for (Object& o : scene_.objects) {
        const bool along = !selected_.count(o.id) && o.cuts && selected_.count(o.cuts) && isCutter(o); // (an editable cutter goes with what it cuts)
        if ((!selected_.count(o.id) && !along) || o.locked) continue;
        o.position = glm::vec3(T * glm::vec4(o.position, 1.0f));
        if (drag_.tool == Tool::Rotate) o.rotation = glm::normalize(glm::quat_cast(glm::mat3(T)) * o.rotation);
        if (drag_.tool == Tool::Scale) {
            // On its own axes: the gizmo's where they're the same, else how much each of its
            // axes lies along each of the gizmo's.
            for (int j = 0; j < 3; ++j) {
                const glm::vec3 own = o.rotation * glm::vec3(j == 0, j == 1, j == 2);
                float f = 1.0f;
                for (int k = 0; k < 3; ++k) f += (objectScale[k] - 1.0f) * glm::dot(own, axes[size_t(k)]) * glm::dot(own, axes[size_t(k)]);
                o.scale[j] *= f;
            }
        }
        touched(o.id);
    }
}

// ───────────────────────────── 3D ─────────────────────────────

// Solid: a plain grey room and soft studio light, no shadows or effects — for seeing shapes.
// Rendered: sky, sun, shadows and effects.
// How far the shade in corners reaches (units): a share of how far away you look, so close up it
// stays in the corners instead of smearing over everything in a pattern. (24 at the first view.)
auto App::aoReach() const -> float {
    return std::clamp(viewScale() * kUnits * 0.15f, 0.005f, 400.0f);
}

auto App::applyShading() -> void {
    const Look& k = scene_.look;
    const std::string want =
        shading() == Shading::Rendered
            ? std::format("r_ssao {}; r_ssao_radius {:.2g}; r_bloom {}; r_vignette {}; r_exposure {}; r_saturation {}; r_sharpen {}; r_shadowsoft {}; r_gi {}; r_fog 0; r_skycolor \"{} {} {}\"", k.ao, aoReach(),
                          k.bloom, k.vignette, k.exposure, k.saturation, k.sharpen, k.shadowSoft, kRayLight, k.background.r, k.background.g, k.background.b)
            : std::string("r_ssao 0; r_bloom 0; r_vignette 0; r_exposure 1; r_saturation 1; r_sharpen 0; r_gi 0; r_fog 0; r_skycolor \"0.2 0.2 0.215\"");
    if (want == appliedLook_) return;
    appliedLook_ = want;
    keel::execute(want);
}

auto App::modelOf(const Object& o) -> keel::ModelId {
    const bool editingThis = mode_ == Mode::Edit && o.id == active_;
    const Mesh& mesh = shownMesh(o);
    uint64_t want = (meshKey(o, mesh) * 2 + editingThis) * 2 + painted(o);
    want = (want ^ uint64_t(o.smooth) ^ uint64_t(std::bit_cast<uint32_t>(o.smoothAngle)) << 1) * 1099511628211ull; // (how it's lit: smooth or not, and the angle)
    auto it = models_.find(o.id);
    if (it != models_.end() && it->second.second == want) return it->second.first;
    keel::KmdlModel km;
    keel::KmdlMaterial mat{};
    mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = mat.baseColor[3] = 1.0f;
    const bool withPicture = painted(o);
    if (withPicture) std::snprintf(mat.texture, sizeof mat.texture, "fry:paint:%u", o.id);
    km.materials.push_back(mat);
    const auto normals = cornerNormals(mesh, o.smooth, o.smoothAngle);
    for (size_t f = 0; f < mesh.faces.size(); ++f) {
        const auto& face = mesh.faces[f];
        for (const auto& tri : triangulate(mesh, int(f)))
            for (int k = 0; k < 3; ++k) {
                const size_t corner = size_t(std::find(face.begin(), face.end(), tri[size_t(k)]) - face.begin());
                const glm::vec3 p = mesh.verts[size_t(tri[size_t(k)])], n = normals[f][corner];
                km.indices.push_back(uint32_t(km.vertices.size()));
                const glm::vec2 uv = withPicture ? o.uvs[f][corner] : glm::vec2(0.0f);
                km.vertices.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, {uv.x, uv.y}, {0, 0}});
            }
    }
    if (km.vertices.empty()) km.vertices.push_back({}), km.indices = {0, 0, 0};
    km.submeshes.push_back({0, uint32_t(km.indices.size()), 0});
    const glm::vec3 lo = boundsMin(mesh), hi = boundsMax(mesh);
    std::copy_n(&lo.x, 3, km.header.boundsMin);
    std::copy_n(&hi.x, 3, km.header.boundsMax);
    const keel::ModelId id = r_->updateModel(std::format("fry:object{}", o.id), km);
    models_[o.id] = {id, want};
    return id;
}

auto App::picture(const Reference& ref) -> const Picture& {
    auto it = pictures_.find(ref.path);
    if (it != pictures_.end()) return it->second;
    Picture& p = pictures_[ref.path];
    int w = 0, h = 0, n = 0;
    if (stbi_uc* px = stbi_load(ref.path.c_str(), &w, &h, &n, 4)) {
        r_->createTexture("fry:picture:" + ref.path, uint32_t(w), uint32_t(h), px);
        stbi_image_free(px);
        p.ok = true, p.aspect = float(h) / float(std::max(w, 1));
    } else {
        say("Couldn't load the picture " + ref.path);
    }
    return p;
}

auto App::loadLogo() -> void {
    if (logoTried_) return;
    logoTried_ = true;
    if (auto png = keel::fs::read("branding/logo.png")) {
        int w = 0, h = 0, n = 0;
        if (stbi_uc* px = stbi_load_from_memory(png->data(), int(png->size()), &w, &h, &n, 4)) {
            logo_ = r_->createTexture("fry:logo", uint32_t(w), uint32_t(h), px);
            stbi_image_free(px);
            haveLogo_ = true;
        }
    }
}

auto App::logoImage(float size) -> void {
    if (!haveLogo_) return;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    uiImage(ImGui::GetWindowDrawList(), logo_, at, ImVec2(at.x + size, at.y + size));
}

// A picture of ours in the UI, read the way ImGui's colours are (not turned linear: too dark).
auto App::uiImage(ImDrawList* dl, keel::TextureId t, ImVec2 a, ImVec2 b) -> void {
    const keel::UiRawRead raw = r_->uiRawRead(t);
    if (raw.begin) dl->AddCallback(raw.begin, raw.data);
    dl->AddImage(ImTextureID(r_->uiTexture(t)), a, b);
    if (raw.end) dl->AddCallback(raw.end, raw.data);
}

auto App::draw3D(keel::Renderer& r) -> void {
    settleView();
    followCuts();
    keel::Camera cam;
    cam.origin = eye_ * kUnits, cam.yaw = yaw_, cam.pitch = pitch_, cam.fovY = fov_, cam.shift = shift_, cam.zNear = nearFar_.x, cam.zFar = nearFar_.y;
    r.setCamera(cam);
    const Look& look = scene_.look;
    const bool rendered = shading() == Shading::Rendered;
    const float turn = glm::radians(look.sunTurn), up = glm::radians(look.sunHeight);
    r.setSun(rendered && look.sunStrength > 0, glm::vec3(std::cos(up) * std::cos(turn), std::cos(up) * std::sin(turn), std::sin(up)), look.sunColor * look.sunStrength, true);
    keel::Renderer::Atmosphere atm;
    atm.sky = rendered && look.sky ? "skies/day.ksky" : "";
    atm.fogDensity = 0.0f;
    r.setAtmosphere(atm);
    // Rendered: light from the sky all round. Solid: a studio's — bright from above and the
    // front-left, darker below, so every side of a shape reads differently.
    const keel::ModelLighting light =
        rendered ? keel::ModelLighting{{{0.22f, 0.22f, 0.24f}, {0.13f, 0.13f, 0.15f}, {0.2f, 0.2f, 0.22f}, {0.16f, 0.16f, 0.18f}, {0.3f, 0.31f, 0.35f}, {0.06f, 0.06f, 0.06f}}, 1.0f}
                 : keel::ModelLighting{{{0.5f, 0.49f, 0.47f}, {0.2f, 0.21f, 0.24f}, {0.72f, 0.71f, 0.69f}, {0.3f, 0.31f, 0.34f}, {0.95f, 0.95f, 0.95f}, {0.08f, 0.08f, 0.09f}}, 0.0f};
    const glm::mat4 toUnits = glm::scale(glm::mat4(1.0f), glm::vec3(kUnits));
    // Painted pictures: laid out again if the shape's faces changed, and sent to the card.
    for (Object& o : scene_.objects)
        if (o.picture && !o.uvsFit() && !o.hasFinish()) o.uvs = autoUnwrap(o.mesh), touched(o.id);
    for (const Object& o : scene_.objects)
        if (o.picture) uploadPicture(o, !(paint_.active && paint_.object == o.id));
    if (shading() != Shading::Wire)
        for (const Object& o : scene_.objects) {
            if (!o.visible || isCutter(o)) continue;
            const glm::vec4 tint = painted(o) ? glm::vec4(1.0f) : glm::vec4(glm::pow(o.color, glm::vec3(2.2f)), 1.0f); // (colours are picked as seen: sRGB)
            const keel::Surface surface{o.metal, o.rough};
            r.drawModel(modelOf(o), toUnits * objectMatrix(o), tint, nullptr, &light, rendered, nullptr, keel::LiquidStyle::None, &surface);
        }
    // Editable cutters: see-through, so the hole they make shows (not in a saved picture).
    if (shading() != Shading::Wire && !capturing())
        for (const Object& o : scene_.objects)
            if (o.visible && isCutter(o)) r.drawModel(modelOf(o), toUnits * objectMatrix(o), glm::vec4(0.9f, 0.35f, 0.3f, 0.3f), nullptr, &light, false);
    // Rendered: a ground to stand on and catch the shadows (not part of what you make).
    if (rendered && look.ground) {
        if (!haveGround_) {
            haveGround_ = true;
            keel::KmdlModel km;
            keel::KmdlMaterial mat{};
            mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = mat.baseColor[3] = 1.0f;
            km.materials.push_back(mat);
            const float h = 500.0f;
            for (glm::vec2 c : {glm::vec2(-h, -h), glm::vec2(h, -h), glm::vec2(h, h), glm::vec2(-h, h)}) km.vertices.push_back({{c.x, c.y, -0.0005f}, {0, 0, 1}, {0, 0}, {0, 0}});
            km.indices = {0, 1, 2, 0, 2, 3};
            km.submeshes.push_back({0, 6, 0});
            ground_ = r.updateModel("fry:ground", km);
        }
        r.drawModel(ground_, toUnits, glm::vec4(glm::pow(look.groundColor, glm::vec3(2.2f)), 1.0f), nullptr, &light, false);
    }
    // Reference pictures: flat, see-through, a little behind the plane through the middle.
    for (const Reference& ref : scene_.references) {
        if (!ref.visible || (ref.ownViewOnly && !lookingAt(ref.axis))) continue;
        const Picture& pic = picture(ref);
        if (!pic.ok) continue;
        const auto c = referenceCorners(ref, pic.aspect);
        const glm::vec3 n = glm::normalize(glm::cross(c[1] - c[0], c[3] - c[0]));
        keel::KmdlModel km;
        keel::KmdlMaterial mat{};
        std::snprintf(mat.texture, sizeof mat.texture, "fry:picture:%s", ref.path.c_str());
        mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = 1.0f, mat.baseColor[3] = 1.0f;
        km.materials.push_back(mat);
        const glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        for (int side = 0; side < 2; ++side) // (both sides: seen from behind too)
            for (int k : {0, 1, 2, 0, 2, 3}) {
                const int i = side ? (3 - k + 1) % 4 : k;
                const glm::vec3 p = c[size_t(i)] - n * 0.001f, nn = side ? -n : n;
                km.indices.push_back(uint32_t(km.vertices.size()));
                km.vertices.push_back({{p.x, p.y, p.z}, {nn.x, nn.y, nn.z}, {uv[i].x, uv[i].y}, {0, 0}});
            }
        km.submeshes.push_back({0, uint32_t(km.indices.size()), 0});
        const keel::ModelId id = r.updateModel(std::format("fry:ref{}", ref.id), km);
        const keel::ModelLighting flat{{glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f)}, 0.0f};
        r.drawModel(id, toUnits, glm::vec4(1.0f, 1.0f, 1.0f, ref.opacity), nullptr, &flat, false);
    }
    // The grid on the work plane (the ground, or what a flat view looks at), and the axes.
    // Four sets of lines, each ten times further apart than the one before, each drawn round
    // where you look and fading out before its edge. A line's look goes only by how far apart
    // it and its like are on the screen: too close together, it fades away; far apart, it's
    // brighter and a little wider. So nothing jumps as the view moves or the steps change: new
    // lines come in too close together to see and fade in. Each line is drawn by the widest set
    // it belongs to (once), in pieces, each a pixel wide where it is.
    if (tab_ == Tab::Render ? renderGrid_ : showGrid_) {
        glm::vec3 p, n;
        workPlane(p, n);
        const glm::vec3 u = n.z > 0.5f ? glm::vec3(1, 0, 0) : n.x > 0.5f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 v = glm::cross(n, u);
        const float first = gridStep() * 0.1f;
        const int count = 20, levels = 4;
        const glm::vec3 centre = target_ - n * glm::dot(target_, n);
        const float cu = glm::dot(centre, u), cv = glm::dot(centre, v);
        const float shade = rendered ? 0.0f : 1.0f; // (dark lines on the sunny ground, light ones in the grey room)
        const float dim = rendered ? 0.14f : 0.05f, bright = rendered ? 0.4f : 0.16f;
        auto lift = [&](const glm::vec3& q) { return q + (eye_ - q) * 0.002f; }; // (towards the eye along its line of sight: the same place on the screen, but in front of the ground)
        // One line, at `at` across, from `from` to `to` along; s: how far apart it and its like
        // are; ring: how far its set reaches from the middle (0: no end, the axes).
        auto line = [&](int dir, float at, float from, float to, float s, float ring, glm::vec4 col, bool axis) {
            const glm::vec3 along = dir ? u : v, across = dir ? v : u;
            const float mid = dir ? cu : cv, side = std::abs(at - (dir ? cv : cu));
            // (short pieces near the eye, longer further off: each one's width and fading is
            // worked out at its middle, so it has to be about the same all along it. Each piece
            // reaches a quarter further from the eye than the last: from the eye's nearest
            // point on the line, steps growing with how far the eye is from the line)
            const float near = std::clamp(glm::dot(eye_, along), from, to);
            const glm::vec3 off = eye_ - across * at;
            const float gap = std::max(glm::length(off - along * glm::dot(off, along)), 1e-4f); // (from the eye to the line)
            for (int sideOf : {-1, 1}) {
                const float len = sideOf < 0 ? near - from : to - near;
                if (len <= 0.0f) continue;
                const int pieces = std::clamp(int(std::ceil(std::log1p(len / gap) / std::log(1.25f))), 2, 64);
                for (int j = 0; j < pieces; ++j) {
                    const float t0 = near + float(sideOf) * gap * std::expm1(std::log1p(len / gap) * float(j) / float(pieces));
                    const float t1 = near + float(sideOf) * gap * std::expm1(std::log1p(len / gap) * float(j + 1) / float(pieces));
                    const glm::vec3 a = across * at + along * t0, b = across * at + along * t1, c = (a + b) * 0.5f;
                    // (pixels to the next line like it, across this one as it lies on the screen)
                    const glm::vec4 c0 = vp_ * glm::vec4(c, 1.0f), c1 = vp_ * glm::vec4(c + across * s, 1.0f), c2 = vp_ * glm::vec4(c + along * s, 1.0f);
                    if (c0.w <= 1e-6f || c1.w <= 1e-6f || c2.w <= 1e-6f) continue; // (behind the eye)
                    const glm::vec2 half = glm::vec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y) * 0.5f;
                    const glm::vec2 s0 = glm::vec2(c0) / c0.w * half, s1 = glm::vec2(c1) / c1.w * half, s2 = glm::vec2(c2) / c2.w * half;
                    const glm::vec2 way = s2 - s0;
                    const float wayLen = glm::length(way), d = s1.x - s0.x, e = s1.y - s0.y;
                    const float apart = wayLen > 1e-6f ? std::abs(d * way.y - e * way.x) / wayLen : glm::length(s1 - s0);
                    float alpha = axis ? std::max(glm::smoothstep(1.5f, 6.0f, apart), 0.35f) : glm::smoothstep(5.0f, 20.0f, apart);
                    if (ring > 0.0f) alpha *= glm::smoothstep(ring, ring * 0.6f, std::max(side, std::abs((t0 + t1) * 0.5f - mid)));
                    const float strong = axis ? 1.0f : glm::smoothstep(20.0f, 160.0f, apart);
                    glm::vec4 pc = axis ? col : glm::vec4(glm::vec3(shade), glm::mix(dim, bright, strong));
                    pc.a *= alpha;
                    if (pc.a < 0.004f) continue;
                    const float thick = (1.0f + 0.6f * strong) * kUnits; // (a pixel or so at each end: the pieces meet exactly)
                    r.drawStreak(lift(a) * kUnits, lift(b) * kUnits, pixelSize(a) * thick, pc, keel::kWhiteTexture, false, pixelSize(b) * thick);
                }
            }
        };
        for (int level = 0; level < levels; ++level) {
            const float s = first * std::pow(10.0f, float(level)), ring = count * s;
            const bool top = level == levels - 1;
            for (int dir = 0; dir < 2; ++dir) {
                const float c0 = snapTo(dir ? cv : cu, s), mid = dir ? cu : cv;
                for (int i = -count; i <= count; ++i) {
                    const float at = c0 + float(i) * s;
                    if (std::abs(at) < s * 0.01f) continue; // (the axis: drawn on its own)
                    if (!top && std::abs(snapTo(at, s * 10.0f) - at) < s * 0.01f) continue; // (the next set's)
                    line(dir, at, mid - ring, mid + ring, s, ring, {}, false);
                }
            }
        }
        // The axes: right through, as far as the widest set reaches.
        const float reach = count * first * std::pow(10.0f, float(levels - 1));
        for (int dir = 0; dir < 2; ++dir) {
            const glm::vec4 col = dir ? (n.x > 0.5f ? glm::vec4(0.3f, 0.75f, 0.25f, 0.9f) : glm::vec4(0.9f, 0.25f, 0.25f, 0.9f))
                                      : (n.z > 0.5f ? glm::vec4(0.3f, 0.75f, 0.25f, 0.9f) : glm::vec4(0.25f, 0.45f, 0.95f, 0.9f));
            const float mid = dir ? cu : cv;
            line(dir, 0.0f, mid - reach, mid + reach, first, 0.0f, col, true);
        }
    }
    if (tab_ == Tab::Print) drawPrintBed(r, light);
}

// (the Print tab's: the printer's plate and overhangs)
// On top of the scene: outlines and wires, points, the gizmo, the selection shapes.
// An empty scene: how to start, in the middle of the view (gone once there's anything).
auto App::emptyHints(ImDrawList* dl) -> void {
    if (!scene_.objects.empty() || capturing() || tab_ != Tab::Model) return;
    const char* lines[] = {touch() ? "Nothing here yet: tap + to add a shape" : "Nothing here yet: Add (menu) for a shape, or draw one (D)",
                           touch() ? "Hold a finger on the view for more" : "F3 or Ctrl+Space: find any command by name",
                           touch() ? "" : "Space: tools round the mouse    Shift+Space: views",
                           touch() ? "" : "File > Import: STL, OBJ, 3MF, glTF from elsewhere"};
    const float lh = ImGui::GetTextLineHeightWithSpacing() * 1.25f;
    float y = (viewMin_.y + viewMax_.y) * 0.5f - lh * 2, widest = 0;
    int shown = 0;
    for (const char* l : lines) widest = std::max(widest, ImGui::CalcTextSize(l).x), shown += *l != 0;
    const float cx = (viewMin_.x + viewMax_.x) * 0.5f;
    dl->AddRectFilled(ImVec2(cx - widest * 0.5f - 18, y - 12), ImVec2(cx + widest * 0.5f + 18, y + lh * float(shown) + 6), IM_COL32(28, 28, 32, 200), 8.0f);
    for (int i = 0; i < 4; ++i, y += lh) {
        if (!*lines[i]) continue;
        const ImVec2 size = ImGui::CalcTextSize(lines[i]);
        dl->AddText(ImVec2((viewMin_.x + viewMax_.x - size.x) * 0.5f, y), i == 0 ? IM_COL32(230, 230, 235, 200) : IM_COL32(200, 200, 205, 130), lines[i]);
    }
}

auto App::overlays() -> void {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(viewMin_, viewMax_);
    const ImGuiIO& io = ImGui::GetIO();
    emptyHints(dl);
    // Every edge of a mesh (X-ray, wireframe), or just the edges the eye can see.
    auto lines = [&](const Object& o, const Mesh& mesh, ImU32 col, float width, bool seenOnly) {
        const glm::mat4 m = objectMatrix(o);
        const bool hide = seenOnly && !xray_;
        std::vector<ImVec2> s(mesh.verts.size());
        std::vector<char> ok(mesh.verts.size());
        for (size_t i = 0; i < s.size(); ++i) ok[i] = project(glm::vec3(m * glm::vec4(mesh.verts[i], 1.0f)), s[i]);
        for (const auto& e : adjacencyOf(o, mesh).edges) {
            if (!ok[size_t(e[0])] || !ok[size_t(e[1])] || (hide && !(seen(o, mesh, e[0]) && seen(o, mesh, e[1])))) continue;
            dl->AddLine(s[size_t(e[0])], s[size_t(e[1])], col, width);
        }
    };
    // The outline round a selected object (where it turns away from you, and open edges).
    auto outline = [&](const Object& o, ImU32 col, float width) {
        const Mesh& mesh = shownMesh(o);
        const glm::mat4 m = objectMatrix(o);
        const Adjacency& adj = adjacencyOf(o, mesh);
        // (which faces look at the eye: the eye taken into the object's own space, no point moved)
        const glm::vec3 eye = glm::vec3(glm::inverse(m) * glm::vec4(eye_, 1.0f));
        std::vector<char> facing(adj.plane.size());
        for (size_t f = 0; f < facing.size(); ++f) facing[f] = glm::dot(glm::vec3(adj.plane[f]), eye) > adj.plane[f].w;
        for (const auto& e : adj.edges) {
            const bool silhouette = e[3] >= 0 && facing[size_t(e[2])] != facing[size_t(e[3])], open = e[3] < 0 && facing[size_t(e[2])];
            if (!silhouette && !open) continue;
            if (!xray_ && !(seen(o, mesh, e[0]) && seen(o, mesh, e[1]))) continue; // (not the edges of a pocket behind its rim)
            ImVec2 a, b;
            if (project(glm::vec3(m * glm::vec4(mesh.verts[size_t(e[0])], 1.0f)), a) && project(glm::vec3(m * glm::vec4(mesh.verts[size_t(e[1])], 1.0f)), b)) dl->AddLine(a, b, col, width);
        }
    };
    const bool clean = capturing(); // (a picture being saved: no outlines or gizmos)
    Object* ed = editing();
    for (const Object& o : scene_.objects) {
        if (!o.visible) continue;
        const bool sel = selected_.count(o.id) > 0 && !clean && tab_ != Tab::Paint;
        if (ed && o.id == ed->id) continue;
        if (isCutter(o)) {
            if (!clean) lines(o, shownMesh(o), sel ? kAccent : IM_COL32(235, 95, 85, 150), sel ? 1.6f : 1.0f, false);
            continue;
        }
        if (shading() == Shading::Wire || xray_) lines(o, shownMesh(o), sel ? kAccent : IM_COL32(200, 200, 205, 150), sel ? 1.6f : 1.0f, false);
        else if (sel) outline(o, kAccent, 2.5f);
    }
    if (tab_ == Tab::Paint) {
        if (Object* so = find(ledit_.object); so && editLayer() && editLayer()->sticker) { // the sticker's frame and handles
            std::array<glm::vec3, 6> hw;
            ImVec2 q[6];
            bool ok = stickerHandles(*so, *editLayer(), hw);
            for (int i = 0; ok && i < 6; ++i) ok = project(hw[size_t(i)], q[i]);
            if (ok) {
                const int hot = ledit_.drag >= 0 ? ledit_.drag : stickerHandle(*so, *editLayer(), io.MousePos);
                dl->AddPolyline(q, 4, hot == 4 ? kHot : kAccent, ImDrawFlags_Closed, 2.0f);
                const ImVec2 top((q[0].x + q[1].x) * 0.5f, (q[0].y + q[1].y) * 0.5f);
                dl->AddLine(top, q[4], kAccent, 1.5f);
                dl->AddCircleFilled(q[4], 7.0f, hot == 5 ? kHot : IM_COL32(255, 255, 255, 240));
                dl->AddCircle(q[4], 7.0f, IM_COL32(20, 20, 25, 255));
                for (int i = 0; i < 4; ++i) {
                    dl->AddRectFilled(ImVec2(q[i].x - 6, q[i].y - 6), ImVec2(q[i].x + 6, q[i].y + 6), hot == i ? kHot : IM_COL32(255, 255, 255, 240));
                    dl->AddRect(ImVec2(q[i].x - 6, q[i].y - 6), ImVec2(q[i].x + 6, q[i].y + 6), IM_COL32(20, 20, 25, 255));
                }
            }
        }
        if (stencil_.size() >= 2) { // (the stencil's edge; where paint can't go, shaded)
            dl->AddPolyline(stencil_.data(), int(stencil_.size()), stencilOn_ || drawingStencil_ ? kAccent : IM_COL32(200, 200, 205, 120), drawingStencil_ ? 0 : ImDrawFlags_Closed, 2.0f);
            if (stencilOn_ && !drawingStencil_ && stencil_.size() >= 3) dl->AddConcavePolyFilled(stencil_.data(), int(stencil_.size()), IM_COL32(255, 160, 90, 28));
        }
        if (sculpting_) {
            if (mouseInView(io)) dl->AddCircle(io.MousePos, sculptPx_, io.KeyCtrl ? IM_COL32(120, 180, 255, 220) : IM_COL32(255, 255, 255, 200), 40, 1.5f);
        } else if (mouseInView(io) && !editLayer() && paintTool_ != PaintTool::Picture)
            dl->AddCircle(io.MousePos, brushPx_, paintTool_ == PaintTool::Fill || paintTool_ == PaintTool::Picker ? IM_COL32(255, 255, 255, 90) : IM_COL32(255, 255, 255, 200), 40, 1.5f);
        dl->PopClipRect();
        return;
    }
    if (tab_ == Tab::Layout) {
        layoutOverlay(dl);
        dl->PopClipRect();
        return;
    }
    if ((tab_ == Tab::Print || printTool_ == PrintTool::Measure) && !clean) printOverlay(dl, io);
    if (tab_ == Tab::Print && showLayer_ && !clean) layerOverlay(dl);
    if (clean) {
        dl->PopClipRect();
        return;
    }
    if (ed) {
        if (ed->hasFinish() && ed->mirror && shading() != Shading::Wire) // (the mirrored half: faint)
            lines(*ed, shownMesh(*ed), IM_COL32(20, 20, 25, 70), 1.0f, false);
        lines(*ed, ed->mesh, shading() == Shading::Wire ? IM_COL32(230, 230, 235, 230) : IM_COL32(12, 12, 15, 235), 1.3f, true);
    }
    if (ed && !ed->creases.empty()) { // (creased edges: smoothing keeps them sharp)
        const glm::mat4 m = objectMatrix(*ed);
        const int n = int(ed->mesh.verts.size());
        for (auto [a, b] : ed->creases) {
            ImVec2 sa, sb;
            if (a < n && b < n && (xray_ || (pointSeen(*ed, a) && pointSeen(*ed, b))) && project(glm::vec3(m * glm::vec4(ed->mesh.verts[size_t(a)], 1.0f)), sa) &&
                project(glm::vec3(m * glm::vec4(ed->mesh.verts[size_t(b)], 1.0f)), sb))
                dl->AddLine(sa, sb, IM_COL32(230, 70, 200, 255), 2.5f);
        }
    }
    if (Object* o = ed) {
        const glm::mat4 m = objectMatrix(*o);
        if (elem_ == Elem::Vertex) {
            for (size_t v = 0; v < o->mesh.verts.size(); ++v) {
                ImVec2 s;
                const bool sel = selVerts_.count(int(v));
                if (!project(glm::vec3(m * glm::vec4(o->mesh.verts[v], 1.0f)), s) || (!sel && !pointSeen(*o, int(v)))) continue;
                dl->AddRectFilled(ImVec2(s.x - 3, s.y - 3), ImVec2(s.x + 3, s.y + 3), sel ? kAccent : IM_COL32(15, 15, 18, 255));
            }
        } else if (elem_ == Elem::Edge) {
            for (auto [a, b] : selEdges_) {
                ImVec2 sa, sb;
                if (a < int(o->mesh.verts.size()) && b < int(o->mesh.verts.size()) && project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(a)], 1.0f)), sa) &&
                    project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(b)], 1.0f)), sb))
                    dl->AddLine(sa, sb, kAccent, 3.0f);
            }
        } else { // the selected faces the eye can see: an orange skin, a dot in the middle
            const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m)));
            for (int f : selFaces_) {
                if (f >= int(o->mesh.faces.size())) continue;
                const glm::vec3 c(m * glm::vec4(faceCenter(o->mesh, f), 1.0f));
                const bool front = glm::dot(nm * faceNormal(o->mesh, f), eye_ - c) > 0;
                if (!xray_ && !front) continue;
                const auto& fv = o->mesh.faces[size_t(f)];
                if (!xray_ && !std::all_of(fv.begin(), fv.end(), [&](int v) { return pointSeen(*o, v); }) && !visible(*o, c)) continue;
                std::vector<ImVec2> poly(fv.size());
                bool ok = true;
                for (size_t k = 0; k < fv.size(); ++k) ok &= project(glm::vec3(m * glm::vec4(o->mesh.verts[size_t(fv[k])], 1.0f)), poly[k]);
                if (ok) dl->AddConcavePolyFilled(poly.data(), int(poly.size()), IM_COL32(255, 120, 40, 70));
                ImVec2 s;
                if (project(c, s)) dl->AddCircleFilled(s, 3.5f, kAccent);
            }
        }
        // The loop cut to be: across each face of the ring.
        if (tool_ == Tool::LoopCut && cutHover_.first >= 0)
            for (const auto& q : ringQuads(o->mesh, cutHover_))
                for (int c = 1; c <= cuts_; ++c) {
                    const float t = float(c) / float(cuts_ + 1);
                    const glm::vec3 a = glm::mix(o->mesh.verts[size_t(q[0])], o->mesh.verts[size_t(q[1])], t), b = glm::mix(o->mesh.verts[size_t(q[3])], o->mesh.verts[size_t(q[2])], t);
                    ImVec2 sa, sb;
                    if (project(glm::vec3(m * glm::vec4(a, 1.0f)), sa) && project(glm::vec3(m * glm::vec4(b, 1.0f)), sb)) dl->AddLine(sa, sb, kHot, 2.5f);
                }
    }
    // The knife's line: its points, on to the mouse; cutting through, on past the ends.
    if (tool_ == Tool::Knife && (!knifePts_.empty() || knifeDown_)) {
        std::vector<ImVec2> pts = knifePts_;
        if (knifePts_.empty()) pts.push_back(knifePress_);
        if (mouseInView(io) || knifeDown_) pts.push_back(knifeEnd(io));
        for (size_t i = 0; i + 1 < pts.size(); ++i) dl->AddLine(pts[i], pts[i + 1], kHot, 2.0f);
        if (knifeThrough_ && pts.size() >= 2)
            for (int end = 0; end < 2; ++end) {
                const ImVec2 a = end ? pts.back() : pts.front(), b = end ? pts[pts.size() - 2] : pts[1];
                const float dx = a.x - b.x, dy = a.y - b.y, l = std::max(std::hypot(dx, dy), 1e-3f);
                for (int k = 0; k < 60; k += 2)
                    dl->AddLine(ImVec2(a.x + dx / l * float(k) * 12, a.y + dy / l * float(k) * 12), ImVec2(a.x + dx / l * float(k + 1) * 12, a.y + dy / l * float(k + 1) * 12),
                                (kHot & 0x00FFFFFF) | 0x90000000, 1.5f);
            }
        for (const ImVec2& p : knifePts_) dl->AddCircleFilled(p, 4.0f, kHot);
    }
    // The hole to be.
    if (tool_ == Tool::Hole && (mouseInView(io) || hole_.active)) {
        const auto ring = holePreview(io);
        std::vector<ImVec2> pts;
        for (const glm::vec3& w : ring) {
            ImVec2 sp;
            if (project(w, sp)) pts.push_back(sp);
        }
        if (pts.size() >= 3) dl->AddPolyline(pts.data(), int(pts.size()), hole_.active ? kHot : IM_COL32(255, 255, 255, 200), ImDrawFlags_Closed, 2.0f);
    }
    if (boxing_ && !touchOrbit_ && std::hypot(io.MousePos.x - boxStart_.x, io.MousePos.y - boxStart_.y) > 4) {
        dl->AddRectFilled(boxStart_, io.MousePos, IM_COL32(255, 140, 60, 30));
        dl->AddRect(boxStart_, io.MousePos, kAccent);
    }
    if (lassoing_ && lasso_.size() > 1) dl->AddPolyline(lasso_.data(), int(lasso_.size()), kAccent, ImDrawFlags_Closed, 1.5f);
    if (selShape_ == SelShape::Brush && tool_ == Tool::Select && mode_ == Mode::Edit && mouseInView(io))
        dl->AddCircle(io.MousePos, brushRadius_, painting_ ? kAccent : IM_COL32(255, 255, 255, 160), 32, 1.5f);
    if (tool_ == Tool::Draw) {
        ImVec2 prev, s;
        for (size_t i = 0; i < drawPts_.size(); ++i) {
            if (!project(drawPts_[i], s)) continue;
            if (i) dl->AddLine(prev, s, kAccent, 2.0f);
            dl->AddCircleFilled(s, i == 0 ? 6.0f : 4.0f, i == 0 ? IM_COL32(255, 255, 255, 255) : kAccent);
            prev = s;
        }
        if (mouseInView(io) && !touch()) // (a finger isn't hovering anywhere)
            if (auto p = drawPoint(io); p && project(*p, s)) {
                if (!drawPts_.empty()) dl->AddLine(prev, s, IM_COL32(255, 160, 90, 180), 1.5f);
                dl->AddCircle(s, 5.0f, IM_COL32(255, 255, 255, 220), 0, 1.5f);
                if (!drawPts_.empty()) { // (the line's length by the mouse, and what's typed)
                    const std::string label = std::format("{:.4g} m{}", glm::distance(drawPts_.back(), *p), sketchTyped_.empty() ? "   (type a length, Enter; Shift: 15 deg steps)" : "   typed " + sketchTyped_);
                    const ImVec2 at(io.MousePos.x + 16, io.MousePos.y + 14), size = ImGui::CalcTextSize(label.c_str());
                    dl->AddRectFilled(ImVec2(at.x - 5, at.y - 3), ImVec2(at.x + size.x + 5, at.y + size.y + 3), IM_COL32(24, 24, 28, 225), 4.0f);
                    dl->AddText(at, sketchTyped_.empty() ? IM_COL32(235, 235, 240, 255) : kAccent, label.c_str());
                }
            }
    }
    gizmo(dl, io);
    dl->PopClipRect();
}

auto App::gizmo(ImDrawList* dl, const ImGuiIO& io) -> void {
    const int hot = mouseInView(io) || drag_.active ? hoverHandle(io.MousePos) : -1;
    if (softSel_ && mode_ == Mode::Edit && (tool_ == Tool::Move || tool_ == Tool::Rotate || tool_ == Tool::Scale) && !selectedPoints().empty()) {
        const glm::vec3 at = drag_.active ? drag_.pivot : pivotOfSelection();
        ImVec2 c;
        if (project(at, c)) dl->AddCircle(c, softReach_ / std::max(pixelSize(at), 1e-9f), keel::ui::accent(0.8f), 64, 1.5f);
    }
    if (tool_ == Tool::Extrude || tool_ == Tool::Inset) {
        glm::vec3 at, n;
        if (!faceHandle(at, n)) return;
        ImVec2 a, b;
        if (!project(at, a) || !project(at + n * pixelSize(at) * 90.0f, b)) return;
        const ImU32 col = hot == 0 ? kHot : IM_COL32(255, 255, 255, 255);
        dl->AddLine(a, b, col, 3.0f);
        if (tool_ == Tool::Extrude) dl->AddCircleFilled(b, 7.0f, col);
        else dl->AddRectFilled(ImVec2(b.x - 6, b.y - 6), ImVec2(b.x + 6, b.y + 6), col);
        return;
    }
    if (tool_ == Tool::Bevel) {
        ImVec2 c, h;
        if (!bevelHandle(c, h)) return;
        const ImU32 col = hot == 0 ? kHot : IM_COL32(255, 255, 255, 255);
        dl->AddLine(c, h, (col & 0x00FFFFFF) | 0x90000000, 2.0f);
        dl->AddCircleFilled(h, 9.0f, col);
        drawIcon(dl, Icon::Bevel, ImVec2(h.x - 7, h.y - 7), 14.0f, IM_COL32(30, 30, 35, 255));
        return;
    }
    if (tool_ != Tool::Move && tool_ != Tool::Rotate && tool_ != Tool::Scale) return;
    if (!hasSelection()) return;
    const Axes axes = drag_.active ? drag_.axes : gizmoAxes();
    if (tool_ == Tool::Scale) { // the box round the selection, a handle on each side
        const auto [lo, hi] = selectionBox(axes);
        for (int e = 0; e < 12; ++e) {
            const int k = e / 4, u = (k + 1) % 3, v = (k + 2) % 3;
            glm::vec3 a, b;
            a[k] = lo[k], b[k] = hi[k];
            a[u] = b[u] = e & 1 ? hi[u] : lo[u];
            a[v] = b[v] = e & 2 ? hi[v] : lo[v];
            ImVec2 sa, sb;
            if (project(fromFrame(axes, a), sa) && project(fromFrame(axes, b), sb)) dl->AddLine(sa, sb, IM_COL32(255, 255, 255, 110), 1.0f);
        }
        for (int h : {0, 1, 2, 4, 5, 6, 3}) {
            ImVec2 s;
            if (!project(scaleHandle(axes, lo, hi, h), s)) continue;
            const ImU32 col = hot == h ? kHot : h == 3 ? IM_COL32(255, 255, 255, 235) : kAxisCol[h < 3 ? h : h - 4];
            dl->AddRectFilled(ImVec2(s.x - 6, s.y - 6), ImVec2(s.x + 6, s.y + 6), col);
            dl->AddRect(ImVec2(s.x - 6, s.y - 6), ImVec2(s.x + 6, s.y + 6), IM_COL32(20, 20, 25, 200));
        }
        return;
    }
    const glm::vec3 p = drag_.active ? (tool_ == Tool::Rotate ? drag_.pivot : selectionMiddle(axes)) : (tool_ == Tool::Rotate ? rotatePivot(axes) : selectionMiddle(axes));
    ImVec2 c;
    if (!project(p, c)) return;
    const float len = pixelSize(p) * 90.0f;
    if (tool_ == Tool::Rotate) {
        for (int k = 0; k < 3; ++k) {
            const glm::vec3 u = glm::normalize(glm::cross(axes[size_t(k)], std::abs(axes[size_t(k)].z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
            const glm::vec3 v = glm::cross(axes[size_t(k)], u);
            ImVec2 prev;
            bool havePrev = false;
            for (int i = 0; i <= 64; ++i) {
                const float a = 2 * kPi * float(i) / 64.0f;
                const glm::vec3 w = p + (u * std::cos(a) + v * std::sin(a)) * len;
                ImVec2 s;
                if (!project(w, s)) {
                    havePrev = false;
                    continue;
                }
                const bool front = glm::dot(w - p, eye_ - p) > -0.2f * len * glm::length(eye_ - p);
                if (havePrev) dl->AddLine(prev, s, hot == k ? kHot : (front ? kAxisCol[k] : (kAxisCol[k] & 0x00FFFFFF) | 0x50000000), hot == k ? 3.5f : 2.5f);
                prev = s, havePrev = true;
            }
        }
        dl->AddCircle(c, 102.0f, hot == 3 ? kHot : IM_COL32(255, 255, 255, 160), 64, 2.0f);
        return;
    }
    for (int k = 0; k < 3; ++k) {
        ImVec2 e;
        if (!project(p + axes[size_t(k)] * len, e)) continue;
        const ImU32 col = hot == k ? kHot : kAxisCol[k];
        dl->AddLine(c, e, col, 3.0f);
        const float dx = e.x - c.x, dy = e.y - c.y, l = std::max(std::hypot(dx, dy), 1.0f);
        const ImVec2 dir(dx / l, dy / l), side(-dir.y, dir.x);
        dl->AddTriangleFilled(ImVec2(e.x + dir.x * 14, e.y + dir.y * 14), ImVec2(e.x + side.x * 6, e.y + side.y * 6), ImVec2(e.x - side.x * 6, e.y - side.y * 6), col);
    }
    dl->AddRect(ImVec2(c.x - 8, c.y - 8), ImVec2(c.x + 8, c.y + 8), hot == 3 ? kHot : IM_COL32(255, 255, 255, 230), 0, 0, 2.0f);
}

} // namespace fryapp
