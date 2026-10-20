// Everything the page says. To update the site, change this file only:
// add a feature to `features`, a picture to `shots`, a version to `versions` (newest first).
window.FRY = {
  name: "Fry3DX",
  long: "Frying 3D Extreme",
  author: "Nexoniarz",
  authorUrl: "https://github.com/Nexoniarz",
  repo: "https://github.com/Nexoniarz/Fry3DX",
  tagline: "A 3D modeller that does what you'd expect.",
  intro: "Pick a tool, click, drag. Plain words instead of jargon, a toolbox like GIMP's, and the useful stuff from the big modellers without the hundred menus around it. On Windows, macOS, Linux and your phone.",
  releases: "https://github.com/Nexoniarz/Fry3DX/releases/latest",

  why: {
    title: "Why does this exist?",
    text: [
      "Because the well-known modellers suck at being easy. You open them and get a wall of panels, shortcuts nobody would ever guess, and modes inside modes. Making a box with a hole for a screw shouldn't need a tutorial.",
      "So Nexoniarz made one that's built around how people actually think: pick a tool, click on the thing, drag. Every button says what it does, every tool has a tooltip, and the clever parts (rounding, mirroring, combining, checking a print) are one click away.",
    ],
  },

  // The feature tabs, in order (each feature names its group).
  featureGroups: [
    { id: "make", title: "Make" },
    { id: "paint", title: "Paint & sculpt" },
    { id: "print", title: "Render & print" },
    { id: "work", title: "Work" },
  ],

  features: [
    { group: "make", icon: "cube", title: "Shapes that stay editable", text: "Cube, cylinder, sphere, torus, stairs, arch and more, plus starter models like a table, chair or barrel. Change their sizes any time in Properties." },
    { group: "make", icon: "pen", title: "Draw a shape anywhere", text: "Click an outline on the ground or straight on the face of another object: it becomes a new part, gets added to it, or cuts into it." },
    { group: "make", icon: "combine", title: "Add, subtract, overlap", text: "Combine objects: add them together, take one out of another, or keep only where they overlap. The result is always closed and ready to print." },
    { group: "make", icon: "points", title: "Real shape editing", text: "Points, edges and faces. Extrude, inset, round edges, loop cut, knife, merge. Shift+click picks a whole loop, Shift+Ctrl+click a ring." },
    { group: "make", icon: "hole", title: "Holes for screws and nuts", text: "Round, rectangle, triangle or hexagon. M2 to M8 screw and nut sizes ready, with a little room added so the print actually fits." },
    { group: "make", icon: "copies", title: "Copies and thickness", text: "Copies in a row or round in a circle. Make a surface thick, or a solid hollow with walls exactly as thick as you need." },
    { group: "paint", icon: "sculpt", title: "Sculpt it like clay", text: "Raise, puff out, smooth, flatten, pinch and grab the surface with a brush, the same on both sides if you like. A shape with too few points is made finer first, keeping its shape." },
    { group: "print", icon: "sphere", title: "Materials you can see", text: "Gold, chrome, brushed steel, wood, ceramic, rubber and more, or set how metal and rough it is yourself. Shown in the view, not only in the render, and exported with it." },
    { group: "paint", icon: "brush", title: "Paint and stickers", text: "Paint on the model or on its flat picture. Paste any picture (Ctrl+V) and it goes on like a sticker that bends over edges and corners." },
    { group: "paint", icon: "layout", title: "Layout tab", text: "Move, turn and size the flat frames of the surface by hand. The paint follows, so the model looks the same." },
    { group: "print", icon: "print", title: "Made for 3D printing", text: "Your printer's plate in the view, sizes in millimetres, checks for holes and flipped faces with fix buttons, overhangs shown red, weight and filament." },
    { group: "print", icon: "sun", title: "See it rendered", text: "Solid view for modelling, a real renderer with sky, sun and soft shadows when you want to see it properly. Save a picture with F12." },
    { group: "work", icon: "folder", title: "Folders and groups", text: "Layers with folders inside folders, drag and drop, show/hide and lock whole groups, and a button on every row that brings the camera to it." },
    { group: "work", icon: "phone", title: "On your phone too", text: "Not a viewer: the whole modeller. Big buttons, one finger turns the view, two move and pinch to zoom, holding still is a right-click. Files through the phone's own picker." },
    { group: "work", icon: "code", title: "Scripts, if you want them", text: "Lua in a Script panel: build a town in a loop, turn a profile into a vase, move points by a formula. F5 runs it, Ctrl+Z takes it all back." },
    { group: "work", icon: "save", title: "Never lose work", text: "Its own packed .fry file, autosave in the background, recovery after a crash, and a Save / Don't save / Cancel when closing." },
  ],

  shots: [
    { img: "img/model.webp", title: "Model", text: "Starter models with materials (wood, glossy glass, ceramic), a folder for what's on the table, rendered with sky and shadows while you work." },
    { img: "img/sculpt.webp", title: "Sculpt", text: "A ball pushed into a head with the brushes: a nose raised, eyes dug in, a smile pinched, ears grabbed out. Tab in the Paint tab." },
    { img: "img/materials.webp", title: "Materials", text: "Gold, chrome, wood, rubber, copper, ceramic, steel: picked in Properties, shown in the view, exported with the model." },
    { img: "img/paint.webp", title: "Paint", text: "The logo pasted onto a box as a sticker: drag its corners to fit, Enter to lock it on. The Picture panel shows where it lands." },
    { img: "img/layout.webp", title: "Layout", text: "The box's surface laid out flat: move, turn and size the frames, and the paint follows." },
    { img: "img/render.webp", title: "Render", text: "Sun, sky and ground, brightness, soft shadows and glow, and F12 for a picture of just the view." },
    { img: "img/print.webp", title: "Print", text: "Two gears and an arch on a Prusa's plate: sizes in millimetres, checked to be closed, the arch's overhang flagged, weight and filament." },
    { img: "img/combine.webp", title: "Combine", text: "A hole taken out of a box, pegs as copies in a folder, a plane made thick: everything stays closed." },
    { img: "img/scripts.webp", title: "Scripts", text: "A little town from a dozen lines of Lua: random houses, each a folder of walls and roof. F5 runs it, Ctrl+Z takes it back." },
    { img: "img/light.webp", title: "Light", text: "The light appearance, warm paper instead of graphite. Or let it follow your desktop (View > Appearance)." },
    { img: "img/phone.webp", title: "Phone", text: "The phone layout: big buttons along the bottom, Shift and Ctrl that stay down, panels when you want them." },
    { img: "img/crash.webp", title: "Even the crash screen", text: "If something does get burnt, it says so plainly, keeps a report for you, and your work comes back from the last autosave." },
  ],

  scripting: {
    title: "When clicking gets repetitive, write it down.",
    text: "Fry3DX has Lua built in, and a Script panel to write it in. Everything you can click, a script can do: add shapes and change their settings, combine them, move every point by a formula, set the sun. It's optional, small, and can't touch anything outside the scene.",
    points: [
      "F5 runs it; one Ctrl+Z takes back everything it did",
      "If it stops with an error, nothing it did is kept",
      "Examples to start from: stairs, a vase, a gear, a town, a box with a lid",
      "The ? button lists every function, right next to your script",
    ],
    file: "spiral_stairs.lua",
    code: `-- Spiral stairs round a pillar: change the numbers, press F5 again.
local steps, rise, turn = 18, 0.18, 22

fry.add("pillar", {radius = 0.12, height = steps * rise + 0.3})
for i = 0, steps - 1 do
  local step = fry.add("cube", {width = 0.9, depth = 0.32, height = 0.05,
                                color = "#b07a4a"})
  local a = math.rad(i * turn)
  step.turn = {0, 0, i * turn}
  step.position = {math.cos(a) * 0.45, math.sin(a) * 0.45, (i + 1) * rise}
end
fry.frame()`,
  },

  formats: {
    open: [".fry"],
    save: [".fry (packed, with pictures)"],
    export: ["GLB", "glTF", "FBX", "OBJ + MTL", "3MF", "STL", "PLY"],
    note: "FBX opens in Blender, Unity, Unreal and Maya. 3MF and STL are in millimetres for slicers (PrusaSlicer, Bambu Studio, Cura, OrcaSlicer).",
  },

  keys: [
    ["Q W E R", "Select, move, turn, scale"],
    ["Tab", "Edit an object's shape"],
    ["D", "Draw a shape"],
    ["O", "Make a hole"],
    ["Ctrl+G", "Group into a folder"],
    ["Ctrl+Shift + / −", "Add together / take out"],
    ["Ctrl+1 … 5", "Model, Paint, Layout, Render, Print"],
    ["F1", "Every key, in the app"],
  ],

  downloads: [
    { os: "Windows", what: "10 and 11, 64-bit", file: "windows-x64.zip", how: "Unpack it, run fry3dx.exe." },
    { os: "macOS", what: "13.3 and newer, Apple silicon and Intel", file: "macos.zip", how: "Unzip it, open Fry3DX.app. The first time: right-click it and choose Open." },
    { os: "Linux", what: "any distro from 2022 on, 64-bit", file: "linux-x64.AppImage", how: "Make it executable, run it. A .tar.gz is there too." },
    { os: "Android", what: "8.0 and newer, 64-bit, Vulkan", file: "android-arm64.apk", how: "Open it on the phone to install (allow installing from your browser or files app)." },
  ],

  build: {
    text: "Or build it from source with CMake: everything else it needs is downloaded while building. On NixOS, shell.nix has the tools.",
    steps: [
      "git clone https://github.com/Nexoniarz/Fry3DX.git",
      "cd Fry3DX",
      "cmake --preset release && cmake --build build/release",
      "build/release/bin/fry3dx",
    ],
    needs: "Needs: CMake 3.25+, Ninja, a C++20 compiler (GCC 13+, Clang 17+, Xcode 15+) and glslc. Presets for macOS, Windows (cross-built) and Android are in the README.",
  },

  versions: [
    { v: "0.6", date: "2026-10-20", items: ["Sculpting, scripts and a Mac app", "Sculpting: raise, puff out, smooth, flatten, pinch and grab", "Materials shown in the view, sixteen to start from", "Scripts: Lua and a Script panel, with examples", "macOS: one app for Apple silicon and Intel", "A new look (Niveus): light and dark, following the desktop", "A new logo", "Drop a file on the window to open it"] },
    { v: "0.5", date: "2026-10-19", items: ["Builds for Windows, Linux (AppImage) and Android", "A phone layout: big buttons, finger gestures, the phone's file picker", "Apache License 2.0"] },
    { v: "0.4", date: "2026-10-19", items: ["Combine objects: add, subtract, overlap", "Draw shapes onto other objects", "Copies in a row or circle, thicken and hollow", "FBX export", "Folders and groups in Layers, go-to buttons", "Shift scales both sides, Shift+click loops and rings", "A crash screen of its own"] },
    { v: "0.3", date: "2026-10-19", items: ["Stickers that bend over edges, picture layers", "Packed .fry files, autosave and recovery", "Layout tab, Print tab, 3MF / STL / OBJ / PLY / GLB export", "Hole tool: hexagon nut pockets, rectangles, outlines"] },
    { v: "0.1", date: "2026-10-19", items: ["First version: shapes, shape editing, knife, painting, rendering"] },
  ],
};
