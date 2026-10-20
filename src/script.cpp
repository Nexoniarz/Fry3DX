#include "script.h"
#include "shapes.h"

// (Lua is built as C++ here — cmake/Dependencies.cmake — so its errors are exceptions and unwind
// through this file's strings and vectors properly.)
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

namespace fry {
namespace {

const char* kObject = "fry.object";
const char* kVec = "fry.vec";

struct Context {
    ScriptHost* host;
    std::chrono::steady_clock::time_point start;
    int made = 0;
};

Context& context(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "fry.context");
    auto* c = static_cast<Context*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *c;
}
Scene& scene(lua_State* L) { return *context(L).host->scene; }

Object* findObject(Scene& s, uint32_t id) {
    for (Object& o : s.objects)
        if (o.id == id) return &o;
    return nullptr;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ── Vectors: {x, y, z} tables that print and add up ──

void pushVec(lua_State* L, glm::vec3 v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x), lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y), lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z), lua_setfield(L, -2, "z");
    luaL_setmetatable(L, kVec);
}
// A vector from a table ({x=, y=, z=} or {1, 2, 3}) or one number (the same all ways).
glm::vec3 toVec(lua_State* L, int i) {
    if (lua_isnumber(L, i)) return glm::vec3(float(lua_tonumber(L, i)));
    luaL_checktype(L, i, LUA_TTABLE);
    glm::vec3 v(0.0f);
    const char* names[] = {"x", "y", "z"};
    for (int k = 0; k < 3; ++k) {
        if (lua_getfield(L, i, names[k]) == LUA_TNIL) lua_pop(L, 1), lua_rawgeti(L, i, k + 1);
        v[k] = float(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return v;
}
// Arguments from i: three numbers, or one vector.
glm::vec3 argVec(lua_State* L, int i) {
    if (lua_isnumber(L, i) && lua_isnumber(L, i + 1)) return {float(lua_tonumber(L, i)), float(luaL_checknumber(L, i + 1)), float(luaL_optnumber(L, i + 2, 0))};
    return toVec(L, i);
}
int vecOp(lua_State* L, char op) {
    const bool scalarB = lua_isnumber(L, 2), scalarA = lua_isnumber(L, 1);
    const glm::vec3 a = toVec(L, 1), b = toVec(L, 2);
    if (op == '+') pushVec(L, a + b);
    else if (op == '-') pushVec(L, a - b);
    else if (op == '*') pushVec(L, a * b);
    else pushVec(L, scalarA && !scalarB ? b / a : a / b);
    return 1;
}
int vecAdd(lua_State* L) { return vecOp(L, '+'); }
int vecSub(lua_State* L) { return vecOp(L, '-'); }
int vecMul(lua_State* L) { return vecOp(L, '*'); }
int vecDiv(lua_State* L) { return vecOp(L, '/'); }
int vecUnm(lua_State* L) { return pushVec(L, -toVec(L, 1)), 1; }
int vecLen(lua_State* L) { return lua_pushnumber(L, glm::length(toVec(L, 1))), 1; }
int vecString(lua_State* L) {
    const glm::vec3 v = toVec(L, 1);
    lua_pushstring(L, std::format("({:.4g}, {:.4g}, {:.4g})", v.x, v.y, v.z).c_str());
    return 1;
}

// A colour: {r, g, b} (0..1), "#rrggbb", or a vector.
glm::vec3 toColor(lua_State* L, int i) {
    if (lua_type(L, i) == LUA_TSTRING) {
        std::string s = lua_tostring(L, i);
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        if (s.size() == 3) s = {s[0], s[0], s[1], s[1], s[2], s[2]};
        unsigned v = 0;
        if (s.size() != 6 || std::sscanf(s.c_str(), "%x", &v) != 1) luaL_error(L, "a colour is \"#rrggbb\" or {r, g, b} (0 to 1)");
        return glm::vec3(float((v >> 16) & 255), float((v >> 8) & 255), float(v & 255)) / 255.0f;
    }
    if (lua_istable(L, i)) {
        glm::vec3 c(0.0f);
        const char* names[] = {"r", "g", "b"};
        for (int k = 0; k < 3; ++k) {
            if (lua_getfield(L, i, names[k]) == LUA_TNIL) {
                lua_pop(L, 1);
                if (lua_getfield(L, i, k == 0 ? "x" : k == 1 ? "y" : "z") == LUA_TNIL) lua_pop(L, 1), lua_rawgeti(L, i, k + 1);
            }
            c[k] = float(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        return glm::clamp(c, 0.0f, 1.0f);
    }
    luaL_error(L, "a colour is \"#rrggbb\" or {r, g, b} (0 to 1)");
    return {};
}

// ── Objects: handles by id ──

void pushObject(lua_State* L, uint32_t id) {
    *static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0)) = id;
    luaL_setmetatable(L, kObject);
}
uint32_t objectId(lua_State* L, int i) { return *static_cast<uint32_t*>(luaL_checkudata(L, i, kObject)); }
Object& object(lua_State* L, int i) {
    Object* o = findObject(scene(L), objectId(L, i));
    if (!o) luaL_error(L, "that object isn't there any more");
    return *o;
}

std::string uniqueName(Scene& s, const std::string& base) {
    auto taken = [&](const std::string& n) { return std::any_of(s.objects.begin(), s.objects.end(), [&](const Object& o) { return o.name == n; }); };
    if (!taken(base)) return base;
    for (int i = 2;; ++i)
        if (!taken(std::format("{} {}", base, i))) return std::format("{} {}", base, i);
}

Object& newObject(lua_State* L, const std::string& name, Mesh mesh) {
    Scene& s = scene(L);
    Object o;
    o.id = s.nextId++;
    o.name = uniqueName(s, name);
    o.mesh = std::move(mesh);
    s.objects.push_back(std::move(o));
    ++context(L).made;
    return s.objects.back();
}

void handMade(Object& o) { o.recipe.clear(), o.settings.clear(); }

std::string mirrorText(int m) {
    std::string s;
    for (int a = 0; a < 3; ++a)
        if (m & (1 << a)) s += char('x' + a);
    return s;
}

// Reading a property: false if there's none by that name.
bool getProperty(lua_State* L, Object& o, const std::string& k) {
    if (k == "id") lua_pushinteger(L, o.id);
    else if (k == "name") lua_pushstring(L, o.name.c_str());
    else if (k == "position") pushVec(L, o.position);
    else if (k == "turn") pushVec(L, toEuler(o.rotation));
    else if (k == "size") pushVec(L, o.scale);
    else if (k == "color" || k == "colour") {
        lua_createtable(L, 3, 0);
        for (int i = 0; i < 3; ++i) lua_pushnumber(L, o.color[i]), lua_rawseti(L, -2, i + 1);
    } else if (k == "metal") lua_pushnumber(L, o.metal);
    else if (k == "rough") lua_pushnumber(L, o.rough);
    else if (k == "smooth") lua_pushboolean(L, o.smooth);
    else if (k == "visible") lua_pushboolean(L, o.visible);
    else if (k == "locked") lua_pushboolean(L, o.locked);
    else if (k == "mirror") lua_pushstring(L, mirrorText(o.mirror).c_str());
    else if (k == "round") lua_pushnumber(L, o.round);
    else if (k == "round_steps") lua_pushinteger(L, o.roundSteps);
    else if (k == "smooth_levels") lua_pushinteger(L, o.smoothLevels);
    else if (k == "shape") lua_pushstring(L, o.recipe.c_str());
    else if (k == "point_count") lua_pushinteger(L, lua_Integer(o.mesh.verts.size()));
    else if (k == "face_count") lua_pushinteger(L, lua_Integer(o.mesh.faces.size()));
    else return false;
    return true;
}

// Changing one: false if there's none by that name.
bool setProperty(lua_State* L, Object& o, const std::string& k, int v) {
    if (k == "name") o.name = luaL_checkstring(L, v);
    else if (k == "position") o.position = toVec(L, v);
    else if (k == "turn") o.rotation = fromEuler(toVec(L, v));
    else if (k == "size") o.scale = toVec(L, v);
    else if (k == "color" || k == "colour") o.color = toColor(L, v);
    else if (k == "metal") o.metal = std::clamp(float(luaL_checknumber(L, v)), 0.0f, 1.0f);
    else if (k == "rough") o.rough = std::clamp(float(luaL_checknumber(L, v)), 0.0f, 1.0f);
    else if (k == "smooth") o.smooth = lua_toboolean(L, v);
    else if (k == "visible") o.visible = lua_toboolean(L, v);
    else if (k == "locked") o.locked = lua_toboolean(L, v);
    else if (k == "mirror") {
        const std::string s = lower(luaL_checkstring(L, v));
        o.mirror = (s.find('x') != std::string::npos ? 1 : 0) | (s.find('y') != std::string::npos ? 2 : 0) | (s.find('z') != std::string::npos ? 4 : 0);
    } else if (k == "material") {
        const MaterialPreset* m = findMaterial(lower(luaL_checkstring(L, v)));
        if (!m) luaL_error(L, "there's no material called %s (fry.materials() lists them)", lua_tostring(L, v));
        else applyMaterial(o, *m);
    } else if (k == "round") o.round = std::max(0.0f, float(luaL_checknumber(L, v)));
    else if (k == "round_steps") o.roundSteps = std::clamp(int(luaL_checkinteger(L, v)), 1, 12);
    else if (k == "smooth_levels") o.smoothLevels = std::clamp(int(luaL_checkinteger(L, v)), 0, 4);
    else return false;
    return true;
}

// A library shape's setting by name (any case): its index, or -1.
int settingIndex(const ShapeRecipe& r, const std::string& name) {
    for (size_t i = 0; i < r.settings.size(); ++i)
        if (lower(r.settings[i].name) == lower(name)) return int(i);
    return -1;
}
void remake(Object& o) {
    if (const ShapeRecipe* r = findRecipe(o.recipe)) o.mesh = makeFromRecipe(*r, o.settings);
}

// The objects in arguments from `first` on: objects, or lists of them.
std::vector<uint32_t> objectArgs(lua_State* L, int first) {
    std::vector<uint32_t> ids;
    for (int i = first; i <= lua_gettop(L); ++i) {
        if (lua_istable(L, i) && !luaL_testudata(L, i, kObject)) {
            const lua_Integer n = luaL_len(L, i);
            for (lua_Integer k = 1; k <= n; ++k) {
                lua_rawgeti(L, i, k);
                ids.push_back(objectId(L, -1));
                lua_pop(L, 1);
            }
        } else ids.push_back(objectId(L, i));
    }
    return ids;
}

// ── Object methods ──

int objMove(lua_State* L) {
    object(L, 1).position += argVec(L, 2);
    lua_settop(L, 1);
    return 1;
}
int objRotate(lua_State* L) {
    Object& o = object(L, 1);
    o.rotation = glm::normalize(fromEuler(argVec(L, 2)) * o.rotation);
    lua_settop(L, 1);
    return 1;
}
int objScale(lua_State* L) {
    object(L, 1).scale *= lua_gettop(L) == 2 && lua_isnumber(L, 2) ? glm::vec3(float(lua_tonumber(L, 2))) : argVec(L, 2);
    lua_settop(L, 1);
    return 1;
}
int objDelete(lua_State* L) {
    const uint32_t id = objectId(L, 1);
    Context& c = context(L);
    std::erase_if(c.host->scene->objects, [&](const Object& o) { return o.id == id; });
    if (c.host->selected) c.host->selected->erase(id);
    if (c.host->active && *c.host->active == id) *c.host->active = 0;
    return 0;
}
int objCopy(lua_State* L) {
    Object copy = object(L, 1);
    Scene& s = scene(L);
    copy.id = s.nextId++;
    copy.name = uniqueName(s, copy.name);
    s.objects.push_back(std::move(copy));
    ++context(L).made;
    pushObject(L, s.objects.back().id);
    return 1;
}
int objCombine(lua_State* L, Combine op) {
    const uint32_t keep = objectId(L, 1);
    const std::vector<uint32_t> others = objectArgs(L, 2);
    Scene& s = scene(L);
    Object& a = object(L, 1);
    Mesh result = a.hasFinish() ? finishedMesh(a) : a.mesh;
    const glm::mat4 into = glm::inverse(objectMatrix(a));
    for (uint32_t id : others) {
        const Object* b = findObject(s, id);
        if (!b || id == keep) continue;
        Mesh m = b->hasFinish() ? finishedMesh(*b) : b->mesh;
        transform(m, into * objectMatrix(*b));
        if (!combine(result, m, op)) return luaL_error(L, op == Combine::Overlap ? "they don't overlap: nothing would be left" : "that wouldn't leave anything");
    }
    Object& kept = object(L, 1);
    kept.mesh = std::move(result);
    kept.mirror = 0, kept.round = 0, kept.smoothLevels = 0;
    handMade(kept);
    std::erase_if(s.objects, [&](const Object& o) { return o.id != keep && std::find(others.begin(), others.end(), o.id) != others.end(); });
    lua_settop(L, 1);
    return 1;
}
int objAdd(lua_State* L) { return objCombine(L, Combine::Add); }
int objSubtract(lua_State* L) { return objCombine(L, Combine::Subtract); }
int objOverlap(lua_State* L) { return objCombine(L, Combine::Overlap); }
int objJoin(lua_State* L) {
    const uint32_t keep = objectId(L, 1);
    const std::vector<uint32_t> others = objectArgs(L, 2);
    Scene& s = scene(L);
    const glm::mat4 into = glm::inverse(objectMatrix(object(L, 1)));
    Mesh joined = object(L, 1).mesh;
    for (uint32_t id : others)
        if (const Object* b = findObject(s, id); b && id != keep) append(joined, b->mesh, into * objectMatrix(*b));
    Object& a = object(L, 1);
    a.mesh = std::move(joined);
    handMade(a);
    std::erase_if(s.objects, [&](const Object& o) { return o.id != keep && std::find(others.begin(), others.end(), o.id) != others.end(); });
    lua_settop(L, 1);
    return 1;
}
int objThicken(lua_State* L) {
    Object& o = object(L, 1);
    thicken(o.mesh, float(luaL_checknumber(L, 2)));
    handMade(o);
    lua_settop(L, 1);
    return 1;
}
int objSubdivide(lua_State* L) {
    Object& o = object(L, 1);
    const bool smooth = lua_isnone(L, 2) || lua_toboolean(L, 2);
    const int times = std::clamp(int(luaL_optinteger(L, 3, 1)), 1, 4);
    for (int i = 0; i < times; ++i) subdivide(o.mesh, smooth);
    if (o.mesh.faces.size() > 2000000) return luaL_error(L, "that's too many faces");
    handMade(o);
    lua_settop(L, 1);
    return 1;
}
int objBevel(lua_State* L) {
    Object& o = object(L, 1);
    const float width = float(luaL_checknumber(L, 2));
    const int steps = std::clamp(int(luaL_optinteger(L, 3, 3)), 1, 12);
    const float angle = float(luaL_optnumber(L, 4, 30.0));
    bevelEdges(o.mesh, sharpEdges(o.mesh, angle), width, steps);
    handMade(o);
    lua_settop(L, 1);
    return 1;
}
int objBake(lua_State* L) {
    Object& o = object(L, 1);
    o.mesh = finishedMesh(o);
    o.mirror = 0, o.round = 0, o.smoothLevels = 0;
    handMade(o);
    lua_settop(L, 1);
    return 1;
}
int objPoints(lua_State* L) {
    const Object& o = object(L, 1);
    lua_createtable(L, int(o.mesh.verts.size()), 0);
    for (size_t i = 0; i < o.mesh.verts.size(); ++i) pushVec(L, o.mesh.verts[i]), lua_rawseti(L, -2, lua_Integer(i + 1));
    return 1;
}
int objFaces(lua_State* L) {
    const Object& o = object(L, 1);
    lua_createtable(L, int(o.mesh.faces.size()), 0);
    for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
        lua_createtable(L, int(o.mesh.faces[f].size()), 0);
        for (size_t c = 0; c < o.mesh.faces[f].size(); ++c) lua_pushinteger(L, o.mesh.faces[f][c] + 1), lua_rawseti(L, -2, lua_Integer(c + 1));
        lua_rawseti(L, -2, lua_Integer(f + 1));
    }
    return 1;
}
std::vector<glm::vec3> pointList(lua_State* L, int i) {
    luaL_checktype(L, i, LUA_TTABLE);
    std::vector<glm::vec3> pts;
    const lua_Integer n = luaL_len(L, i);
    for (lua_Integer k = 1; k <= n; ++k) {
        lua_rawgeti(L, i, k);
        pts.push_back(toVec(L, lua_gettop(L)));
        lua_pop(L, 1);
    }
    return pts;
}
std::vector<std::vector<int>> faceList(lua_State* L, int i, size_t points) {
    luaL_checktype(L, i, LUA_TTABLE);
    std::vector<std::vector<int>> faces;
    const lua_Integer n = luaL_len(L, i);
    for (lua_Integer k = 1; k <= n; ++k) {
        lua_rawgeti(L, i, k);
        luaL_checktype(L, -1, LUA_TTABLE);
        std::vector<int> f;
        const lua_Integer m = luaL_len(L, -1);
        for (lua_Integer c = 1; c <= m; ++c) {
            lua_rawgeti(L, -1, c);
            const lua_Integer v = lua_tointeger(L, -1);
            lua_pop(L, 1);
            if (v < 1 || size_t(v) > points) luaL_error(L, "face %d: point %d isn't one of the %d points", int(k), int(v), int(points));
            f.push_back(int(v - 1));
        }
        if (f.size() < 3) luaL_error(L, "face %d has fewer than 3 points", int(k));
        faces.push_back(std::move(f));
        lua_pop(L, 1);
    }
    return faces;
}
int objSetPoints(lua_State* L) {
    std::vector<glm::vec3> pts = pointList(L, 2);
    Object& o = object(L, 1);
    if (pts.size() != o.mesh.verts.size()) return luaL_error(L, "it has %d points, not %d (setshape changes how many)", int(o.mesh.verts.size()), int(pts.size()));
    o.mesh.verts = std::move(pts);
    handMade(o);
    lua_settop(L, 1);
    return 1;
}
int objSetShape(lua_State* L) {
    std::vector<glm::vec3> pts = pointList(L, 2);
    auto faces = faceList(L, 3, pts.size());
    Object& o = object(L, 1);
    o.mesh = {std::move(pts), std::move(faces)};
    o.uvs.clear(), o.picture.reset(), o.layers.clear();
    handMade(o);
    lua_settop(L, 1);
    return 1;
}
int objSet(lua_State* L) { // o:set("Width", 2) or o:set{width = 2, height = 1}
    Object& o = object(L, 1);
    const ShapeRecipe* r = findRecipe(o.recipe);
    if (!r) return luaL_error(L, "%s isn't a library shape any more (its shape was changed by hand)", o.name.c_str());
    auto one = [&](const std::string& name, float value) {
        const int i = settingIndex(*r, name);
        if (i < 0) luaL_error(L, "a %s has no setting called %s", r->name, name.c_str());
        o.settings.resize(r->settings.size());
        const ShapeSetting& st = r->settings[size_t(i)];
        o.settings[size_t(i)] = std::clamp(st.whole ? std::round(value) : value, st.min, st.max);
    };
    if (lua_istable(L, 2)) {
        lua_pushnil(L);
        while (lua_next(L, 2)) one(luaL_checkstring(L, -2), float(luaL_checknumber(L, -1))), lua_pop(L, 1);
    } else one(luaL_checkstring(L, 2), float(luaL_checknumber(L, 3)));
    remake(o);
    lua_settop(L, 1);
    return 1;
}
int objSettings(lua_State* L) {
    const Object& o = object(L, 1);
    lua_newtable(L);
    if (const ShapeRecipe* r = findRecipe(o.recipe)) {
        const std::vector<float> s = o.settings.size() == r->settings.size() ? o.settings : defaultSettings(*r);
        for (size_t i = 0; i < r->settings.size(); ++i) lua_pushnumber(L, s[i]), lua_setfield(L, -2, lower(r->settings[i].name).c_str());
    }
    return 1;
}
int objBounds(lua_State* L) {
    const Object& o = object(L, 1);
    glm::vec3 lo(1e30f), hi(-1e30f);
    const glm::mat4 m = objectMatrix(o);
    for (const glm::vec3& v : o.mesh.verts) {
        const glm::vec3 w = glm::vec3(m * glm::vec4(v, 1.0f));
        lo = glm::min(lo, w), hi = glm::max(hi, w);
    }
    if (o.mesh.verts.empty()) lo = hi = o.position;
    pushVec(L, lo), pushVec(L, hi);
    return 2;
}
int objSelect(lua_State* L) {
    const uint32_t id = objectId(L, 1);
    Context& c = context(L);
    if (c.host->selected) c.host->selected->insert(id);
    if (c.host->active) *c.host->active = id;
    lua_settop(L, 1);
    return 1;
}
int objDrop(lua_State* L) { // stands it on the ground (z = 0)
    lua_settop(L, 1);
    lua_getfield(L, 1, "bounds");
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    const float bottom = toVec(L, 2).z;
    object(L, 1).position.z -= bottom;
    lua_settop(L, 1);
    return 1;
}

const luaL_Reg kMethods[] = {
    {"move", objMove},       {"rotate", objRotate},     {"scale", objScale},       {"delete", objDelete},   {"copy", objCopy},
    {"add", objAdd},         {"subtract", objSubtract}, {"overlap", objOverlap},   {"join", objJoin},       {"thicken", objThicken},
    {"subdivide", objSubdivide}, {"bevel", objBevel},   {"bake", objBake},         {"points", objPoints},   {"faces", objFaces},
    {"setpoints", objSetPoints}, {"setshape", objSetShape}, {"set", objSet},       {"settings", objSettings}, {"bounds", objBounds},
    {"select", objSelect},   {"drop", objDrop},         {nullptr, nullptr},
};

int objIndex(lua_State* L) {
    const std::string k = luaL_checkstring(L, 2);
    for (const luaL_Reg* m = kMethods; m->name; ++m)
        if (k == m->name) return lua_pushcfunction(L, m->func), 1;
    Object& o = object(L, 1);
    if (getProperty(L, o, k)) return 1;
    if (const ShapeRecipe* r = findRecipe(o.recipe); r && settingIndex(*r, k) >= 0) { // o.width, o.sides... for library shapes
        const std::vector<float> s = o.settings.size() == r->settings.size() ? o.settings : defaultSettings(*r);
        return lua_pushnumber(L, s[size_t(settingIndex(*r, k))]), 1;
    }
    return luaL_error(L, "objects have no %s (see the Script panel's help)", k.c_str());
}
int objNewIndex(lua_State* L) {
    const std::string k = luaL_checkstring(L, 2);
    Object& o = object(L, 1);
    if (setProperty(L, o, k, 3)) return 0;
    if (const ShapeRecipe* r = findRecipe(o.recipe); r && settingIndex(*r, k) >= 0) {
        lua_settop(L, 3);
        lua_pushcfunction(L, objSet);
        lua_pushvalue(L, 1), lua_pushvalue(L, 2), lua_pushvalue(L, 3);
        lua_call(L, 3, 0);
        return 0;
    }
    return luaL_error(L, "objects have no %s to change (see the Script panel's help)", k.c_str());
}
int objString(lua_State* L) {
    const Object* o = findObject(scene(L), objectId(L, 1));
    lua_pushstring(L, o ? std::format("{} (#{})", o->name, o->id).c_str() : "(gone)");
    return 1;
}
int objEq(lua_State* L) { return lua_pushboolean(L, objectId(L, 1) == objectId(L, 2)), 1; }

// Sets the properties (and library settings) in a table on the object at index o.
void applyOptions(lua_State* L, int obj, int opts) {
    if (!lua_istable(L, opts)) return;
    lua_pushnil(L);
    while (lua_next(L, opts)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            lua_pushvalue(L, -2), lua_insert(L, -2); // key, key, value
            lua_pushcfunction(L, objNewIndex);
            lua_pushvalue(L, obj), lua_pushvalue(L, -4), lua_pushvalue(L, -4);
            lua_call(L, 3, 0);
            lua_pop(L, 2);
        } else lua_pop(L, 1);
    }
}

