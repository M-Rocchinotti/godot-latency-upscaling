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
                              timings, modes, frame-rate limit (present-lock bound), swapchain
                              recreation, minimised window, local device, second window, overlay
  reflex (separate render ...) the same with Godot's separate render thread
  pass-through                a rendered frame is identical with and without the hook (also for a
                              project that uses Godot's FSR2, with the addon's upscaler inactive)
  upscaler (reference)        Phase 2 plumbing with the reference upscaler: FSR2 slot, jitter vs
                              Godot's UBO, image quality and conventions, velocity, motion
  upscaler (native)           the same, recorded natively into Godot's command buffer by the
                              layer's marker interception; equals Godot's own recording bit for bit
  nvidia runtime              fetching the NVIDIA runtime: tools/fetch_nvidia_sdk.py and the editor
                              dialog (licence acceptance, lock file, archive handling) with synthetic
                              archives; NVIDIA's real archive against the lock when a copy is in
                              build/cache/nvidia/ (nothing is downloaded)
  fallback: ...               forced failure cases give a clean status and no crash
  editor                      the editor opens the project with the addon, no errors
  editor with Reflex enabled  enable_in_editor (environment override): Reflex in the editor process
  editor: addon added ...     the addon loaded into a running editor stays dormant, no crash
  validation                  Vulkan validation layers (LunarG SDK) report nothing with the hook
  archive                     `git archive` ships only the addon and the licences
  portable api                the README examples parse and run with the extension loaded, removed,
                              without a library for the platform, and in filtered (incl. Linux)
                              exports (tests/run_portable.py)
  package                     the release zip (both DLLs) installed into a fresh project: editor,
                              game run from the editor, debug and release export (tests/run_package.py)

A skipped item is reported as SKIP, never as PASS, and fails the run unless it is listed in
ALLOWED_SKIPS (with the reason) or accepted with --allow-skip NAME.

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


# A skip is neither a pass nor a failure: it is reported separately, and it fails the run unless
# the skipped item is listed here (with the reason it cannot run on the reference machine) or
# allowed with --allow-skip NAME.
ALLOWED_SKIPS = {
    "fallback: D3D12 driver": "Godot 4.7.2's own D3D12 driver crashes on the reference GTX 1080, with or "
                              "without the addon; the refusal path is covered by code review and the status tests",
    "reflex (separate render thread): second native window":
        "Godot 4.7.2 crashes (signal 11 in the driver) when a second native window is used under the separate "
        "render thread, with the addon disabled too; the same check runs in the default thread model and as a "
        "unit test (ll2_other_windows_swapchains_pass_through)",
}
SKIPPED: list[str] = []


def skip(name: str, reason: str) -> None:
    SKIPPED.append(name)
    print(f"SKIP  {name}  ({reason})", flush=True)


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


RUNTIME_ZIP_CACHE = ROOT / "build" / "cache" / "nvidia"


