// Scripts without a window: they make, change, combine and delete objects; mistakes come back as
// errors with their line, and an endless loop is stopped.
#include "script.h"

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
    r = runScript(host, "fry.clear()");
    CHECK(r.ok && scene.objects.empty() && active == 0 && selected.empty(), "clear");

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