// ── fry.* ──

int fryAdd(lua_State* L) {
    const std::string id = lower(luaL_checkstring(L, 1));
    const ShapeRecipe* r = findRecipe(id);
    if (!r) return luaL_error(L, "there's no shape called %s (fry.shapes() lists them)", id.c_str());
    Object& o = newObject(L, r->name, makeFromRecipe(*r, defaultSettings(*r)));
    o.recipe = r->id, o.settings = defaultSettings(*r), o.smooth = r->smooth;
    pushObject(L, o.id);
    applyOptions(L, lua_gettop(L), 2);
    return 1;
}
int fryMesh(lua_State* L) {
    std::vector<glm::vec3> pts = pointList(L, 1);
    auto faces = faceList(L, 2, pts.size());
    Object& o = newObject(L, luaL_optstring(L, 3, "Mesh"), Mesh{std::move(pts), std::move(faces)});
    pushObject(L, o.id);
    return 1;
}
int fryLathe(lua_State* L) { // a profile of {out, up} points turned round Z
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<glm::vec2> profile;
    const lua_Integer n = luaL_len(L, 1);
    for (lua_Integer k = 1; k <= n; ++k) {
        lua_rawgeti(L, 1, k);
        const glm::vec3 v = toVec(L, lua_gettop(L));
        profile.push_back({std::max(v.x, 0.0f), v.y});
        lua_pop(L, 1);
    }
    if (profile.size() < 2) return luaL_error(L, "a lathe needs two or more {out, up} points");
    const int sides = std::clamp(int(luaL_optinteger(L, 2, 32)), 3, 512);
    Object& o = newObject(L, luaL_optstring(L, 3, "Turned"), lathe(profile, sides));
    o.smooth = true;
    pushObject(L, o.id);
    return 1;
}
int fryExtrude(lua_State* L) { // an outline on the ground ({x, y} points) made `height` tall
    std::vector<glm::vec3> pts = pointList(L, 1);
    const float h = float(luaL_checknumber(L, 2));
    if (pts.size() < 3) return luaL_error(L, "an outline needs three or more points");
    for (glm::vec3& p : pts) p.z = 0;
    Mesh m = extrudeOutline(pts, glm::vec3(0, 0, 1), h);
    transform(m, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, h * 0.5f)));
    Object& o = newObject(L, luaL_optstring(L, 3, "Outline"), std::move(m));
    pushObject(L, o.id);
    return 1;
}
int fryObjects(lua_State* L) {
    const Scene& s = scene(L);
    lua_createtable(L, int(s.objects.size()), 0);
    for (size_t i = 0; i < s.objects.size(); ++i) pushObject(L, s.objects[i].id), lua_rawseti(L, -2, lua_Integer(i + 1));
    return 1;
}
int fryFind(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    for (const Object& o : scene(L).objects)
        if (o.name == name) return pushObject(L, o.id), 1;
    return lua_pushnil(L), 1;
}
int frySelected(lua_State* L) {
    Context& c = context(L);
    lua_newtable(L);
    if (!c.host->selected) return 1;
    int i = 0;
    for (const Object& o : c.host->scene->objects) // (in the scene's order, the last picked last)
        if (c.host->selected->count(o.id) && (!c.host->active || o.id != *c.host->active)) pushObject(L, o.id), lua_rawseti(L, -2, ++i);
    if (c.host->active && c.host->selected->count(*c.host->active) && findObject(*c.host->scene, *c.host->active))
        pushObject(L, *c.host->active), lua_rawseti(L, -2, ++i);
    return 1;
}
int frySelect(lua_State* L) {
    const std::vector<uint32_t> ids = objectArgs(L, 1);
    Context& c = context(L);
    if (c.host->selected) c.host->selected->clear(), c.host->selected->insert(ids.begin(), ids.end());
    if (c.host->active) *c.host->active = ids.empty() ? 0 : ids.back();
    return 0;
}
int fryClear(lua_State* L) {
    Context& c = context(L);
    c.host->scene->objects.clear();
    c.host->scene->folders.clear();
    if (c.host->selected) c.host->selected->clear();
    if (c.host->active) *c.host->active = 0;
    return 0;
}
int fryGroup(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    const std::vector<uint32_t> ids = objectArgs(L, 2);
    Scene& s = scene(L);
    Folder f;
    f.id = s.nextId++;
    f.name = name;
    for (Object& o : s.objects)
        if (std::find(ids.begin(), ids.end(), o.id) != ids.end()) o.folder = f.id;
    s.folders.push_back(f);
    return 0;
}
int fryShapes(lua_State* L) {
    lua_newtable(L);
    int i = 0;
    for (const ShapeRecipe& r : shapeRecipes()) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, r.id), lua_setfield(L, -2, "id");
        lua_pushstring(L, r.name), lua_setfield(L, -2, "name");
        lua_pushboolean(L, r.starter), lua_setfield(L, -2, "starter");
        lua_createtable(L, int(r.settings.size()), 0);
        for (size_t k = 0; k < r.settings.size(); ++k) lua_pushstring(L, lower(r.settings[k].name).c_str()), lua_rawseti(L, -2, lua_Integer(k + 1));
        lua_setfield(L, -2, "settings");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}
