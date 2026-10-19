// Everything the page says. To update the site, change this file only:
// add a feature to `features`, a picture to `shots`, a version to `versions` (newest first).
window.FRY = {
  name: "Fry3DX",
  long: "Frying 3D Extreme",
  author: "Nexoniarz",
  authorUrl: "https://github.com/Nexoniarz",
  repo: "https://github.com/Nexoniarz/Fry3DX",
  tagline: "A 3D modeller that does what you'd expect.",
  intro: "Pick a tool, click, drag. Plain words instead of jargon, a toolbox like GIMP's, and the useful stuff from the big modellers without the hundred menus around it. On Windows, Linux and your phone.",
  releases: "https://github.com/Nexoniarz/Fry3DX/releases/latest",

  why: {
    title: "Why does this exist?",
    text: [
      "Because the well-known modellers suck at being easy. You open them and get a wall of panels, shortcuts nobody would ever guess, and modes inside modes. Making a box with a hole for a screw shouldn't need a tutorial.",
      "So Nexoniarz made one that's built around how people actually think: pick a tool, click on the thing, drag. Every button says what it does, every tool has a tooltip, and the clever parts (rounding, mirroring, combining, checking a print) are one click away.",
    ],
  },

  features: [
    { icon: "▣", title: "Shapes that stay editable", text: "Cube, cylinder, sphere, torus, stairs, arch and more, plus starter models like a table, chair or barrel. Change their sizes any time in Properties." },
    { icon: "✎", title: "Draw a shape anywhere", text: "Click an outline on the ground or straight on the face of another object: it becomes a new part, gets added to it, or cuts into it." },
    { icon: "⊕", title: "Add, subtract, overlap", text: "Combine objects: add them together, take one out of another, or keep only where they overlap. The result is always closed and ready to print." },
    { icon: "◫", title: "Real shape editing", text: "Points, edges and faces. Extrude, inset, round edges, loop cut, knife, merge. Shift+click picks a whole loop, Shift+Ctrl+click a ring." },
    { icon: "◎", title: "Holes for screws and nuts", text: "Round, rectangle, triangle or hexagon. M2 to M8 screw and nut sizes ready, with a little room added so the print actually fits." },
    { icon: "▤", title: "Copies and thickness", text: "Copies in a row or round in a circle. Make a surface thick, or a solid hollow with walls exactly as thick as you need." },
    { icon: "🖌", title: "Paint and stickers", text: "Paint on the model or on its flat picture. Paste any picture (Ctrl+V) and it goes on like a sticker that bends over edges and corners." },
    { icon: "▦", title: "Layout tab", text: "Move, turn and size the flat frames of the surface by hand. The paint follows, so the model looks the same." },
    { icon: "⎙", title: "Made for 3D printing", text: "Your printer's plate in the view, sizes in millimetres, checks for holes and flipped faces with fix buttons, overhangs shown red, weight and filament." },
    { icon: "☀", title: "See it rendered", text: "Solid view for modelling, a real renderer with sky, sun and soft shadows when you want to see it properly. Save a picture with F12." },
    { icon: "🗀", title: "Folders and groups", text: "Layers with folders inside folders, drag and drop, show/hide and lock whole groups, and a button on every row that brings the camera to it." },
    { icon: "📱", title: "On your phone too", text: "Not a viewer: the whole modeller. Big buttons, one finger turns the view, two move and pinch to zoom, holding still is a right-click. Files through the phone's own picker." },
    { icon: "💾", title: "Never lose work", text: "Its own packed .fry file, autosave in the background, recovery after a crash, and a Save / Don't save / Cancel when closing." },
  ],

  shots: [
    { img: "img/model.webp", title: "Model", text: "Starter models, a folder of parts, a shape drawn onto a cube's face, rendered with sky and shadows." },
    { img: "img/paint.webp", title: "Paint", text: "A picture pasted on as a sticker: it bends over the cube's edge like a real one." },
    { img: "img/print.webp", title: "Print", text: "The printer's plate, sizes in millimetres, checks and overhangs before you slice." },
    { img: "img/phone.webp", title: "Phone", text: "The phone layout: big buttons along the bottom, panels when you want them, fingers for the view." },
    { img: "img/combine.webp", title: "Combine", text: "A cylinder taken out of a cube, copies in a row in a folder, a plane made thick." },
    { img: "img/crash.webp", title: "Even the crash screen", text: "If something does get burnt, your work comes back from the last autosave." },
  ],

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
    needs: "Needs: CMake 3.25+, Ninja, a C++20 compiler (GCC 13+, Clang 17+) and glslc. Presets for Windows (cross-built) and Android are in the README.",
  },

  versions: [
    { v: "0.5", date: "2026-10-19", items: ["Builds for Windows, Linux (AppImage) and Android", "A phone layout: big buttons, finger gestures, the phone's file picker", "Apache License 2.0"] },
    { v: "0.4", date: "2026-10-19", items: ["Combine objects: add, subtract, overlap", "Draw shapes onto other objects", "Copies in a row or circle, thicken and hollow", "FBX export", "Folders and groups in Layers, go-to buttons", "Shift scales both sides, Shift+click loops and rings", "A crash screen of its own"] },
    { v: "0.3", date: "2026-10-19", items: ["Stickers that bend over edges, picture layers", "Packed .fry files, autosave and recovery", "Layout tab, Print tab, 3MF / STL / OBJ / PLY / GLB export", "Hole tool: hexagon nut pockets, rectangles, outlines"] },
    { v: "0.1", date: "2026-10-19", items: ["First version: shapes, shape editing, knife, painting, rendering"] },
  ],
};
