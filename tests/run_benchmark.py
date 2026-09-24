#!/usr/bin/env python
"""Reflex latency (Off / On / On + Boost) and hook overhead on this machine.

    python tests/run_benchmark.py            # waits (up to 10 min) for an idle GPU first
    python tests/run_benchmark.py --no-wait  # measure now; results are labelled if the GPU was busy

The GPU of the reference machine is shared with other work, so every measurement is gated on
nvidia-smi reporting an idle GPU before AND after it; a busy reading marks the numbers as
CONTAMINATED instead of silently mixing them in. Results: stdout and build/benchmark.json.

1. Latency, GPU-bound (demo scene, --load=6000, VSync off), modes Off, On, Boost, twice
   interleaved in one process: frame interval and simulation-start-to-GPU-end latency per frame
   from vkGetLatencyTimingsNV (NVIDIA's "total game-to-render latency").
2. Overhead, light scene (--load=0, VSync off), separate processes, interleaved twice:
   no addon | layer only (Reflex off the device) | Reflex Off (markers + sleep calls) | Reflex On.
"""
from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "tools" / "godot.py"
IDLE_PERCENT = 10
WAIT_SECONDS = 600


def gpu_busy_samples(count: int = 5) -> list[int]:
    samples = []
    for _ in range(count):
        out = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True).stdout.strip()
        samples.append(int(out.splitlines()[0]) if out else 100)
        time.sleep(1)
    return samples


def wait_for_idle_gpu(wait: bool) -> tuple[bool, list[int]]:
    deadline = time.time() + (WAIT_SECONDS if wait else 0)
    while True:
        samples = gpu_busy_samples()
        if max(samples) < IDLE_PERCENT:
            return True, samples
        if time.time() >= deadline:
            return False, samples
        print(f"[bench] GPU busy {samples}, waiting ...", flush=True)
        time.sleep(30)


def bench(args: list[str], env: dict[str, str], log: str) -> list[dict]:
    cmd = [sys.executable, str(RUNNER), "--timeout", "600", "--log", log]
    for k, v in env.items():
        cmd += ["--env", f"{k}={v}"]
    cmd += ["--resolution", "1280x720", "--script", "res://tests/benchmark.gd", "--"] + args
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    return [json.loads(line[6:]) for line in out.splitlines() if line.startswith("BENCH ")]


def measured(label: str, args: list[str], env: dict[str, str], wait: bool) -> dict:
    idle_before, before = wait_for_idle_gpu(wait)
    results = bench(args, env, "bench_" + label)
    after = gpu_busy_samples(3)
    idle_after = max(after) < IDLE_PERCENT
    return {"label": label, "results": results, "gpu_before": before, "gpu_after": after,
            "contaminated": not (idle_before and idle_after)}


def fmt(summary: dict) -> str:
    if not summary or summary.get("n", 0) == 0:
        return "n/a"
    return f"{summary['mean']:6.2f} / {summary['median']:6.2f} / {summary['p95']:6.2f}"


def main(argv: list[str]) -> int:
    wait = "--no-wait" not in argv
    report = {"date": time.strftime("%Y-%m-%d %H:%M"), "runs": []}
    names = {"0": "Off", "1": "On", "2": "On + Boost", "-": "-"}

    latency = measured("latency", ["--load=6000", "--modes=0,1,2,0,1,2", "--frames=600"], {}, wait)
    report["runs"].append(latency)
    print("\nReflex latency, GPU-bound demo (load 6000, VSync off); mean / median / p95 in ms"
          + ("  [CONTAMINATED: GPU busy]" if latency["contaminated"] else ""))
    print(f"{'mode':<12}{'frame interval':>26}{'sim start -> GPU end':>28}{'sleep':>9}")
    for r in latency["results"]:
        print(f"{names[r['mode']]:<12}{fmt(r['frame_ms']):>26}{fmt(r['latency_ms']):>28}{r['sleep_ms']:>9.2f}")

    variants = [
        ("no addon", ["--modes=-"], {"GODOT_LATENCY_UPSCALING_DISABLE": "1"}),
        ("layer only", ["--modes=-"], {"GODOT_LATENCY_UPSCALING_TEST_HIDE_EXTENSIONS": "VK_NV_low_latency2"}),
        ("Reflex Off", ["--modes=0"], {}),
        ("Reflex On", ["--modes=1"], {}),
    ]
    print("\nHook overhead, light scene (load 0, VSync off), frame interval mean / median / p95 in ms")
    for round_index in range(2):
        for label, modes, env in variants:
            run = measured(f"overhead_{label.replace(' ', '_')}_{round_index}", ["--load=0", "--frames=1500"] + modes, env, wait)
            report["runs"].append(run)
            r = run["results"][0] if run["results"] else {}
            print(f"{label:<14} round {round_index + 1}: {fmt(r.get('frame_ms', {}))}"
                  + ("  [CONTAMINATED: GPU busy]" if run["contaminated"] else ""))
    out = ROOT / "build" / "benchmark.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"\nwritten {out}")
    ok = all(run["results"] for run in report["runs"])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