int fryMaterials(lua_State* L) { // fry.materials(): every material's name, for o.material = ...
    lua_newtable(L);
    int i = 0;
    for (const MaterialPreset& m : materialPresets()) lua_pushstring(L, m.id), lua_rawseti(L, -2, ++i);
    return 1;
}
int fryLook(lua_State* L) { // fry.look{sun_turn = 40, sky = false, ...}; returns the look as it is
    Look& k = scene(L).look;
    struct Num {
        const char* name;
        float* v;
    } nums[] = {{"sun_turn", &k.sunTurn}, {"sun_height", &k.sunHeight}, {"sun_strength", &k.sunStrength}, {"exposure", &k.exposure},
                {"bloom", &k.bloom}, {"saturation", &k.saturation}, {"vignette", &k.vignette}, {"shadow_soft", &k.shadowSoft}};
    struct Col {
        const char* name;
        glm::vec3* v;
    } cols[] = {{"sun_color", &k.sunColor}, {"background", &k.background}, {"ground_color", &k.groundColor}};
    if (lua_istable(L, 1)) {
        for (const Num& n : nums)
            if (lua_getfield(L, 1, n.name) != LUA_TNIL) *n.v = float(luaL_checknumber(L, -1)), lua_pop(L, 1);
            else lua_pop(L, 1);
        for (const Col& c : cols)
            if (lua_getfield(L, 1, c.name) != LUA_TNIL) *c.v = toColor(L, lua_gettop(L)), lua_pop(L, 1);
            else lua_pop(L, 1);
        if (lua_getfield(L, 1, "sky") != LUA_TNIL) k.sky = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, 1, "ground") != LUA_TNIL) k.ground = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    lua_newtable(L);
    for (const Num& n : nums) lua_pushnumber(L, *n.v), lua_setfield(L, -2, n.name);
    for (const Col& c : cols) {
        lua_createtable(L, 3, 0);
        for (int i = 0; i < 3; ++i) lua_pushnumber(L, (*c.v)[i]), lua_rawseti(L, -2, i + 1);
        lua_setfield(L, -2, c.name);
    }
    lua_pushboolean(L, k.sky), lua_setfield(L, -2, "sky");
    lua_pushboolean(L, k.ground), lua_setfield(L, -2, "ground");
    return 1;
}
int fryVec(lua_State* L) { return pushVec(L, lua_istable(L, 1) ? toVec(L, 1) : glm::vec3(float(luaL_optnumber(L, 1, 0)), float(luaL_optnumber(L, 2, 0)), float(luaL_optnumber(L, 3, 0)))), 1; }
int fryPrint(lua_State* L) {
    std::string line;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        if (i > 1) line += "\t";
        line += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    if (const auto& p = context(L).host->print) p(line);
    return 0;
}
int fryFrame(lua_State* L) {
    if (const auto& f = context(L).host->frame) f();
    return 0;
}
int fryHelp(lua_State* L) {
    if (const auto& p = context(L).host->print) p(scriptHelp());
    return 0;
}

