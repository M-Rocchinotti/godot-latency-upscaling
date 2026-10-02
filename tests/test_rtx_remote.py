#!/usr/bin/env python
"""Tests of tools/rtx_remote.py's privacy rule: the preflight on the user's PC may establish only the
GPU load, whether a fullscreen app is in front and whether a known game is running. No process,
app or window name may leave that PC, be printed or reach docs/RTX_WINDOWED_RUNS.md.

    python tests/test_rtx_remote.py

No SSH connection is made: the remote side is a fake. The preflight's PowerShell check itself runs
here (Windows) and must write exactly the two yes/no fields.
"""
from __future__ import annotations

import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import rtx_remote as R  # noqa: E402

SUMMARY = re.compile(r"GPU (\d+ %|load unknown), (a|no) fullscreen app in front, (a|no) game running")
# Rows logged before the preflight was reduced to yes/no answers say "in front" for the game check,
# which is what it established then (a known game or launcher in front).
# Under the user's durable RTX grant, short non-focus windowed runs happen regardless of what else
# is on the box, so both the fullscreen-app and the game answers may be yes or no. The privacy
# guarantee this test enforces is unchanged: every field is a fixed yes/no boolean (plus the GPU
# load) -- no app, window, or process name can appear here.
LOGGED = re.compile(r"GPU \d+ %, (a|no) fullscreen app in front, (a|no) game (in front|running)")


class FakeRemote:
    """Answers the preflight's two remote calls: the GPU load, then the session check's output."""

    def __init__(self, gpu: str, check_output: str):
        self.root = r"C:\fake-root"
        self.cfg = {"host": "fake"}
        self.outputs = [gpu, "", check_output]

    def ps(self, script: str, timeout: int = 120, check: bool = True) -> tuple[int, str]:
        return 0, self.outputs.pop(0) if self.outputs else ""

    def scp_to(self, local: Path, remote_path: str, timeout: int = 1800) -> None:
        pass


