#!/usr/bin/env python3
"""Packs builds into the files people download, in dist/:

    Fry3DX-<version>-linux-x64.tar.gz     from build/portable (cmake --preset portable)
    Fry3DX-<version>-linux-x64.AppImage   the same, as one file (needs appimagetool: $APPIMAGETOOL or on PATH)
    Fry3DX-<version>-windows-x64.zip      from build/windows, or build/<dir> given with --windows-bin
    Fry3DX-<version>-macos.zip            from build/macos-arm64 and build/macos-x64 (on a Mac): one
                                          Fry3DX.app for Apple silicon and Intel

    python3 tools/package.py linux|appimage|windows|macos [--bin DIR]

Each unpacks to one Fry3DX/ folder: the program, its content (engine/, Fry3DX/; on a Mac inside
the app), LICENSE and a README.txt. (The Android APK: android/build_apk.py.)
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
Needs {needs}. F1 in the app lists every key.
Your settings and autosaves: {prefs}

Open source under the Apache License 2.0 (LICENSE). Sky pictures: Poly Haven, CC0.
"""


def version():
    with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
        for line in f:
            if line.startswith("project(Fry3DX VERSION "):
                return line.split()[2]
    return "0"


PC_NEEDS = "a graphics card with Vulkan (or OpenGL 4.6)"


def readme(folder, start, prefs, needs=PC_NEEDS, crlf=False):
    shutil.copy2(os.path.join(ROOT, "LICENSE"), folder)
    with open(os.path.join(folder, "README.txt"), "w", newline="\r\n" if crlf else "\n") as f:
        f.write(README.format(version=version(), start=start, prefs=prefs, needs=needs))


def stage(bin_dir, exe, extra, start, prefs):
    out = os.path.join(DIST, "stage", "Fry3DX")
    shutil.rmtree(os.path.dirname(out), ignore_errors=True)
    os.makedirs(out)
    for name in [exe, *extra]:
        shutil.copy2(os.path.join(bin_dir, name), out)
    for d in ("engine", "Fry3DX"):
        shutil.copytree(os.path.join(bin_dir, d), os.path.join(out, d))
    readme(out, start, prefs, crlf=exe.endswith(".exe"))
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


def macos(bin_dir):
    """One Fry3DX.app for both kinds of Mac: the Apple silicon build's app, its program joined
    (lipo) with the Intel build's; signed ad hoc (no Apple account: enough to run, with a right-click
    Open the first time)."""
    arm = os.path.join(bin_dir, "Fry3DX.app")
    intel = os.path.join(ROOT, "build", "macos-x64", "bin", "Fry3DX.app")
    out = os.path.join(DIST, "stage", "Fry3DX")
    shutil.rmtree(os.path.dirname(out), ignore_errors=True)
    os.makedirs(out)
    app = os.path.join(out, "Fry3DX.app")
    shutil.copytree(arm, app, symlinks=True)
    exe = os.path.join("Contents", "MacOS", "Fry3DX")
    if os.path.exists(os.path.join(intel, exe)):
        subprocess.run(["lipo", "-create", os.path.join(arm, exe), os.path.join(intel, exe), "-output", os.path.join(app, exe)], check=True)
        kind = "macos"
    else:
        print("(no build/macos-x64: Apple silicon only)")
        kind = "macos-arm64"
    subprocess.run(["strip", "-x", os.path.join(app, exe)], check=True)
    subprocess.run(["codesign", "--force", "--deep", "--sign", "-", app], check=True)
    readme(out, "Fry3DX.app (move it to Applications if you like). The first time: right-click it, Open,\n"
                "       as it isn't from the App Store", "~/Library/Application Support/Nexoniarz/Fry3DX",
           "macOS 13.3 or newer; Vulkan through MoltenVK (inside the app), else OpenGL 4.1")
    path = os.path.join(DIST, f"Fry3DX-{version()}-{kind}.zip")
    # (ditto keeps what a Mac app needs: links, permissions, the signature)
    subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", out, path], check=True)
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
        default = {"linux": "build/portable/bin", "appimage": "build/portable/bin", "windows": "build/windows/bin", "macos": "build/macos-arm64/bin"}[what]
        path = {"linux": linux, "appimage": appimage, "windows": windows, "macos": macos}[what](bin_dir or os.path.join(ROOT, default))
        print(f"{os.path.relpath(path, ROOT)}: {os.path.getsize(path) / 1048576:.1f} MB")
    shutil.rmtree(os.path.join(DIST, "stage"), ignore_errors=True)


if __name__ == "__main__":
    main()