const luaL_Reg kFry[] = {
    {"add", fryAdd},       {"mesh", fryMesh},         {"lathe", fryLathe}, {"extrude", fryExtrude}, {"objects", fryObjects}, {"find", fryFind},
    {"selected", frySelected}, {"select", frySelect}, {"clear", fryClear},  {"group", fryGroup},     {"shapes", fryShapes},   {"look", fryLook}, {"materials", fryMaterials},
    {"vec", fryVec},       {"say", fryPrint},         {"frame", fryFrame}, {"help", fryHelp},       {nullptr, nullptr},
};

// Stops a script that's run too long (an endless loop).
void watchdog(lua_State* L, lua_Debug*) {
    const Context& c = context(L);
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - c.start).count() > c.host->timeLimit)
        luaL_error(L, "stopped: it ran for more than %d seconds", int(c.host->timeLimit));
}

// Not more memory than this (a runaway table).
void* allocate(void* ud, void* ptr, size_t old, size_t size) {
    size_t& used = *static_cast<size_t*>(ud);
    if (size == 0) {
        used -= ptr ? old : 0;
        std::free(ptr);
        return nullptr;
    }
    const size_t before = ptr ? old : 0;
    if (used - before + size > (size_t(1) << 30)) return nullptr;
    void* p = std::realloc(ptr, size);
    if (p) used = used - before + size;
    return p;
}

int traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

} // namespace

ScriptResult runScript(ScriptHost& host, const std::string& code, const std::string& name) {
    size_t used = 0;
    lua_State* L = lua_newstate(allocate, &used);
    if (!L) return {false, "out of memory"};
    Context ctx{&host, std::chrono::steady_clock::now()};
    lua_pushlightuserdata(L, &ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, "fry.context");
    const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},          {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                             {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine}};
    for (const luaL_Reg& l : libs) luaL_requiref(L, l.name, l.func, 1), lua_pop(L, 1);
    for (const char* gone : {"dofile", "loadfile", "load", "collectgarbage"}) lua_pushnil(L), lua_setglobal(L, gone);
    lua_pushcfunction(L, fryPrint);
    lua_setglobal(L, "print");

    luaL_newmetatable(L, kVec);
    const luaL_Reg vecMeta[] = {{"__add", vecAdd}, {"__sub", vecSub}, {"__mul", vecMul}, {"__div", vecDiv},
                                {"__unm", vecUnm}, {"__len", vecLen}, {"__tostring", vecString}, {nullptr, nullptr}};
    luaL_setfuncs(L, vecMeta, 0);
    lua_pop(L, 1);
    luaL_newmetatable(L, kObject);
    const luaL_Reg objMeta[] = {{"__index", objIndex}, {"__newindex", objNewIndex}, {"__tostring", objString}, {"__eq", objEq}, {nullptr, nullptr}};
    luaL_setfuncs(L, objMeta, 0);
    lua_pop(L, 1);
    luaL_newlib(L, kFry);
    lua_setglobal(L, "fry");
    lua_sethook(L, watchdog, LUA_MASKCOUNT, 100000);

    ScriptResult result;
    lua_pushcfunction(L, traceback);
    try {
        if (luaL_loadbuffer(L, code.data(), code.size(), ("=" + name).c_str()) != LUA_OK || lua_pcall(L, 0, 0, 1) != LUA_OK) {
            result.ok = false;
            result.error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "an error";
        }
    } catch (const std::exception& e) { // (out of memory inside the modelling code)
        result.ok = false;
        result.error = e.what();
    }
    result.objectsMade = ctx.made;
    lua_close(L);
    // Nothing selected that's gone.
    if (host.selected)
        std::erase_if(*host.selected, [&](uint32_t id) { return !findObject(*host.scene, id); });
    if (host.active && !findObject(*host.scene, *host.active)) *host.active = host.selected && !host.selected->empty() ? *host.selected->rbegin() : 0;
    return result;
}

