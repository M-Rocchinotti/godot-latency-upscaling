#!/usr/bin/env python
"""Remote test harness for the RTX PC: copy, launch, collect, clean up.

The RTX machine is someone's personal PC, reached over SSH with a key. The rules this script
enforces, so no caller can break them by accident:

- Everything stays under one folder (default C:\\Users\\lol\\claude-rtx-test\\). Nothing is
  installed, no setting (system, driver, security) is touched; only portable files are copied.
- No window opens on the user's screen unless the caller passes --window-ok (or sets
  LU_RTX_WINDOW_OK=1), which it may only do after the user has been told and agreed ("WINDOW OK").
  Without it every Godot run must be --headless, and the probe creates no surface.
- Every process this script starts runs from inside the folder; `cleanup` stops exactly those
  (matched by executable path) and removes the scheduled tasks it created (name prefix below).

Configuration: tools/.rtx_remote.json (git-ignored), e.g.
    {"host": "100.78.168.91", "user": "lol", "key": "~/.ssh/crater_rtx_test",
     "root": "C:\\\\Users\\\\lol\\\\claude-rtx-test"}

Commands:
    status                       read-only: GPU, driver, load, logged-on sessions, free space, our
                                 folder, our processes and tasks
    deploy [--godot]             copy the project (demo, tests, addon with its DLLs) and the tools
                                 (vk_probe.exe, unit_tests.exe); --godot also copies Godot 4.7.2
    probe                        run vk_probe.exe and the unit tests over SSH (no window)
    run NAME -- ARGS...          run Godot over SSH (a non-interactive session; ARGS must include
                                 --headless); log and exit code in runs\\NAME\\
    session NAME -- ARGS...      run Godot inside the user's interactive desktop session through a
                                 one-shot scheduled task (needed for a real swapchain later). Hidden
                                 launcher, no console. --headless required unless --window-ok
    collect NAME                 fetch runs\\NAME\\ (log, exit code, PNG captures) to build/rtx/NAME/
    cleanup [--purge]            stop our processes, delete our tasks; --purge removes the folder
    selftest                     all of the above without a window (deploy, probe, SSH and session
                                 runs, capture collection, window refusal, nothing left running)

Screenshots come from inside Godot (a test script saves viewport captures into its run folder),
never from the user's desktop.
"""
from __future__ import annotations

import base64
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG = Path(__file__).with_name(".rtx_remote.json")
LOCAL_OUT = ROOT / "build" / "rtx"
STAGE = ROOT / "build" / "rtx_stage"
TASK_PREFIX = "claude-rtx-test-"
GODOT_EXES = ["Godot_v4.7.2-stable_win64.exe", "Godot_v4.7.2-stable_win64_console.exe"]


def load_config() -> dict:
    cfg = {"root": r"C:\Users\lol\claude-rtx-test"}
    if CONFIG.exists():
        cfg.update(json.loads(CONFIG.read_text(encoding="utf-8")))
    for key in ("host", "user", "key"):
        env = os.environ.get(f"LU_RTX_{key.upper()}")
        if env:
            cfg[key] = env
    missing = [k for k in ("host", "user", "key") if not cfg.get(k)]
    if missing:
        sys.exit(f"rtx_remote: missing {', '.join(missing)} in {CONFIG.name} (or LU_RTX_HOST/USER/KEY)")
    cfg["key"] = str(Path(os.path.expanduser(cfg["key"])))
    return cfg