def suite_nvidia_runtime() -> bool:
    proc = subprocess.run([sys.executable, str(ROOT / "tests" / "test_fetch_nvidia_sdk.py")], cwd=ROOT,
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    py_summary = re.search(r"Ran (\d+) tests", proc.stderr)
    all_ok = result("nvidia runtime: fetch tool", proc.returncode == 0,
                    f"{py_summary.group(1) if py_summary else '?'} Python tests",
                    [l for l in proc.stderr.splitlines() if l.startswith(("FAIL:", "ERROR:"))])
    manifest = json.loads((ROOT / "addons" / "latency_upscaling" / "nvidia_runtime.json").read_text(encoding="utf-8"))
    cached = RUNTIME_ZIP_CACHE / manifest["asset"]
    env = {"GODOT_LATENCY_UPSCALING_TEST_RUNTIME_ZIP": str(cached)} if cached.exists() else {}
    code, out = godot(["--headless", "--script", "res://tests/nvidia_runtime_tests.gd"], "test_nvidia_runtime", env, timeout=300)
    summary = re.search(r"nvidia runtime tests: (\d+) passed, (\d+) failed", out)
    problems = problems_in(out)
    all_ok &= result("nvidia runtime: editor dialog", code == 0 and summary is not None and summary.group(2) == "0" and not problems,
                     summary.group(0) if summary else f"exit {code}, no summary",
                     [l for l in out.splitlines() if l.startswith("FAIL")] + problems)
    if not cached.exists():
        skip("nvidia runtime: real archive", f"no copy of {manifest['asset']} in {RUNTIME_ZIP_CACHE}")
    return all_ok


def suite_import() -> bool:
    code, out = godot(["--headless", "--import"], "test_import")
    problems = problems_in(out)
    return result("import", code == 0 and EXTENSION_LIST.exists() and not problems, f"exit {code}", problems)


def report_test_skips(suite: str, out: str) -> None:
    for line in out.splitlines():
        m = re.match(r"SKIP (.+?)  \((.*)\)$", line)
        if m:
            skip(f"{suite}: {m.group(1)}", m.group(2))


def suite_reflex() -> bool:
    code, out = godot(["--script", "res://tests/reflex_tests.gd"], "test_reflex")
    report_test_skips("reflex", out)
    summary = re.search(r"reflex tests: (\d+) passed, (\d+) failed", out)
    fails = [l for l in out.splitlines() if l.startswith("FAIL")]
    problems = problems_in(out, [r"Reflex mode must be", r"Reflex frame-rate limit must be"])
    ok = code == 0 and summary is not None and summary.group(2) == "0" and not problems
    return result("reflex", ok, summary.group(0) if summary else f"exit {code}, no summary", fails + problems)


def suite_reflex_render_thread() -> bool:
    """The same tests with Godot's separate render thread (thread model "Separate"): the render
    submission then runs on another thread than the simulation and the sleep."""
    code, out = godot(["--render-thread", "separate", "--script", "res://tests/reflex_tests.gd"], "test_reflex_render_thread")
    report_test_skips("reflex (separate render thread)", out)
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


def capture(name: str, env: dict[str, str], extra: list[str] | None = None) -> tuple[bytes | None, list[str]]:
    rel = f"build/captures/{name}.png"
    code, out = godot(["--resolution", "640x360", "--script", "res://tests/capture_frame.gd", "--", f"--out={rel}"] + (extra or []),
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
    # A project using Godot's own FSR2: with the upscaler inactive (no DLSS here, no test backend)
    # the addon must leave that path untouched too.
    for name, env in (("fsr2_baseline", {"GODOT_LATENCY_UPSCALING_DISABLE": "1"}), ("fsr2_hook", {})):
        data, p = capture(name, env, ["--fsr2=0.5"])
        pixels[name] = data
        problems += [f"{name}: {x}" for x in p]
    if pixels["fsr2_baseline"] is None or pixels["fsr2_hook"] is None:
        return result("pass-through", False, "an FSR2 capture failed", problems)
    digest = {k: hashlib.sha256(v).hexdigest()[:12] for k, v in pixels.items()}
    ok = (pixels["baseline_a"] == pixels["baseline_b"] == pixels["hook_reflex"] == pixels["hook_only"]
          and pixels["fsr2_baseline"] == pixels["fsr2_hook"]) and not problems
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
     "vkCreateDevice retried without the addon's extensions; the device is Godot's",
     [r"vkCreateDevice with the addon's extensions failed"]),  # the fault provokes this warning
    ("first swapchain create fails", [], {"GODOT_LATENCY_UPSCALING_TEST_FAULT": "swapchain_create"},
     lambda s: s["hook"]["active"] and s["reflex"]["supported"] and not s["reflex"]["active"]
     and "without it" in s["reflex"]["reason"],
     "swapchain retried without latency mode; Reflex reports the fallback, no crash",
     [r"creating the swapchain with latency mode failed"]),
    ("loader missing entry points", [], {"GODOT_LATENCY_UPSCALING_TEST_FAULT": "loader_incomplete"},
     lambda s: s["hook"]["installed"] and not s["hook"]["active"] and "does not export" in s["hook"]["reason"]
     and s["vulkan"]["loader_intercepted"] and not s["reflex"]["supported"],
     "Vulkan passes through the real loader untouched; the game still runs",
     [r"the Vulkan loader does not export"]),
    # Phase 2: native recording without VK_KHR_push_descriptor falls back to Godot recording the
    # reference upscaler itself (the marker is never registered), and the device gets nothing added.
    ("native upscaler without push descriptors", [],
     {HIDE: "VK_KHR_push_descriptor", "GODOT_LATENCY_UPSCALING_TEST_UPSCALER": "native",
      "GODOT_LATENCY_UPSCALING_SETTINGS": "dlss/quality=4"},
     lambda s: s["upscaling"]["active"] and s["upscaling"]["frames_upscaled"] > 20 and not s["upscaling"]["native"]["active"]
     and "VK_KHR_push_descriptor" in s["upscaling"]["native"]["error"] and s["upscaling"]["native"]["marker_dispatches"] == 0
     and "VK_KHR_push_descriptor" not in s["vulkan"]["added_device_extensions"] and s["reflex"]["supported"],
     "Godot records the reference upscaler itself; nothing intercepted, push descriptors not added, Reflex unaffected",
     [r"native recording unavailable"], "res://tests/upscaler_run.gd"),
]


