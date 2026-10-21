// Fry3DX: the Print tab: the plate, checks (overhangs, thin walls), the slice preview, sizes.
#include "app.h"

using namespace fry;
namespace stdfs = std::filesystem;

namespace fryapp {

// ───────────────────────────── the Print tab ─────────────────────────────

auto App::bedSize() const -> glm::vec3 { // (metres)
    const int i = std::clamp(fry_printer.i(), 0, int(std::size(kPrinters)) - 1);
    const PrinterInfo& p = kPrinters[i];
    return p.x > 0 ? glm::vec3(p.x, p.y, p.z) * 0.001f : glm::vec3(fry_bed_x.f(), fry_bed_y.f(), fry_bed_z.f()) * 0.001f;
}

auto App::printInfo(const Object& o) -> const PrintInfo& {
    PrintInfo& p = printInfos_[o.id];
    uint64_t key = versions_[o.id] * 1000003ull + uint64_t(overhangDeg_ * 10) + uint64_t(minWall_ * 1e5f) * 7919ull;
    for (float f : {o.position.x, o.position.y, o.position.z, o.rotation.x, o.rotation.y, o.rotation.z, o.rotation.w, o.scale.x, o.scale.y, o.scale.z}) {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        key = (key ^ b) * 1099511628211ull;
    }
    if (p.key == key) return p;
    p.key = key;
    p.world = cutKeys_.count(o.id) ? fry::cutMesh(scene_, o) : o.hasFinish() ? finishedMesh(o) : o.mesh;
    transform(p.world, objectMatrix(o));
    p.lo = boundsMin(p.world), p.hi = boundsMax(p.world);
    p.check = checkForPrint(p.world, overhangDeg_, 0.0f);
    // Overhangs: those faces again a hair above the surface, to show red.
    keel::KmdlModel km;
    keel::KmdlMaterial mat{};
    mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = mat.baseColor[3] = 1.0f;
    km.materials.push_back(mat);
    for (size_t f = 0; f < p.world.faces.size(); ++f) {
        if (!overhangs(p.world, int(f), overhangDeg_, 0.0f)) continue;
        const glm::vec3 n = faceNormal(p.world, int(f));
        for (const auto& t : triangulate(p.world, int(f)))
            for (int k : t) {
                const glm::vec3 q = p.world.verts[size_t(k)] + n * 0.0004f;
                km.indices.push_back(uint32_t(km.vertices.size()));
                km.vertices.push_back({{q.x, q.y, q.z}, {n.x, n.y, n.z}, {0, 0}, {0, 0}});
            }
    }
    p.hasOverhang = !km.indices.empty();
    if (p.hasOverhang) {
        km.submeshes.push_back({0, uint32_t(km.indices.size()), 0});
        p.overhang = r_->updateModel(std::format("fry:overhang:{}", o.id), km);
    }
    // Thin walls (only of a closed shape: an open one has no inside to be thick).
    uint64_t thickKey = shapeKey(o) * 1000003ull;
    for (float f : {o.scale.x, o.scale.y, o.scale.z}) {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        thickKey = (thickKey ^ b) * 1099511628211ull;
    }
    if (p.thickKey != thickKey || p.thick.size() != p.world.faces.size())
        p.thickKey = thickKey, p.thick = p.check.closed() ? fry::wallThickness(p.world) : std::vector<float>(p.world.faces.size(), 1e30f);
    p.thinFaces = 0, p.thinnest = 1e30f;
    km.vertices.clear(), km.indices.clear(), km.submeshes.clear();
    for (size_t f = 0; f < p.world.faces.size(); ++f) {
        p.thinnest = std::min(p.thinnest, p.thick[f]);
        if (p.thick[f] >= minWall_) continue;
        ++p.thinFaces;
        const glm::vec3 n = faceNormal(p.world, int(f));
        for (const auto& t : triangulate(p.world, int(f)))
            for (int k : t) {
                const glm::vec3 q = p.world.verts[size_t(k)] + n * 0.0005f;
                km.indices.push_back(uint32_t(km.vertices.size()));
                km.vertices.push_back({{q.x, q.y, q.z}, {n.x, n.y, n.z}, {0, 0}, {0, 0}});
            }
    }
    if (p.thinFaces) {
        km.submeshes.push_back({0, uint32_t(km.indices.size()), 0});
        p.thin = r_->updateModel(std::format("fry:thin:{}", o.id), km);
    }
    return p;
}

auto App::printTargets() -> std::vector<Object*> { // (what's picked, or everything shown)
    std::vector<Object*> out;
    for (Object& o : scene_.objects)
        if (o.visible && !isCutter(o) && (selected_.empty() || selected_.count(o.id))) out.push_back(&o);
    return out;
}

auto App::drawPrintBed(keel::Renderer& r, const keel::ModelLighting& light) -> void {
    const glm::vec3 b = bedSize();
    const glm::vec3 lo(-b.x * 0.5f, -b.y * 0.5f, 0.0f), hi(b.x * 0.5f, b.y * 0.5f, b.z);
    const glm::vec4 plate(0.1f, 0.1f, 0.12f, 1.0f);
    if (bedFor_ != b) {
        bedFor_ = b;
        keel::KmdlModel km;
        keel::KmdlMaterial mat{};
        mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = mat.baseColor[3] = 1.0f;
        km.materials.push_back(mat);
        for (glm::vec2 c : {glm::vec2(lo.x, lo.y), glm::vec2(hi.x, lo.y), glm::vec2(hi.x, hi.y), glm::vec2(lo.x, hi.y)}) km.vertices.push_back({{c.x, c.y, -0.0006f}, {0, 0, 1}, {0, 0}, {0, 0}});
        km.indices = {0, 1, 2, 0, 2, 3};
        km.submeshes.push_back({0, 6, 0});
        bed_ = r.updateModel("fry:bed", km);
    }
    r.drawModel(bed_, glm::scale(glm::mat4(1.0f), glm::vec3(kUnits)), plate, nullptr, &light, false);
    // Its outline, and the space the printer reaches (fainter).
    const glm::vec3 c[8] = {{lo.x, lo.y, 0}, {hi.x, lo.y, 0}, {hi.x, hi.y, 0}, {lo.x, hi.y, 0}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    bool fits = true;
    for (Object* o : printTargets()) {
        const PrintInfo& p = printInfo(*o);
        fits &= glm::all(glm::greaterThanEqual(p.lo, lo - 1e-4f)) && glm::all(glm::lessThanEqual(p.hi, hi + 1e-4f));
    }
    const glm::vec4 col = fits ? glm::vec4(0.9f, 0.9f, 0.95f, 0.5f) : glm::vec4(1.0f, 0.3f, 0.2f, 0.9f);
    for (int i = 0; i < 12; ++i)
        r.drawStreak(c[e[i][0]] * kUnits, c[e[i][1]] * kUnits, pixelSize(target_) * (i < 4 ? 2.0f : 1.0f) * kUnits, i < 4 ? col : col * glm::vec4(1, 1, 1, 0.5f),
                     keel::kWhiteTexture);
    if (showOverhangs_) {
        const keel::ModelLighting flat{{glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f)}, 0.0f};
        for (const Object& o : scene_.objects)
            if (o.visible && !isCutter(o))
                if (const PrintInfo& p = printInfo(o); p.hasOverhang)
                    r.drawModel(p.overhang, glm::scale(glm::mat4(1.0f), glm::vec3(kUnits)), glm::vec4(0.95f, 0.15f, 0.1f, 0.55f), nullptr, &flat, false);
    }
    if (showThin_) {
        const keel::ModelLighting flat{{glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f)}, 0.0f};
        for (const Object& o : scene_.objects)
            if (o.visible && !isCutter(o))
                if (const PrintInfo& p = printInfo(o); p.thinFaces)
                    r.drawModel(p.thin, glm::scale(glm::mat4(1.0f), glm::vec3(kUnits)), glm::vec4(1.0f, 0.8f, 0.1f, 0.7f), nullptr, &flat, false);
    }
}