class Remote:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.root = cfg["root"].rstrip("\\")
        self.target = f"{cfg['user']}@{cfg['host']}"
        self.base = ["-i", cfg["key"], "-o", "BatchMode=yes", "-o", "ConnectTimeout=20", "-o", "LogLevel=ERROR"]

    def ps(self, script: str, timeout: int = 120, check: bool = True) -> tuple[int, str]:
        """Runs a PowerShell script remotely (encoded, so no quoting issues)."""
        script = "$ProgressPreference = 'SilentlyContinue'\n" + script  # no CLIXML progress records
        encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
        cmd = ["ssh"] + self.base + [self.target, f"powershell -NoProfile -NonInteractive -EncodedCommand {encoded}"]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        except subprocess.TimeoutExpired:
            if check:
                sys.exit("rtx_remote: remote command timed out")
            return 124, ""
        out = (p.stdout + p.stderr).replace("\r\n", "\n")
        # PowerShell serialises stray progress/error records onto stderr as CLIXML.
        out = "\n".join(line for line in out.split("\n") if not line.startswith(("#< CLIXML", "<Objs ")))
        if check and p.returncode != 0:
            sys.exit(f"rtx_remote: remote command failed ({p.returncode}):\n{out}")
        return p.returncode, out

    def scp_to(self, local: Path, remote_path: str, timeout: int = 1800) -> None:
        dest = f"{self.target}:{remote_path.replace(chr(92), '/')}"
        p = subprocess.run(["scp"] + self.base + [str(local), dest], capture_output=True, text=True, timeout=timeout)
        if p.returncode != 0:
            sys.exit(f"rtx_remote: copy failed: {p.stderr.strip()}")

    def scp_from(self, remote_path: str, local: Path, timeout: int = 600) -> bool:
        local.parent.mkdir(parents=True, exist_ok=True)
        src = f"{self.target}:{remote_path.replace(chr(92), '/')}"
        p = subprocess.run(["scp", "-r"] + self.base + [src, str(local)], capture_output=True, text=True, timeout=timeout)
        return p.returncode == 0


def q(path: str) -> str:
    """PowerShell single-quoted literal."""
    return "'" + path.replace("'", "''") + "'"


# --- commands ------------------------------------------------------------------------------------

def cmd_status(r: Remote) -> int:
    _, out = r.ps(f"""
$ErrorActionPreference = 'SilentlyContinue'
'host: ' + $env:COMPUTERNAME
'gpu: ' + (& nvidia-smi --query-gpu=name,driver_version,utilization.gpu,memory.used --format=csv,noheader)
'sessions:'; (& query user) | ForEach-Object {{ '  ' + $_ }}
'free C: GB: ' + [math]::Round((Get-PSDrive C).Free / 1GB, 1)
$root = {q(r.root)}
'folder: ' + $(if (Test-Path $root) {{ $root + ' (' + [math]::Round(((Get-ChildItem $root -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB, 1) + ' MB)' }} else {{ 'absent' }})
'our processes:'; Get-CimInstance Win32_Process | Where-Object {{ $_.ExecutablePath -like ($root + '\\*') -or $_.CommandLine -like ('*' + $root + '*') }} | ForEach-Object {{ '  ' + $_.ProcessId + ' ' + $_.Name }}
'our tasks:'; Get-ScheduledTask -TaskName '{TASK_PREFIX}*' | ForEach-Object {{ '  ' + $_.TaskName + ' ' + $_.State }}
""")
    print(out.strip())
    return 0


def stage_project() -> list[Path]:
    """Build the tarballs to copy: the project (always) and the tools."""
    if STAGE.exists():
        shutil.rmtree(STAGE)
    project = STAGE / "project"
    project.mkdir(parents=True)
    for name in ("project.godot", "icon.svg"):
        if (ROOT / name).exists():
            shutil.copy2(ROOT / name, project / name)
    ignore = shutil.ignore_patterns("*.lib", "*.exp", "*.pdb", "~*", "*.import")
    shutil.copytree(ROOT / "demo", project / "demo", ignore=ignore)
    shutil.copytree(ROOT / "tests", project / "tests", ignore=shutil.ignore_patterns("unit", "__pycache__", "*.py"))
    shutil.copytree(ROOT / "addons" / "latency_upscaling", project / "addons" / "latency_upscaling", ignore=ignore)
    tools = STAGE / "tools"
    tools.mkdir()
    for exe in (ROOT / "build" / "tools" / "vk_probe.exe", ROOT / "build" / "unit_tests" / "unit_tests.exe"):
        if not exe.exists():
            sys.exit(f"rtx_remote: {exe.relative_to(ROOT)} missing: build it (tools/build.py vk_probe tests)")
        shutil.copy2(exe, tools / exe.name)
    out = STAGE / "project_and_tools.tar.gz"
    with tarfile.open(out, "w:gz") as tar:
        tar.add(project, arcname="project")
        tar.add(tools, arcname="tools")
    return [out]