def suite_fallbacks() -> bool:
    all_ok = True
    for entry in FALLBACKS:
        name, args, env, predicate, expectation = entry[:5]
        expected_warnings = entry[5] if len(entry) > 5 else []  # warnings the case provokes on purpose
        script = entry[6] if len(entry) > 6 else "res://tests/print_status.gd"
        log = "test_fallback_" + re.sub(r"\W+", "_", name)
        code, out = godot(args + ["--script", script, "--", "--frames=60"], log, env)
        status = status_of(out)
        if not status and args:
            # The engine itself may not run in this configuration on this machine (Godot 4.7.2's
            # D3D12 driver fails to create its swapchain on the reference GTX 1080). If it fails
            # the same way without the addon, the case cannot be tested here: report, don't fail.
            base_code, base_out = godot(args + ["--script", "res://tests/print_status.gd", "--", "--frames=30"],
                                        log + "_without_addon", {"GODOT_LATENCY_UPSCALING_DISABLE": "1"})
            if base_code != 0 and base_code == code and not status_of(base_out):
                skip(f"fallback: {name}", f"Godot exits with {code} here even without the addon")
                continue
        problems = problems_in(out, expected_warnings)
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
    summary = re.search(r"Latency & Upscaling [\d.]+: (.*)", out)
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


STREAMLINE_RUNTIME = ROOT / "addons" / "latency_upscaling" / "bin" / "nvidia" / "sl.interposer.dll"


def vulkan_sdk() -> Path | None:
    value = os.environ.get("VULKAN_SDK", "").strip()
    local = ROOT / "tools" / ".vulkan_sdk_path"
    if not value and local.exists():
        value = local.read_text(encoding="utf-8").strip()
    return Path(value) if value else None


