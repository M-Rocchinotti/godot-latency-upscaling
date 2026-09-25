# godot-latency-upscaling

A Godot 4.7 addon (GDExtension) that brings NVIDIA technologies to any Vulkan project without
engine changes and without project setup: **NVIDIA Reflex** (low-latency mode, frame-rate limiter,
latency markers and measurements) and **DLSS Super Resolution / DLAA**. Frame Generation is planned
for the next phase on the same foundation: an in-process Vulkan dispatch layer the addon installs in
front of Godot at start-up.

> Status: **Phase 2 of 3 (unreleased; the last release is 0.1.0, Reflex only).** Reflex is tested
> on a GTX 1080 and an RTX 5080, DLSS Super Resolution / DLAA on an RTX 5080. DLSS needs NVIDIA's
> runtime, which the developer fetches after accepting NVIDIA's licences (see
> [DLSS](#dlss-the-nvidia-runtime)); it is never part of this addon. The addon relies on Godot 4.7.x
> internals; other versions are refused cleanly.

## Features

| Feature | Status | Notes |
|---|---|---|
| NVIDIA Reflex Low Latency: Off / On / On + Boost | done | `VK_NV_low_latency2`, default On |
| Reflex frame-rate limiter | done | driver-paced, replaces `Engine.max_fps` |
| Latency markers (simulation, render submit, present, input, flash) | done | every frame, all modes |
| Latency measurements (`vkGetLatencyTimingsNV`) | done | the singleton's `get_latency_stats()` (reach it through `Engine`, see Usage) |
| Reflex Flash Indicator marker on left clicks | done | the driver decides whether to draw it |
| Small latency overlay | done | optional, off by default |
| Clean fallback with a readable reason | done | wrong Godot version, other GPUs, other renderers, missing driver support |
| DLSS Super Resolution / DLAA | done (Phase 2) | modes DLAA, Quality, Balanced, Performance, Ultra Performance, Auto; presets J/K/L/M; history reset on camera cuts (automatic, and `reset_dlss_history()`); motion vectors for the sky; NVIDIA runtime fetched by the developer |
| DLSS Frame Generation / Multi Frame Generation | planned (Phase 3) | |
| DLSS Ray Reconstruction | not planned | Godot 4.7 has no ray-traced signals to denoise |
| D3D12 renderer, Compatibility renderer | not supported | the addon stays idle and says why |
| Linux, macOS, Android, iOS, Web, Windows on ARM | not supported | no library: exclude the addon from those exports (see [Platforms](#platforms)) |

## Requirements

| Feature | GPU | Driver | Other |
|---|---|---|---|
| Reflex (this version) | NVIDIA GPU whose driver exposes `VK_NV_low_latency2`: NVIDIA lists Reflex for GeForce 900 series and newer; tested on GTX 1080 (Pascal) | `VK_NV_low_latency2` first shipped in 551.70; tested with 582.28 | Windows 10/11 x64, Godot 4.7.x, Vulkan renderer (Forward+ or Mobile) |
| DLSS Super Resolution / DLAA | GeForce RTX 20 series (Turing) or newer; tested on RTX 5080 | 512.15 or newer (DLSS guide 2.1, Streamline); tested with 591.86 | Windows 10 1709+, Vulkan renderer; the NVIDIA runtime (fetched, see below) |
| DLSS Frame Generation (planned) | GeForce RTX 40 series (Ada) or newer | 520 or newer (527.64+ for native Vulkan optical flow) | Windows 10 20H1+, hardware-accelerated GPU scheduling on |
| Multi Frame Generation (planned) | GeForce RTX 50 series | | as Frame Generation |

Other GPUs, older drivers and unsupported configurations do not break anything: Godot runs
exactly as without the addon, and `get_reflex_status()` / `get_dlss_status()` say why a feature is
unavailable.

To build from source: Python 3.10+, MSVC (Visual Studio 2022 Build Tools or a portable MSVC
14.4x with the Windows SDK), Git. SCons is installed automatically. For the validation-layer test:
the LunarG Vulkan SDK.

## Installation

1. Download a **release zip** and unzip it into your project folder, so that
   `addons/latency_upscaling/` sits at the project root (keep the path). The zip contains both DLLs:
   the editor, games run from it and debug exports load the debug DLL, release exports the release
   DLL. GitHub's green **"Download ZIP"** button (a `git archive` of the sources) is **not
   installable**: it has no DLLs, which are build outputs. Without a release, build them (below).
2. Open the project in Godot 4.7. The GDExtension loads by itself. If you copied the folder while
   the editor was running, restart the editor once (games you start from it work immediately).
3. Optional: Project > Project Settings > Plugins > enable **Latency & Upscaling**. The plugin adds a
   status message and Project > Tools > "Latency & Upscaling: Print Status". The GDExtension and all
   settings work without enabling the plugin.

Nothing else is needed for Reflex: no autoload, no node in your scenes, no change to
`project.godot`. DLSS additionally needs NVIDIA's runtime:

### DLSS: the NVIDIA runtime

DLSS runs on NVIDIA's own binaries (Streamline 2.14.1 and the DLSS library). They are licensed by
NVIDIA (the DLSS library under the NVIDIA RTX SDKs licence, which does not allow redistributing it
stand-alone), so they are **not** in this addon or its release zip. Fetch them once per project:

- **In the editor:** enable the plugin (step 3). With DLSS on and the runtime missing, a dialog
  shows NVIDIA's licences: the licence files of exactly the pinned release (`nvngx_dlss.license.txt`,
  the NVIDIA RTX SDKs licence, and Streamline's `license.txt`), each checked against the SHA-256 the
  lock records for that file in NVIDIA's archive, so what you accept is what ships with the DLLs.
  It shows the download size (276 MB; 70 MB installed) and the folder the archive goes to
  (by default next to the runtime's folder, changeable with Browse; it is deleted after the
  install). **Download** stays disabled until the texts are verified and you tick that you have
  read and accept them. It downloads `streamline-sdk-v2.14.1.zip` from NVIDIA's GitHub release,
  checks it against the release's SHA-256 and the hashes pinned in
  `addons/latency_upscaling/nvidia_runtime.lock.json`, installs the needed files into
  `addons/latency_upscaling/bin/nvidia/` (keep that folder out of version control; this
  repository's `.gitignore` does) and records there who accepted, when and how
  (`licence_acceptance.json`). The dialog is also under Project > Tools > "Latency & Upscaling: Get
  the NVIDIA DLSS Runtime".
- **From the command line:** `python tools/fetch_nvidia_sdk.py --show-licences` (the same verified
  texts), then `python tools/fetch_nvidia_sdk.py --download --accept-nvidia-licences --accepted-by
  "Your Name"`. `--accepted-on YYYY-MM-DD` and `--how "..."` record an acceptance given earlier or
  elsewhere; the tool never records itself as the one accepting.

Restart the editor and your game afterwards. Windows exports then carry the four DLLs DLSS Super
Resolution needs and NVIDIA's licence files in `nvidia/` beside the game executable. That needs the
editor plugin enabled; an export made with it disabled warns that it goes without the runtime.
Before you ship, read the NVIDIA section at the end of this file: NVIDIA's licence has conditions
for games (notification, attribution).

### Building

```
git clone --recurse-submodules <repository url> godot-latency-upscaling
cd godot-latency-upscaling
build.cmd            # debug + release DLLs and the C++ unit tests
build.cmd debug      # only the DLL the editor and debug exports use
```

`build.cmd` (= `python tools/build.py`) creates `.venv/` with the pinned SCons, finds MSVC and runs
SCons with 2 parallel jobs (`-j N` to change). MSVC is taken from, in this order: an already set up
developer prompt (INCLUDE and LIB set); a dev-shell script named in `GODOT_LATENCY_UPSCALING_DEVSHELL` or
`tools/.devshell_path` that runs its arguments inside the MSVC environment; SCons' Visual Studio
detection. Output: `addons/latency_upscaling/bin/godot_latency_upscaling.windows.template_{debug,release}.x86_64.dll`.

## Platforms

The addon has one library: **Windows x86_64** (Vulkan renderer). There is none for Linux, macOS,
Android, iOS, Web or Windows on ARM.

- **Exclude the addon from exports to other platforms.** In each such export preset, Resources tab >
  "Filters to exclude files/folders from project", enter `addons/latency_upscaling/*`. Godot then
  leaves the addon, and its entry in the exported extension list, out of that export.
- Without the filter the export still succeeds, with a warning (`No "x86_64" library found for
  GDExtension: "res://addons/latency_upscaling/latency_upscaling.gdextension" ...`), and the game
  prints Godot's standard messages at start-up:

  ```
  ERROR: No GDExtension library found for current OS and architecture (linux.x86_64) in configuration file: res://addons/latency_upscaling/latency_upscaling.gdextension
  ERROR: GDExtension dynamic library not found: 'res://addons/latency_upscaling/latency_upscaling.gdextension'.
  ERROR: Error loading extension: 'res://addons/latency_upscaling/latency_upscaling.gdextension'.
  ```

  They are **harmless**: the game runs normally, only without the addon. The filter just avoids them.
- On those platforms the `LatencyUpscaling` singleton does not exist, so scripts must reach it
  through `Engine` (see [Usage](#usage)); a script that names it directly does not parse there.

`tests/run_portable.py` checks all of this: the README examples with the extension loaded, with the
addon removed, with no library for the platform (the start-up messages above), in a Windows export
with the filter, and real Linux exports with and without the filter. A cross-platform no-op stub
library exists in `stub/` but is experimental and not shipped (see `stub/README.md`).

## Configuration

Settings are resolved in this order (later wins):

1. built-in defaults (compiled in; the table below);
2. Project Settings `latency_upscaling/...` — the override store. They appear in the editor's
   Project Settings, are written to `project.godot` (and thus into the exported `project.binary`)
   only when you change them from the default, and are read at CORE init before the Vulkan device
   exists, so an exported game honours them **whether or not the editor plugin is enabled** — no
   separate config file is shipped or needed;
3. the environment: `GODOT_LATENCY_UPSCALING_SETTINGS="key=value;key=value"` (for example
   `general/enable_in_editor=true;reflex/mode=2`), then `GODOT_LATENCY_UPSCALING_DISABLE=1`, which
   disables the addon completely;
4. calls to the `LatencyUpscaling` singleton at run time.

| Setting | Default | Meaning |
|---|---|---|
| `general/enabled` | `true` | `false`: the Vulkan hook is not installed (restart needed) |
| `general/enable_in_editor` | `false` | Reflex in the editor process itself (restart needed) |
| `reflex/mode` | `1` (On) | 0 Off, 1 On, 2 On + Boost |
| `reflex/fps_limit` | `0` | frame-rate limit applied by the Reflex sleep; 0 or 10 to 1000 |
| `reflex/flash_indicator_marker` | `true` | send the flash marker on left mouse clicks |
| `overlay/enabled` | `false` | show the latency overlay in games |
| `dlss/enabled` | `true` | DLSS where available; the runtime is only loaded when this is on at startup |
| `dlss/quality` | `0` (Auto) | 0 Auto (by output size: below 1920x1080 off, up to 1440p Quality, up to 4K Performance, above Ultra Performance), 1 DLAA, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance |
| `dlss/preset` | `0` (Default) | 0 Default (NVIDIA's choice per mode), 1 J, 2 K, 3 L, 4 M |
| `dlss/cut_angle_degrees` | `60` | a camera turn larger than this in one frame is a cut: the upscaler's history is reset (0: never) |
| `dlss/cut_distance` | `10` | a camera move larger than this (world units) in one frame is a cut (0: never) |
| `dlss/project_id` | empty | the project ID NVIDIA's runtime gets, GUID-like (`a0f57b54-1daf-4934-90ae-c4035c19df04`); empty: derived from `application/config/name`, so give projects that share a name their own (restart needed) |

Other environment variables: `GODOT_LATENCY_UPSCALING_VERBOSE=1` prints informational lines (also
enabled by `--verbose`). Debug builds read three test knobs used by the fallback tests:
`GODOT_LATENCY_UPSCALING_TEST_HIDE_EXTENSIONS`, `GODOT_LATENCY_UPSCALING_TEST_GODOT_VERSION` and
`GODOT_LATENCY_UPSCALING_TEST_FAULT` (forced failures: `device_extension`, `swapchain_create`,
`loader_incomplete`). Release builds ignore all three.

## Usage

The singleton `LatencyUpscaling` is registered automatically. Reflex is On by default.

**Write your scripts against it through `Engine`, as below.** The singleton exists only where the
extension is loaded (Windows x86_64). Code that names it directly (`LatencyUpscaling.reflex_mode =
1`) does not even parse on other platforms or in an export that excludes the addon; this style
parses and runs everywhere and simply does nothing where the addon is absent.
`tests/run_portable.py` runs this exact block with the extension loaded, with the addon removed,
with no library for the platform, and in an export that excludes the addon.

<!-- portable-example -->
```gdscript
extends Node
## Latency & Upscaling from GDScript: parses and runs on every platform. Where the extension is
## not loaded (another OS, or the addon excluded from an export) these functions do nothing.

const SINGLETON := "LatencyUpscaling"
const REFLEX_MODE_OFF := 0 # the values of LatencyUpscaling.REFLEX_MODE_*
const REFLEX_MODE_ON := 1
const REFLEX_MODE_ON_BOOST := 2


# For a settings menu. NVIDIA's UI guidelines: name it "NVIDIA Reflex Low Latency" with the
# options Off / On / On + Boost, and disable the control when Reflex is unavailable.
func is_reflex_available() -> bool:
	return Engine.has_singleton(SINGLETON) and Engine.get_singleton(SINGLETON).is_reflex_supported()


func set_reflex_mode(mode: int) -> void:
	if Engine.has_singleton(SINGLETON):
		Engine.get_singleton(SINGLETON).set("reflex_mode", mode)


func set_fps_limit(fps: float) -> void: # driver-paced: use it instead of Engine.max_fps
	if Engine.has_singleton(SINGLETON):
		Engine.get_singleton(SINGLETON).set("reflex_fps_limit", fps)


func show_latency_overlay(on: bool) -> void:
	if Engine.has_singleton(SINGLETON):
		Engine.get_singleton(SINGLETON).set("overlay_visible", on)


func reflex_status() -> String: # "active", "unavailable: <reason>" or "not available on this platform"
	if not Engine.has_singleton(SINGLETON):
		return "not available on this platform"
	return Engine.get_singleton(SINGLETON).get_reflex_status()


func latency_ms() -> float: # simulation start to GPU render end, average of the last 64 frames
	if not Engine.has_singleton(SINGLETON):
		return 0.0
	var stats: Dictionary = Engine.get_singleton(SINGLETON).get_latency_stats()
	return stats.get("sim_start_to_gpu_end_ms", 0.0)


# DLSS. NVIDIA's UI guidelines: "NVIDIA DLSS" with the modes by name; hide or disable the control
# when DLSS is unavailable. Quality: 0 Auto, 1 DLAA, 2 Quality, 3 Balanced, 4 Performance,
# 5 Ultra Performance (the values of LatencyUpscaling.DLSS_QUALITY_*).
func is_dlss_available() -> bool:
	return Engine.has_singleton(SINGLETON) and Engine.get_singleton(SINGLETON).is_dlss_supported()


func set_dlss(enabled: bool, quality: int) -> void:
	if Engine.has_singleton(SINGLETON):
		var addon := Engine.get_singleton(SINGLETON)
		addon.set("dlss_enabled", enabled)
		addon.set("dlss_quality", quality)


func dlss_status() -> String: # "active (Quality, 1280x720 -> 1920x1080)", "off: ...", "unavailable: ..."
	if not Engine.has_singleton(SINGLETON):
		return "not available on this platform"
	return Engine.get_singleton(SINGLETON).get_dlss_status()


# Call on camera cuts the automatic detection cannot see (a short teleport, a level change behind a
# fade): the upscaler starts over instead of blending the old view into the new one.
func on_camera_cut() -> void:
	if Engine.has_singleton(SINGLETON):
		Engine.get_singleton(SINGLETON).reset_dlss_history()
```

**Windows-only projects** may use the global name and the typed constants directly. Such a script
is shorter but **fails to parse** wherever the extension is not loaded, so never use this style in a
project that also exports to other platforms (the same test checks both behaviours):

<!-- windows-only-example -->
```gdscript
extends Node


func _ready() -> void:
	LatencyUpscaling.reflex_mode = LatencyUpscaling.REFLEX_MODE_ON_BOOST
	LatencyUpscaling.reflex_fps_limit = 144.0
	print(LatencyUpscaling.get_reflex_status())
```

| Member | Description |
|---|---|
| `reflex_mode` (`REFLEX_MODE_OFF`, `_ON`, `_ON_BOOST`) | Reflex Low Latency mode |
| `reflex_fps_limit: float` | frame-rate limit applied by the Reflex sleep (0 = none) |
| `overlay_visible: bool` | the latency overlay |
| `is_reflex_supported() -> bool` | the device has `VK_NV_low_latency2` enabled |
| `is_reflex_active() -> bool` | the window swapchain runs in latency mode |
| `get_reflex_status() -> String` | "active", "active (markers only, sleep stopped: ...)" or "unavailable: ..." |
| `get_latency_stats() -> Dictionary` | averages over the last 64 frame reports (see below) |
| `get_latency_reports() -> Array` | the raw per-frame reports (microsecond timestamps) |
| `trigger_flash()` | send one TRIGGER_FLASH marker (for input other than the left mouse button) |
| `dlss_enabled: bool` | DLSS on / off at run time (off: Godot renders as configured) |
| `dlss_quality` (`DLSS_QUALITY_AUTO`, `_DLAA`, `_QUALITY`, `_BALANCED`, `_PERFORMANCE`, `_ULTRA_PERFORMANCE`) | DLSS mode; the render size follows it |
| `dlss_preset` (`DLSS_PRESET_DEFAULT`, `_J`, `_K`, `_L`, `_M`) | DLSS model preset |
| `is_dlss_supported() -> bool` | DLSS runs on this GPU with the installed runtime |
| `is_dlss_active() -> bool` | DLSS is upscaling the main viewport now |
| `get_dlss_status() -> String` | "active (mode, render -> output)", "off: ..." or "unavailable: ..." with the reason |
| `reset_dlss_history()` | throw the upscaler's history away on the next frame (a camera cut the detection cannot see) |
| `get_status() -> Dictionary` | everything the addon knows: hook, Vulkan device, Reflex, upscaling and DLSS, settings and their sources |
| `get_version() -> String` | addon version |

`get_latency_stats()` keys: `frames`, `sim_start_to_gpu_end_ms` (NVIDIA's "total game-to-render
latency"), `input_sample_to_gpu_end_ms`, `render_submit_start_to_gpu_end_ms`, `simulation_ms`,
`render_submit_ms`, `present_ms`, `driver_ms`, `os_render_queue_ms`, `gpu_render_ms`,
`frame_interval_ms`, `gpu_frame_interval_ms`, `reflex_sleep_ms`, `mode`, `fps_limit`.

**What the addon does automatically:** installs its Vulkan layer before Godot creates the device,
enables `VK_NV_low_latency2` where supported, puts the window swapchain in latency mode, sleeps
before input sampling after every presented frame, sends the latency markers, stamps present IDs, and falls back
cleanly everywhere else. With the NVIDIA runtime installed and an RTX GPU it also upscales the main
viewport with DLSS: it puts the viewport in Godot's FSR2 mode at the size DLSS asks for, takes the
upscaling step over and has DLSS evaluate inside Godot's own frame, and gives the viewport back
when DLSS is switched off. It resets DLSS's history on camera cuts (a switch to another camera, a
turn past `dlss/cut_angle_degrees` or a jump past `dlss/cut_distance` in one frame) and gives the
sky and background the camera's motion, which Godot only computes for its own FSR2. **What it cannot do by itself:** switch Reflex or DLSS in a game's own
settings menu (call `reflex_mode`, `dlss_quality`), fetch NVIDIA's runtime without your acceptance
of NVIDIA's licences, upscale sub-viewports or XR (multiview), and activate inside an editor it was
added to while running (restart).

Reflex lowers latency when the GPU is the bottleneck. With VSync on and no variable refresh rate,
or when the CPU is the bottleneck, there is little to gain (NVIDIA's own support matrix).

### Demo and tests

`demo/demo_main.tscn` (the project's main scene) has a GPU-load slider, the Reflex options and the
overlay. Hold the right mouse button to look around.

```
python tests/run_all.py                # all suites (headed runs open windows), about 25 minutes
python tests/run_all.py --only reflex  # suites whose name contains a word
python tests/run_package.py            # build the release zip, install it into a fresh project
python tests/run_portable.py           # README examples without the extension, filtered exports
python tests/run_export.py             # settings reach an exported game without the plugin
python tests/run_benchmark.py          # latency Off / On / Boost and hook overhead (idle GPU)
python tests/run_mutations.py          # mutation checks: the tests catch re-introduced bugs
python tests/test_fetch_nvidia_sdk.py  # the NVIDIA runtime fetch tool (synthetic archives)
```

DLSS itself needs an RTX GPU; its tests run on one (below, or locally on an RTX machine):

```
python tools/rtx_remote.py session dlss_tests --window-ok -- --script res://tests/dlss_tests.gd
python tools/rtx_remote.py session dlss_bench --window-ok -- --script res://tests/dlss_bench.gd -- --size=1920x1080
python tools/rtx_remote.py deploy --validation-layer      # once: copies the Khronos layer there
python tools/rtx_remote.py session validation_dlss --window-ok --validation --env GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=2 -- --verbose --script res://tests/upscaler_run.gd -- --frames=240
```

Tests that need an RTX GPU run on a remote Windows PC through `tools/rtx_remote.py` (SSH with a
key; configuration in the git-ignored `tools/.rtx_remote.json`): it copies Godot, the project and
the tools into one folder there, runs Godot headless over SSH or inside the user's desktop session
through a one-shot scheduled task, collects logs and in-engine captures, and cleans up. It refuses
any run that would open a window unless `--window-ok` is given, which is reserved for runs the
PC's owner has agreed to; such runs are short (at most 120 s), start only when a preflight finds the
PC idle (no GPU load, no fullscreen window or game in front), open a normal window without taking
the focus, and are logged in `docs/RTX_WINDOWED_RUNS.md`. `python tools/rtx_remote.py selftest`
checks the whole chain without a window; `build/tools/vk_probe.exe` (`build.cmd vk_probe`) reports
what a GPU's Vulkan driver offers for Reflex, DLSS and Frame Generation, and
`build/tools/sl_probe.exe` (`build.cmd sl_probe`) what Streamline says about DLSS on it (support,
requirements, optimal render sizes), both without a window. `session --validation` runs Godot with
the validation layer on that PC without touching its settings: UAC is off there, so every process
is elevated and the Vulkan loader would ignore `VK_ADD_LAYER_PATH`; `build/tools/medium_launcher.exe`
(`build.cmd medium_launcher`) starts Godot with a copy of its token lowered to medium integrity.

`run_all.py` reports skipped checks as SKIP, never as PASS, and fails on any skip that is not
explicitly allowed (only three are, each with its reason: Godot 4.7.2's own D3D12 crash here, its
crash with a second native window under the separate render thread, and NVIDIA's real archive,
which the tests never download: with a copy in `build/cache/nvidia/` it is installed against the
lock too).

Engine path: `GODOT_BIN` or one line in `tools/.godot_path` (use the `_console.exe`). Vulkan SDK for
the validation suite: `VULKAN_SDK` or `tools/.vulkan_sdk_path`. In an elevated shell the Vulkan
loader ignores `VK_ADD_LAYER_PATH`, so `tools/godot.py --medium-integrity` starts Godot with a
medium-integrity token for that suite.

## Reflex results

Reference machine: GTX 1080 (Pascal), driver 582.28, Windows 10 19045, HAGS on, 1280x720
windowed, VSync off. `python tests/run_benchmark.py`, 2026-09-24. The GPU was idle before and
after every run (nvidia-smi); the CPU was 40-55 % busy with other programs during the runs, which
mostly affects the light-scene numbers.

**GPU-bound demo** (full-screen shader load 6000, about 80 fps), 600 frames per mode, two rounds
interleaved in one process. Latency = simulation start to GPU render end per frame, from
`vkGetLatencyTimingsNV` (NVIDIA's "total game-to-render latency"). Mean / median / p95:

| Mode | Frame interval, round 1 (ms) | Latency, round 1 (ms) | Latency, round 2 (ms) | Reflex sleep (ms) |
|---|---|---|---|---|
| Off | 12.49 / 12.46 / 13.03 | 50.07 / 50.03 / 51.11 | 50.91 / 50.92 / 51.83 | 0.04 |
| On | 13.01 / 12.67 / 14.70 | 40.81 / 40.59 / 43.25 | 41.30 / 41.06 / 43.78 | 10.1 |
| On + Boost | 12.70 / 12.61 / 13.46 | 40.85 / 40.76 / 42.25 | 43.61 / 43.82 / 45.34 | 10.1 |

The sleep column was measured before 0.1.0's fix of the sleep timer and counts only the
semaphore wait after `vkLatencySleepNV`; the addon now also counts the time the call itself
blocks (under a frame-rate cap this driver sleeps inside the call, which the old timer reported
as ~0 ms). Latency and frame intervals are unaffected.

Reflex On cuts the latency by about 9 ms (18 %, from about 4.0 to 3.2 frames) for about 7 % more
frame time; Boost is the same within noise on this GPU. The ideal of about one frame is not reached
on Pascal. The per-stage timeline from `vkGetLatencyTimingsNV` (printed by `run_benchmark.py`) shows
where the time goes with Reflex On (mean / median / p95 ms, windowed):

| Stage | ms | | Stage | ms |
|---|---|---|---|---|
| simulation | 0.5 / 0 / 1 | | present call | 0.2 / 0 / 1 |
| render submit | 0.7 / 1 / 1 | | **present_end -> gpu_start** | **27.0 / 26.7 / 29.8** |
| submit -> present | 0.0 | | gpu render | 12.5 / 12.6 / 12.9 |

The dominant interval is **present_end -> gpu_start (about 27 ms)** — the wait between the present
call finishing and the GPU starting the next frame — which the Reflex sleep does not remove.

**The cause is not established.** Two candidates were considered: (a) windowed (DWM-composited)
presentation, and (b) Pascal / the render graph's wait on `COLOR_ATTACHMENT_OUTPUT` before each
frame's work (`rendering_device_driver_vulkan.cpp:3204`). The benchmark repeats the run in
**exclusive fullscreen** to probe (a): the gap is **not removed by exclusive fullscreen** — it is
about **30 ms** there (overall latency about 46 ms). That run was at the monitor's resolution, not
1280x720, so the two numbers are not directly comparable and (a) is weakened, not excluded; (b)
remains the leading candidate. NVIDIA's own Streamline 2.14 no longer offers Vulkan Reflex before Turing; RTX GPUs
are expected to do better but were not available for testing.

> An earlier version of this note blamed Godot's frame queue and cited `frame_queue_size = 1` as
> evidence. That was wrong: Godot clamps the queue depth to at least two frames (`MAX(2U, ...)` in
> `rendering_device.cpp`), so setting it to 1 has no effect and the "measures the same" result
> proves nothing.

**Overhead of the addon** (light scene, about 900 fps), 1500 frames per run, frame interval mean /
median / p95 in ms, two rounds:

| Variant | Round 1 | Round 2 |
|---|---|---|
| No addon (`GODOT_LATENCY_UPSCALING_DISABLE=1`) | 1.40 / 1.08 / 3.24 | 1.49 / 1.20 / 3.29 |
| Layer only (Reflex off the device) | 1.43 / 1.12 / 3.28 | 1.39 / 1.07 / 3.21 |
| Reflex Off (markers and sleep calls) | 1.70 / 1.35 / 3.56 | 1.48 / 1.13 / 3.37 |
| Reflex On | 1.55 / 1.30 / 3.35 | 1.46 / 1.12 / 3.27 |

The layer alone is below the run-to-run noise (about 0.1 ms). The Reflex calls cost up to about
0.2 ms per frame in these runs, within the noise of a machine that was not otherwise idle.

## DLSS results

Test machine: GeForce RTX 5080, driver 591.86, Windows 10 19045, DLSS 310.9.1 through Streamline
2.14.1, a normal window, VSync off, the GPU idle before each run (0 % load, checked by
`tools/rtx_remote.py preflight`). 2026-09-25.

**Image quality** (`tests/dlss_tests.gd`, the procedural still scene of the tests at 1280x720; PSNR
in dB against a 2x supersampled render of the same view, higher is closer; 64 frames to converge):

| Configuration | Render size | PSNR (dB) |
|---|---|---|
| DLAA | 1280x720 | 33.64 |
| DLSS Quality | 853x480 | 32.08 |
| Godot's FSR2 at 1.0 (its temporal AA) | 1280x720 | 31.31 |
| Godot's FSR2 at the DLSS Quality size | 853x480 | 30.67 |
| Native, no anti-aliasing | 1280x720 | 29.47 |
| Bilinear upscale of the DLSS Quality size | 853x480 | 24.69 |
| DLSS Quality, jitter ignored / X flipped / Y flipped | 853x480 | 25.99 / 29.18 / 22.48 |
| DLSS Quality under camera motion, vectors as handed over / flipped | 853x480 | 29.84 / 24.42 |

The last two rows are the convention checks: with the jitter and motion vectors exactly as the
addon hands them over DLSS scores best; any deliberate change scores lower. All five modes render at
the size DLSS asks for, all presets evaluate, Auto is off below 1080p, and every upscaled frame is
evaluated by DLSS. PSNR is not a perceptual measure; the captures are written next to the log for a look.

**Camera cuts and the sky** (same scene; "bad" pixels: pixels of the plain background, away from
any edge, that differ from a supersampled still of the same view by more than 20 of 255):

| Check | DLSS Quality | DLAA | FSR2 at the DLSS Quality size | Control |
|---|---|---|---|---|
| 150 degree cut, first and second frame of the new view | 0 / 0 of 372 850 | 0 / 0 | | 1 507 (no reset, no sky motion: the state before the fix) |
| History reset on the first frame of the new view | yes | yes | | |
| Turn at 1.2 degrees per frame over thin bars against a plain background | 0 of 304 777 | 0 | 0 | 12 (DLSS without the sky's motion) |

26 of 26 checks in `tests/dlss_tests.gd`, 2026-09-25; the same run under the validation layer adds
no message from the addon (see `docs/ARCHITECTURE.md` 7.5 for the messages NVIDIA's runtime itself
causes when a DLSS mode changes).

**GPU time** (`tests/dlss_bench.gd`, the demo scene at 1920x1080, median over 200 frames of the main
viewport's GPU time, which includes the upscaler; the load shader runs at the render resolution):

| Configuration | Render size | Light scene (ms) | With load 1500 (ms) |
|---|---|---|---|
| Native, no AA | 1920x1080 | 0.20 | 1.95 |
| FSR2 at 1.0 | 1920x1080 | 0.49 | 2.29 |
| FSR2 Quality | 1280x720 | 0.37 | 1.17 |
| DLAA | 1920x1080 | 0.73 | 2.48 |
| DLSS Quality | 1280x720 | 0.66 | 1.44 |
| DLSS Balanced | 1114x626 | 0.65 | 1.24 |
| DLSS Performance | 960x540 | 0.64 | 1.10 |
| DLSS Ultra Performance | 640x360 | 0.59 | 0.80 |

In the light scene DLSS adds about 0.4 to 0.5 ms over native rendering at 1080p output on this GPU
(with the presets NVIDIA uses by default); it pays off once the scene's own cost at full resolution
is higher than that, as with the load: every DLSS mode is then faster than native (Quality by about
25 %), and DLAA costs about 0.5 ms more than native for its anti-aliasing.

## Project structure

| Path | Contents |
|---|---|
| `addons/latency_upscaling/` | the addon: `.gdextension`, `bin/` (the built debug and release DLLs; `bin/nvidia/` the fetched NVIDIA runtime, git-ignored), editor plugin, NVIDIA runtime dialog, export plugin, `nvidia_runtime.json` / `.lock.json`, latency overlay, licences |
| `src/` | C++ sources: `hook/` (loader hook), `vk/` (dispatch layer, marker interception), `reflex/`, `upscale/` (quality modes, camera cuts, native recording), `dlss/` (DLSS feature, Streamline, recorder, project ID), `godot/` (singleton, bootstrap, FSR2-slot upscaler, sky motion), `core/` |
| `demo/` | demo scene (the repository root is the Godot project hosting demo and tests) |
| `tests/` | GDScript integration tests (Reflex, upscaler, DLSS, NVIDIA runtime), Python runners (`run_all.py`, `run_mutations.py`, `run_package.py`, `run_portable.py`, `run_export.py`, `run_benchmark.py`, `test_fetch_nvidia_sdk.py`), `unit/` C++ unit tests |
| `tools/` | `build.py`, `godot.py` (engine runner), `package.py` (release zip), `fetch_nvidia_sdk.py` (the NVIDIA runtime), `rtx_remote.py` (tests on a remote RTX PC), `vk_probe/`, `sl_probe/` (window-less probes), `medium_launcher/` (validation runs where UAC is off) |
| `stub/` | experimental cross-platform no-op library, **not shipped** (see `stub/README.md`) |
| `thirdparty/` | `godot-cpp` 10.0.0 and `Vulkan-Headers` v1.4.357 (git submodules), `streamline/` (Streamline 2.14.1 public headers, MIT) |
| `docs/ARCHITECTURE.md` | how it works, marker placement, safety net, DLSS (section 7) |
| `docs/RTX_WINDOWED_RUNS.md` | every test run that opened a window on the remote RTX PC |

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `get_reflex_status()` says "restart the editor to activate it" | The addon was added while the editor was running. Restart the editor. Games started from it already work |
| "Godot x.y.z is not supported" | This build targets Godot 4.7.x only |
| "... is not an NVIDIA GPU" / "does not expose VK_NV_low_latency2" | Other GPU vendor or an old driver: update the NVIDIA driver (551.70+) |
| "the Vulkan loader was never requested (rendering driver 'd3d12' ...)" | The project uses D3D12 or the Compatibility renderer. Reflex needs the Vulkan driver (`rendering/rendering_device/driver.windows = "vulkan"`) |
| "inactive in the editor" | By design: the editor only passes Vulkan calls through. Set `latency_upscaling/general/enable_in_editor` to change that |
| "markers only, sleep stopped: ..." | The driver rejected or did not answer the Reflex sleep; latency markers still work. After timeouts the sleep resumes with the next window swapchain (resize); after an error it stays off. Please report the driver version |
| No latency gain | Reflex only helps when the GPU is the bottleneck; with VSync on and no VRR there is little to gain |
| `get_dlss_status()`: "unavailable: the NVIDIA DLSS runtime is not installed" | Fetch it (see [DLSS: the NVIDIA runtime](#dlss-the-nvidia-runtime)), then restart the game. In an export: `nvidia/` must sit beside the executable (export with the editor plugin enabled) |
| "unavailable: ... does not expose VK_NVX_binary_import ..." / "does not support this GPU" | Not an RTX GPU (DLSS needs GeForce RTX 20 series or newer) or an old driver: update the NVIDIA driver |
| "unavailable: ... older than 512.15" | Update the NVIDIA driver |
| "unavailable: sl.interposer.dll ... does not carry NVIDIA's signature" (or in `get_status()["upscaling"]["dlss"]["runtime_reason"]`) | The runtime files were modified or are not NVIDIA's: fetch them again |
| "unavailable: disabled ... at startup" | `dlss/enabled` was off when the game started; the runtime is only loaded when DLSS is on at startup |
| "off: Auto: output below 1920x1080" | NVIDIA recommends DLSS off below 1080p; pick a mode explicitly (`dlss_quality`) to use it anyway |
| Streaks of the previous view for a frame after a teleport or a scene change | The automatic detection only sees camera switches and turns / moves past `dlss/cut_angle_degrees` / `dlss/cut_distance` in one frame: call `reset_dlss_history()` on that frame, or lower the thresholds |
| The log shows `Hook sl.common:Vulkan:CmdBindPipeline` (also `CmdBindDescriptorSets`, `BeginCommandBuffer`) `is NOT supported, plugin will not function properly` | Expected with Streamline 2.14.1 on Vulkan, harmless: `sl.common` asks for these command-state hooks, Streamline's Vulkan plugin manager does not provide them, and the addon does not use that tracking (it re-binds Godot's state itself). See `docs/ARCHITECTURE.md` 7.3 |
| The export log says "the Latency & Upscaling editor plugin is disabled, so this Windows export does not carry the NVIDIA DLSS runtime" | Enable the plugin (Project Settings > Plugins) and export again; its export plugin puts the runtime into `nvidia/` |
| Anything else | `print(JSON.stringify(Engine.get_singleton("LatencyUpscaling").get_status(), "  "))` (or the Tools menu entry) shows every decision the addon made. `GODOT_LATENCY_UPSCALING_DISABLE=1` turns it off without removing it |
| Anti-cheat software complains | The addon patches `godot.exe`'s import table in memory (see `docs/ARCHITECTURE.md`). Test with your anti-cheat before shipping |

## Version

0.1.0 released (Phase 1: hook layer and Reflex); Phase 2 (DLSS Super Resolution / DLAA) is on the
main branch, unreleased. See `CHANGELOG.md`. Semantic versioning.

## NVIDIA technologies, trademarks and attribution

- NVIDIA, GeForce, RTX, DLSS and NVIDIA Reflex are trademarks and/or registered trademarks of NVIDIA
  Corporation. This project is not affiliated with, sponsored by or endorsed by NVIDIA. Its name
  (godot-latency-upscaling, "Latency & Upscaling") deliberately contains no NVIDIA mark; NVIDIA's
  names appear only where they describe the vendor technology the addon works with.
- Reflex contains no NVIDIA code or binary: it runs through the Khronos-registered Vulkan
  extension `VK_NV_low_latency2` implemented by the NVIDIA driver.
- DLSS uses NVIDIA's Streamline and DLSS binaries, which are **not** in this repository or its
  release zip: the developer fetches them after accepting NVIDIA's licences, and exports carry them
  together with NVIDIA's licence files. The DLSS library is licensed under the NVIDIA RTX SDKs
  licence. A game that ships it must notify NVIDIA before a commercial release
  (https://developer.nvidia.com/sw-notification), attribute the DLSS SDK and show the NVIDIA marks
  on its splash screen, in its about box and in its credits as the licence requires, and have each
  use of the marks approved by NVIDIA. The addon cannot do that for you. This repository contains
  Streamline's public headers (MIT). See `THIRD_PARTY_NOTICES.md`.
- UI wording follows NVIDIA's RTX UI guidelines as far as they are known: "NVIDIA Reflex Low
  Latency" with Off / On / On + Boost; "NVIDIA DLSS" with the mode names (DLAA, Quality, Balanced,
  Performance, Ultra Performance, Auto); controls disabled when unavailable.

## AI-usage disclosure

This project is developed with substantial help from an AI coding assistant (Anthropic's Claude,
through Claude Code): research, design, code, tests and documentation were drafted by the
assistant and reviewed, tested and accepted by the maintainer. Measurements in this repository
come from real runs on the stated hardware, not from the assistant.

## License

MIT for the code of this repository (`LICENSE`). Third-party components and NVIDIA terms:
`THIRD_PARTY_NOTICES.md`.