def stage_godot() -> Path:
    godot_dir = Path((ROOT / "tools" / ".godot_path").read_text(encoding="utf-8").strip()).parent
    out = STAGE / "godot.tar.gz"
    with tarfile.open(out, "w:gz") as tar:
        for exe in GODOT_EXES:
            tar.add(godot_dir / exe, arcname=f"godot/{exe}")
    return out


def cmd_deploy(r: Remote, with_godot: bool) -> int:
    archives = stage_project()
    _, out = r.ps(f"$r = {q(r.root)}; if (Test-Path (Join-Path $r 'godot\\{GODOT_EXES[0]}')) {{ 'yes' }} else {{ 'no' }}")
    if with_godot or out.strip() != "yes":
        archives.append(stage_godot())
    r.ps(f"New-Item -ItemType Directory -Force -Path {q(r.root)}, {q(r.root + chr(92) + 'runs')} | Out-Null\n"
         f"foreach ($p in @({q(r.root + chr(92) + 'project')}, {q(r.root + chr(92) + 'tools')})) "
         "{ if (Test-Path $p) { Remove-Item -Recurse -Force $p } }\nexit 0")
    for archive in archives:
        size = archive.stat().st_size / 1e6
        print(f"copying {archive.name} ({size:.1f} MB)...", flush=True)
        remote_archive = f"{r.root}\\{archive.name}"
        r.scp_to(archive, remote_archive)
        r.ps(f"tar -xzf {q(remote_archive)} -C {q(r.root)}; if ($LASTEXITCODE) {{ exit $LASTEXITCODE }}; Remove-Item {q(remote_archive)}", timeout=600)
    shutil.rmtree(STAGE)
    print(f"deployed to {r.cfg['host']}:{r.root}")
    return 0


def cmd_probe(r: Remote) -> int:
    code, out = r.ps(f"$t = {q(r.root + chr(92) + 'tools')}; & (Join-Path $t 'vk_probe.exe'); '---UNIT---'; & (Join-Path $t 'unit_tests.exe') | Select-Object -Last 3",
                     timeout=300, check=False)
    LOCAL_OUT.mkdir(parents=True, exist_ok=True)
    (LOCAL_OUT / "probe.txt").write_text(out, encoding="utf-8")
    probe, _, unit = out.partition("---UNIT---")
    try:
        print(json.dumps(json.loads(probe.strip()), indent=2))
    except json.JSONDecodeError:
        print(probe.strip())
    print(unit.strip())
    return code


def godot_args_ok(args: list[str], window_ok: bool) -> None:
    if "--headless" not in args and not window_ok:
        sys.exit("rtx_remote: refusing a Godot run that would open a window on the user's screen. Add --headless, "
                 "or pass --window-ok only after the user has agreed (\"WINDOW OK\").")


def run_dir(r: Remote, name: str) -> str:
    if not name.replace("_", "").replace("-", "").isalnum():
        sys.exit("rtx_remote: run names may only contain letters, digits, - and _")
    return f"{r.root}\\runs\\{name}"


