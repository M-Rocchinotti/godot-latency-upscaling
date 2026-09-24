#!/usr/bin/env python
"""EXPERIMENTAL, NOT SHIPPED: cross-compile the no-op GDExtension stub with `zig cc`.

The stub is kept for a later release (see stub/README.md for why it is not shipped yet). It is
built into build/stubs/, never into the addon, and the .gdextension does not reference it.

    python stub/build_stubs.py            # build every target into build/stubs/
    python stub/build_stubs.py --check     # only report which targets would build

Requires zig (https://ziglang.org). Set ZIG or put zig.exe on PATH; E:\\zig\\zig.exe is tried too.
zig ships the cross sysroots (including macOS), so all targets build from Windows with no other SDK.
The identical C runs on every 64-bit target, so a passing Windows-x64 check of the same source
(see stub/test_stub.py) validates the logic for all of them.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "stub" / "latency_upscaling_stub.c"
INCLUDE = ROOT / "thirdparty" / "godot-cpp" / "gen" / "include"
OUT = ROOT / "build" / "stubs"  # never the addon's bin/: the stubs are not shipped

# (feature tag it would use in a .gdextension, zig target triple, output filename)
TARGETS = [
    ("linux.x86_64", "x86_64-linux-gnu", "godot_latency_upscaling.linux.x86_64.so"),
    ("linux.arm64", "aarch64-linux-gnu", "godot_latency_upscaling.linux.arm64.so"),
    ("macos.x86_64", "x86_64-macos", "godot_latency_upscaling.macos.x86_64.dylib"),
    ("macos.arm64", "aarch64-macos", "godot_latency_upscaling.macos.arm64.dylib"),
    ("windows.arm64", "aarch64-windows-gnu", "godot_latency_upscaling.windows.arm64.dll"),
]


def find_zig() -> str | None:
    for candidate in (os.environ.get("ZIG"), "E:/zig/zig.exe", shutil.which("zig")):
        if candidate and Path(candidate).exists():
            return str(candidate)
    return shutil.which("zig")


def compile_target(zig: str, triple: str, out: Path) -> tuple[bool, str]:
    cmd = [zig, "cc", "-shared", "-O2", "-fvisibility=hidden", "-fno-sanitize=undefined",
           "-target", triple, "-I", str(INCLUDE), str(SRC), "-o", str(out)]
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    # A Windows DLL build also emits an import .lib and a .pdb next to the output; the addon ships
    # only the runtime library, so drop them (matches "no .lib/.exp in bin" for the MSVC build).
    for extra in (out.with_suffix(".lib"), out.with_suffix(".pdb"), out.parent / "latency_upscaling_stub.lib"):
        extra.unlink(missing_ok=True)
    return p.returncode == 0, (p.stderr or p.stdout).strip()


def main(argv: list[str]) -> int:
    zig = find_zig()
    if zig is None:
        print("zig not found: install it and set ZIG or add it to PATH (see this script's header)")
        return 1
    print(f"zig: {zig}")
    if "--check" in argv:
        for key, triple, name in TARGETS:
            print(f"  would build {key:<15} {triple:<22} -> build/stubs/{name}")
        return 0
    OUT.mkdir(parents=True, exist_ok=True)
    failed = 0
    for key, triple, name in TARGETS:
        out = OUT / name
        ok, msg = compile_target(zig, triple, out)
        size = out.stat().st_size if out.exists() else 0
        print(f"{'OK  ' if ok else 'FAIL'} {key:<15} {triple:<22} -> build/stubs/{name} ({size} bytes)")
        if not ok:
            print("   " + msg.replace("\n", "\n   "))
            failed += 1
    print(f"\nstub build: {len(TARGETS) - failed} ok, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
