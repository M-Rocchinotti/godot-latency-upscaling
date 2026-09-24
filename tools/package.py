#!/usr/bin/env python
"""Build the distributable addon zip.

The zip contains only what a user drops into their project: the addon (addons/latency_upscaling/**)
with the release Windows DLL and the cross-platform no-op stubs, plus the licence files and the
addon README (which carries the AI-usage disclosure). No debug DLL, no import libs, no source.

    python tools/package.py               # -> dist/godot-latency-upscaling-v<version>.zip
    python tools/package.py --skip-build   # use the binaries already in bin/

Steps: build the release DLL (tools/build.py release) and the stubs (stub/build_stubs.py) unless
--skip-build, then stage the addon with only the shipped binaries and zip it. Releases come from
this script; `git archive` (Download ZIP / Asset Library) ships the same addon tree minus the
git-ignored binaries (see .gitattributes) — the two are reconciled by tests/run_all.py's archive
suite. Ends "package: <zip> (<n> files, <size>)".
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

# The only binaries that ship (release Windows DLL + the five cross-platform stubs).
SHIPPED_BINARIES = [
    "godot_latency_upscaling.windows.template_release.x86_64.dll",
    "godot_latency_upscaling.linux.x86_64.so",
    "godot_latency_upscaling.linux.arm64.so",
    "godot_latency_upscaling.macos.x86_64.dylib",
    "godot_latency_upscaling.macos.arm64.dylib",
    "godot_latency_upscaling.windows.arm64.dll",
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
    release = subprocess.run([sys.executable, str(ROOT / "tools" / "build.py"), "release", "-j", "2"],
                             cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if release.returncode:
        print(release.stdout[-2000:])
        print("release build failed")
        return False
    stubs = subprocess.run([sys.executable, str(ROOT / "stub" / "build_stubs.py")],
                           cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if stubs.returncode:
        print(stubs.stdout[-2000:])
        print("stub build failed (is zig installed?)")
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