def cmd_run(r: Remote, name: str, args: list[str], window_ok: bool, timeout: int) -> int:
    godot_args_ok(args, window_ok)
    d = run_dir(r, name)
    exe = f"{r.root}\\godot\\{GODOT_EXES[1]}"  # console wrapper: output reaches the log file
    arglist = ", ".join(q(a) for a in ["--path", f"{r.root}\\project", "--log-file", f"{d}\\godot.log"] + args)
    code, out = r.ps(f"""
$d = {q(d)}; Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $d; New-Item -ItemType Directory -Force -Path $d | Out-Null
$env:LU_RUN_DIR = $d  # test scripts save their captures here (tests/remote_capture.gd)
$p = Start-Process -FilePath {q(exe)} -ArgumentList @({arglist}) -WorkingDirectory $d -PassThru -NoNewWindow `
     -RedirectStandardOutput (Join-Path $d 'stdout.txt') -RedirectStandardError (Join-Path $d 'stderr.txt')
$null = $p.Handle  # without a cached handle ExitCode stays empty after the process exits
if (-not $p.WaitForExit({timeout * 1000})) {{
    Get-CimInstance Win32_Process | Where-Object {{ $_.ExecutablePath -like ({q(r.root)} + '\\*') }} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}
    'timeout' | Out-File (Join-Path $d 'exit.txt'); 'TIMEOUT'; exit 124
}}
$p.ExitCode | Out-File (Join-Path $d 'exit.txt'); 'exit ' + $p.ExitCode
""", timeout=timeout + 60, check=False)
    print(out.strip())
    return cmd_collect(r, name) if code == 0 else code


def cmd_session(r: Remote, name: str, args: list[str], window_ok: bool, timeout: int) -> int:
    godot_args_ok(args, window_ok)
    d = run_dir(r, name)
    exe = f"{r.root}\\godot\\{GODOT_EXES[0]}"  # GUI subsystem: no console window
    # VBScript string: double quotes doubled. The launcher runs hidden (window style 0) and waits.
    cmdline = " ".join('""' + a + '""' for a in [exe, "--path", f"{r.root}\\project", "--log-file", f"{d}\\godot.log"] + args)
    vbs = (f'Set sh = CreateObject("WScript.Shell")\r\n'
           f'sh.Environment("Process")("LU_RUN_DIR") = "{d}"\r\n'
           f'rc = sh.Run("{cmdline}", 0, True)\r\n'
           f'Set f = CreateObject("Scripting.FileSystemObject").CreateTextFile("{d}\\exit.txt", True)\r\n'
           f'f.WriteLine rc\r\nf.Close\r\n')
    task = TASK_PREFIX + name
    code, out = r.ps(f"""
$d = {q(d)}; Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $d; New-Item -ItemType Directory -Force -Path $d | Out-Null
$active = (& query user) -match 'Active'
if (-not $active) {{ 'NO_INTERACTIVE_SESSION'; exit 3 }}
Set-Content -Path (Join-Path $d 'launch.vbs') -Value {q(vbs)} -Encoding ASCII
$action = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument ('//B //NoLogo "' + (Join-Path $d 'launch.vbs') + '"')
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive
Register-ScheduledTask -TaskName {q(task)} -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName {q(task)}
$deadline = (Get-Date).AddSeconds({timeout})
while (-not (Test-Path (Join-Path $d 'exit.txt')) -and (Get-Date) -lt $deadline) {{ Start-Sleep -Seconds 2 }}
$result = if (Test-Path (Join-Path $d 'exit.txt')) {{ 'exit ' + (Get-Content (Join-Path $d 'exit.txt') -Raw).Trim() }} else {{ 'TIMEOUT' }}
if ($result -eq 'TIMEOUT') {{
    Get-CimInstance Win32_Process | Where-Object {{ $_.ExecutablePath -like ({q(r.root)} + '\\*') -or ($_.Name -eq 'wscript.exe' -and $_.CommandLine -like ('*' + $d + '*')) }} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}
}}
Unregister-ScheduledTask -TaskName {q(task)} -Confirm:$false
$result
""", timeout=timeout + 120, check=False)
    print(out.strip())
    if "NO_INTERACTIVE_SESSION" in out:
        print("rtx_remote: nobody is logged on interactively; a session run is not possible now")
        return 3
    return cmd_collect(r, name) if "exit" in out else 124


def cmd_collect(r: Remote, name: str) -> int:
    local = LOCAL_OUT / name
    if local.exists():
        shutil.rmtree(local)
    if not r.scp_from(run_dir(r, name), local):
        print(f"rtx_remote: nothing to collect for {name}")
        return 1
    files = sorted(p.relative_to(local) for p in local.rglob("*") if p.is_file())
    print(f"collected {len(files)} file(s) into {local.relative_to(ROOT)}: {', '.join(map(str, files))}")
    return 0