def suite_validation() -> bool:
    sdk = vulkan_sdk()
    if sdk is None or not (sdk / "Bin" / "VkLayer_khronos_validation.json").exists():
        skip("validation", "Vulkan SDK not found: set VULKAN_SDK or tools/.vulkan_sdk_path")
        return None
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
        # Phase 2: Godot's own FSR2 without the addon (it runs once before every takeover), then
        # the reference upscaler in the FSR2 slot.
        ("fsr2 baseline", ["--gpu-validation", "--verbose", "--script", "res://tests/upscaler_run.gd", "--", "--frames=240", "--fsr2=0.5"],
         {"GODOT_LATENCY_UPSCALING_DISABLE": "1"}),
        ("reference upscaler", ["--gpu-validation", "--verbose", "--script", "res://tests/upscaler_run.gd", "--", "--frames=240"],
         {"GODOT_LATENCY_UPSCALING_TEST_UPSCALER": "reference", "GODOT_LATENCY_UPSCALING_SETTINGS": "dlss/quality=4"}),
        # ... and recorded natively into Godot's command buffer (push descriptors, re-bound state).
        ("native upscaler", ["--gpu-validation", "--verbose", "--script", "res://tests/upscaler_run.gd", "--", "--frames=240"],
         {"GODOT_LATENCY_UPSCALING_TEST_UPSCALER": "native", "GODOT_LATENCY_UPSCALING_SETTINGS": "dlss/quality=4"}),
    ) + ((
        # With the NVIDIA runtime fetched: Vulkan through Streamline's interposer (forced on this
        # GPU, where DLSS itself cannot run), with the native recording at the marker.
        ("streamline pass-through", ["--gpu-validation", "--verbose", "--script", "res://tests/upscaler_run.gd", "--", "--frames=240"],
         {"GODOT_LATENCY_UPSCALING_TEST_FORCE_STREAMLINE": "1", "GODOT_LATENCY_UPSCALING_TEST_UPSCALER": "native",
          "GODOT_LATENCY_UPSCALING_SETTINGS": "dlss/quality=4"}),
    ) if STREAMLINE_RUNTIME.exists() else ()):
        code, out = godot(args, f"test_validation_{name.replace(' ', '_')}", {**env_base, **extra}, timeout=600, medium=True)
        messages = [l.strip() for l in out.splitlines() if VALIDATION_LINE.search(l)]
        runs[name] = (code, ids(messages), out)
    hook_code, hook_ids, hook_out = runs["hook"]
    base_ids = runs["baseline"][1]
    fault_code, fault_ids, _ = runs["swapchain fault"]
    fsr2_ids = runs["fsr2 baseline"][1]
    up_code, up_ids, up_out = runs["reference upscaler"]
    native_code, native_ids, native_out = runs["native upscaler"]
    sl_code, sl_ids, sl_out = runs.get("streamline pass-through", (0, Counter(), ""))
    sl_ran = not STREAMLINE_RUNTIME.exists() or (
        '"loader_override_active":true' in sl_out and re.search(r'"substituted":(\d+)', sl_out) is not None
        and int(re.search(r'"substituted":(\d+)', sl_out).group(1)) > 100)
    tests_ok = re.search(r"reflex tests: \d+ passed, 0 failed", hook_out) is not None
    layer_seen = "VK_LAYER_KHRONOS_validation" in hook_out
    upscaled = re.search(r'"frames_upscaled":(\d+)', up_out)
    upscaler_ran = upscaled is not None and int(upscaled.group(1)) > 100
    substituted = re.search(r'"substituted":(\d+)', native_out)
    native_ran = substituted is not None and int(substituted.group(1)) > 100
    # Anything either the normal or the fallback-retry run reports more often than the run without
    # the addon is attributed to the addon; for the upscaler, more often than Godot's own FSR2 path.
    new = sorted(((hook_ids | fault_ids) - base_ids).elements())
    new += sorted(((up_ids | native_ids | sl_ids) - (base_ids | fsr2_ids)).elements())
    ok = (hook_code == 0 and fault_code == 0 and up_code == 0 and native_code == 0 and sl_code == 0 and upscaler_ran
          and native_ran and sl_ran and tests_ok and layer_seen and not new)
    detail = (f"validation layer {'active' if layer_seen else 'NOT active'}; message IDs hook {dict(hook_ids)}, "
              f"swapchain-fault {dict(fault_ids)}, reference upscaler {dict(up_ids)} "
              f"({upscaled.group(1) if upscaled else 0} frames upscaled), native upscaler {dict(native_ids)} "
              f"({substituted.group(1) if substituted else 0} dispatches recorded natively), "
              f"{'Streamline pass-through ' + str(dict(sl_ids)) + (' (through sl.interposer.dll), ' if sl_ran else ' (NOT through Streamline), ') if STREAMLINE_RUNTIME.exists() else 'Streamline pass-through not run (no NVIDIA runtime fetched), '}"
              f"baseline {dict(base_ids)}, "
              f"FSR2 baseline {dict(fsr2_ids)}, caused by the addon {len(new)}")
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


