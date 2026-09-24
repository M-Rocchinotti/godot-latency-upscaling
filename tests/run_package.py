#!/usr/bin/env python
"""Install the release zip into a FRESH, empty Godot project and check it works there.

This is what a user does with a release: create or open a project, unzip, open the editor. It
catches packaging mistakes the repository's own tests cannot, because the repository always has
both DLLs built next to the sources.

    python tests/run_package.py                 # build the zip (tools/package.py), then test it
    python tests/run_package.py --skip-build     # test the zip already in dist/
    python tests/run_package.py --zip PATH       # test another zip (e.g. a broken one, as a control)

Steps, in build/pkgtest/ (recreated every run):
  1 import          `--headless --import` of the fresh project (first open: the extension is
                    discovered and listed in .godot/extension_list.cfg)
  2 editor          `--headless --editor`: the editor process (debug build, "editor" feature)
                    with the addon's plugin enabled; the plugin must find the singleton
  3 game (editor)   the main scene run by the editor executable, as Run/F5 does (debug features)
  4 debug export    `--export-debug`, then the exported game
  5 release export  `--export-release`, then the exported game
Steps 3-5 open a window, so a Vulkan device (and Reflex) exist; each prints its status from a
probe scene. Every step must have the LatencyUpscaling singleton and an active hook; the exports
must contain the matching DLL. Ends "package test: <n> passed, <n> failed".
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "tools" / "godot.py"
WORK = ROOT / "build" / "pkgtest"
PROJECT = WORK / "project"
DEBUG_DLL = "godot_latency_upscaling.windows.template_debug.x86_64.dll"
RELEASE_DLL = "godot_latency_upscaling.windows.template_release.x86_64.dll"

PROJECT_GODOT = """config_version=5

[application]

config/name="LatencyUpscalingPackageTest"
run/main_scene="res://probe.tscn"
config/features=PackedStringArray("4.7", "Forward Plus")

[editor_plugins]

enabled=PackedStringArray("res://addons/latency_upscaling/plugin.cfg")
"""

PROBE_GD = """extends Node
## Prints the addon status as one line ("PKGTEST {...}") after 90 frames, then quits.

var _frames := 0


func _process(_delta: float) -> void:
	_frames += 1
	if _frames < 90:
		return
	var out := {"has_singleton": Engine.has_singleton("LatencyUpscaling")}
	if out["has_singleton"]:
		var status: Dictionary = Engine.get_singleton("LatencyUpscaling").get_status()
		out["hook_installed"] = status["hook"]["installed"]
		out["hook_active"] = status["hook"]["active"]
		out["hook_reason"] = status["hook"]["reason"]
		out["reflex_active"] = status["reflex"]["active"]
		out["reflex_reason"] = status["reflex"]["reason"]
	print("PKGTEST " + JSON.stringify(out))
	get_tree().quit()
