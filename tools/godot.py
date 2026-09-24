#!/usr/bin/env python
"""Run Godot 4.7 against this project, serialised by a lock, with a timeout and a log file.

Two Godot processes importing the same `.godot/` cache at once corrupt it, so every engine
invocation of the tests goes through this wrapper (adapted from godot-post-effects).

Which engine: the GODOT_BIN environment variable, else the path in tools/.godot_path (one line,
git-ignored), else `godot` / an official Godot 4.7.2 binary name on PATH. On Windows use the
*_console.exe build: the plain one detaches from the console and its output cannot be captured.

Usage (from anywhere):
    python tools/godot.py --headless --import
    python tools/godot.py --script res://tests/reflex_tests.gd
    python tools/godot.py --env GODOT_LATENCY_UPSCALING_DISABLE=1 res://demo/demo_main.tscn --quit-after 300

Options handled here (not passed to Godot):
    --timeout N          kill the engine (and its children) after N seconds (default 900)
    --log NAME           write output to build/logs/NAME.log (default: timestamped)
    --env KEY=VALUE      set an environment variable for the engine (repeatable)
    --project DIR        run against another project directory (default: this repository); used by
                         the fresh-project install tests. The lock stays this repository's.
    --medium-integrity   run the engine at medium integrity level when this process is elevated.
                         The Vulkan loader ignores VK_ADD_LAYER_PATH/VK_LAYER_PATH in elevated
                         processes, so the SDK's validation layer is only found this way. Output
                         is then read from Godot's --log-file instead of a pipe.

Exit code is Godot's exit code (124 on timeout, 125 if the lock could not be taken).
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
LOCK = PROJECT / "build" / ".godot.lock"
LOG_DIR = PROJECT / "build" / "logs"
LOCK_WAIT = 3600
STALE_AFTER = 1500
GODOT_NAMES = ["godot", "godot4", "Godot_v4.7.2-stable_win64_console.exe", "Godot_v4.7.2-stable_win64.exe"]


def find_godot() -> Path | None:
    value = os.environ.get("GODOT_BIN", "").strip()
    if value:
        return Path(value)
    local = Path(__file__).with_name(".godot_path")
    if local.exists():
        value = local.read_text(encoding="utf-8").strip()
        if value:
            return Path(value)
    for name in GODOT_NAMES:
        found = shutil.which(name)
        if found:
            return Path(found)
    return None


def holder_alive(pid: int) -> bool:
    """Is the process that wrote the lock still running? Unknown counts as alive.

    Not os.kill(pid, 0): on Windows that terminates the process instead of probing it."""
    if pid <= 0:
        return True
    if os.name != "nt":
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True
    import ctypes

    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    try:
        code = ctypes.c_ulong()
        if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return True
        return code.value == 259  # STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


def acquire_lock() -> bool:
    LOCK.parent.mkdir(parents=True, exist_ok=True)
    (LOCK.parent / ".gdignore").touch(exist_ok=True)  # build/ holds logs and captures, not assets
    deadline = time.time() + LOCK_WAIT
    while True:
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, f"{os.getpid()} {time.time():.0f}\n".encode())
            os.close(fd)
            return True
        except FileExistsError:
            try:
                age = time.time() - LOCK.stat().st_mtime
                first = LOCK.read_text(encoding="utf-8").split()
                pid = int(first[0]) if first else 0
                if (age > 1.0 and pid and not holder_alive(pid)) or age > STALE_AFTER:
                    print(f"[godot.py] breaking lock of pid {pid} ({age:.0f}s old)", file=sys.stderr)
                    LOCK.unlink()
                    continue
            except (FileNotFoundError, ValueError):
                continue
            if time.time() > deadline:
                print("[godot.py] could not acquire the engine lock", file=sys.stderr)
                return False
            time.sleep(2)


def release_lock() -> None:
    try:
        LOCK.unlink()
    except FileNotFoundError:
        pass


def kill_tree(pid: int) -> None:
    """The console exe starts the real engine as a child: kill the whole tree."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        import signal

        try:
            os.killpg(os.getpgid(pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def is_elevated() -> bool:
    if os.name != "nt":
        return False
    import ctypes

    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except OSError:
        return False


def run_medium_integrity(cmd: list[str], timeout: float) -> tuple[int, bool]:
    """Start `cmd` with this process' token lowered to medium integrity. Returns (exit, timed_out)."""
    import ctypes
    from ctypes import wintypes as w

    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class SID_AND_ATTRIBUTES(ctypes.Structure):
        _fields_ = [("Sid", ctypes.c_void_p), ("Attributes", w.DWORD)]

    class STARTUPINFO(ctypes.Structure):
        _fields_ = [("cb", w.DWORD), ("lpReserved", w.LPWSTR), ("lpDesktop", w.LPWSTR), ("lpTitle", w.LPWSTR),
                    ("dwX", w.DWORD), ("dwY", w.DWORD), ("dwXSize", w.DWORD), ("dwYSize", w.DWORD),
                    ("dwXCountChars", w.DWORD), ("dwYCountChars", w.DWORD), ("dwFillAttribute", w.DWORD),
                    ("dwFlags", w.DWORD), ("wShowWindow", w.WORD), ("cbReserved2", w.WORD),
                    ("lpReserved2", ctypes.c_void_p), ("hStdInput", w.HANDLE), ("hStdOutput", w.HANDLE),
                    ("hStdError", w.HANDLE)]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [("hProcess", w.HANDLE), ("hThread", w.HANDLE), ("dwProcessId", w.DWORD), ("dwThreadId", w.DWORD)]

    kernel32.GetCurrentProcess.restype = w.HANDLE
    kernel32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    kernel32.GetExitCodeProcess.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
    kernel32.CloseHandle.argtypes = [w.HANDLE]
    advapi.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)]
    advapi.DuplicateTokenEx.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.POINTER(w.HANDLE)]
    advapi.SetTokenInformation.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
    advapi.CreateProcessAsUserW.argtypes = [w.HANDLE, w.LPCWSTR, w.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, w.BOOL,
                                            w.DWORD, ctypes.c_void_p, w.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p]

    token, primary = w.HANDLE(), w.HANDLE()
    if not advapi.OpenProcessToken(kernel32.GetCurrentProcess(), 0x0001 | 0x0002 | 0x0008 | 0x0080, ctypes.byref(token)):
        raise OSError(ctypes.get_last_error(), "OpenProcessToken")
    try:
        if not advapi.DuplicateTokenEx(token, 0x02000000, None, 2, 1, ctypes.byref(primary)):
            raise OSError(ctypes.get_last_error(), "DuplicateTokenEx")
        sid = ctypes.c_void_p()
        if not advapi.ConvertStringSidToSidW("S-1-16-8192", ctypes.byref(sid)):  # medium mandatory level
            raise OSError(ctypes.get_last_error(), "ConvertStringSidToSidW")
        label = SID_AND_ATTRIBUTES(sid, 0x20)  # SE_GROUP_INTEGRITY
        if not advapi.SetTokenInformation(primary, 25, ctypes.byref(label),  # TokenIntegrityLevel
                                          ctypes.sizeof(label) + advapi.GetLengthSid(sid)):
            raise OSError(ctypes.get_last_error(), "SetTokenInformation")
        si = STARTUPINFO()
        si.cb = ctypes.sizeof(si)
        pi = PROCESS_INFORMATION()
        command_line = ctypes.create_unicode_buffer(subprocess.list2cmdline(cmd))
        if not advapi.CreateProcessAsUserW(primary, None, command_line, None, None, False, 0, None, str(PROJECT),
                                           ctypes.byref(si), ctypes.byref(pi)):
            raise OSError(ctypes.get_last_error(), "CreateProcessAsUserW")
        try:
            timed_out = kernel32.WaitForSingleObject(pi.hProcess, int(timeout * 1000)) != 0
            if timed_out:
                kill_tree(pi.dwProcessId)
                kernel32.WaitForSingleObject(pi.hProcess, 10000)
            code = w.DWORD()
            kernel32.GetExitCodeProcess(pi.hProcess, ctypes.byref(code))
            return code.value, timed_out
        finally:
            kernel32.CloseHandle(pi.hThread)
            kernel32.CloseHandle(pi.hProcess)
    finally:
        kernel32.CloseHandle(token)
        if primary:
            kernel32.CloseHandle(primary)


