#!/usr/bin/env python3
"""Writes a clang VFS overlay that makes the xwin CRT/SDK headers case-insensitive (Windows code
includes <Winsock2.h>, <WinSock2.h>, <winsock2.h>... and Linux filesystems care)."""
import json, os, sys

root, out = sys.argv[1], sys.argv[2]
dirs = ["crt/include", "sdk/include/ucrt", "sdk/include/um", "sdk/include/shared", "sdk/include/winrt"]

def tree(path):
    entries = []
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if os.path.islink(full) and name.lower() == os.readlink(full).lower():
            continue  # xwin's lowercase alias: the overlay covers every casing already
        if os.path.isdir(full):
            entries.append({"type": "directory", "name": name, "contents": tree(full)})
        else:
            entries.append({"type": "file", "name": name, "external-contents": os.path.realpath(full)})
    return entries

overlay = {"version": 0, "case-sensitive": "false", "overlay-relative": False,
           "roots": [{"type": "directory", "name": os.path.join(root, d), "contents": tree(os.path.join(root, d))} for d in dirs]}
tmp = out + f".{os.getpid()}"
with open(tmp, "w") as f:
    json.dump(overlay, f)
os.replace(tmp, out)
