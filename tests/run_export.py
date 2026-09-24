#!/usr/bin/env python
"""Export the demo and prove the exported game honours settings read at CORE init.

This is the one check that the editor is not in the loop: it exports a real PCK and runs the
exported executable. The override store is Project Settings (latency_upscaling/...), which Godot
always writes into project.godot -> project.binary and which are parsed before CORE-level
GDExtensions initialise, so an exported game honours them whether or not the editor plugin is on.

    python tests/run_export.py

Steps: build the release DLL, write a temporary Windows export preset, export with
--export-release, then run the exported demo.console.exe headless (no window: the hook and the
device-extension injection still happen; Reflex needs a swapchain, so "active" is only asserted
in the headed run_all suite). Three configurations, each restoring project.godot afterwards:
  A defaults                              -> hook installed, device gets the extensions
  B latency_upscaling/general/enabled=false, EDITOR PLUGIN DISABLED -> hook NOT installed
  C latency_upscaling/reflex/mode=2                                 -> read as On + Boost
Ends "export test: <n> passed, <n> failed"; exit code is the number of failures.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "tools" / "godot.py"
EXPORT_DIR = ROOT / "build" / "export"
PRESET = ROOT / "export_presets.cfg"
PROJECT = ROOT / "project.godot"

PRESET_TEXT = """[preset.0]

name="Windows Desktop"
platform="Windows Desktop"
runnable=true
export_filter="all_resources"
export_path="build/export/demo.exe"

[preset.0.options]

debug/export_console_wrapper=2
binary_format/embed_pck=false
binary_format/architecture="x86_64"
codesign/enable=false
"""


def build_release() -> bool:
    p = subprocess.run([sys.executable, str(ROOT / "tools" / "build.py"), "release", "-j", "2"], cwd=ROOT,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode:
        print(p.stdout[-2000:])
    return p.returncode == 0


def export() -> tuple[int, str]:
    EXPORT_DIR.mkdir(parents=True, exist_ok=True)
    for stale in EXPORT_DIR.glob("*"):
        stale.unlink()
    code = subprocess.run([sys.executable, str(RUNNER), "--timeout", "300", "--log", "export",
                           "--headless", "--export-release", "Windows Desktop", "build/export/demo.exe"],
                          cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return code.returncode, code.stdout + code.stderr


def run_exported(env_settings: str | None, frames: int = 5) -> dict:
    exe = EXPORT_DIR / "demo.console.exe"
    if not exe.exists():
        return {"error": "exported executable missing"}
    import os

    env = dict(os.environ)
    env["LU_EXPORT_PROBE"] = "1"  # the demo main scene prints LU_STATUS and quits (see demo_main.gd)
    if env_settings is not None:
        env["GODOT_LATENCY_UPSCALING_SETTINGS"] = env_settings
    # Run the exported main scene (not a --script SceneTree main loop, which hangs in an export on
    # Godot 4.7.2); --quit-after is a backstop in case the probe does not fire.
    proc = subprocess.Popen([str(exe), "--headless", "--quit-after", str(max(frames + 30, 60))],
                            cwd=str(EXPORT_DIR), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", env=env)
    try:
        out, _ = proc.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
        out, _ = proc.communicate()
        return {"error": "exported game timed out", "out": out[-800:]}
    for line in out.splitlines():
        if line.startswith("LU_STATUS "):
            return json.loads(line[len("LU_STATUS "):])
    return {"error": "no status line", "out": out[-800:]}


def set_project(lines_added: str, disable_plugin: bool) -> str:
    """Add settings to project.godot; return the original text for restore."""
    original = PROJECT.read_text(encoding="utf-8")
    text = original
    if disable_plugin:
        text = re.sub(r'\nenabled=PackedStringArray\([^\n]*\)\n', "\n", text)
    text += lines_added
    PROJECT.write_bytes(text.encode("utf-8"))
    return original


def main() -> int:
    if not build_release():
        print("export test: 0 passed, 1 failed (release build failed)")
        return 1
    backup_preset = PRESET.read_text(encoding="utf-8") if PRESET.exists() else None
    PRESET.write_bytes(PRESET_TEXT.encode("utf-8"))
    passed = failed = 0

    def check(name: str, ok: bool, detail: str) -> None:
        nonlocal passed, failed
        print(f"{'PASS' if ok else 'FAIL'}  {name}  ({detail})", flush=True)
        passed += ok
        failed += not ok

    try:
        # A: defaults
        code, out = export()
        if code != 0:
            check("export succeeds", False, f"exit {code}: " + " ".join(re.findall(r'ERROR:[^\n]{0,120}', out)[:2]))
            print(f"\nexport test: {passed} passed, {failed + 1} failed")
            return 1
        check("export succeeds", True, "release PCK built")
        a = run_exported(None)
        ok = a.get("hook", {}).get("installed") is True and not a.get("hook", {}).get("active")  # headless: installed, not active
        check("A defaults: hook installed in exported game", a.get("hook", {}).get("installed") is True,
              f"hook={a.get('hook')}" if "error" in a else "installed")

        # B: enabled=false with the editor plugin removed from project.godot, re-export
        original = set_project("\n[latency_upscaling]\n\ngeneral/enabled=false\n", disable_plugin=True)
        try:
            code, out = export()
            b = run_exported(None) if code == 0 else {"error": f"export exit {code}"}
        finally:
            PROJECT.write_bytes(original.encode("utf-8"))
        hook = b.get("hook", {})
        src = b.get("settings", {}).get("general/enabled", {}).get("source", "")
        check("B plugin off + enabled=false: hook NOT installed", hook.get("installed") is False and "project setting" in src,
              f"installed={hook.get('installed')} reason={hook.get('reason')!r} source={src!r}")

        # C: reflex/mode=2 read as On + Boost
        original = set_project("\n[latency_upscaling]\n\nreflex/mode=2\n", disable_plugin=False)
        try:
            code, out = export()
            c = run_exported(None) if code == 0 else {"error": f"export exit {code}"}
        finally:
            PROJECT.write_bytes(original.encode("utf-8"))
        mode = c.get("settings", {}).get("reflex/mode", {})
        check("C reflex/mode=2: honoured from project settings", mode.get("value") == 2 and "project setting" in mode.get("source", ""),
              f"mode={mode}")
    finally:
        if backup_preset is None:
            PRESET.unlink(missing_ok=True)
        else:
            PRESET.write_bytes(backup_preset.encode("utf-8"))
        # project.godot changes leave the extension list referring to the same paths; re-import so
        # a following editor run is clean.
        subprocess.run([sys.executable, str(RUNNER), "--timeout", "180", "--log", "export_reimport", "--headless", "--import"],
                       cwd=ROOT, capture_output=True, text=True)

    print(f"\nexport test: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