"""

PROBE_TSCN = """[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://probe.gd" id="1"]

[node name="Probe" type="Node"]
script = ExtResource("1")
"""

PRESET = """[preset.0]

name="Windows Desktop"
platform="Windows Desktop"
runnable=true
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path="out/release/game.exe"

[preset.0.options]

debug/export_console_wrapper=2
binary_format/embed_pck=false
binary_format/architecture="x86_64"
codesign/enable=false
"""

# Godot's own messages when a listed library is missing or no entry matches this process.
LOAD_FAILURE = re.compile(r"No GDExtension library found|GDExtension dynamic library not found|"
                          r"Failed to load GDExtension|Can't open dynamic library", re.IGNORECASE)


def godot(args: list[str], log: str, timeout: int = 300) -> tuple[int, str]:
    cmd = [sys.executable, str(RUNNER), "--project", str(PROJECT), "--timeout", str(timeout), "--log", log] + args
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.returncode, p.stdout + p.stderr


def probe_line(out: str) -> dict:
    for line in out.splitlines():
        if line.startswith("PKGTEST "):
            return json.loads(line[len("PKGTEST "):])
    return {}


def run_exported(exe: Path) -> tuple[int, str]:
    """Run an exported game (headed) with a timeout; kill its process tree on expiry."""
    proc = subprocess.Popen([str(exe), "--quit-after", "900"], cwd=str(exe.parent), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    try:
        out, _ = proc.communicate(timeout=180)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
        out, _ = proc.communicate()
        return 124, out
    return proc.returncode, out


def make_project(zip_path: Path) -> list[str]:
    if WORK.exists():
        shutil.rmtree(WORK)
    PROJECT.mkdir(parents=True)
    (PROJECT / "project.godot").write_text(PROJECT_GODOT, encoding="utf-8")
    (PROJECT / "probe.gd").write_text(PROBE_GD, encoding="utf-8")
    (PROJECT / "probe.tscn").write_text(PROBE_TSCN, encoding="utf-8")
    (PROJECT / "export_presets.cfg").write_text(PRESET, encoding="utf-8")
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(PROJECT)
        return z.namelist()


def main(argv: list[str]) -> int:
    if "--zip" in argv:
        zip_path = Path(argv[argv.index("--zip") + 1]).resolve()
    else:
        if "--skip-build" not in argv:
            p = subprocess.run([sys.executable, str(ROOT / "tools" / "package.py")], cwd=ROOT,
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            if p.returncode:
                print(p.stdout[-1500:])
                print("package test: 0 passed, 1 failed (package.py failed)")
                return 1
        zips = sorted((ROOT / "dist").glob("godot-latency-upscaling-v*.zip"), key=lambda z: z.stat().st_mtime)
        if not zips:
            print("package test: 0 passed, 1 failed (no zip in dist/)")
            return 1
        zip_path = zips[-1]
    print(f"zip: {zip_path}")

    passed = failed = 0

    def check(name: str, ok: bool, detail: str) -> bool:
        nonlocal passed, failed
        print(f"{'PASS' if ok else 'FAIL'}  {name}  ({detail})", flush=True)
        passed += ok
        failed += not ok
        return ok

    names = make_project(zip_path)
    check("zip contains the debug and the release DLL",
          all(f"addons/latency_upscaling/bin/{d}" in names for d in (DEBUG_DLL, RELEASE_DLL)),
          ", ".join(n.rsplit("/", 1)[-1] for n in names if n.endswith(".dll")) or "no DLL")

    code, out = godot(["--headless", "--import"], "pkg_import")
    failures = [l.strip() for l in out.splitlines() if LOAD_FAILURE.search(l)]
    check("1 fresh project imports the addon", code == 0 and not failures, f"exit {code}" + (f"; {failures[0]}" if failures else ""))

    code, out = godot(["--headless", "--editor", "--quit-after", "300"], "pkg_editor")
    summary = re.search(r"Latency & Upscaling [\d.]+: (.*)", out)
    failures = [l.strip() for l in out.splitlines() if LOAD_FAILURE.search(l) or "not loaded" in l]
    check("2 editor (debug build) has the singleton", code == 0 and summary is not None and not failures,
          summary.group(0) if summary else f"exit {code}, no plugin status line" + (f"; {failures[0]}" if failures else ""))

    def game_ok(label: str, code: int, out: str) -> None:
        st = probe_line(out)
        failures = [l.strip() for l in out.splitlines() if LOAD_FAILURE.search(l)]
        ok = bool(st.get("has_singleton")) and st.get("hook_active") is True and st.get("reflex_active") is True and not failures
        detail = (f"singleton={st.get('has_singleton')} hook_active={st.get('hook_active')} "
                  f"reflex_active={st.get('reflex_active')} ({st.get('reflex_reason') or st.get('hook_reason') or ''})"
                  if st else f"exit {code}, no probe line") + (f"; {failures[0]}" if failures else "")
        check(label, ok, detail)

    code, out = godot(["--quit-after", "900"], "pkg_game_from_editor")
    game_ok("3 game run from the editor executable (debug)", code, out)

    for kind, dll in (("debug", DEBUG_DLL), ("release", RELEASE_DLL)):
        exe = PROJECT / "out" / kind / "game.exe"
        exe.parent.mkdir(parents=True, exist_ok=True)
        code, out = godot(["--headless", f"--export-{kind}", "Windows Desktop", str(exe.relative_to(PROJECT)).replace("\\", "/")],
                          f"pkg_export_{kind}", timeout=400)
        shipped = sorted(p.name for p in exe.parent.glob("*.dll"))
        if not check(f"{4 if kind == 'debug' else 5} {kind} export contains the {kind} DLL", code == 0 and dll in shipped,
                     f"exit {code}; DLLs next to the game: {', '.join(shipped) or 'none'}"):
            continue
        console = exe.with_name("game.console.exe")
        code, out = run_exported(console if console.exists() else exe)
        game_ok(f"{4 if kind == 'debug' else 5} {kind} export runs with the singleton", code, out)

    print(f"\npackage test: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
