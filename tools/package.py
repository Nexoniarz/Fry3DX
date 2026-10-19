#!/usr/bin/env python3
"""Packs builds into the files people download, in dist/:

    Fry3DX-<version>-linux-x64.tar.gz     from build/portable (cmake --preset portable)
    Fry3DX-<version>-linux-x64.AppImage   the same, as one file (needs appimagetool: $APPIMAGETOOL or on PATH)
    Fry3DX-<version>-windows-x64.zip      from build/windows, or build/<dir> given with --windows-bin

    python3 tools/package.py linux|appimage|windows [--bin DIR]

Each unpacks to one Fry3DX/ folder: the program, its content (engine/, Fry3DX/), LICENSE and a
README.txt. (The Android APK: android/build_apk.py.)
"""
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DIST = os.path.join(ROOT, "dist")
README = """Fry3DX {version} - Frying 3D Extreme
=====================================

A 3D modeller that does what you'd expect: pick a tool, click, drag.
Made by Nexoniarz - https://nexoniarz.github.io/Fry3DX/

Start: {start}
Needs a graphics card with Vulkan (or OpenGL 4.6). F1 in the app lists every key.
Your settings and autosaves: {prefs}

Open source under the Apache License 2.0 (LICENSE). Sky pictures: Poly Haven, CC0.
"""


def version():
    with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
        for line in f:
            if line.startswith("project(Fry3DX VERSION "):
                return line.split()[2]
    return "0"


def stage(bin_dir, exe, extra, start, prefs):
    out = os.path.join(DIST, "stage", "Fry3DX")
    shutil.rmtree(os.path.dirname(out), ignore_errors=True)
    os.makedirs(out)
    for name in [exe, *extra]:
        shutil.copy2(os.path.join(bin_dir, name), out)
    for d in ("engine", "Fry3DX"):
        shutil.copytree(os.path.join(bin_dir, d), os.path.join(out, d))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), out)
    with open(os.path.join(out, "README.txt"), "w", newline="\r\n" if exe.endswith(".exe") else "\n") as f:
        f.write(README.format(version=version(), start=start, prefs=prefs))
    return out


def linux(bin_dir):
    out = stage(bin_dir, "fry3dx", [], "./fry3dx (or double-click it)", "~/.local/share/Nexoniarz/Fry3DX")
    subprocess.run(["strip", os.path.join(out, "fry3dx")], check=True)
    path = os.path.join(DIST, f"Fry3DX-{version()}-linux-x64.tar.gz")
    with tarfile.open(path, "w:gz") as t:
        t.add(out, "Fry3DX")
    return path


def appimage(bin_dir):
    tool = os.environ.get("APPIMAGETOOL") or shutil.which("appimagetool")
    if not tool:
        sys.exit("No appimagetool (set APPIMAGETOOL)")
    out = stage(bin_dir, "fry3dx", [], "the AppImage itself", "~/.local/share/Nexoniarz/Fry3DX")
    app = os.path.join(DIST, "stage", "Fry3DX.AppDir")
    # The program and its content where it looks for them (next to it), in usr/bin.
    shutil.copytree(out, os.path.join(app, "usr", "bin"))
    subprocess.run(["strip", os.path.join(app, "usr", "bin", "fry3dx")], check=True)
    os.symlink("usr/bin/fry3dx", os.path.join(app, "AppRun"))
    shutil.copy2(os.path.join(ROOT, "branding", "logo.png"), os.path.join(app, "fry3dx.png"))
    with open(os.path.join(app, "fry3dx.desktop"), "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=Fry3DX\nComment=A 3D modeller that does what you'd expect\n"
                "Exec=fry3dx %f\nIcon=fry3dx\nCategories=Graphics;3DGraphics;\nTerminal=false\n")
    path = os.path.join(DIST, f"Fry3DX-{version()}-linux-x64.AppImage")
    subprocess.run([tool, "--no-appstream", app, path], check=True, env={**os.environ, "ARCH": "x86_64"})
    return path


def windows(bin_dir):
    out = stage(bin_dir, "fry3dx.exe", ["SDL3.dll"], "fry3dx.exe", "%APPDATA%\\Nexoniarz\\Fry3DX")
    path = os.path.join(DIST, f"Fry3DX-{version()}-windows-x64.zip")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for r, _, names in os.walk(out):
            for n in names:
                full = os.path.join(r, n)
                z.write(full, os.path.join("Fry3DX", os.path.relpath(full, out)))
    return path


def main():
    args = sys.argv[1:]
    bin_dir = None
    if "--bin" in args:
        i = args.index("--bin")
        bin_dir = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    os.makedirs(DIST, exist_ok=True)
    for what in args:
        default = {"linux": "build/portable/bin", "appimage": "build/portable/bin", "windows": "build/windows/bin"}[what]
        path = {"linux": linux, "appimage": appimage, "windows": windows}[what](bin_dir or os.path.join(ROOT, default))
        print(f"{os.path.relpath(path, ROOT)}: {os.path.getsize(path) / 1048576:.1f} MB")
    shutil.rmtree(os.path.join(DIST, "stage"), ignore_errors=True)


if __name__ == "__main__":
    main()