auto App::putOnBed() -> void {
    const auto targets = printTargets();
    if (targets.empty()) return;
    pushUndo();
    for (Object* o : targets) o->position.z -= printInfo(*o).lo.z, touched(o->id);
    say("Put on the bed");
}

auto App::middleOfBed() -> void {
    const auto targets = printTargets();
    if (targets.empty()) return;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (Object* o : targets) lo = glm::min(lo, printInfo(*o).lo), hi = glm::max(hi, printInfo(*o).hi);
    pushUndo();
    const glm::vec2 d = -(glm::vec2(lo) + glm::vec2(hi)) * 0.5f;
    for (Object* o : targets) o->position += glm::vec3(d, 0.0f), touched(o->id);
}

// Everything picked made `mm` big along `axis`, keeping its shape, standing where it stood.
auto App::printResize(int axis, float mm) -> void {
    const auto targets = printTargets();
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (Object* o : targets) lo = glm::min(lo, printInfo(*o).lo), hi = glm::max(hi, printInfo(*o).hi);
    const float now_ = (hi - lo)[axis] * 1000.0f;
    if (targets.empty() || now_ < 1e-3f || mm <= 0.01f) return;
    const float k = mm / now_;
    const glm::vec3 pivot((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, lo.z);
    for (Object* o : targets) o->position = pivot + (o->position - pivot) * k, o->scale *= k, touched(o->id);
}

// Everything picked made `k` times as big, standing where it stood.
auto App::printScale(float k) -> void {
    const auto targets = printTargets();
    if (targets.empty() || !(k > 0) || std::abs(k - 1) < 1e-6f) return;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (Object* o : targets) lo = glm::min(lo, printInfo(*o).lo), hi = glm::max(hi, printInfo(*o).hi);
    const glm::vec3 pivot((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, lo.z);
    for (Object* o : targets) o->position = pivot + (o->position - pivot) * k, o->scale *= k, touched(o->id);
}

auto App::printScaler(float w) -> void {
    const auto targets = printTargets();
    if (targets.empty()) return;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (Object* o : targets) lo = glm::min(lo, printInfo(*o).lo), hi = glm::max(hi, printInfo(*o).hi);
    const glm::vec3 size = (hi - lo) * 1000.0f, bed = bedSize() * 1000.0f;
    auto scale = [&](float k, const char* what) {
        pushUndo();
        printScale(k);
        say(std::format("{}: now {:.1f} x {:.1f} x {:.1f} mm", what, size.x * k, size.y * k, size.z * k));
    };
    const float bw = (w - ImGui::GetStyle().ItemSpacing.x * 3) / 4;
    ImGui::SetNextItemWidth(w - bw - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputFloat("##pct", &scalePercent_, 0, 0, "%.1f %%");
    scalePercent_ = std::clamp(scalePercent_, 0.1f, 100000.0f);
    ImGui::SameLine();
    if (ImGui::Button("Scale", ImVec2(bw, 0))) scale(scalePercent_ / 100, std::format("Scaled to {:g}%", scalePercent_).c_str()), scalePercent_ = 100;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make it this share of the size it is now (50%%: half, 200%%: twice)");
    const struct {
        const char* label;
        float k;
        const char* tip;
    } quick[] = {{"x 0.5", 0.5f, "Half the size"}, {"x 2", 2.0f, "Twice the size"}, {"in > mm", 25.4f, "It was made in inches: 1 became 1 mm, should be 25.4"},
                 {"cm > mm", 10.0f, "It was made in centimetres: 1 became 1 mm, should be 10"}};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(quick[i].label, ImVec2(bw, 0))) scale(quick[i].k, quick[i].tip);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", quick[i].tip);
    }
    const float fit = std::min({bed.x / std::max(size.x, 1e-3f), bed.y / std::max(size.y, 1e-3f), bed.z / std::max(size.z, 1e-3f)}) * 0.95f;
    if (ImGui::Button(std::format("Fit the plate ({:.0f}%)", fit * 100).c_str(), ImVec2(w * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f, 0))) {
        scale(fit, "Made as big as fits the plate");
        middleOfBed();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("As big as fits on this printer (a little room left round it), in the middle");
    ImGui::SameLine();
    if (ImGui::Button("m > mm", ImVec2(w * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f, 0))) scale(1000.0f, "It was made in metres: 1 became 1 mm");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("It came in a thousand times too small (made in metres, read as millimetres)");
}

auto App::printSizeWindow() -> void {
    if (!showPrintSize_) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Print size", &showPrintSize_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) return ImGui::End();
    const float w = 320;
    const auto targets = printTargets();
    if (targets.empty()) ImGui::TextWrapped("Nothing to size: add something in the Model tab.");
    else {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (Object* o : targets) lo = glm::min(lo, printInfo(*o).lo), hi = glm::max(hi, printInfo(*o).hi);
        const glm::vec3 size = (hi - lo) * 1000.0f;
        ImGui::TextDisabled("%s", selected_.empty() ? "Everything" : targets.size() == 1 ? targets[0]->name.c_str() : "What's picked");
        const char* axes[] = {"across", "deep", "high"};
        for (int a = 0; a < 3; ++a) {
            float v = size[a];
            ImGui::SetNextItemWidth(w * 0.7f);
            ImGui::PushID(a);
            if (ImGui::InputFloat("##size", &v, 0, 0, "%.2f mm", ImGuiInputTextFlags_EnterReturnsTrue)) pushUndo(), printResize(a, v);
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", axes[a]);
        }
        ImGui::Spacing();
        printScaler(w);
    }
    ImGui::End();
}

auto App::printToolbox(float w) -> void {
    ImGui::Spacing();
    if (wideButton("##measure", Icon::Ruler, "Measure", printTool_ == PrintTool::Measure, w, "Click two points to see how far apart they are (M)"))
        printTool_ = printTool_ == PrintTool::Measure ? PrintTool::None : PrintTool::Measure, measure_.clear();
    if (wideButton("##layflat", Icon::LayFlat, "Lay flat", printTool_ == PrintTool::LayFlat, w, "Click the face it should stand on (L)"))
        printTool_ = printTool_ == PrintTool::LayFlat ? PrintTool::None : PrintTool::LayFlat;
    if (wideButton("##bed", Icon::Move, "Put on the bed", false, w, "Down (or up) so it stands on the printer's plate (B)")) putOnBed();
    if (wideButton("##mid", Icon::Box, "Middle of the bed", false, w, "To the middle of the printer's plate")) middleOfBed();
    ImGui::Spacing();
    if (wideButton("##3mf", Icon::Print, "Save for the slicer", false, w, "3MF: opens in PrusaSlicer, Bambu Studio, OrcaSlicer, Cura")) dialog(8);
    if (wideButton("##stl", Icon::Print, "Save as STL", false, w, "STL, in millimetres")) dialog(9);
}

auto App::printOptions(float w) -> void {
    if (printTool_ == PrintTool::Measure) {
        ImGui::SeparatorText("Measure");
        ImGui::Checkbox("Catch corners", &measureSnap_);
        if (measure_.size() == 2) {
            const glm::vec3 d = (measure_[1] - measure_[0]) * 1000.0f;
            ImGui::Text("%.2f mm", glm::length(d));
            ImGui::TextDisabled("across %.2f   deep %.2f   up %.2f", std::abs(d.x), std::abs(d.y), std::abs(d.z));
        } else
            ImGui::TextWrapped("Click a point on the model, then another.");
        if (ImGui::Button("Clear", ImVec2(w, 0))) measure_.clear();
    } else {
        ImGui::SeparatorText("Lay flat");
        ImGui::TextWrapped("Click the face it should stand on: it turns so that face is down, and sits on the bed. A big flat face down prints best, with least support.");
    }
}

auto App::printViewport(const ImGuiIO& io) -> void {
    if (!mouseInView(io) || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (printTool_ == PrintTool::Measure) {
        if (const auto p = measurePoint(io.MousePos)) {
            if (measure_.size() >= 2) measure_.clear();
            measure_.push_back(*p);
            if (measure_.size() == 2) say(std::format("{:.2f} mm", glm::distance(measure_[0], measure_[1]) * 1000.0f));
        }
        return;
    }
    const Hit h = pickObject(mouseRay(io.MousePos));
    Object* o = h.object ? find(h.object) : nullptr;
    if (!o) return;
    const Mesh& m = printInfo(*o).world;
    if (h.face < 0 || h.face >= int(m.faces.size())) return;
    pushUndo();
    const glm::vec3 n = faceNormal(m, h.face);
    o->rotation = glm::normalize(glm::rotation(n, glm::vec3(0, 0, -1)) * o->rotation);
    touched(o->id);
    o->position.z -= printInfo(*o).lo.z;
    touched(o->id);
    selected_ = {o->id}, active_ = o->id;
    say("Laid flat on that face");
}

// Where the mouse is on the model (a corner if near one), or on the bed.
auto App::measurePoint(ImVec2 mouse) -> std::optional<glm::vec3> {
    const Ray ray = mouseRay(mouse);
    const Hit h = pickObject(ray);
    if (h.object) {
        glm::vec3 p = ray.origin + ray.dir * h.t;
        if (measureSnap_)
            if (const Object* o = find(h.object)) {
                float best = 12.0f;
                for (const glm::vec3& v : printInfo(*o).world.verts) {
                    ImVec2 q;
                    if (project(v, q))
                        if (const float d = std::hypot(q.x - mouse.x, q.y - mouse.y); d < best) best = d, p = v;
                }
            }
        return p;
    }
    if (std::abs(ray.dir.z) > 1e-5f)
        if (const float t = -ray.origin.z / ray.dir.z; t > 0) return ray.origin + ray.dir * t;
    return std::nullopt;
}

// The slice preview: where the printer goes round at the chosen layer, over the model.
auto App::layerOverlay(ImDrawList* dl) -> void {
    const float z = (float(layer_) - 0.5f) * layerHeight_;
    for (Object* o : printTargets()) {
        const PrintInfo& p = printInfo(*o);
        if (p.sliceKey != p.key || p.sliceZ != z) p.sliceKey = p.key, p.sliceZ = z, p.slice = fry::sliceAt(p.world, z);
        for (const auto& [a, b] : p.slice) {
            ImVec2 sa, sb;
            if (project(a, sa) && project(b, sb)) dl->AddLine(sa, sb, IM_COL32(60, 220, 255, 255), 2.5f);
        }
    }
}

auto App::printOverlay(ImDrawList* dl, const ImGuiIO& io) -> void {
    if (printTool_ != PrintTool::Measure) return;
    std::vector<glm::vec3> pts = measure_;
    if (pts.size() == 1 && mouseInView(io))
        if (auto p = measurePoint(io.MousePos)) pts.push_back(*p);
    ImVec2 q[2];
    for (size_t i = 0; i < pts.size(); ++i)
        if (project(pts[i], q[i])) dl->AddCircleFilled(q[i], 5, kAccent);
    if (pts.size() == 2 && project(pts[0], q[0]) && project(pts[1], q[1])) {
        dl->AddLine(q[0], q[1], kAccent, 2.0f);
        const std::string t = std::format("{:.2f} mm", glm::distance(pts[0], pts[1]) * 1000.0f);
        const ImVec2 mid((q[0].x + q[1].x) * 0.5f, (q[0].y + q[1].y) * 0.5f), ts = ImGui::CalcTextSize(t.c_str());
        dl->AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 6, mid.y - ts.y - 10), ImVec2(mid.x + ts.x * 0.5f + 6, mid.y - 4), IM_COL32(20, 20, 24, 230), 4.0f);
        dl->AddText(ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y - 7), IM_COL32(255, 255, 255, 255), t.c_str());
    }
}