def suite_upscaler() -> bool:
    # Phase 2 plumbing with the reference upscaler (debug builds): the FSR2 slot, jitter against
    # Godot's own UBO, still-image quality and conventions, velocity sign/scale, motion. Once
    # recorded by Godot (RenderingDevice), once natively by the layer's marker interception.
    all_ok = True
    for backend in ("reference", "native"):
        code, out = godot(["--script", "res://tests/upscaler_tests.gd"], f"test_upscaler_{backend}",
                          {"GODOT_LATENCY_UPSCALING_TEST_UPSCALER": backend,
                           "GODOT_LATENCY_UPSCALING_SETTINGS": "dlss/quality=4"}, timeout=600)
        summary = re.search(r"upscaler tests: (\d+) passed, (\d+) failed", out)
        fails = [l for l in out.splitlines() if l.startswith("FAIL")]
        problems = problems_in(out)
        ok = code == 0 and summary is not None and summary.group(2) == "0" and not problems
        measured = [l for l in out.splitlines() if l.startswith(("STILL ", "VELOCITY ", "FILL ", "MOTION "))]
        all_ok &= result(f"upscaler ({backend})", ok, summary.group(0) if summary else f"exit {code}, no summary",
                         fails + problems + measured)
    return all_ok


def suite_portable() -> bool:
    # README examples with the extension loaded / removed / without a library, filtered exports.
    proc = subprocess.run([sys.executable, str(ROOT / "tests" / "run_portable.py")], cwd=ROOT,
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    lines = proc.stdout.splitlines()
    summary = next((l for l in lines if l.startswith("portable test:")), "no summary")
    return result("portable api", proc.returncode == 0, summary, [l for l in lines if l.startswith("FAIL")])


def suite_package() -> bool:
    # Builds the release zip (both DLLs) and installs it into a fresh project: editor, game from the
    # editor, debug export, release export (tests/run_package.py).
    proc = subprocess.run([sys.executable, str(ROOT / "tests" / "run_package.py")], cwd=ROOT,
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    lines = proc.stdout.splitlines()
    summary = next((l for l in lines if l.startswith("package test:")), "no summary")
    return result("package", proc.returncode == 0, summary, [l for l in lines if l.startswith("FAIL")])


SUITES = [
    ("unit tests", suite_unit_tests),
    ("import", suite_import),
    ("reflex", suite_reflex),
    ("reflex render thread", suite_reflex_render_thread),
    ("pass-through", suite_pass_through),
    ("upscaler", suite_upscaler),
    ("nvidia runtime", suite_nvidia_runtime),
    ("fallback", suite_fallbacks),
    ("editor", suite_editor),
    ("editor reflex", suite_editor_with_reflex),
    ("late load", suite_editor_late_load),
    ("validation", suite_validation),
    ("archive", suite_archive),
    ("portable api", suite_portable),
    ("package", suite_package),
]


def main(argv: list[str]) -> int:
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    allowed = dict(ALLOWED_SKIPS)
    for i, a in enumerate(argv):
        if a == "--allow-skip" and i + 1 < len(argv):
            allowed[argv[i + 1]] = "allowed on the command line"
    results = []
    for name, suite in SUITES:
        if only and only not in name:
            continue
        outcome = suite()
        if outcome is not None:  # None: the whole suite was skipped (already reported)
            results.append(outcome)
    extra = []
    if "--bench" in argv:
        extra.append("run_benchmark.py")
    if "--mutations" in argv:
        extra.append("run_mutations.py")
    for script in extra:
        proc = subprocess.run([sys.executable, str(ROOT / "tests" / script)], cwd=ROOT)
        results.append(result(script, proc.returncode == 0, f"exit {proc.returncode}"))
    passed, failed = sum(results), len(results) - sum(results)
    unallowed = [s for s in SKIPPED if s not in allowed]
    print(f"\n{passed} suites passed, {failed} failed, {len(SKIPPED)} skipped"
          + (f" ({len(SKIPPED) - len(unallowed)} allowed)" if SKIPPED else ""))
    for s in SKIPPED:
        print(f"  skipped: {s}: " + (allowed[s] if s in allowed else "NOT ALLOWED (use --allow-skip to accept)"))
    return 0 if failed == 0 and not unallowed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
