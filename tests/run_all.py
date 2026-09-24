#!/usr/bin/env python
"""Run every test suite of godot-latency-upscaling.

    python tests/run_all.py                 # everything below except benchmark and mutations
    python tests/run_all.py --only reflex   # suites whose name contains a word
    python tests/run_all.py --bench         # ... plus tests/run_benchmark.py (waits for an idle GPU)
    python tests/run_all.py --mutations     # ... plus tests/run_mutations.py (slow)

Suites (headed ones open a window: a swapchain is needed for Reflex):
  unit tests                  C++ tests of the pure modules (no GPU)
  import                      headless import, creates .godot/extension_list.cfg
  reflex                      hook loaded, extensions enabled on Godot's device, markers, sleep,
                              timings, modes, frame-rate limit, swapchain recreation, minimised
                              window, overlay
  reflex (separate render ...) the same with Godot's separate render thread
  pass-through                a rendered frame is identical with and without the hook
  fallback: ...               forced failure cases give a clean status and no crash
  editor                      the editor opens the project with the addon, no errors
  editor with Reflex enabled  enable_in_editor (environment override): Reflex in the editor process
  editor: addon added ...     the addon loaded into a running editor stays dormant, no crash
  validation                  Vulkan validation layers (LunarG SDK) report nothing with the hook
  archive                     `git archive` ships only the addon and the licences

Requirements: Godot 4.7.2 (tools/.godot_path or GODOT_BIN), a built debug DLL (build.cmd debug),
for "validation" the Vulkan SDK (VULKAN_SDK or tools/.vulkan_sdk_path). Every Godot process is
started through tools/godot.py (lock, timeout, log file under build/logs/).
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "tools" / "godot.py"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from log_scan import problems_in  # noqa: E402

EXTENSION_LIST = ROOT / ".godot" / "extension_list.cfg"
HIDE = "GODOT_LATENCY_UPSCALING_TEST_HIDE_EXTENSIONS"


def godot(args: list[str], log: str, env: dict[str, str] | None = None, timeout: int = 300,
          medium: bool = False) -> tuple[int, str]:
    cmd = [sys.executable, str(RUNNER), "--timeout", str(timeout), "--log", log]
    for key, value in (env or {}).items():
        cmd += ["--env", f"{key}={value}"]
    if medium:
        cmd.append("--medium-integrity")
    proc = subprocess.run(cmd + args, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return proc.returncode, proc.stdout + proc.stderr


def status_of(output: str) -> dict:
    for line in output.splitlines():
        if line.startswith("LU_STATUS "):
            return json.loads(line[len("LU_STATUS "):])
    return {}


def result(name: str, ok: bool, detail: str = "", problems: list[str] | None = None) -> bool:
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""), flush=True)
    for p in (problems or [])[:12]:
        print(f"        {p}")
    return ok


# --- suites --------------------------------------------------------------------------------

def suite_unit_tests() -> bool:
    build = subprocess.run([sys.executable, str(ROOT / "tools" / "build.py"), "tests"], cwd=ROOT,
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
    if build.returncode != 0:
        return result("unit tests", False, "build failed", (build.stdout + build.stderr).splitlines()[-12:])
    run = subprocess.run([str(ROOT / "build" / "unit_tests" / "unit_tests.exe")], capture_output=True, text=True)
    summary = re.search(r"unit tests: (\d+) passed, (\d+) failed", run.stdout)
    failures = [l for l in run.stdout.splitlines() if l.startswith("FAIL") or l.startswith("    ")]
    return result("unit tests", run.returncode == 0 and summary is not None,
                  summary.group(0) if summary else "no summary", failures)


def suite_import() -> bool:
    code, out = godot(["--headless", "--import"], "test_import")
    problems = problems_in(out)
    return result("import", code == 0 and EXTENSION_LIST.exists() and not problems, f"exit {code}", problems)


def suite_reflex() -> bool:
    code, out = godot(["--script", "res://tests/reflex_tests.gd"], "test_reflex")
    summary = re.search(r"reflex tests: (\d+) passed, (\d+) failed", out)
    fails = [l for l in out.splitlines() if l.startswith("FAIL")]
    problems = problems_in(out, [r"Reflex mode must be", r"Reflex frame-rate limit must be"])
    ok = code == 0 and summary is not None and summary.group(2) == "0" and not problems
    return result("reflex", ok, summary.group(0) if summary else f"exit {code}, no summary", fails + problems)


def suite_reflex_render_thread() -> bool:
    """The same tests with Godot's separate render thread (thread model "Separate"): the render
    submission then runs on another thread than the simulation and the sleep."""
    code, out = godot(["--render-thread", "separate", "--script", "res://tests/reflex_tests.gd"], "test_reflex_render_thread")
    summary = re.search(r"reflex tests: (\d+) passed, (\d+) failed", out)
    fails = [l for l in out.splitlines() if l.startswith("FAIL")]
    # Godot 4.7.2 itself reports empty 2D texture updates for the demo in this experimental thread
    # model, with or without the addon.
    problems = problems_in(out, [r"Reflex mode must be", r"Reflex frame-rate limit must be",
                                 r'Condition "p_image\.is_null\(\) \|\| p_image->is_empty\(\)" is true',
                                 r"This function \(finalize\) can only be called from the render thread",
                                 r"2 ObjectDB instances were leaked at exit"])
    ok = code == 0 and summary is not None and summary.group(2) == "0" and not problems
    return result("reflex (separate render thread)", ok, summary.group(0) if summary else f"exit {code}, no summary",
                  fails + problems)


def suite_editor_with_reflex() -> bool:
    """Reflex enabled in the editor process itself (enable_in_editor through the environment
    override; the editor ignores override.cfg): the editor's main window gets latency mode, its
    other windows pass through, and nothing reports an error."""
    code, out = godot(["--editor", "--quit-after", "300"], "test_editor_reflex",
                      {"GODOT_LATENCY_UPSCALING_SETTINGS": "general/enable_in_editor=true"})
    problems = problems_in(out)
    active = "Reflex active in this process" in out
    return result("editor with Reflex enabled", code == 0 and active and not problems,
                  f"exit {code}" + ("" if active else ", Reflex not active"), problems)


def capture(name: str, env: dict[str, str]) -> tuple[bytes | None, list[str]]:
    rel = f"build/captures/{name}.png"
    code, out = godot(["--resolution", "640x360", "--script", "res://tests/capture_frame.gd", "--", f"--out={rel}"],
                      f"test_capture_{name}", env)
    raw = ROOT / "build" / "captures" / f"{name}.rgba"
    problems = problems_in(out) + ([f"exit {code}"] if code != 0 else [])
    return (raw.read_bytes() if raw.exists() and code == 0 else None), problems


def suite_pass_through() -> bool:
    runs = {
        "baseline_a": {"GODOT_LATENCY_UPSCALING_DISABLE": "1"},
        "baseline_b": {"GODOT_LATENCY_UPSCALING_DISABLE": "1"},  # control: is the scene deterministic at all?
        "hook_reflex": {},
        "hook_only": {HIDE: "VK_NV_low_latency2"},  # layer in place, nothing added
    }
    pixels, problems = {}, []
    for name, env in runs.items():
        data, p = capture(name, env)
        pixels[name] = data
        problems += [f"{name}: {x}" for x in p]
    if any(v is None for v in pixels.values()):
        return result("pass-through", False, "a capture failed", problems)
    digest = {k: hashlib.sha256(v).hexdigest()[:12] for k, v in pixels.items()}
    ok = (pixels["baseline_a"] == pixels["baseline_b"] == pixels["hook_reflex"] == pixels["hook_only"]) and not problems
    return result("pass-through", ok, ", ".join(f"{k}={v}" for k, v in digest.items()), problems)


# (name, engine args, env, expected-status predicate, description of the expectation)
FALLBACKS = [
    ("extension missing", [], {HIDE: "VK_NV_low_latency2"},
     lambda s: s["hook"]["active"] and not s["reflex"]["supported"] and "VK_NV_low_latency2" in s["reflex"]["reason"]
     and "VK_NV_low_latency2" not in s["vulkan"]["device_extensions"] and s["vulkan"]["device_handle"] == s["godot_vk_device"],
     "hook active, device unchanged, Reflex unavailable naming the extension"),
    ("present ID missing", [], {HIDE: "VK_KHR_present_id"},
     lambda s: not s["reflex"]["supported"] and "VK_KHR_present_id" in s["reflex"]["reason"]
     and not s["vulkan"]["added_device_extensions"],
     "Reflex unavailable naming VK_KHR_present_id, nothing added"),
    ("wrong Godot version", [], {"GODOT_LATENCY_UPSCALING_TEST_GODOT_VERSION": "4.8.0"},
     lambda s: not s["hook"]["installed"] and "4.8.0 is not supported" in s["hook"]["reason"]
     and not s["vulkan"]["loader_intercepted"] and not s["reflex"]["supported"],
     "no hook, reason names the version"),
    ("disabled by environment", [], {"GODOT_LATENCY_UPSCALING_DISABLE": "1"},
     lambda s: not s["hook"]["installed"] and "GODOT_LATENCY_UPSCALING_DISABLE" in s["hook"]["reason"],
     "no hook, reason names the variable"),
    ("Compatibility renderer", ["--rendering-method", "gl_compatibility"], {},
     lambda s: s["hook"]["installed"] and not s["hook"]["active"] and "never requested" in s["hook"]["reason"]
     and not s["reflex"]["supported"],
     "hook idle, reason names the renderer"),
    ("D3D12 driver", ["--rendering-driver", "d3d12"], {},
     lambda s: not s["hook"]["active"] and "d3d12" in s["hook"]["reason"] and not s["reflex"]["supported"],
     "hook idle, reason names d3d12"),  # skipped when Godot's D3D12 driver fails without the addon too
    ("headless", ["--headless"], {},
     lambda s: not s["hook"]["active"] and "headless" in s["hook"]["reason"] and not s["reflex"]["supported"],
     "hook idle, reason says headless"),
    # Forced faults (debug-only switch): the retry safety nets must recover with no crash.
    ("device create fails (bogus extension)", [], {"GODOT_LATENCY_UPSCALING_TEST_FAULT": "device_extension"},
     lambda s: s["hook"]["active"] and s["vulkan"]["devices_created"] >= 1
     and s["vulkan"]["device_handle"] == s["godot_vk_device"] and not s["reflex"]["supported"]
     and "without them" in s["vulkan"]["note"],
     "vkCreateDevice retried without the addon's extensions; the device is Godot's"),
    ("first swapchain create fails", [], {"GODOT_LATENCY_UPSCALING_TEST_FAULT": "swapchain_create"},
     lambda s: s["hook"]["active"] and s["reflex"]["supported"] and not s["reflex"]["active"]
     and "without it" in s["reflex"]["reason"],
     "swapchain retried without latency mode; Reflex reports the fallback, no crash"),
]


def suite_fallbacks() -> bool:
    all_ok = True
    for name, args, env, predicate, expectation in FALLBACKS:
        log = "test_fallback_" + re.sub(r"\W+", "_", name)
        code, out = godot(args + ["--script", "res://tests/print_status.gd", "--", "--frames=30"], log, env)
        status = status_of(out)
        if not status and args:
            # The engine itself may not run in this configuration on this machine (Godot 4.7.2's
            # D3D12 driver fails to create its swapchain on the reference GTX 1080). If it fails
            # the same way without the addon, the case cannot be tested here: report, don't fail.
            base_code, base_out = godot(args + ["--script", "res://tests/print_status.gd", "--", "--frames=30"],
                                        log + "_without_addon", {"GODOT_LATENCY_UPSCALING_DISABLE": "1"})
            if base_code != 0 and base_code == code and not status_of(base_out):
                print(f"SKIP  fallback: {name}  (Godot exits with {code} here even without the addon)", flush=True)
                continue
        problems = problems_in(out)
        try:
            expected = bool(status) and predicate(status)
        except KeyError as missing:
            expected, problems = False, problems + [f"status lacks {missing}"]
        reason = status.get("reflex", {}).get("reason", "") if status else "no status line"
        ok = code == 0 and expected and not problems
        all_ok &= result(f"fallback: {name}", ok, f"{expectation}; reflex: {reason}", problems)
    return all_ok


def suite_editor() -> bool:
    code, out = godot(["--editor", "--quit-after", "300"], "test_editor")
    problems = problems_in(out)
    summary = re.search(r"NVIDIA DLSS [\d.]+: (.*)", out)
    return result("editor", code == 0 and summary is not None and not problems,
                  summary.group(1) if summary else f"exit {code}, no status line", problems)


def suite_editor_late_load() -> bool:
    """The addon appears while an editor runs: simulated by removing the extension list, which
    makes the editor load the extension during its first file-system scan, i.e. after the Vulkan
    device exists. Both an interactive editor and a CI-style `--headless --import` must exit
    cleanly (Godot 4.7.2 crashed there before the addon learned to stay dormant)."""
    all_ok = True
    for label, args in (("editor", ["--editor", "--quit-after", "300"]), ("headless import", ["--headless", "--import"])):
        if EXTENSION_LIST.exists():
            EXTENSION_LIST.unlink()
        code, out = godot(args, "test_late_load_" + label.replace(" ", "_"))
        problems = problems_in(out)
        dormant = "restart the editor to activate" in out
        all_ok &= result(f"editor: addon added while running ({label})", code == 0 and dormant and not problems,
                         f"exit {code}" + ("" if dormant else ", no restart notice"), problems)
    if not EXTENSION_LIST.exists():
        godot(["--headless", "--import"], "test_import_restore")
    return all_ok


VALIDATION_LINE = re.compile(r"VALIDATION - |Validation Error|Validation Warning|Vulkan Debug Report")
MESSAGE_ID = re.compile(r"Message Id Name: (\S+)")


def vulkan_sdk() -> Path | None:
    value = os.environ.get("VULKAN_SDK", "").strip()
    local = ROOT / "tools" / ".vulkan_sdk_path"
    if not value and local.exists():
        value = local.read_text(encoding="utf-8").strip()
    return Path(value) if value else None


def suite_validation() -> bool:
    sdk = vulkan_sdk()
    if sdk is None or not (sdk / "Bin" / "VkLayer_khronos_validation.json").exists():
        return result("validation", False, "Vulkan SDK not found: set VULKAN_SDK or tools/.vulkan_sdk_path")
    env_base = {"VK_ADD_LAYER_PATH": str(sdk / "Bin")}
    args = ["--gpu-validation", "--verbose", "--script", "res://tests/reflex_tests.gd"]
    from collections import Counter

    ids = lambda messages: Counter(m.group(1) for m in map(MESSAGE_ID.search, messages) if m)  # noqa: E731
    runs = {}
    # baseline (no addon), hook active, and hook on the swapchain-fallback retry path.
    for name, args, extra in (
        ("baseline", ["--gpu-validation", "--verbose", "--script", "res://tests/reflex_tests.gd"], {"GODOT_LATENCY_UPSCALING_DISABLE": "1"}),
        ("hook", ["--gpu-validation", "--verbose", "--script", "res://tests/reflex_tests.gd"], {}),
        ("swapchain fault", ["--gpu-validation", "--verbose", "--script", "res://tests/print_status.gd", "--", "--frames=60"],
         {"GODOT_LATENCY_UPSCALING_TEST_FAULT": "swapchain_create"}),
    ):
        code, out = godot(args, f"test_validation_{name.replace(' ', '_')}", {**env_base, **extra}, timeout=600, medium=True)
        messages = [l.strip() for l in out.splitlines() if VALIDATION_LINE.search(l)]
        runs[name] = (code, ids(messages), out)
    hook_code, hook_ids, hook_out = runs["hook"]
    base_ids = runs["baseline"][1]
    fault_code, fault_ids, _ = runs["swapchain fault"]
    tests_ok = re.search(r"reflex tests: \d+ passed, 0 failed", hook_out) is not None
    layer_seen = "VK_LAYER_KHRONOS_validation" in hook_out
    # Anything either the normal or the fallback-retry run reports more often than the run without
    # the addon is attributed to the addon.
    new = sorted(((hook_ids | fault_ids) - base_ids).elements())
    ok = hook_code == 0 and fault_code == 0 and tests_ok and layer_seen and not new
    detail = (f"validation layer {'active' if layer_seen else 'NOT active'}; message IDs hook {dict(hook_ids)}, "
              f"swapchain-fault {dict(fault_ids)}, baseline {dict(base_ids)}, caused by the addon {len(new)}")
    return result("validation", ok, detail, [f"caused by the addon: {n}" for n in new])


def suite_archive() -> bool:
    proc = subprocess.run(["git", "archive", "--format=tar", "HEAD"], cwd=ROOT, capture_output=True)
    if proc.returncode != 0:
        return result("archive", False, "git archive failed (no commit yet?)")
    import io
    import tarfile

    names = tarfile.open(fileobj=io.BytesIO(proc.stdout)).getnames()
    unexpected = [n for n in names if not (n.startswith("addons/latency_upscaling") or n in ("LICENSE", "THIRD_PARTY_NOTICES.md", "addons"))]
    required = ["addons/latency_upscaling/latency_upscaling.gdextension", "addons/latency_upscaling/LICENSE",
                "addons/latency_upscaling/THIRD_PARTY_NOTICES.md", "addons/latency_upscaling/README.md", "LICENSE"]
    missing = [r for r in required if r not in names]
    # The addon ships on its own (Asset Library): its licence files must match the repository's.
    stale = [f"addons/latency_upscaling/{name} differs from {name}" for name in ("LICENSE", "THIRD_PARTY_NOTICES.md")
             if (ROOT / name).read_bytes() != (ROOT / "addons" / "latency_upscaling" / name).read_bytes()]
    return result("archive", not unexpected and not missing and not stale, f"{len(names)} entries",
                  unexpected[:10] + missing + stale)


SUITES = [
    ("unit tests", suite_unit_tests),
    ("import", suite_import),
    ("reflex", suite_reflex),
    ("reflex render thread", suite_reflex_render_thread),
    ("pass-through", suite_pass_through),
    ("fallback", suite_fallbacks),
    ("editor", suite_editor),
    ("editor reflex", suite_editor_with_reflex),
    ("late load", suite_editor_late_load),
    ("validation", suite_validation),
    ("archive", suite_archive),
]


def main(argv: list[str]) -> int:
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    results = []
    for name, suite in SUITES:
        if only and only not in name:
            continue
        results.append(suite())
    extra = []
    if "--bench" in argv:
        extra.append("run_benchmark.py")
    if "--mutations" in argv:
        extra.append("run_mutations.py")
    for script in extra:
        proc = subprocess.run([sys.executable, str(ROOT / "tests" / script)], cwd=ROOT)
        results.append(result(script, proc.returncode == 0, f"exit {proc.returncode}"))
    print(f"\n{sum(results)}/{len(results)} suites passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
