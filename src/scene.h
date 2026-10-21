#pragma once
// What you're making: objects (a mesh each, placed, coloured) and reference pictures to trace.
// Saved as .fry (plain text), exported as glTF for games (Blender, Godot, Unity).
#include "image.h"
#include "mesh.h"

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fry {

// A picture put onto an object's painting, kept apart from it (a layer) so it can be moved again.
// A sticker sits on the surface: `centre` on face `face` (the object's own space), `up` its top's
// way, `width` x `height` across, and wraps over edges like a real one. Otherwise it's pinned onto
// the flat picture by its four corners (pixels: top-left, top-right, bottom-right, bottom-left).
struct PictureLayer {
    std::string name;
    std::shared_ptr<const Image> image;
    bool sticker = true, visible = true;
    float opacity = 1.0f;
    std::array<glm::vec2, 4> corners{};
    int face = 0;
    glm::vec3 centre{0.0f}, up{0.0f, 0.0f, 1.0f};
    float width = 0.5f, height = 0.5f;
    std::string file; // (its PNG's name, set when saving)
};

// A folder in the Layers panel: objects (and folders) kept together, and picked together (a group).
struct Folder {
    uint32_t id = 0;
    std::string name;
    uint32_t parent = 0; // (0: at the top)
    bool open = true, visible = true, locked = false;
};

struct Object {
    uint32_t id = 0;
    std::string name;
    uint32_t folder = 0; // (0: at the top)
    Mesh mesh;                  // around its origin (the point it turns and scales about)
    glm::vec3 position{0.0f};   // metres
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    glm::vec3 color{0.62f, 0.62f, 0.64f}; // as picked (sRGB)
    float metal = 0.0f, rough = 0.5f;
    bool smooth = false;        // smooth shading, creases sharper than smoothAngle kept
    float smoothAngle = 35.0f;
    bool visible = true, locked = false;
    // A shape from the library keeps its recipe and settings (changeable in Properties) until
    // its shape is edited by hand.
    std::string recipe;
    std::vector<float> settings;
    // Finishing touches, not baked into the mesh until applied: mirrored copies across its own
    // X / Y / Z (bits 1, 2, 4), rounded edges (those sharper than roundAngle), smoothing.
    int mirror = 0;
    float round = 0.0f, roundAngle = 30.0f;
    int roundSteps = 3;
    int smoothLevels = 0;
    bool smoothKeepSharp = false;
    std::set<std::pair<int, int>> creases; // edges (points, the lower first) smoothing keeps sharp
    // An editable cut: this object is taken out of that one (its id), and stays an object you can
    // move and change (the hole follows); shown see-through, not exported or printed itself.
    uint32_t cuts = 0;
    bool hasFinish() const { return mirror || round > 0.0f || smoothLevels > 0; }
    // A painted picture on it: where each face corner sits on it (0..1), and the picture
    // (shared with the undo steps until it's painted on again). Saved beside the .fry as a PNG.
    std::vector<std::vector<glm::vec2>> uvs;
    std::shared_ptr<const Image> picture;
    std::string pictureFile; // (the PNG's name, set when saving or exporting)
    std::vector<PictureLayer> layers; // on top of the painting, bottom first
    bool uvsFit() const {
        if (uvs.size() != mesh.faces.size()) return false;
        for (size_t f = 0; f < uvs.size(); ++f)
            if (uvs[f].size() != mesh.faces[f].size()) return false;
        return true;
    }
};

// The picture as it shows: the painting with its layers on top. owner (if given): for each pixel,
// which is on top there (0 the painting, i + 1 layer i).
Image composePicture(const Object& o, std::vector<uint8_t>* owner = nullptr);
// A sticker's face (the one it was put on, or the nearest if the shape's changed), and its axes
// there: right, up (along the face), and the face's normal.
int stickerFace(const Mesh& m, const PictureLayer& l);
bool stickerFrame(const Mesh& m, const PictureLayer& l, glm::vec3& right, glm::vec3& up, glm::vec3& normal);
// A sticker whose middle has gone past its face's edge moves onto the face its middle is on now
// (bent over the same way): it lies on the face with most of it and bends from there.
void settleSticker(const Mesh& m, PictureLayer& l);
// A sticker painted onto a picture (uvs: the object's), unfolded across edges from its face.
void stampSticker(Image& dst, const Mesh& m, const std::vector<std::vector<glm::vec2>>& uvs, const PictureLayer& l, std::vector<uint8_t>* owner = nullptr, uint8_t id = 0);

// After frames of the layout moved (`before` to the object's uvs now): what was painted on those
// faces moved with them, so the model looks the same.
void remapPicture(Image& img, const Mesh& m, const std::vector<std::vector<glm::vec2>>& before, const std::vector<std::vector<glm::vec2>>& after,
                  const std::vector<int>& faces);

// The mesh as shown and exported: with its mirror, rounding and smoothing.
Mesh finishedMesh(const Object& o, bool mirrorOnly = false);

// A picture to trace over, standing on the plane through the origin that a view looks at.
enum class Axis { Front, Side, Top }; // the view it faces: from +X, from +Y (the left), from above
struct Reference {
    uint32_t id = 0;
    std::string path;           // the picture file (PNG, JPG...)
    Axis axis = Axis::Side;
    glm::vec2 offset{0.0f};     // its middle on that plane, metres
    float width = 1.0f;         // metres across
    float opacity = 0.6f;
    bool visible = true;
    bool ownViewOnly = false; // (shown only looking straight at it: a front drawing not in the way from the side)
};

// The Render tab's light and look.
struct Look {
    float sunTurn = 35.0f, sunHeight = 50.0f, sunStrength = 0.6f; // degrees round (from +X), degrees up
    glm::vec3 sunColor{1.0f, 0.96f, 0.9f};
    bool sky = true, ground = true;
    glm::vec3 background{0.2f, 0.2f, 0.215f}, groundColor{0.33f, 0.33f, 0.35f}; // (sRGB)
    float exposure = 1.0f, ao = 1.0f, bloom = 0.035f, saturation = 1.05f, vignette = 0.25f, sharpen = 0.3f, shadowSoft = 1.0f;
};

// A script kept in the file (the Script panel's list): run when you run it, never by itself.
struct SceneScript {
    std::string name, code;
};

struct Scene {
    Look look;
    std::vector<SceneScript> scripts;
    std::vector<Object> objects;
    std::vector<Folder> folders;
    std::vector<Reference> references;
    uint32_t nextId = 1;
};

// Editable cuts: whether something's cut out of it, and it with them cut out (and its finishing
// touches; only the mirror with mirrorOnly) in its own space. The scene with each cut made for
// good and its cutters hidden, for exporting.
const Object* cutTarget(const Scene& s, const Object& cutter); // (what it cuts, if it's there)
bool hasCuts(const Scene& s, const Object& o);
Mesh cutMesh(const Scene& s, const Object& o, bool mirrorOnly = false);
Scene withCutsMade(const Scene& s);

// Materials to start from: a colour (unless `keepColor`: only how metal and rough it is) and
// how metal and rough. Shown in the view (Solid and Rendered) and exported with the object.
struct MaterialPreset {
    std::string id;   // as scripts name it (lower case)
    std::string name; // as shown
    glm::vec3 color;  // sRGB
    float metal, rough;
    bool keepColor = false;
    std::string from; // your own: who made it ("" saved from the picker; else a plug-in's or script's name)
};
const std::vector<MaterialPreset>& materialPresets();
// Your own, after those: saved from the picker (ownMaterialsText, kept by the app) or made by
// scripts and plug-ins (fry.material). One with the same id is replaced.
const std::vector<MaterialPreset>& ownMaterials();
void addOwnMaterial(MaterialPreset m);
bool removeOwnMaterial(const std::string& id);
void clearOwnMaterials(const std::string& from); // a plug-in's, when it's reloaded or turned off
std::string materialId(const std::string& name);    // "Teal plastic" -> "teal plastic"
// The picker's (from == "") as text, and back (added to the rest).
std::string ownMaterialsText();
void parseOwnMaterials(const std::string& text);
const MaterialPreset* findMaterial(std::string_view id); // yours first
void applyMaterial(Object& o, const MaterialPreset& m);

glm::mat4 objectMatrix(const Object& o);
// A turn as degrees about X, then Y, then Z (what the Properties panel shows), and back.
glm::vec3 toEuler(const glm::quat& q);
glm::quat fromEuler(const glm::vec3& degrees);
// Points of a reference's picture: its four corners (counter-clockwise, as seen from its view),
// given its aspect (height / width).
std::array<glm::vec3, 4> referenceCorners(const Reference& r, float aspect);

// The scene as plain text (the old .fry, and what's inside a new one), pictures by file name.
std::string sceneText(const Scene& scene);
std::optional<Scene> parseScene(std::istream& in, const std::string& path, std::string* error = nullptr);
bool saveScene(const std::string& path, const Scene& scene, std::string* error = nullptr);
std::optional<Scene> loadScene(const std::string& path, std::string* error = nullptr);
// Written beside it then moved over it: a crash mid-save never leaves half a file.
bool writeFileSafely(const std::string& path, const void* data, size_t size, std::string* error = nullptr);

// Fry3DX's own file (.fry): one file, the scene and every picture in it, packed small (pictures
// as colour planes of differences, then all of it zstd'd). Opens the old text .fry files too.
// level: zstd's, 1 (quick) to 19 (smallest).
bool savePackage(const std::string& path, const Scene& scene, int level = 12, std::string* error = nullptr);
std::optional<Scene> loadPackage(const std::string& path, std::string* error = nullptr);
bool isPackage(const std::string& path);

// Exported for other programs, by the file's ending:
//   .gltf (and a .bin and PNGs beside it), .glb (all in one): glTF 2.0, its own axes, metres; each
//         visible object a node at its origin, turned and scaled into its points, one material each.
//   .fbx (binary 7.4; pictures inside it and beside it): Y up, centimetres.
//   .obj (and an .mtl and PNGs): Y up, metres.   .ply: Z up, metres, with colours.
//   .stl (binary): Z up, millimetres (for 3D printing), all objects as one.
//   .3mf: for slicers (PrusaSlicer, Bambu Studio, Cura, OrcaSlicer): millimetres, each object apart.
// pngs: each object's picture as a PNG file's bytes (or empty: just its colour).
bool exportModel(const std::string& path, const Scene& scene, const std::vector<std::vector<uint8_t>>& pngs, std::string* error = nullptr);
// What exportModel can write: (ending, name) pairs.
const std::vector<std::pair<std::string, std::string>>& exportFormats();

} // namespace fry
