# godot-latency-upscaling

A Godot 4.7 addon (GDExtension) that brings NVIDIA technologies to any Vulkan project without
engine changes and without project setup. Phase 1, this version, delivers **NVIDIA Reflex**
(low-latency mode, frame-rate limiter, latency markers and measurements). DLSS Super Resolution /
DLAA and Frame Generation are planned for the next phases on the same foundation: an in-process
Vulkan dispatch layer the addon installs in front of Godot at start-up.

> Status: **0.1.0, Phase 1 of 3.** Reflex works and is tested on a GTX 1080. DLSS is not
> implemented yet. The addon relies on Godot 4.7.x internals; other versions are refused cleanly.

## Features

| Feature | Status | Notes |
|---|---|---|
| NVIDIA Reflex Low Latency: Off / On / On + Boost | done | `VK_NV_low_latency2`, default On |
| Reflex frame-rate limiter | done | driver-paced, replaces `Engine.max_fps` |
| Latency markers (simulation, render submit, present, input, flash) | done | every frame, all modes |
| Latency measurements (`vkGetLatencyTimingsNV`) | done | `LatencyUpscaling.get_latency_stats()` |
| Reflex Flash Indicator marker on left clicks | done | the driver decides whether to draw it |
| Small latency overlay | done | optional, off by default |
| Clean fallback with a readable reason | done | wrong Godot version, other GPUs, other renderers, missing driver support |
| DLSS Super Resolution / DLAA | planned (Phase 2) | design in `docs/ARCHITECTURE.md` |
| DLSS Frame Generation / Multi Frame Generation | planned (Phase 3) | |
| DLSS Ray Reconstruction | not planned | Godot 4.7 has no ray-traced signals to denoise |
| D3D12 renderer, Compatibility renderer, Linux, macOS | not supported | the addon stays idle and says so |

## Requirements

| Feature | GPU | Driver | Other |
|---|---|---|---|
| Reflex (this version) | NVIDIA GPU whose driver exposes `VK_NV_low_latency2`: NVIDIA lists Reflex for GeForce 900 series and newer; tested on GTX 1080 (Pascal) | `VK_NV_low_latency2` first shipped in 551.70; tested with 582.28 | Windows 10/11 x64, Godot 4.7.x, Vulkan renderer (Forward+ or Mobile) |
| DLSS Super Resolution / DLAA (planned) | GeForce RTX 20 series (Turing) or newer | 512.15 or newer (Streamline) | Windows 10 1709+, Vulkan 1.1+ |
| DLSS Frame Generation (planned) | GeForce RTX 40 series (Ada) or newer | 520 or newer (527.64+ for native Vulkan optical flow) | Windows 10 20H1+, hardware-accelerated GPU scheduling on |
| Multi Frame Generation (planned) | GeForce RTX 50 series | | as Frame Generation |

Other GPUs, older drivers and unsupported configurations do not break anything: Godot runs
exactly as without the addon and `LatencyUpscaling.get_reflex_status()` says why Reflex is unavailable.

To build from source: Python 3.10+, MSVC (Visual Studio 2022 Build Tools or a portable MSVC
14.4x with the Windows SDK), Git. SCons is installed automatically. For the validation-layer test:
the LunarG Vulkan SDK.

## Installation

1. Copy `addons/latency_upscaling/` into your project, keeping the path, with the built DLLs in
   `addons/latency_upscaling/bin/` (from a release, or build them: see below).
2. Open the project in Godot 4.7. The GDExtension loads by itself. If you copied the folder while
   the editor was running, restart the editor once (games you start from it work immediately).
3. Optional: Project > Project Settings > Plugins > enable **Latency & Upscaling**. The plugin adds a
   status message and Project > Tools > "Latency & Upscaling: Print Status". The GDExtension and all
   settings work without enabling the plugin.

Nothing else is needed: no autoload, no node in your scenes, no change to `project.godot`.

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

Other environment variables: `GODOT_LATENCY_UPSCALING_VERBOSE=1` prints informational lines (also
enabled by `--verbose`). Debug builds read two test knobs used by the fallback tests:
`GODOT_LATENCY_UPSCALING_TEST_HIDE_EXTENSIONS` and `GODOT_LATENCY_UPSCALING_TEST_GODOT_VERSION`.

## Usage

The singleton `LatencyUpscaling` is registered automatically. Reflex is On by default.

