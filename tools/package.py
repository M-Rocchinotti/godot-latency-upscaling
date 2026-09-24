#!/usr/bin/env python
"""Build the distributable addon zip (the only installable form of the addon).

The zip contains only what a user drops into their project: the addon (addons/latency_upscaling/**)
with BOTH Windows x86_64 DLLs, plus the licence files and the addon README (which carries the
AI-usage disclosure). No import libs, no source, no stubs.

Both DLLs are needed: Godot picks the [libraries] entry with the most matching feature tags, so
the editor, games started from it and debug exports load the debug DLL, and only release exports
load the release DLL. A zip with only one of them does not load in the other set of processes.

    python tools/package.py               # -> dist/godot-latency-upscaling-v<version>.zip
    python tools/package.py --skip-build   # use the binaries already in bin/

GitHub's "Download ZIP" (git archive) is NOT installable: it has no binaries (they are build
outputs). Releases come from this script; tests/run_package.py installs the zip into a fresh
project and checks the editor, a game run from it, and a debug and a release export.
Ends "package: <zip> (<n> files, <size>)".
"""
from __future__ import annotations

import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADDON = ROOT / "addons" / "latency_upscaling"
BIN = ADDON / "bin"
DIST = ROOT / "dist"

VERSION = (ROOT / "src" / "core" / "version.h").read_text(encoding="utf-8")
VERSION = VERSION.split('LU_VERSION "', 1)[1].split('"', 1)[0]

# The only binaries that ship: both Windows x86_64 DLLs (see the module docstring).
SHIPPED_BINARIES = [
    "godot_latency_upscaling.windows.template_debug.x86_64.dll",
    "godot_latency_upscaling.windows.template_release.x86_64.dll",
]

# Addon files to ship (everything but bin/, handled separately; no editor .uid caches).
def addon_files() -> list[Path]:
    keep = []
    for path in sorted(ADDON.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(ADDON)
        if rel.parts and rel.parts[0] == "bin":
            continue
        if path.suffix == ".uid":  # editor-regenerated caches
            continue
        keep.append(path)
    return keep


def build() -> bool:
    proc = subprocess.run([sys.executable, str(ROOT / "tools" / "build.py"), "debug", "release", "-j", "2"],
                          cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode:
        print(proc.stdout[-2000:])
        print("debug/release build failed")
        return False
    return True


def main(argv: list[str]) -> int:
    if "--skip-build" not in argv and not build():
        return 1

    missing = [name for name in SHIPPED_BINARIES if not (BIN / name).exists()]
    if missing:
        print("missing binaries (build first):")
        for name in missing:
            print("  " + name)
        return 1

    DIST.mkdir(exist_ok=True)
    zip_path = DIST / f"godot-latency-upscaling-v{VERSION}.zip"
    count = 0
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for path in addon_files():
            z.write(path, str(path.relative_to(ROOT)).replace("\\", "/"))
            count += 1
        for name in SHIPPED_BINARIES:
            z.write(BIN / name, f"addons/latency_upscaling/bin/{name}")
            count += 1
        # Top-level licence too, so the zip is self-documenting outside the addon folder.
        z.write(ROOT / "LICENSE", "LICENSE")
        count += 1

    size_kb = zip_path.stat().st_size / 1024
    print(f"\npackage: {zip_path.relative_to(ROOT)} ({count} files, {size_kb:.0f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
