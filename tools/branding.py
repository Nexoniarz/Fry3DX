#!/usr/bin/env python3
"""Makes every picture of the logo from branding/logo.svg (needs Inkscape):
the app's own (content/), the Windows .ico, the macOS .icns, Android's launcher icons and
the website's.  python3 tools/branding.py"""
import os, shutil, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG = os.path.join(ROOT, "branding", "logo.svg")


def render(size, out):
    subprocess.run(["inkscape", SVG, "-w", str(size), "-h", str(size), "-o", out], check=True, capture_output=True)


def ico(pngs, out):  # an .ico of PNGs (Vista and newer read them)
    data = [open(p, "rb").read() for _, p in pngs]
    head = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    for (size, _), d in zip(pngs, data):
        s = 0 if size >= 256 else size
        head += struct.pack("<BBBBHHII", s, s, 0, 0, 1, 32, len(d), offset)
        offset += len(d)
    open(out, "wb").write(head + b"".join(data))


def icns(pngs, out):  # an .icns of PNGs (macOS 10.7 and newer)
    kinds = {16: b"icp4", 32: b"icp5", 64: b"icp6", 128: b"ic07", 256: b"ic08", 512: b"ic09", 1024: b"ic10"}
    body = b""
    for size, p in pngs:
        d = open(p, "rb").read()
        body += kinds[size] + struct.pack(">I", 8 + len(d)) + d
    open(out, "wb").write(b"icns" + struct.pack(">I", 8 + len(body)) + body)


def main():
    if not shutil.which("inkscape"):
        sys.exit("needs inkscape")
    with tempfile.TemporaryDirectory() as tmp:
        sizes = {}
        for s in (16, 24, 32, 48, 64, 72, 96, 128, 144, 192, 256, 512, 1024):
            sizes[s] = os.path.join(tmp, f"{s}.png")
            render(s, sizes[s])
        copy = lambda s, *to: shutil.copyfile(sizes[s], os.path.join(ROOT, *to))
        copy(256, "branding", "logo.png")
        copy(64, "branding", "icon.png")
        copy(256, "content", "Fry3DX", "branding", "logo.png")
        copy(64, "content", "Fry3DX", "branding", "icon.png")
        copy(64, "docs", "img", "icon.png")
        copy(512, "docs", "img", "logo.png")
        shutil.copyfile(SVG, os.path.join(ROOT, "docs", "img", "logo.svg"))
        for d, s in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
            copy(s, "android", "res", f"mipmap-{d}", "ic_launcher.png")
        ico([(s, sizes[s]) for s in (16, 24, 32, 48, 64, 128, 256)], os.path.join(ROOT, "branding", "fry3dx.ico"))
        icns([(s, sizes[s]) for s in (16, 32, 64, 128, 256, 512, 1024)], os.path.join(ROOT, "branding", "fry3dx.icns"))
    print("done")


if __name__ == "__main__":
    main()