const char* scriptHelp() {
    return R"(Scripts are Lua (lua.org/manual/5.4). Sizes are metres, turns degrees, Z is up.

fry.add(shape, {setting = value, property = value, ...})   a library shape: fry.add("cube", {width = 2, color = "#ff8000"})
fry.shapes()                     every shape: {id, name, settings}
fry.materials()                  every material's name, for o.material
fry.mesh(points, faces, name)    your own: points {{x,y,z}, ...}, faces {{1,2,3,4}, ...} (counter-clockwise from outside)
fry.lathe(profile, sides, name)  a profile of {out, up} points turned round the Z axis (bottles, vases, bowls)
fry.extrude(outline, height, name)  an outline of {x, y} points on the ground, made that tall
fry.objects()  fry.find(name)  fry.selected()  fry.select(objects)  fry.clear()
fry.group(name, objects)         puts them in a folder
fry.look{sun_turn, sun_height, sun_strength, sun_color, sky, ground, background, ground_color, exposure, bloom, saturation, vignette, shadow_soft}
fry.vec(x, y, z)                 a vector (+ - * / work on them; #v is its length)
fry.frame()  fry.say(text)  print(...)  fry.help()

An object (o):
  o.name  o.position  o.turn  o.size  o.color  o.metal  o.rough  o.smooth  o.visible  o.locked
  o.material = "gold"  sets colour, metal and rough ("matte", "satin", "glossy" keep the colour)
  o.mirror ("x", "xy"...)  o.round  o.round_steps  o.smooth_levels   (finishing touches, not baked)
  o.width, o.sides ... a library shape's own settings, changeable until its shape is changed by hand
  o.id  o.shape  o.point_count  o.face_count   (to read)
  o:move(x, y, z)  o:rotate(x, y, z)  o:scale(k or x, y, z)  o:drop() (stand it on the ground)
  o:copy()  o:delete()  o:select()  o:bounds() -> lowest, highest corner
  o:add(others...)  o:subtract(others...)  o:overlap(others...)  o:join(others...)   (the others go)
  o:thicken(t)  o:subdivide(smooth, times)  o:bevel(width, steps, angle)  o:bake()
  o:points()  o:faces()  o:setpoints(points)  o:setshape(points, faces)  o:set(name, value)  o:settings()
)";
}

} // namespace fry
