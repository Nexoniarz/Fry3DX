#!/usr/bin/env python3
"""Builds Fry3DX's Android APK (arm64, Vulkan) with no Gradle: the NDK build (cmake --preset
android), the content as plain files in assets/, SDL's Java layer, the icon, then aapt2, d8,
zipalign and apksigner.

    nix-shell android/shell.nix --run "python3 android/build_apk.py [--install]"

Out: dist/Fry3DX-<version>-android-arm64.apk. --install puts it on a phone over adb (USB
debugging on) and starts it. The first start copies the content out (a moment).
Signed with ~/.local/share/Nexoniarz/Fry3DX-signing/release.keystore (made the first time),
or the key in $FRY_KEYSTORE / $FRY_KEYSTORE_PASS. Keep it safe and keep using it: Android only
updates an app signed with the key it was installed with.
"""
import json
import os
import secrets
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD = os.path.join(ROOT, "build", "android")
APP = "Fry3DX"
PACKAGE = "io.github.nexoniarz.fry3dx"


def run(*cmd, **kw):
    subprocess.run(list(cmd), check=True, **kw)


def app_version():
    with open(os.path.join(ROOT, "project.json")) as f:  # (the version and the rest live there)
        return json.load(f)["version"]


def main():
    sdk, ndk = os.environ["ANDROID_HOME"], os.environ["ANDROID_NDK_ROOT"]
    tools = os.path.join(sdk, "build-tools", "35.0.0")
    android_jar = os.path.join(sdk, "platforms", "android-35", "android.jar")
    llvm = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin")

    if not os.path.isdir(BUILD):
        # A current glslc: $GLSLC, or the one on PATH. (Not the NDK's: its 2022 shaderc makes compute
        # shaders some phone drivers can't build, e.g. the Galaxy S22's.)
        glslc = os.environ.get("GLSLC") or shutil.which("glslc")
        if not glslc:
            sys.exit("No glslc: install shaderc (or the Vulkan SDK), or set GLSLC")
        run("cmake", "--preset", "android", f"-DGLSLC={glslc}", cwd=ROOT)
    run("cmake", "--build", "--preset", "android", cwd=ROOT)

    stage = os.path.join(BUILD, "apk")
    shutil.rmtree(stage, ignore_errors=True)
    assets, libs = os.path.join(stage, "assets"), os.path.join(stage, "lib", "arm64-v8a")
    os.makedirs(libs)
    bin_dir = os.path.join(BUILD, "bin")
    sdl = os.path.join(BUILD, "_deps", "sdl3-build", "libSDL3.so")
    for src in (os.path.join(bin_dir, "libmain.so"), sdl):
        run(os.path.join(llvm, "llvm-strip"), "--strip-debug", "-o", os.path.join(libs, os.path.basename(src)), src)

    # Content: the folders next to the program on a desktop, and the list it unpacks from (a
    # version on its first line, so an update unpacks again).
    for d in ("engine", APP):
        shutil.copytree(os.path.join(bin_dir, d), os.path.join(assets, d))
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip() or "local"
    version = f"{app_version()}-{commit}"
    files = [os.path.relpath(os.path.join(r, n), assets).replace(os.sep, "/") for r, _, ns in os.walk(assets) for n in ns]
    with open(os.path.join(assets, "content.txt"), "w") as f:
        f.write(f"{version}-{int(time.time())}\n" + "\n".join(sorted(files)) + "\n")

    # Java: SDL's activity and helpers, and ours on top; then dex.
    java_src = [os.path.join(ROOT, "android", "java"), os.path.join(BUILD, "_deps", "sdl3-src", "android-project", "app", "src", "main", "java")]
    sources = [os.path.join(r, n) for d in java_src for r, _, ns in os.walk(d) for n in ns if n.endswith(".java")]
    classes = os.path.join(BUILD, "java-classes")
    shutil.rmtree(classes, ignore_errors=True)
    os.makedirs(classes)
    print(f"Compiling {len(sources)} Java files")
    run("javac", "-nowarn", "-source", "8", "-target", "8", "-bootclasspath", android_jar, "-d", classes, *sources,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    class_files = [os.path.join(r, n) for r, _, ns in os.walk(classes) for n in ns if n.endswith(".class")]
    run(os.path.join(tools, "d8"), "--release", "--min-api", "28", "--lib", android_jar, "--output", stage, *class_files)

    # Resources (the icon), the manifest and the assets; then dex and libraries; aligned, signed.
    res_zip = os.path.join(BUILD, "res.zip")
    run(os.path.join(tools, "aapt2"), "compile", "--dir", os.path.join(ROOT, "android", "res"), "-o", res_zip)
    unsigned = os.path.join(BUILD, "unsigned.apk")
    vcode = str(int(time.time()) // 60 % 2000000000)
    run(os.path.join(tools, "aapt2"), "link", "-o", unsigned, "-I", android_jar, "--manifest", os.path.join(ROOT, "android", "AndroidManifest.xml"),
        "-A", assets, "-R", res_zip, "--auto-add-overlay", "--min-sdk-version", "28", "--target-sdk-version", "35",
        "--version-code", vcode, "--version-name", version)
    with zipfile.ZipFile(unsigned, "a") as z:
        z.write(os.path.join(stage, "classes.dex"), "classes.dex")
        for n in os.listdir(libs):
            z.write(os.path.join(libs, n), f"lib/arm64-v8a/{n}")
    aligned = os.path.join(BUILD, "aligned.apk")
    run(os.path.join(tools, "zipalign"), "-p", "-f", "4", unsigned, aligned)
    keystore = os.environ.get("FRY_KEYSTORE") or os.path.expanduser("~/.local/share/Nexoniarz/Fry3DX-signing/release.keystore")
    pass_file = keystore + ".pass"  # (its password, beside it, when made here)
    password = os.environ.get("FRY_KEYSTORE_PASS") or (open(pass_file).read().strip() if os.path.exists(pass_file) else "")
    if not os.path.exists(keystore):
        os.makedirs(os.path.dirname(keystore), exist_ok=True)
        password = password or secrets.token_urlsafe(24)
        with open(os.open(pass_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
            f.write(password + "\n")
        run("keytool", "-genkeypair", "-keystore", keystore, "-storepass", password, "-keypass", password, "-alias", "fry3dx",
            "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname", "CN=Fry3DX", stdout=subprocess.DEVNULL)
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    out = os.path.join(ROOT, "dist", f"{APP}-{app_version()}-android-arm64.apk")
    run(os.path.join(tools, "apksigner"), "sign", "--v4-signing-enabled", "false", "--ks", keystore, "--ks-pass", f"pass:{password}", "--out", out, aligned)
    print(f"{os.path.relpath(out, ROOT)}: {os.path.getsize(out) // (1024 * 1024)} MB")
    if "--install" in sys.argv:
        run("adb", "install", "-r", out)
        run("adb", "shell", "am", "start", "-n", f"{PACKAGE}/.FryActivity")


if __name__ == "__main__":
    main()