auto App::printProperties(float w) -> void {
    auto good = [](const char* t) { ImGui::TextColored(roleColor(keel::ui::Role::SuccessText), "%s", t); };
    auto bad = [](const std::string& t) { ImGui::PushStyleColor(ImGuiCol_Text, roleColor(keel::ui::Role::DangerText)), ImGui::TextWrapped("%s", t.c_str()), ImGui::PopStyleColor(); };
    ImGui::SeparatorText("Printer");
    int printer = std::clamp(fry_printer.i(), 0, int(std::size(kPrinters)) - 1);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##printer", kPrinters[printer].name)) {
        for (int i = 0; i < int(std::size(kPrinters)); ++i)
            if (ImGui::Selectable(kPrinters[i].name, i == printer)) keel::execute(std::format("fry_printer {}", i));
        ImGui::EndCombo();
    }
    const glm::vec3 bed = bedSize() * 1000.0f;
    if (kPrinters[printer].x <= 0) {
        float b[3] = {bed.x, bed.y, bed.z};
        ImGui::SetNextItemWidth(w);
        if (ImGui::DragFloat3("##bed", b, 1.0f, 50.0f, 2000.0f, "%.0f mm"))
            keel::execute(std::format("fry_bed_x {}; fry_bed_y {}; fry_bed_z {}", b[0], b[1], b[2]));
    } else
        ImGui::TextDisabled("Plate %.0f x %.0f mm, %.0f mm high", bed.x, bed.y, bed.z);

    const auto targets = printTargets();
    if (targets.empty()) return void(ImGui::TextWrapped("Nothing to print: add something in the Model tab."));
    glm::vec3 lo(1e30f), hi(-1e30f);
    PrintCheck all;
    for (Object* o : targets) {
        const PrintInfo& p = printInfo(*o);
        lo = glm::min(lo, p.lo), hi = glm::max(hi, p.hi);
        all.openEdges += p.check.openEdges, all.crowdedEdges += p.check.crowdedEdges, all.flippedEdges += p.check.flippedEdges;
        all.tinyFaces += p.check.tinyFaces, all.pieces += p.check.pieces, all.insideOut |= p.check.insideOut;
        all.volume += std::abs(p.check.volume), all.area += p.check.area, all.overhangArea += p.check.overhangArea;
    }
    const glm::vec3 size = (hi - lo) * 1000.0f;
    ImGui::SeparatorText(selected_.empty() ? "Everything" : targets.size() == 1 ? targets[0]->name.c_str() : "What's picked");
    // Its size, typed in millimetres (keeps its shape).
    const char* axes[] = {"across", "deep", "high"};
    for (int a = 0; a < 3; ++a) {
        float v = size[a];
        ImGui::SetNextItemWidth(w * 0.62f);
        ImGui::PushID(a);
        if (ImGui::DragFloat("##size", &v, 0.5f, 0.1f, 100000.0f, "%.2f mm")) {
            if (ImGui::IsItemActivated() || !sizing_) pushUndo(), sizing_ = true;
            printResize(a, v);
        }
        if (ImGui::IsItemDeactivated()) sizing_ = false;
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", axes[a]);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type a size: it keeps its shape");
    printScaler(w);
    const glm::vec3 b = bedSize();
    const bool fits = size.x <= bed.x + 0.05f && size.y <= bed.y + 0.05f && size.z <= bed.z + 0.05f;
    const bool onPlate = lo.x >= -b.x * 0.5f - 1e-4f && hi.x <= b.x * 0.5f + 1e-4f && lo.y >= -b.y * 0.5f - 1e-4f && hi.y <= b.y * 0.5f + 1e-4f && hi.z <= b.z + 1e-4f;
    if (!fits) {
        const float k = std::min({bed.x / std::max(size.x, 1e-3f), bed.y / std::max(size.y, 1e-3f), bed.z / std::max(size.z, 1e-3f)});
        bad(std::format("Too big for this printer: it would fit at {:.0f}% of this size (or in parts).", std::floor(k * 1000) / 10));
    } else if (!onPlate) {
        bad("It fits, but it's off the plate.");
        if (ImGui::Button("Middle of the bed", ImVec2(w, 0))) middleOfBed();
    } else
        good("Fits on the plate");
    if (std::abs(lo.z) > 1e-4f) {
        bad(lo.z > 0 ? std::format("It floats {:.2f} mm above the bed.", lo.z * 1000) : std::format("It goes {:.2f} mm into the bed (that part won't print).", -lo.z * 1000));
        if (ImGui::Button("Put it on the bed (B)", ImVec2(w, 0))) putOnBed();
    }

    ImGui::SeparatorText("Ready to print?");
    if (!all.closed() || all.insideOut || all.tinyFaces) {
        pushAccent();
        if (ImGui::Button("Repair it (all at once)", ImVec2(w, 0))) repairSelected();
        popAccent();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Joins loose corners, takes away empty and doubled faces, turns faces outwards, closes holes");
    }
    if (all.openEdges) {
        bad(std::format("Not closed: {} edges have a face on one side only (holes). A slicer may fill them wrongly or not print it.", all.openEdges));
        if (ImGui::Button("Close the holes", ImVec2(w, 0))) {
            pushUndo();
            int n = 0;
            for (Object* o : targets) n += closeHoles(o->mesh), touched(o->id);
            say(std::format("Closed {} hole{}", n, n == 1 ? "" : "s"));
        }
    } else
        good("Closed all round");
    if (all.flippedEdges || all.insideOut) {
        bad("Some faces face inwards: the slicer can't tell inside from out.");
        if (ImGui::Button("Turn them the right way", ImVec2(w, 0))) {
            pushUndo();
            int n = 0;
            for (Object* o : targets) n += faceOutward(o->mesh), touched(o->id);
            say(std::format("Turned {} face{}", n, n == 1 ? "" : "s"));
        }
    } else
        good("Faces all face outwards");
    if (all.crowdedEdges) bad(std::format("{} edges have more than two faces on them (shapes stuck together there): separate or rebuild that part.", all.crowdedEdges));
    if (all.tinyFaces) bad(std::format("{} faces have no size: Merge by distance in the Model tab cleans them up.", all.tinyFaces));
    if (all.pieces > int(targets.size())) ImGui::TextDisabled("%d separate pieces", all.pieces);
    const float hang = all.overhangArea * 1e4f;
    if (hang > 0.01f) bad(std::format("{:.1f} cm2 hangs over the air (red): it needs supports, or turn it (Lay flat).", hang));
    else good("Nothing hangs over the air");
    ImGui::Checkbox("Show overhangs in red", &showOverhangs_);
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##hang", &overhangDeg_, 20.0f, 80.0f, "steeper than %.0f degrees");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far from upright a face can lean over before it needs support (45 for most printers)");
    int thin = 0;
    float thinnest = 1e30f;
    for (Object* o : targets) thin += printInfo(*o).thinFaces, thinnest = std::min(thinnest, printInfo(*o).thinnest);
    if (thin) bad(std::format("{} face{} on walls thinner than {:.1f} mm (yellow), the thinnest {:.2f} mm: it may not print there, or break.", thin, thin == 1 ? "" : "s", minWall_ * 1000, thinnest * 1000));
    else if (all.openEdges == 0) good(std::format("Walls {:.1f} mm thick or more", minWall_ * 1000).c_str());
    ImGui::Checkbox("Show thin walls in yellow", &showThin_);
    float mm = minWall_ * 1000;
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderFloat("##minwall", &mm, 0.2f, 5.0f, "thinner than %.1f mm")) minWall_ = mm / 1000;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The thinnest wall your printer makes well: about twice the nozzle (0.8 mm for a 0.4 mm nozzle)");

    ImGui::SeparatorText("Layers");
    {
        const int layers = std::max(1, int(std::ceil((hi.z - std::max(lo.z, 0.0f)) / layerHeight_ - 1e-4f)));
        ImGui::Checkbox("Show one layer (as the printer makes it)", &showLayer_);
        if (showLayer_) {
            layer_ = std::clamp(layer_, 1, layers);
            ImGui::SetNextItemWidth(w);
            ImGui::SliderInt("##layer", &layer_, 1, layers, std::format("layer %d of {}", layers).c_str());
            float mm = layerHeight_ * 1000;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##lh", &mm, 0.04f, 0.6f, "%.2f mm a layer")) layerHeight_ = std::max(mm, 0.01f) / 1000;
            ImGui::TextDisabled("At %.2f mm up", (float(layer_) - 0.5f) * layerHeight_ * 1000);
        } else
            ImGui::TextDisabled("%d layers of %.2f mm", layers, layerHeight_ * 1000);
    }
    ImGui::SeparatorText("Material");
    ImGui::SetNextItemWidth(w);
    ImGui::Combo("##material", &material_, [](void*, int i) { return kMaterials[i].name; }, nullptr, int(std::size(kMaterials)));
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("##infill", &infill_, 0.0f, 100.0f, "infill %.0f%%");
    // (walls about 1.2 mm thick all round, the inside filled as much as the infill says)
    const float vol = all.volume * 1e6f, walls = std::min(vol, all.area * 1e4f * 0.12f);
    const float used = walls + (vol - walls) * infill_ / 100.0f, grams = used * kMaterials[material_].density;
    ImGui::Text("Holds %.1f cm3", vol);
    ImGui::Text("About %.0f g, %.1f m of 1.75 mm filament", grams, used / (3.14159f * 0.0875f * 0.0875f) / 100.0f);
    ImGui::TextDisabled("(a guess: the slicer knows best)");
}

} // namespace fryapp
