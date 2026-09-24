# Changelog

All notable changes. Versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased] - Phase 2: DLSS Super Resolution / DLAA

### Added

- DLSS Super Resolution and DLAA through NVIDIA Streamline 2.14.1 (DLSS 310.9.1 on the test
  machine), on any Godot 4.7 Vulkan project without engine changes: quality modes DLAA, Quality,
  Balanced, Performance, Ultra Performance and Auto (NVIDIA's table by output size), presets
  Default / J / K / L / M, at the render size DLSS asks for. Tested on an RTX 5080 (driver 591.86):
  image tests against a supersampled truth, FSR2 and native rendering, all modes and presets, and
  a GPU-time benchmark (README, "DLSS results").
- The FSR2 slot: Godot's own FSR2 mode computes render size, jitter, mip bias and TAA-off; its
  render buffers are then re-configured to "no upscaler" and the upscaler writes the target-size
  `color_upscaled` texture tonemapping reads.
- Marker interception: the upscaler is recorded into Godot's command buffer where Godot's render
  graph put a marker compute dispatch, with Godot's state bound again afterwards (the layer hooks
  six `vkCmd*` / command-buffer functions, only in runs that need it).
- Streamline runs as an interposer in front of the Vulkan loader, loaded only when a GPU passes
  the addon's checks and Streamline's, after its NVIDIA signature is verified; no OTA, no log files.
- API: `dlss_enabled`, `dlss_quality`, `dlss_preset`, `is_dlss_supported()`, `is_dlss_active()`,
  `get_dlss_status()` ("active (...)", "off: ...", "unavailable: <reason>"); settings
  `latency_upscaling/dlss/enabled`, `dlss/quality`, `dlss/preset`; the status reports the DLSS
  runtime, evaluated frames and failures.
- The NVIDIA runtime is fetched by the developer, never shipped with the addon: the editor plugin
  shows NVIDIA's licences and downloads only after the developer accepts them
  (`tools/fetch_nvidia_sdk.py` does the same from the command line). The release archive and every
  file taken from it are pinned by SHA-256 (`nvidia_runtime.lock.json`). Windows exports carry the
  four DLLs DLSS Super Resolution needs and NVIDIA's licence files in `nvidia/` beside the game.
- The layer: queue requests and more feature bits (buffer device address, private data) for
  features; an optional loader override (Streamline's interposer).
- Tools: `tools/rtx_remote.py` (tests on a remote RTX PC: deploy, headless runs over SSH, short
  windowed runs in the user's session after a preflight check, logs, cleanup), `tools/sl_probe`
  (Streamline's requirements and DLSS support without a window), `vk_probe`.
- Tests: reference and native-recording image tests of the FSR2 slot (jitter against Godot's UBO,
  conventions, velocity, motion), DLSS image tests and benchmark on the RTX GPU, validation-layer
  runs of the native recording and of the Streamline path, the runtime fetch (synthetic archives,
  the real archive against the lock), exports, mutations for the new code.

### Changed

- The DLSS feature is registered in every game run with the hook (not in the editor); without the
  runtime or a suitable GPU it changes nothing and reports why.

## [0.1.0] - 2026-09-24

Phase 0 (repository and build) and Phase 1 (Vulkan hook layer and NVIDIA Reflex).

### Added

- In-process Vulkan dispatch layer, installed at GDExtension CORE initialisation by patching
  `godot.exe`'s `LoadLibraryA/W/ExA/ExW` imports: Godot's `vulkan-1.dll` request returns the
  addon's module, whose exported `vkGetInstanceProcAddr` passes everything through except
  instance / device creation and the swapchain, submit and present calls Reflex needs.
- Device creation patch: adds extensions and feature bits without rewriting the host's
  structures; retries unpatched if the driver rejects it.
- NVIDIA Reflex through `VK_NV_low_latency2`: Off / On / On + Boost, frame-rate limit, sleep
  before input sampling, all latency markers, present IDs on presents and submissions, latency
  reports through `vkGetLatencyTimingsNV`, flash-indicator marker on left clicks.
- `LatencyUpscaling` engine singleton (auto-registered): Reflex API, status with reasons, latency
  statistics and raw reports, optional latency overlay.
- Settings: built-in defaults, overridden by Project Settings (`latency_upscaling/...`, which are
  always exported and read at CORE init, so they reach exported games with or without the editor
  plugin) and then by the `GODOT_LATENCY_UPSCALING_SETTINGS` environment variable;
  `GODOT_LATENCY_UPSCALING_DISABLE=1` kill switch.
- Works with Godot's default and separate render-thread models; one Reflex sleep per presented
  frame; a sleep that times out is suspended until the next window swapchain.
- Safety: refuses Godot versions other than 4.7.x; stays dormant when added to a running editor
  (restart to activate); passes through on other GPUs, drivers and renderers with a readable
  reason; the DLL pins itself so it is never unloaded under Godot's function pointers.
- Platforms: Windows x86_64 only. Exports to other platforms exclude the addon with the export
  preset filter `addons/latency_upscaling/*` (documented; without it Godot prints its harmless "No
  GDExtension library found" messages). README examples reach the singleton through `Engine`, so
  scripts written from them parse and run on every platform. A cross-platform no-op stub is kept in
  `stub/` as experimental and is not shipped.
- Editor plugin: status report and a Tools menu entry.
- Build: `build.cmd` / `tools/build.py` (venv with pinned SCons 4.11.1, MSVC via developer prompt
  or dev-shell script), godot-cpp 10.0.0 and Vulkan-Headers v1.4.357 as submodules; a packaging
  script (`tools/package.py`) that builds the release zip with both the debug DLL (editor, games
  run from it, debug exports) and the release DLL (release exports). Only release zips are
  installable; GitHub's "Download ZIP" has no DLLs.
- Demo scene with a GPU-load control; tests: C++ unit tests (including fake-driver tests of the
  Reflex device and proxy tests against the real loader), headed Reflex integration tests (both
  render-thread models, editor with Reflex, a second native window), pixel-identical pass-through
  test, an export test, forced-fault fallback tests (bogus device extension, failing swapchain,
  loader without entry points), editor and late-load tests, Vulkan validation-layer run, a release
  zip installed into a fresh project (editor, game from the editor, debug and release export), the
  README examples run without the extension and in filtered Windows and Linux exports, benchmark,
  and mutation checks including the reviewers' mutations. Skipped checks are reported separately and
  fail the run unless explicitly allowed.
- The reported Reflex sleep time includes the time `vkLatencySleepNV` itself blocks (under a
  frame-rate cap the driver sleeps inside the call).
- Phase 2 groundwork: `UpscalerProvider` interface, CPU reproduction of Godot's camera jitter,
  `tools/fetch_nvidia_sdk.py` dry-run stub for the NVIDIA binaries (nothing is downloaded).

### Measured (GTX 1080, driver 582.28)

- GPU-bound, VSync off: Reflex On lowers game-to-render latency from about 50 ms to 41 ms
  (4.0 to 3.2 frames) for about 7 % more frame time; On + Boost the same within noise. The
  dominant remaining interval is ~27 ms between present-end and the next frame's GPU start; its
  cause is not established. The gap is not removed by exclusive fullscreen (measured at a different
  resolution, so not directly comparable). See README "Reflex results".
- Overhead of the layer below measurement noise; Reflex calls up to about 0.2 ms per frame.
