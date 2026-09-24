#!/usr/bin/env python
"""Reproducible one-command build of the godot-latency-upscaling GDExtension.

    build.cmd                      # debug + release DLLs and the C++ unit tests
    build.cmd debug                # only template_debug (used by the editor and debug exports)
    build.cmd release              # only template_release
    build.cmd tests                # only the C++ unit tests (build/unit_tests/unit_tests.exe)
    build.cmd vk_probe             # the window-less Vulkan probe (build/tools/vk_probe.exe)
    build.cmd sl_probe             # the window-less Streamline/DLSS probe (build/tools/sl_probe.exe)
    build.cmd -j 3                 # parallel compile jobs (default 2: the reference machine has 4 cores)
    build.cmd --clean              # remove the SCons outputs for the selected targets

What it does:
1. Creates `.venv/` with the SCons version pinned in tools/requirements-build.txt (pip cache in
   build/pip-cache, so nothing is written to the system drive).
2. Finds the MSVC environment:
   - already set up (INCLUDE and LIB exist, e.g. a "x64 Native Tools" prompt): used as is;
   - else a dev-shell script from GODOT_LATENCY_UPSCALING_DEVSHELL or tools/.devshell_path (one line,
     git-ignored). The script must run the command given to it as arguments inside its
     environment, like E:\\dev\\devshell.cmd; the build re-runs itself through it;
   - else SCons' own Visual Studio autodetection.
3. Runs SCons for each requested target, below normal process priority.
"""
from __future__ import annotations

import os
import subprocess
import sys
import venv
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENV = ROOT / ".venv"
REQUIREMENTS = ROOT / "tools" / "requirements-build.txt"
INNER_FLAG = "--inside-devshell"
# Compilers run below normal priority (inherited by cl.exe) so a build never starves the
# interactive work on the machine.
LOW_PRIORITY = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
TARGETS = {"debug": ["target=template_debug"], "release": ["target=template_release"], "tests": ["unit_tests"],
           "vk_probe": ["vk_probe"], "sl_probe": ["sl_probe"]}


def venv_python() -> Path:
    return VENV / "Scripts" / "python.exe" if os.name == "nt" else VENV / "bin" / "python"


def ensure_venv() -> Path:
    """Create the venv and install the pinned requirements when they changed."""
    py = venv_python()
    if not py.exists():
        print(f"[build] creating {VENV}")
        venv.EnvBuilder(with_pip=True).create(VENV)
    stamp = VENV / ".requirements.stamp"
    wanted = REQUIREMENTS.read_text(encoding="utf-8")
    if not stamp.exists() or stamp.read_text(encoding="utf-8") != wanted:
        print("[build] installing build requirements")
        cache = ROOT / "build" / "pip-cache"
        subprocess.run([str(py), "-m", "pip", "install", "--disable-pip-version-check", "--cache-dir", str(cache),
                        "-r", str(REQUIREMENTS)], check=True)
        stamp.write_text(wanted, encoding="utf-8")
    return py


def devshell_script() -> Path | None:
    value = os.environ.get("GODOT_LATENCY_UPSCALING_DEVSHELL", "").strip()
    local = ROOT / "tools" / ".devshell_path"
    if not value and local.exists():
        value = local.read_text(encoding="utf-8").strip()
    return Path(value) if value else None


def parse(argv: list[str]) -> tuple[list[str], int, bool]:
    targets: list[str] = []
    jobs = 2
    clean = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-j", "--jobs") and i + 1 < len(argv):
            jobs = int(argv[i + 1])
            i += 2
            continue
        if a.startswith("-j") and a[2:].isdigit():
            jobs = int(a[2:])
        elif a == "--clean":
            clean = True
        elif a in TARGETS:
            targets.append(a)
        elif a != INNER_FLAG:
            raise SystemExit(f"[build] unknown argument {a!r}; see the docstring of tools/build.py")
        i += 1
    return targets or ["debug", "release", "tests"], max(1, jobs), clean


def run_scons(py: Path, targets: list[str], jobs: int, clean: bool) -> int:
    for name in targets:
        cmd = [str(py), "-m", "SCons", "-C", str(ROOT), f"-j{jobs}"] + TARGETS[name]
        if clean:
            cmd.append("--clean")
        print("[build] " + " ".join(cmd), flush=True)
        code = subprocess.run(cmd, cwd=ROOT, creationflags=LOW_PRIORITY).returncode
        if code != 0:
            print(f"[build] FAILED: {name} (exit {code})", file=sys.stderr)
            return code
    print("[build] OK: " + ", ".join(targets))
    return 0


def main(argv: list[str]) -> int:
    targets, jobs, clean = parse(argv)
    py = ensure_venv()
    have_msvc_env = bool(os.environ.get("INCLUDE") and os.environ.get("LIB"))
    script = devshell_script()
    if not have_msvc_env and script is not None and INNER_FLAG not in argv:
        if not script.exists():
            print(f"[build] dev shell {script} does not exist", file=sys.stderr)
            return 2
        print(f"[build] entering {script}", flush=True)
        cmd = ["cmd", "/d", "/c", "call", str(script), sys.executable, str(Path(__file__).resolve()), INNER_FLAG] + argv
        return subprocess.run(cmd, cwd=ROOT, creationflags=LOW_PRIORITY).returncode
    if not have_msvc_env:
        print("[build] no MSVC environment found; relying on SCons' Visual Studio autodetection")
    return run_scons(py, targets, jobs, clean)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