def main(argv: list[str]) -> int:
    timeout = 900.0
    log_name = None
    medium = False
    env_overrides: dict[str, str] = {}
    project = PROJECT
    passthrough: list[str] = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--timeout" and i + 1 < len(argv):
            timeout = float(argv[i + 1]); i += 2; continue
        if a == "--log" and i + 1 < len(argv):
            log_name = argv[i + 1]; i += 2; continue
        if a == "--env" and i + 1 < len(argv):
            key, _, value = argv[i + 1].partition("=")
            env_overrides[key] = value; i += 2; continue
        if a == "--medium-integrity":
            medium = True; i += 1; continue
        if a == "--project" and i + 1 < len(argv):
            project = Path(argv[i + 1]).resolve(); i += 2; continue
        passthrough.append(a); i += 1

    godot = find_godot()
    if godot is None or not godot.exists():
        print("[godot.py] Godot 4.7 not found: set GODOT_BIN or write its path into tools/.godot_path", file=sys.stderr)
        return 2
    if not acquire_lock():
        return 125
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"{log_name or time.strftime('%Y%m%d-%H%M%S')}.log"
    cmd = [str(godot), "--path", str(project)] + passthrough
    env = dict(os.environ)
    env.update(env_overrides)
    print(f"[godot.py] {' '.join(cmd)}" + (f"  (env {env_overrides})" if env_overrides else ""), flush=True)
    started = time.time()
    try:
        if medium and is_elevated():
            # The child inherits this process' environment block.
            saved = {k: os.environ.get(k) for k in env_overrides}
            os.environ.update(env_overrides)
            try:
                if log_path.exists():
                    log_path.unlink()
                code, timed_out = run_medium_integrity(cmd + ["--log-file", str(log_path)], timeout)
            finally:
                for k, v in saved.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v
            if log_path.exists():
                sys.stdout.write(log_path.read_text(encoding="utf-8", errors="replace"))
        else:
            timed_out_event = threading.Event()
            with open(log_path, "w", encoding="utf-8", errors="replace") as log:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=str(project), env=env,
                                        text=True, encoding="utf-8", errors="replace")
                done = threading.Event()

                def watchdog() -> None:
                    if not done.wait(timeout):
                        timed_out_event.set()
                        kill_tree(proc.pid)

                threading.Thread(target=watchdog, daemon=True).start()
                for line in proc.stdout:
                    sys.stdout.write(line)
                    log.write(line)
                    log.flush()
                proc.wait()
                done.set()
            code, timed_out = proc.returncode, timed_out_event.is_set()
        if timed_out:
            print(f"[godot.py] timeout after {timeout:.0f}s (killed)", file=sys.stderr)
            return 124
        print(f"[godot.py] exit {code} in {time.time() - started:.1f}s (log: {log_path})", flush=True)
        return code
    finally:
        release_lock()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
