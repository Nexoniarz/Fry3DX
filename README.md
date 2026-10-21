<p align="center"><img src="branding/logo.png" width="96" alt=""></p>

<h1 align="center">Fry3DX</h1>
<p align="center"><b>Frying 3D Extreme — a 3D modeller that does what you'd expect.</b><br>
Pick a tool, click, drag. On Windows, macOS, Linux and your phone.</p>
<p align="center">
  <a href="https://nexoniarz.github.io/Fry3DX/">Website</a> ·
  <a href="https://github.com/Nexoniarz/Fry3DX/releases">Download</a> ·
  <a href="#build-it-yourself">Build it</a> ·
  made by <a href="https://github.com/Nexoniarz">Nexoniarz</a>
</p>

![Fry3DX](docs/img/model.webp)

## Why

Because the well-known modellers suck at being easy: a wall of panels, shortcuts nobody would
guess, modes inside modes. Fry3DX is built the way people think — a toolbox like GIMP's, plain
words on every button, tooltips everywhere — and keeps the useful parts (rounding, mirroring,
combining, holes for screws, checks for 3D printing) one click away.

And it works on a phone. Not a viewer: the whole modeller, with big buttons and fingers.

## Download

From [Releases](https://github.com/Nexoniarz/Fry3DX/releases):

| | |
|---|---|
| **Windows** (10 and 11, x64) | `Fry3DX-…-windows-x64.zip`: unpack, run `fry3dx.exe` |
| **macOS** (13.3 and newer, Apple silicon and Intel) | `Fry3DX-…-macos.zip`: unzip, open `Fry3DX.app` (the first time: right-click it, Open) |
| **Linux** (any distro from 2022 on, x64) | `Fry3DX-…-linux-x64.AppImage`: make it executable, run it. Or the `.tar.gz` |
| **Android** (8.0 and newer, 64-bit, Vulkan) | `Fry3DX-…-android-arm64.apk`: open it on the phone to install |

Needs a graphics card (or phone) with Vulkan; on a PC, OpenGL 4.6 works too. On a Mac, Vulkan
comes inside the app (MoltenVK, on Metal), and OpenGL 4.1 is the fallback.

## What it does

Five tabs along the top: **Model**, **Paint**, **Layout**, **Render**, **Print** (Ctrl+1 to 5).

**Finding things**
- **Any command by name**: F3 or Ctrl+Space, type a few letters. **Round the mouse**: hold Space
  for the tools, Shift+Space for the views. Give any command **your own key** (Help > Keys and
  mouse). **Undo history** (Edit > Undo history): go back to any step. **Workspaces**: the panels as
  you had them, kept under a name.

**Making shapes**
- **Shapes that stay editable**: cube, rounded cube, plane, circle, cylinder, tube, cone, pyramid,
  sphere, capsule, torus, wedge, stairs, arch, and starter models (table, chair, shelf, barrel,
  bottle, pillar, door frame, crate). Their sizes stay changeable in Properties.
- **Draw shape** (D): click an outline on the ground, on a side view's plane, or straight on any
  face of another object: a new part standing on it, added to it, or cut into it.
- **Move, turn, scale** (W, E, R): the arrows turn with the object. Scale keeps the other side
  where it is; Shift scales both sides at once. Ctrl snaps: to steps, or to points and the middles
  of edges and faces.
- **Exact sizes**: type a number while dragging (move, turn, scale, extrude, inset, round edges)
  and Enter for exactly that. Dimensions in Properties; measure anything (View > Measure). Drawing
  a shape shows each line's length: type one for exactly that, Shift holds it to 15° steps.
- **3D text** from any font (Add > Text). **Spin**: an outline turned round an axis into a vase or
  a wheel, or rising into a screw thread. **Import** STL, OBJ, 3MF, glTF and GLB (Ctrl+Shift+O).
- **Round, smooth, mirror** any object, changeable any time, baked in when you like.
- **Materials**: gold, chrome, steel, wood, ceramic, rubber and more, or how metal and rough it
  is; shown in the view (Solid too), exported with it. **Your own**: "Save its look as a
  material" in the picker keeps one for every file.

**Editing the shape** (Tab, or double-click)
- Points, edges, faces (1, 2, 3). Extrude (X), inset (I), round edges (B), loop cut (C), knife (K),
  cut in four, smooth, merge (M), make a face, flip, separate (P), smooth points, push out /
  pull in along the surface (Alt+S), **bridge** two faces with a tunnel, **edge slide** (Alt+G).
- **Crease** (Shift+E): picked edges stay sharp when the shape is smoothed. **Mirror editing**:
  both sides change together. **Select similar** (Shift+G), loops and rings.
- **Soft selection** (Shift+O): points near what you move follow it, less the further they are;
  the wheel while dragging sets how far (Blender's proportional editing, Maya's soft select).
- **Hole** (O): round (M2–M8 screw sizes ready), rectangle, triangle or hexagon (M2–M8 nut
  sizes, with room for prints); all the way through, a pocket, or only its outline as a new face.
- **Selecting**: click, box, brush or lasso. Shift+click a whole loop, Shift+Ctrl+click an
  edge's ring, Ctrl+click adds or takes one.

**Whole objects** (the Object menu)
- **Add together, take one out of another, keep only where they overlap**: exact
  ([Manifold](https://github.com/elalish/manifold)) and always closed, so always printable
  (Ctrl+Shift+ and Ctrl+Shift−).
- **Cut out, editable**: the cutters stay objects. Move one, turn it, change its shape, and the
  hole follows; they go with what they cut. Apply makes it for good.
- **Copies** in a row, round a circle, or **along a path** (turning as it turns); **make it thick** (a surface gets a back, a solid goes
  hollow); join objects; join flat faces.
- **Layers** with folders inside folders, drag and drop, hide and lock whole groups, and a button
  on every row that brings the camera to it. **Show only the selected** (/ or Shift+H; again:
  everything).

**Sculpting** (the Paint tab's other side: Tab)
- Raise, puff out, smooth, flatten, pinch and grab the surface (1 to 6); Ctrl goes the other way,
  Shift smooths, X does the same on both sides. A shape with too few points is made finer first,
  keeping its shape.

**Painting**
- Paint on the model or on its flat picture, on **both sides** at once, or only inside a
  **stencil** you drag round an area. **Paste any picture** (Ctrl+V): it goes on like a
  sticker that bends over edges and corners; Enter locks it as a layer, clicking it in the
  Picture panel unlocks it again.
- **Layout tab**: move, turn and size the flat frames of the surface yourself; the paint follows.

**Seeing it**
- **Getting round**: right drag orbits, middle drag pans, the wheel goes towards what's under the
  mouse; the **arrow keys** move you along the ground (Page Up / Down: up and down, Shift: faster).
- **Solid** view for modelling, **Rendered** with a real sky, sun and soft shadows (Z). Wireframe
  (Shift+Z), X-ray (Alt+Z). The Render tab: the sun, sky, ground, brightness, shadows, glow;
  F12 saves a picture of just the view.
- **Reference pictures** for the front, side and top views, see-through, shown only in their own
  view if you like.

**3D printing** (the Print tab)
- Your printer's plate in the view (Prusa, Bambu, Creality, Elegoo, Anycubic, or your own),
  sizes in millimetres, checks for holes and flipped faces with **one repair button**, overhangs
  in red, **walls too thin** in yellow, weight and filament, measuring, lay flat on a face, save
  3MF or STL.
- **Size it**: by percent, ×½, ×2, inches or centimetres to millimetres, or as big as fits the
  plate. **Slice preview**: one layer as the printer makes it, at any layer height.

**Scripts** (the Script panel, F5)
- Lua with a `fry` library: add shapes and change their settings, combine them, move points by a
  formula, sculpt, set the materials and the sun, make your own materials (`fry.material{}`).
  One Ctrl+Z takes back all a script did; an error takes it back itself. Examples in
  Script > Examples; the **?** button lists every function. It can't reach anything outside the
  scene (no files, no programs).
- **Kept in the file**: the Script panel lists the `.fry`'s scripts (new, examples, open,
  duplicate, rename, remove); they're saved with the model.

**Plug-ins** (Script > Plug-ins)
- Scripts that stay loaded: put a `.lua` in the plug-ins folder and it can add **commands to the
  Add, Object or Script menu** (`fry.command{name, menu, settings, run}`; the settings are asked
  for in a small form first) and **materials** to the picker. Turn each on or off in the Plug-ins
  window, look for one, or make your own copy of one to change. **Mouse tools** too
  (`fry.tool{name, click}`): listed in the Toolbox, a click in the view calls it with the spot.
  Four come with it: stairs (straight and spiral), scatter copies, stamp (a mouse tool), and more
  materials.

```lua
fry.plugin{name = "Shelves", about = "A bookcase from the Add menu"}
fry.command{name = "Bookcase", menu = "add", settings = {{"shelves", 5}, {"width", 0.8}},
  run = function(s)
    for i = 0, s.shelves - 1 do
      fry.add("cube", {width = s.width, depth = 0.3, height = 0.02}).position = {0, 0, i * 0.35}
    end
  end}
```

**The look**: light or dark, or following the desktop (View > Appearance), in
[Niveus](https://github.com/Nexoniarz/niveus): keys that look like keys, fields set into the panel.

**Files**
- `.fry`: Fry3DX's own file, everything in one, packed small. **Autosave** in the background,
  offered back after a crash; Save / Don't save / Cancel when closing. Drop a `.fry`, a script or
  a picture on the window to open it.
- **Export**: GLB, glTF, FBX (Blender, Unity, Unreal, Maya), OBJ, 3MF, STL, PLY.

**On a phone or tablet**: everything bigger, the side panels when you want them, a bar of big
buttons (undo, redo, Shift and Ctrl that stay down, edit the shape, delete, frame). One finger
turns the view (or drags the arrows), two fingers move it and pinch to zoom, holding a finger
still is a right-click. Files open and save through the phone's own file picker. It stays
sideways, and going to another app and back keeps your work.

F1 in the app lists every key.

## Build it yourself

Everything it uses is downloaded while building; you need CMake 3.25+, Ninja, a C++20 compiler
(GCC 13+, Clang 17+, Xcode 15+ or clang-cl) and glslc (from shaderc or the Vulkan SDK). On NixOS,
`shell.nix` has it all; on a Mac, `brew install cmake ninja shaderc molten-vk`.

```sh
git clone https://github.com/Nexoniarz/Fry3DX.git && cd Fry3DX
cmake --preset release && cmake --build build/release      # (NixOS: inside nix-shell)
build/release/bin/fry3dx [file.fry]
cmake --build build/debug --target check                     # the tests (after --preset debug)
```

| Preset | What |
|---|---|
| `debug`, `release` | for working on it |
| `portable` | a Linux build to hand out: SDL and the C++ library inside the program |
| `windows` | Windows, cross-built from Linux with clang-cl ([how](cmake/toolchains/windows-msvc.cmake)) |
| `macos-arm64`, `macos-x64` | `Fry3DX.app` for Apple silicon / Intel (both build on either Mac); `tools/package.py macos` joins them into one app |
| `android` | the Android library; `android/build_apk.py` makes the APK ([how](android/build_apk.py)) |

`tools/package.py` packs builds into the downloads (zip, tar.gz, AppImage, the Mac app). Pushing a tag
`v*` makes GitHub build all of them and put them in a release (`.github/workflows/build.yml`).

## What's where

| | |
|---|---|
| `src/app.cpp` | the app: panels, tools, the view, touch |
| `src/mesh.*`, `tools.cpp`, `cut.cpp` | polygon meshes and the edits on them (tested without a window) |
| `src/boolean.cpp`, `modify.cpp` | combining shapes, thickness, joining flat faces |
| `src/shapes.*` | the shape library and starter models |
| `src/scene.*`, `package.cpp` | objects, folders, materials, the `.fry` file |
| `src/sculpt.*` | the sculpting brushes |
| `src/script.*` | scripts: Lua and the `fry` library; examples in `content/Fry3DX/scripts/` |
| `src/export.cpp`, `print.cpp` | GLB/glTF/FBX/OBJ/3MF/STL/PLY, checks for printing |
| `src/uv.cpp`, `sticker.cpp`, `image.cpp` | flat layouts, stickers, pictures |
| `engine/` | window, Vulkan/OpenGL renderer, UI, files |
| `android/`, `tools/`, `.github/` | builds and packages |
| `docs/` | the website (GitHub Pages); its words are all in `docs/content.js` |
| `tests/` | `mesh_test`, `script_test` (every example script runs) |

## Coming

Animation; textures for materials (wood grain, brushed metal); dynamic detail while sculpting.

## Licence

[Apache License 2.0](LICENSE). Copyright 2026 Nexoniarz.
Sky pictures: Poly Haven, CC0. Fonts: Instrument Sans and JetBrains Mono (SIL Open Font
License), DejaVu (its own free licence). Lua: MIT. MoltenVK (in the Mac app): Apache 2.0.