```gdscript
# A settings menu. NVIDIA's UI guidelines: name it "NVIDIA Reflex Low Latency" with the options
# Off / On / On + Boost, and disable the control when Reflex is unavailable.
reflex_option.disabled = not LatencyUpscaling.is_reflex_supported()
LatencyUpscaling.reflex_mode = LatencyUpscaling.REFLEX_MODE_ON_BOOST
LatencyUpscaling.reflex_fps_limit = 144.0     # instead of Engine.max_fps
LatencyUpscaling.overlay_visible = true       # small latency overlay

print(LatencyUpscaling.get_reflex_status())   # "active" or "unavailable: <reason>"
var stats := LatencyUpscaling.get_latency_stats()
print(stats.sim_start_to_gpu_end_ms)    # simulation start to GPU render end, last 64 frames
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
| `get_status() -> Dictionary` | everything the addon knows: hook, Vulkan device, Reflex, settings and their sources |
| `get_version() -> String` | addon version |

`get_latency_stats()` keys: `frames`, `sim_start_to_gpu_end_ms` (NVIDIA's "total game-to-render
latency"), `input_sample_to_gpu_end_ms`, `render_submit_start_to_gpu_end_ms`, `simulation_ms`,
`render_submit_ms`, `present_ms`, `driver_ms`, `os_render_queue_ms`, `gpu_render_ms`,
`frame_interval_ms`, `gpu_frame_interval_ms`, `reflex_sleep_ms`, `mode`, `fps_limit`.

**What the addon does automatically:** installs its Vulkan layer before Godot creates the device,
enables `VK_NV_low_latency2` where supported, puts the window swapchain in latency mode, sleeps
before input sampling after every presented frame, sends the latency markers, stamps present IDs, and falls back
cleanly everywhere else. **What it cannot do by itself:** switch Reflex in a game's own settings
menu (call `reflex_mode`), and activate inside an editor it was added to while running (restart).

Reflex lowers latency when the GPU is the bottleneck. With VSync on and no variable refresh rate,
or when the CPU is the bottleneck, there is little to gain (NVIDIA's own support matrix).

### Demo and tests

`demo/demo_main.tscn` (the project's main scene) has a GPU-load slider, the Reflex options and the
overlay. Hold the right mouse button to look around.

```
python tests/run_all.py                # all suites (headed runs open windows), about 10 minutes
python tests/run_all.py --only reflex  # suites whose name contains a word
python tests/run_benchmark.py          # latency Off / On / Boost and hook overhead (idle GPU)
python tests/run_mutations.py          # mutation checks: the tests catch re-introduced bugs
```

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
frame's work (`rendering_device_driver_vulkan.cpp:3204`). The benchmark measures an
**exclusive-fullscreen** run to test (a): the gap does **not** shrink — it is about **30 ms** there
(overall latency about 46 ms, at the higher fullscreen resolution) — so leaving windowed composition
does not remove it and (a) is ruled out as the cause. That leaves the Pascal / render-graph
explanation (b). NVIDIA's own Streamline 2.14 no longer offers Vulkan Reflex before Turing; RTX GPUs
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

## Project structure

| Path | Contents |
|---|---|
| `addons/latency_upscaling/` | the addon: `.gdextension`, `bin/` (built DLLs and stubs), editor plugin, latency overlay, licences |
| `src/` | C++ sources: `hook/` (loader hook), `vk/` (dispatch layer), `reflex/`, `godot/` (singleton, bootstrap), `dlss/` (Phase 2 interfaces), `core/` |
| `demo/` | demo scene (the repository root is the Godot project hosting demo and tests) |
| `tests/` | GDScript integration tests, Python runners, `unit/` C++ unit tests |
| `tools/` | `build.py`, `godot.py` (engine runner), `fetch_nvidia_sdk.py` (Phase 2 stub) |
| `thirdparty/` | `godot-cpp` 10.0.0 and `Vulkan-Headers` v1.4.357 (git submodules) |
| `docs/ARCHITECTURE.md` | how it works, marker placement, safety net, Phase 2 design |

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
| Anything else | `print(JSON.stringify(LatencyUpscaling.get_status(), "  "))` (or the Tools menu entry) shows every decision the addon made. `GODOT_LATENCY_UPSCALING_DISABLE=1` turns it off without removing it |
| Anti-cheat software complains | The addon patches `godot.exe`'s import table in memory (see `docs/ARCHITECTURE.md`). Test with your anti-cheat before shipping |

## Version

0.1.0 (Phase 1: hook layer and Reflex). See `CHANGELOG.md`. Semantic versioning.

## NVIDIA technologies, trademarks and attribution

- NVIDIA, GeForce, RTX, DLSS and NVIDIA Reflex are trademarks and/or registered trademarks of NVIDIA
  Corporation. This project is not affiliated with, sponsored by or endorsed by NVIDIA; the names
  describe what it works with. Whether a project name containing "NVIDIA" needs NVIDIA's approval
  has not been clarified yet.
- Phase 1 contains no NVIDIA code or binary: Reflex runs through the Khronos-registered Vulkan
  extension `VK_NV_low_latency2` implemented by the NVIDIA driver.
- DLSS (Phase 2+) will use NVIDIA's DLSS / Streamline binaries, which are **not** in this repository.
  They are licensed under the NVIDIA RTX SDKs licence. A game that ships them must notify NVIDIA
  before a commercial release (https://developer.nvidia.com/sw-notification) and show NVIDIA
  attribution and marks as the licence requires. See `THIRD_PARTY_NOTICES.md`.
- UI wording follows NVIDIA's RTX UI guidelines as far as they are known: "NVIDIA Reflex Low
  Latency" with Off / On / On + Boost, controls disabled when unavailable.

## AI-usage disclosure

This project is developed with substantial help from an AI coding assistant (Anthropic's Claude,
through Claude Code): research, design, code, tests and documentation were drafted by the
assistant and reviewed, tested and accepted by the maintainer. Measurements in this repository
come from real runs on the stated hardware, not from the assistant.

## License

MIT for the code of this repository (`LICENSE`). Third-party components and NVIDIA terms:
`THIRD_PARTY_NOTICES.md`.
