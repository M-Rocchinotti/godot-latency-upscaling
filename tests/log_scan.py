"""What counts as a problem in a Godot log, shared by the test runners."""
from __future__ import annotations

import re

# Environment noise on the development machine (no audio device), not project problems.
IGNORED = [
    re.compile(r"WASAPI|audio_driver_wasapi|All audio drivers failed|audio_server\.cpp"),
    re.compile(r'Condition "hr != \(\(HRESULT\)0x00000000\)" is true'),
    re.compile(r"Failed to request display timeout override"),
    re.compile(r"Message Id Name: Loader Message"),
]
PROBLEMS = [
    re.compile(r"SCRIPT ERROR"),
    re.compile(r"^ERROR:"),
    re.compile(r"was leaked|were leaked|leaked at exit", re.IGNORECASE),
    re.compile(r"resources still in use", re.IGNORECASE),
    re.compile(r"^WARNING:.*NVIDIA DLSS"),
    re.compile(r"CrashHandlerException|Program crashed"),
]


def problems_in(output: str, expected: list[str] | None = None) -> list[str]:
    """Problem lines of a run's output. `expected` are regexes of errors the run provokes on purpose."""
    allowed = [re.compile(e) for e in (expected or [])]
    found = []
    for line in output.splitlines():
        if any(p.search(line) for p in IGNORED) or any(a.search(line) for a in allowed):
            continue
        if any(p.search(line) for p in PROBLEMS):
            found.append(line.strip())
    return found
