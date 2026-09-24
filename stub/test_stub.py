#!/usr/bin/env python
"""Validate the no-op stub on Windows-x64, which validates its logic for every 64-bit target.

The GDExtension C ABI is the same on all platforms, so the identical stub source that runs here
runs the same way on the Linux/macOS/Windows-arm64 targets that cannot be run on this machine.

Steps: build the stub as a Windows-x64 DLL with zig, swap it in for the addon's debug DLL, run a
headless Godot probe (tests/stub_probe.gd) that queries the LatencyUpscaling singleton, then
restore the real DLL. Asserts the singleton exists and every method reports "not supported".
Ends "stub test: <n> passed, <n> failed"; exit code is the number of failures.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "stub" / "latency_upscaling_stub.c"
INCLUDE = ROOT / "thirdparty" / "godot-cpp" / "gen" / "include"
RUNNER = ROOT / "tools" / "godot.py"
DEBUG_DLL = ROOT / "addons" / "latency_upscaling" / "bin" / "godot_latency_upscaling.windows.template_debug.x86_64.dll"
STUB_DLL = ROOT / "build" / "stub_test" / "stub.dll"

sys.path.insert(0, str(ROOT / "stub"))
from build_stubs import find_zig  # noqa: E402


def build_windows_stub() -> bool:
    zig = find_zig()
    if zig is None:
        print("zig not found")
        return False
    STUB_DLL.parent.mkdir(parents=True, exist_ok=True)
    cmd = [zig, "cc", "-shared", "-O2", "-fvisibility=hidden", "-target", "x86_64-windows-gnu",
           "-I", str(INCLUDE), str(SRC), "-o", str(STUB_DLL)]
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode:
        print((p.stderr or p.stdout)[-1500:])
    return p.returncode == 0


def run_probe() -> dict:
    p = subprocess.run([sys.executable, str(RUNNER), "--timeout", "120", "--log", "stub_probe",
                        "--headless", "--script", "res://tests/stub_probe.gd"],
                       cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    for line in (p.stdout + p.stderr).splitlines():
        if line.startswith("STUB_PROBE "):
            return json.loads(line[len("STUB_PROBE "):])
    return {"error": "no probe line", "out": (p.stdout + p.stderr)[-1200:]}


def main() -> int:
    if not build_windows_stub():
        print("stub test: 0 passed, 1 failed (stub build failed)")
        return 1
    if not DEBUG_DLL.exists():
        print(f"stub test: 0 passed, 1 failed (real debug DLL missing: build it first: {DEBUG_DLL.name})")
        return 1
    backup = DEBUG_DLL.with_suffix(".dll.real")
    shutil.copy2(DEBUG_DLL, backup)
    passed = failed = 0

    def check(name: str, ok: bool, detail: str) -> None:
        nonlocal passed, failed
        print(f"{'PASS' if ok else 'FAIL'}  {name}  ({detail})", flush=True)
        passed += ok
        failed += not ok

    try:
        shutil.copy2(STUB_DLL, DEBUG_DLL)
        r = run_probe()
        check("singleton registered by the stub", r.get("has_singleton") is True,
              f"probe={r}" if "error" in r else "has_singleton=True")
        check("is_reflex_supported() is false", r.get("is_reflex_supported") is False, str(r.get("is_reflex_supported")))
        check("is_reflex_active() is false", r.get("is_reflex_active") is False, str(r.get("is_reflex_active")))
        check("get_reflex_status() reports not supported",
              r.get("get_reflex_status") == "not supported on this platform", str(r.get("get_reflex_status")))
        check("get_version() returns a version", bool(isinstance(r.get("get_version"), str) and r.get("get_version")),
              str(r.get("get_version")))
        status = r.get("get_status")
        check("get_status() reports not supported",
              isinstance(status, dict) and status.get("platform_supported") is False
              and status.get("reason") == "not supported on this platform", str(status))
    finally:
        shutil.copy2(backup, DEBUG_DLL)
        backup.unlink(missing_ok=True)

    print(f"\nstub test: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