class PreflightPrivacyTests(unittest.TestCase):
    def run_preflight(self, gpu: str, check_output: str) -> tuple[bool, str, str]:
        out = io.StringIO()
        with mock.patch("sys.stdout", new=out):
            ok, summary = R.cmd_preflight(FakeRemote(gpu, check_output))
        return ok, summary, out.getvalue()

    def run_session_check(self) -> tuple[str, str]:
        """Runs the preflight's PowerShell check here; returns (what it printed, the file it wrote)."""
        script = R.preflight_script()
        self.assertNotIn("__GAMES__", script)
        self.assertNotIn("__LAUNCHERS__", script)
        with tempfile.TemporaryDirectory(prefix="lu_preflight_") as tmp:
            ps1 = Path(tmp) / "preflight.ps1"
            ps1.write_text(script, encoding="utf-8-sig")
            env = dict(os.environ, LU_RUN_DIR=tmp)
            proc = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                   "-File", str(ps1)], env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(proc.returncode, 0, proc.stderr[-500:])
            return proc.stdout, (Path(tmp) / "preflight.json").read_text(encoding="utf-8-sig")

    @unittest.skipUnless(os.name == "nt", "the check is Windows PowerShell")
    def test_the_session_check_writes_only_the_two_answers(self) -> None:
        printed, raw = self.run_session_check()
        self.assertEqual(printed.strip(), "")  # nothing printed besides the file
        answers = json.loads(raw)
        self.assertEqual(set(answers), R.PREFLIGHT_FIELDS)
        self.assertTrue(all(isinstance(v, bool) for v in answers.values()))
        self.assertEqual(R.preflight_answers(raw), answers)

    @unittest.skipUnless(os.name == "nt", "the check is Windows PowerShell")
    def test_the_game_list_is_applied_on_the_pc(self) -> None:
        # The check runs in powershell.exe, so with that listed as a "game" the answer must be yes,
        # and still nothing but the two fields comes back.
        with mock.patch.object(R, "GAME_PROCESSES", R.GAME_PROCESSES + ["powershell"]):
            _, raw = self.run_session_check()
        answers = json.loads(raw)
        self.assertEqual(set(answers), R.PREFLIGHT_FIELDS)
        self.assertIs(answers["game_running"], True)
        self.assertNotIn("powershell", raw.lower())

    def test_answers_with_anything_else_are_rejected(self) -> None:
        good = {"fullscreen_in_front": False, "game_running": True}
        self.assertEqual(R.preflight_answers(json.dumps(good)), good)
        self.assertEqual(R.preflight_answers("\ufeff" + json.dumps(good)), good)
        for bad in ({**good, "foreground_process": "someapp"}, {"fullscreen_in_front": False},
                    {"fullscreen_in_front": "no", "game_running": False}, {"fullscreen": False, "game_running": False}, [1, 2]):
            self.assertIsNone(R.preflight_answers(json.dumps(bad)), bad)
        self.assertIsNone(R.preflight_answers("not json"))
        self.assertIsNone(R.preflight_answers(""))

    def test_the_summary_holds_only_the_load_and_the_answers(self) -> None:
        for fullscreen in (False, True):
            for game in (False, True):
                for load in (-1.0, 0.0, 3.4, 99.0):
                    problems, summary = R.preflight_summary(load, {"fullscreen_in_front": fullscreen, "game_running": game})
                    self.assertRegex(summary, "^" + SUMMARY.pattern + "$")
                    self.assertTrue(set(problems) <= {"GPU load unknown", "GPU busy", "fullscreen app in front", "game running"})
                    self.assertEqual(not problems, 0 <= load <= R.GPU_BUSY_PERCENT and not fullscreen and not game)

    def test_the_preflight_never_passes_on_what_the_pc_reports_beyond_the_answers(self) -> None:
        ok, summary, printed = self.run_preflight("0", json.dumps({"fullscreen_in_front": False, "game_running": False,
                                                                    "foreground_process": "someapp"}))
        self.assertFalse(ok)
        self.assertNotIn("someapp", summary + printed)
        ok, summary, printed = self.run_preflight("0", "garbage from someapp")
        self.assertFalse(ok)
        self.assertNotIn("someapp", summary + printed)
        ok, summary, printed = self.run_preflight("2", json.dumps({"fullscreen_in_front": False, "game_running": False}))
        self.assertTrue(ok)
        self.assertEqual(summary, "GPU 2 %, no fullscreen app in front, no game running")
        ok, summary, _ = self.run_preflight("0", json.dumps({"fullscreen_in_front": True, "game_running": False}))
        self.assertFalse(ok)
        self.assertTrue(summary.startswith("GPU 0 %, a fullscreen app in front, no game running"))

    def test_the_run_log_records_only_the_answers(self) -> None:
        with tempfile.TemporaryDirectory(prefix="lu_runlog_") as tmp:
            log = Path(tmp) / "RTX_WINDOWED_RUNS.md"
            with mock.patch.object(R, "WINDOW_LOG", log):
                R.log_windowed_run("dlss_tests", ["--script", "res://tests/dlss_tests.gd"], "2026-09-25 18:00:00", "10",
                                   "exit 0", "GPU 0 %, no fullscreen app in front, no game running")
            row = log.read_text(encoding="utf-8").strip().splitlines()[-1]
        self.assertEqual(row.split("|")[-2].strip(), "GPU 0 %, no fullscreen app in front, no game running")

    def test_the_repository_run_log_names_no_app(self) -> None:
        rows = [line for line in (ROOT / "docs" / "RTX_WINDOWED_RUNS.md").read_text(encoding="utf-8").splitlines()
                if line.startswith("| 20")]
        self.assertTrue(rows)
        for row in rows:
            state = row.split("|")[-2].strip()
            self.assertRegex(state, "^" + LOGGED.pattern + "$", row)
        self.assertNotIn("foreground", (ROOT / "docs" / "RTX_WINDOWED_RUNS.md").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
