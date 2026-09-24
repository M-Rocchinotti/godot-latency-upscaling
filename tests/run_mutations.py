#!/usr/bin/env python
"""Mutation checks: prove that the tests catch the bugs they are meant to catch.

Each mutation re-introduces one bug (or removes one safeguard), rebuilds what it affects, runs
the suite that should catch it and expects that suite to FAIL. Sources are restored afterwards,
also on Ctrl+C, and the binaries are rebuilt from the clean sources at the end.

    python tests/run_mutations.py              # all mutations
    python tests/run_mutations.py sequencer    # only mutations whose name contains a word

Builds run with one compile job to keep the machine responsive. Unit-level mutations take
seconds; engine-level ones rebuild the DLL and run a Godot suite (about a minute each).

Outcomes: CAUGHT (suite failed), SURVIVED (suite passed: a test gap), INVALID (the mutant did not
compile), STALE (the original text is no longer in the file: update the mutation).
Exit code 0 only if every mutation was caught.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = [sys.executable, str(ROOT / "tools" / "build.py")]

PATCH = "src/vk/device_patch.cpp"
SEQ = "src/reflex/frame_sequencer.cpp"
STATS = "src/reflex/latency_stats.cpp"
SETTINGS = "src/core/settings.cpp"
NAME = "src/hook/loader_name.cpp"
IAT = "src/hook/iat_patch.cpp"
JITTER = "src/dlss/jitter.cpp"
REFLEX = "src/reflex/reflex_ll2.cpp"
LL2 = "src/reflex/ll2_patches.cpp"
SINGLETON = "src/godot/latency_upscaling.cpp"
BOOT = "src/godot/bootstrap.cpp"
ENTRY = "src/register_types.cpp"

# (name, file, original text, mutated text, suite: "unit" or a tests/run_all.py --only word)
MUTATIONS = [
    # --- pure modules, caught by the C++ unit tests ---
    ("patch: timeline struct not prepended", PATCH,
     "\t\t*prepend_flag = true;\n", "\t\t*prepend_flag = false;\n", "unit"),
    ("patch: in-place edits not restored", PATCH,
     "\t\t*edit.field = edit.old_value;\n", "\t\t(void)edit;\n", "unit"),
    ("patch: duplicate extensions", PATCH,
     "\t\tif (!present) {\n\t\t\tadded_.push_back(wanted);", "\t\tif (true) {\n\t\t\tadded_.push_back(wanted);", "unit"),
    ("patch: Vulkan 1.2 struct ignored", PATCH,
     "\t\t\t\t\tVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {",
     "\t\t\t\t\tVK_STRUCTURE_TYPE_MAX_ENUM)) {", "unit"),
    ("sequencer: present IDs may repeat", SEQ,
     "render_id_ <= last_presented_", "render_id_ < last_presented_", "unit"),
    ("sequencer: skipped draw leaves simulation open", SEQ,
     "\tif (sim_id_ != 0) {\n\t\temit(sim_id_, Marker::SimulationEnd);\n\t\tsim_id_ = 0;\n\t\tflash_pending_ = false;",
     "\tif (sim_id_ != 0) {\n\t\tsim_id_ = 0;\n\t\tflash_pending_ = false;", "unit"),
    ("sequencer: RENDERSUBMIT_END sent twice", SEQ,
     "\t\temit(render_id_, Marker::RenderSubmitEnd);\n\t\trender_end_sent_ = true;\n\t}\n\temit(render_id_, Marker::PresentStart);",
     "\t\temit(render_id_, Marker::RenderSubmitEnd);\n\t}\n\temit(render_id_, Marker::PresentStart);", "unit"),
    ("sequencer: flash after SIMULATION_END", SEQ,
     "\tif (flash_pending_) {\n\t\temit(id, Marker::TriggerFlash);\n\t\tflash_pending_ = false;\n\t}\n\temit(id, Marker::SimulationEnd);",
     "\temit(id, Marker::SimulationEnd);\n\tif (flash_pending_) {\n\t\temit(id, Marker::TriggerFlash);\n\t\tflash_pending_ = false;\n\t}", "unit"),
    ("stats: frames in flight counted", STATS,
     "r.gpu_render_end_us != 0 && r.gpu_render_end_us >= r.sim_start_us", "r.gpu_render_end_us >= 0", "unit"),
    ("settings: any frame-rate limit accepted", SETTINGS,
     "\treturn fps_limit == 0.0 || (fps_limit >= MIN_FPS_LIMIT && fps_limit <= MAX_FPS_LIMIT);", "\treturn true;", "unit"),
    ("settings: later layers do not win", SETTINGS,
     "\tfor (const SettingsLayer &layer : layers) {", "\tfor (const SettingsLayer &layer : std::vector<SettingsLayer>(layers.rbegin(), layers.rend())) {", "unit"),
    ("loader name: directory not stripped", NAME,
     "\t\t\tbase = p + 1;\n", "\t\t\t(void)p;\n", "unit"),
    ("iat: restore does nothing", IAT,
     "\t\twrite_pointer(slot.slot, slot.original);\n", "\t\t(void)slot;\n", "unit"),
    ("jitter: pixels not halved", JITTER,
     "\tout.x = halton(index, 2) * 0.5f;", "\tout.x = halton(index, 2);", "unit"),
    ("settings: environment override values not parsed", SETTINGS,
     "\t\t\tnumber = std::strtod(value.c_str(), &parse_end);", "\t\t\tnumber = 0.0;", "unit"),
    # Invisible on the reference driver at run time (sleep and reports work without them), so the
    # structures are guarded by unit tests.
    ("ll2: swapchain without latency mode", LL2,
     "\tlatency_.latencyModeEnable = VK_TRUE;", "\tlatency_.latencyModeEnable = VK_FALSE;", "unit"),
    ("ll2: submissions not tagged", LL2,
     "\t\tcopies_[i].pNext = &tags_[i];\n", "", "unit"),
    ("ll2: present ID not attached", LL2,
     "\tinfo_.pNext = &id_info_;\n", "", "unit"),
    # --- engine level, caught by the Godot suites ---
    ("reflex: extension not requested", REFLEX,
     "\trequest.add_extension(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);\n", "", "reflex"),
    ("reflex: sleep never called", SINGLETON,
     "\tdevice->sleep();\n", "", "reflex"),
    ("reflex: SIMULATION_END never sent", SINGLETON,
     "\tconst uint64_t frame_id = sequencer.end_simulation();", "\tconst uint64_t frame_id = 0;", "reflex"),
    ("reflex: present ID repeated (validation layer)", LL2,
     "\tids_[index] = present_id;", "\tids_[index] = 1;", "validation"),
    ("safety: version check removed", BOOT,
     "} else if (major != 4 || minor != 7) {", "} else if (false) {", "fallback"),
    ("safety: late load not dormant", ENTRY,
     "\tif (lu::boot_state().late_load) {", "\tif (false) {", "late load"),
]


def build(target: str) -> bool:
    proc = subprocess.run(BUILD + [target, "-j", "1"], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return proc.returncode == 0


def suite_fails(suite: str) -> bool:
    if suite == "unit":
        proc = subprocess.run([str(ROOT / "build" / "unit_tests" / "unit_tests.exe")], capture_output=True, text=True)
    else:
        proc = subprocess.run([sys.executable, str(ROOT / "tests" / "run_all.py"), "--only", suite], cwd=ROOT,
                              capture_output=True, text=True, encoding="utf-8", errors="replace")
    return proc.returncode != 0


def read(path: Path) -> str:
    with open(path, encoding="utf-8", newline="") as f:  # no newline translation: restore byte-exact
        return f.read()


def write(path: Path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def run(name: str, rel: str, old: str, new: str, suite: str) -> str:
    path = ROOT / rel
    original = read(path)
    if original.count(old) != 1:
        return "STALE"
    try:
        write(path, original.replace(old, new))
        if not build("tests" if suite == "unit" else "debug"):
            return "INVALID"
        return "CAUGHT" if suite_fails(suite) else "SURVIVED"
    finally:
        write(path, original)


def main(argv: list[str]) -> int:
    words = [a for a in argv if not a.startswith("-")]
    selected = [m for m in MUTATIONS if not words or any(w in m[0] for w in words)]
    if "--check" in argv:  # only verify that every mutation still applies, without building
        stale = [m[0] for m in selected if read(ROOT / m[1]).count(m[2]) != 1]
        for name in stale:
            print(f"STALE     {name}")
        print(f"{len(selected) - len(stale)}/{len(selected)} mutations apply")
        return 1 if stale else 0
    outcomes = []
    try:
        for name, rel, old, new, suite in selected:
            outcome = run(name, rel, old, new, suite)
            outcomes.append(outcome)
            print(f"{outcome:<9} {name}  [{suite}]", flush=True)
    finally:
        # Leave clean binaries behind whatever happened above.
        restored = build("tests") and build("debug")
        print("binaries rebuilt from clean sources" if restored else "WARNING: rebuilding clean binaries failed")
    caught = outcomes.count("CAUGHT")
    print(f"\nmutations: {caught} caught, {len(outcomes) - caught} not caught")
    return 0 if caught == len(outcomes) and restored else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