def cmd_cleanup(r: Remote, purge: bool) -> int:
    _, out = r.ps(f"""
$root = {q(r.root)}
$procs = Get-CimInstance Win32_Process | Where-Object {{ $_.ExecutablePath -like ($root + '\\*') -or ($_.Name -eq 'wscript.exe' -and $_.CommandLine -like ('*' + $root + '*')) }}
foreach ($p in $procs) {{ Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue; 'stopped ' + $p.ProcessId + ' ' + $p.Name }}
foreach ($t in (Get-ScheduledTask -TaskName '{TASK_PREFIX}*' -ErrorAction SilentlyContinue)) {{ Unregister-ScheduledTask -TaskName $t.TaskName -Confirm:$false; 'removed task ' + $t.TaskName }}
{"if (Test-Path $root) { Remove-Item -Recurse -Force $root; 'removed ' + $root }" if purge else ""}
'clean'
""")
    print(out.strip())
    return 0


def cmd_selftest(r: Remote) -> int:
    """The whole harness, without a window: deploy, probe, SSH run, session run, collect, cleanup."""
    results = []

    def check(name: str, ok: bool) -> None:
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'}  {name}", flush=True)

    check("deploy", cmd_deploy(r, False) == 0)
    check("probe (vk_probe + unit tests, no surface)", cmd_probe(r) == 0)
    check("headless import over SSH", cmd_run(r, "selftest_import", ["--headless", "--import"], False, 300) == 0)
    for mode, fn in (("SSH", cmd_run), ("interactive session", cmd_session)):
        name = "selftest_" + mode.split()[0].lower()
        ok = fn(r, name, ["--headless", "--script", "res://tests/remote_capture.gd", "--", "--frames=5"], False, 180) == 0
        capture = LOCAL_OUT / name / "capture.json"
        ok = ok and capture.exists() and json.loads(capture.read_text(encoding="utf-8")).get("error") == 0
        check(f"headless capture via {mode}, collected", ok and (LOCAL_OUT / name / "frame.png").exists())
    check("window refused without --window-ok", subprocess.run(
        [sys.executable, __file__, "session", "selftest_refuse", "--", "--script", "res://tests/remote_capture.gd"],
        capture_output=True).returncode != 0)
    cmd_cleanup(r, False)
    _, out = r.ps(f"$root = {q(r.root)}; @(Get-CimInstance Win32_Process | Where-Object {{ $_.ExecutablePath -like ($root + '\\*') }}).Count; "
                  f"@(Get-ScheduledTask -TaskName '{TASK_PREFIX}*' -ErrorAction SilentlyContinue).Count")
    counts = [line.strip() for line in out.strip().splitlines() if line.strip()]
    check("no process or task of ours left", counts == ["0", "0"])
    print(f"\nrtx selftest: {sum(results)} passed, {len(results) - sum(results)} failed")
    return 0 if all(results) else 1


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    command, rest = argv[0], argv[1:]
    window_ok = "--window-ok" in rest or os.environ.get("LU_RTX_WINDOW_OK") == "1"
    rest = [a for a in rest if a != "--window-ok"]
    timeout = 600
    if "--timeout" in rest:
        i = rest.index("--timeout")
        timeout = int(rest[i + 1])
        del rest[i:i + 2]
    godot_args: list[str] = []
    if "--" in rest:
        i = rest.index("--")
        rest, godot_args = rest[:i], rest[i + 1:]
    r = Remote(load_config())
    if command == "status":
        return cmd_status(r)
    if command == "deploy":
        return cmd_deploy(r, "--godot" in rest)
    if command == "probe":
        return cmd_probe(r)
    if command in ("run", "session") and rest:
        return (cmd_run if command == "run" else cmd_session)(r, rest[0], godot_args, window_ok, timeout)
    if command == "collect" and rest:
        return cmd_collect(r, rest[0])
    if command == "cleanup":
        return cmd_cleanup(r, "--purge" in rest)
    if command == "selftest":
        return cmd_selftest(r)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
