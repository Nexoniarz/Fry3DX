// Scripts without a window: they make, change, combine and delete objects; mistakes come back as
// errors with their line, and an endless loop is stopped.
#include "script.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace fry;

static int g_failures = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) ++g_failures, std::printf("FAIL: " __VA_ARGS__); \
        else std::printf("ok: " __VA_ARGS__);                         \
        std::printf("\n");                                            \
    } while (0)

int main() {
    Scene scene;
    std::set<uint32_t> selected;
    uint32_t active = 0;
    std::string out;
    ScriptHost host{&scene, &selected, &active, [&](const std::string& s) { out += s + "\n"; }, {}, 2.0};

    ScriptResult r = runScript(host, R"(
        local box = fry.add("cube", {width = 2, color = "#ff8000", name = "Box"})
        box.position = {0, 0, 1}
        box:move(1, 0, 0)
        print(box.name, box.width, box.position.x)
        local c = fry.add("cylinder", {radius = 0.3, height = 4, position = {1, 0.1, 0}})
        box:subtract(c)
        fry.add("sphere"):delete()
        local t = fry.lathe({{0, 0}, {0.3, 0}, {0.35, 0.5}, {0.1, 0.8}, {0, 0.8}}, 24, "Vase")
        fry.extrude({{0, 0}, {1, 0}, {0, 1}}, 0.2, "Wedge")
        fry.group("Things", {t})
        fry.select(box)
    )");
    CHECK(r.ok, "a script runs (%s)", r.error.c_str());
    CHECK(scene.objects.size() == 3, "box, vase and wedge are left (%zu)", scene.objects.size());
    CHECK(out.find("Box\t2.0\t1.0") != std::string::npos, "print and properties: %s", out.c_str());
    const Object& box = scene.objects[0];
    CHECK(box.name == "Box" && box.recipe.empty() && box.mesh.faces.size() > 6, "the box had the cylinder taken out (%zu faces)", box.mesh.faces.size());
    CHECK(box.color.r > 0.99f && box.color.g > 0.49f && box.color.g < 0.51f, "colour from #ff8000");
    CHECK(scene.folders.size() == 1 && scene.objects[1].folder == scene.folders[0].id, "grouped into a folder");
    CHECK(active == box.id && selected.count(box.id), "selection from the script");

    r = runScript(host, "local o = fry.add('cube')\no.wobble = 3", "bad.lua");
    CHECK(!r.ok && r.error.find("bad.lua:2") != std::string::npos, "an error says where: %s", r.error.substr(0, 80).c_str());
    r = runScript(host, "fry.add('teapot')");
    CHECK(!r.ok && r.error.find("no shape called teapot") != std::string::npos, "an unknown shape is an error");
    r = runScript(host, "while true do end");
    CHECK(!r.ok && r.error.find("stopped") != std::string::npos, "an endless loop is stopped");
    r = runScript(host, "io.open('x')");
    CHECK(!r.ok, "no io");
    r = runScript(host, "local o = fry.add('cube')\nlocal n = #o:points()\nlocal p = o:points()\nfor i = 1, n do p[i].z = p[i].z * 2 end\no:setpoints(p)\nlocal lo, hi = o:bounds()\nprint(hi.z - lo.z)");
    CHECK(r.ok && out.find("\n2.0\n") != std::string::npos, "points round-trip (%s)", r.error.c_str());
    r = runScript(host, "local o = fry.add('cube')\no.material = 'Gold'\nlocal m = fry.add('cube', {color = '#102030', material = 'glossy'})\nprint(#fry.materials())");
    CHECK(r.ok && scene.objects.back().metal == 0.0f && scene.objects.back().rough < 0.2f && scene.objects.back().color.r < 0.1f, "a material keeping its colour (%s)", r.error.c_str());
    CHECK(scene.objects[scene.objects.size() - 2].metal == 1.0f && scene.objects[scene.objects.size() - 2].color.g > 0.7f, "gold");
    r = runScript(host, "local b = fry.add('sphere', {radius = 1})\nb:sculpt('raise', {0, 0, 1}, 0.4, 1, 20)\nlocal lo, hi = b:bounds()\nprint(hi.z > 1.05)");
    CHECK(r.ok && out.find("true\n") != std::string::npos, "o:sculpt raises the top (%s)", r.error.c_str());
    r = runScript(host, "fry.add('cube').material = 'cheese'");
    CHECK(!r.ok && r.error.find("no material called cheese") != std::string::npos, "an unknown material is an error");
    r = runScript(host, "fry.clear()");
    CHECK(r.ok && scene.objects.empty() && active == 0 && selected.empty(), "clear");

    { // Your own materials: from a script, found by name (any case), and as text and back.
        clearOwnMaterials("mats.lua");
        r = runScript(host, "fry.material{name = 'Teal Plastic', color = '#1f9a96', rough = 0.35}\nlocal o = fry.add('cube')\no.material = 'teal plastic'", "mats.lua");
        CHECK(r.ok && findMaterial("teal plastic") && std::abs(scene.objects.back().rough - 0.35f) < 1e-4f && scene.objects.back().color.g > 0.5f,
              "fry.material, then o.material (%s)", r.error.c_str());
        CHECK(findMaterial("teal plastic")->from == "mats.lua", "it says which script made it");
        r = runScript(host, "fry.material{name = 'Gold', color = '#ffffff'}");
        CHECK(!r.ok, "not one of Fry3DX's own names");
        addOwnMaterial({"", "Mine \"quoted\"", {0.1f, 0.2f, 0.3f}, 0.5f, 0.25f, true, ""});
        const std::string text = ownMaterialsText();
        removeOwnMaterial(materialId("Mine \"quoted\""));
        CHECK(!findMaterial(materialId("Mine \"quoted\"")), "removed");
        parseOwnMaterials(text);
        const MaterialPreset* m = findMaterial(materialId("Mine \"quoted\""));
        CHECK(m && m->keepColor && std::abs(m->metal - 0.5f) < 1e-4f && text.find("Teal") == std::string::npos, "the picker's own as text and back (only those)");
        clearOwnMaterials("mats.lua");
        CHECK(!findMaterial("teal plastic"), "a script's materials cleared");
    }
    { // A plug-in: loaded once, its commands run later (with their settings), its errors taken back.
        Plugin p("stairs_and_such.lua");
        Scene scratch;
        ScriptHost load{&scratch, &selected, &active, {}, {}, 2.0};
        std::string error;
        const bool ok = p.load(load, R"(
            fry.plugin{name = "Boxes", about = "Rows of boxes"}
            local made = 0
            fry.command{name = "Row", menu = "add", settings = {{"count", 3}, {"gap", 1.5}}, run = function(s)
                for i = 1, s.count do fry.add("cube").position = {i * s.gap, 0, 0} end
                made = made + 1
                print("made " .. made)
            end}
            fry.command{name = "Broken", run = function() fry.add("cube"); error("no") end}
            fry.add("sphere") -- (loading touches only its own scratch scene)
        )", &error);
        CHECK(ok && p.name == "Boxes" && p.about == "Rows of boxes" && p.commands.size() == 2 && p.commands[0].menu == "add" && p.commands[1].menu == "script" &&
                  p.commands[0].settings.size() == 2 && p.commands[0].settings[1].second == 1.5,
              "a plug-in loads and registers its commands (%s)", error.c_str());
        Scene s2;
        out.clear();
        ScriptHost h{&s2, &selected, &active, [&](const std::string& t) { out += t + "\n"; }, {}, 2.0};
        r = p.run(h, 0, {4, 2});
        r = p.run(h, 0, {1, 2});
        CHECK(r.ok && s2.objects.size() == 5 && std::abs(s2.objects[3].position.x - 8.0f) < 1e-4f && out.find("made 2") != std::string::npos,
              "its command runs with the settings, and keeps its state between runs (%s)", r.error.c_str());
        r = p.run(h, 1, {});
        CHECK(!r.ok && r.error.find("no") != std::string::npos, "an error in a command comes back");
        r = runScript(host, "fry.command{name = 'x', run = function() end}");
        CHECK(!r.ok && r.error.find("plug-ins") != std::string::npos, "fry.command only in plug-ins");
        Plugin bad("bad.lua");
        CHECK(!bad.load(load, "fry.command{name = 'x'}", &error) && bad.commands.empty(), "a command with no run: it doesn't load");
    }
    // The plug-ins that come with it load.
    for (const auto& e : std::filesystem::directory_iterator(FRY_PLUGINS_DIR)) {
        std::ifstream in(e.path());
        const std::string code((std::istreambuf_iterator<char>(in)), {});
        Plugin p(e.path().filename().string());
        Scene fresh;
        std::set<uint32_t> sel;
        uint32_t act = 0;
        ScriptHost h{&fresh, &sel, &act, {}, {}, 20.0};
        std::string error;
        const bool ok = p.load(h, code, &error);
        bool ran = true;
        for (size_t c = 0; c < p.commands.size(); ++c) {
            Scene s;
            s.objects.push_back(Object{});
            s.objects.back().id = s.nextId++, s.objects.back().name = "Rock", s.objects.back().mesh = makeCube();
            std::set<uint32_t> picked{s.objects.back().id};
            uint32_t last = s.objects.back().id;
            ScriptHost run{&s, &picked, &last, {}, {}, 20.0};
            const ScriptResult rr = p.run(run, c, {});
            if (!rr.ok || s.objects.size() < 2) ran = false, error = p.commands[c].name + ": " + rr.error;
        }
        CHECK(ok && ran, "plug-in %s (%zu commands) %s", e.path().filename().string().c_str(), p.commands.size(), error.c_str());
        clearOwnMaterials(e.path().filename().string());
    }

    // The examples that come with it all run.
    for (const auto& e : std::filesystem::directory_iterator(FRY_SCRIPTS_DIR)) {
        std::ifstream in(e.path());
        const std::string code((std::istreambuf_iterator<char>(in)), {});
        Scene fresh;
        std::set<uint32_t> sel;
        uint32_t act = 0;
        ScriptHost h{&fresh, &sel, &act, {}, {}, 20.0};
        r = runScript(h, code, e.path().filename().string());
        CHECK(r.ok && !fresh.objects.empty(), "example %s (%d objects) %s", e.path().filename().string().c_str(), r.objectsMade, r.error.c_str());
    }

    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
