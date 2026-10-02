# Changelog

All notable changes. Versions follow [Semantic Versioning](https://semver.org/).

## [0.3.0] - 2026-10-03

Phase 3: DLSS Frame Generation and Multi Frame Generation. Verified on an RTX 5080 (driver 591.86):
Frame Generation active up to 6x, driving Reflex through `sl.reflex` with DLSS Super Resolution
intact and the UI recomposed cleanly onto generated frames, with no crash or device loss. Frame
Generation is **gated on real FG hardware and off by default**, so on a non-FG GPU (the GTX 1080 dev
box, RTX 20/30, AMD, Intel) permitting it is byte-identical to Frame Generation off — a non-FG player
is never regressed. Not 1.0: a few low-severity validation-layer messages under FG remain
(`docs/ISSUES.md` #3) and the Reflex-latency question on the 5080 is open (#1). See
`docs/PHASE3_FG.md`.

### Frame Generation hardware-gated startup permit, UI-recomposition status, RTX 5080 verification

- The startup Frame Generation permit is now gated on the **real first-adapter capability**:
  `bootstrap_core()` resolves `resolve_fg_caps()` and `fg_permitted(fg/enabled, fg/mode,
  hardware_supported)` (pure, unit-tested in `src/core/settings.*`) **before** the Reflex backend,
  the Streamline load and the optical-flow queue remap read it. On any non-FG adapter the hardware
  term is false, so the permit is false and the Reflex / DLSS / pass-through path — and the Vulkan
  device — are byte-identical to Frame Generation off. Permitting FG in `project.godot` therefore
  never regresses a non-FG player.
- The in-engine Frame Generation status surface gains the authoritative UI-recomposition signal (no
  desktop or swapchain grab needed to verify it): `is_frame_generation_recomposing()` and four
  booleans in `get_status()["upscaling"]["frame_generation"]` — `recomposition_engaged` (DLSS-G is
  recomposing a clean UI this frame), `ui_layers_provided`, `hudless_source_valid`, `ui_layer_valid`
  (which pinpoint why not) — measured from the real evaluate state. `get_frame_generation_status()`
  now ends with `UI recomposed` or `present-only`.
- **Verified on the RTX 5080** (driver 591.86): the device creates cleanly, Frame Generation is
  active (up to 6x), `sl.reflex` drives Reflex, DLSS Super Resolution is intact, UI recomposition
  engages, and there is no crash or device loss; byte-identical pass-through on the non-FG GTX 1080.
  The remaining Frame Generation validation-layer messages are tracked in `docs/ISSUES.md` #3 (low
  severity, independent of the HUD path).
- Tests: the `fg_permitted` gate (both settings terms required, plus the hardware term the API
  reports — RTX 40 supported, GTX 1080 / AMD not), beside the existing Frame Generation unit tests.
  Also widened the RTX run-log privacy test (`tests/test_rtx_remote.py`) to accept the fixed,
  PII-free "the preflight gave no usable answer" state, with the privacy assertions unchanged.

### Milestone H6 (plugin side) — clean UI on generated frames (HUD-less + UI recomposition)

- New engine API: `LatencyUpscaling.set_hudless_source(texture: RID)` (the 3D scene without the HUD,
  the root viewport) and `set_ui_layer(texture: RID)` (the HUD on its own transparent layer, colour +
  alpha); an empty RID clears either. The game passes RenderingServer texture RIDs; the addon resolves
  them to Vulkan images on the render thread (`game_evaluate_image`: `texture_get_rd_texture` ->
  `evaluate_image`, invalid if it does not resolve).
- When DLSS Frame Generation is driving **and** both resolved, `dlss::Streamline::evaluate()` tags
  `kBufferTypeHUDLessColor` and `kBufferTypeUIColorAndAlpha` alongside the four Super Resolution tags
  and sets `DLSSGOptions.enableUserInterfaceRecomposition = true`, so DLSS-G interpolates the HUD-less
  colour and recomposes the UI cleanly onto real and generated frames (fixes the present-only UI
  ghosting). The decision is the pure, unit-tested `dlss::fg_ui_recomposition(fg_driving,
  hud_less_valid, ui_valid)` — all three required.
- No regression: a game that provides neither image (or only one) keeps the present-only path (four
  tags, recomposition off), unchanged; Frame Generation off stays byte-identical. RTX-only to verify
  (the dev box cannot run FG); expected to resolve the UI-related present-only validation nits
  (`vkCmdDraw-09600` / `vkCmdCopyImage-06663` / `VkImageMemoryBarrier-01213`).
- The matching game-side HUD restructuring (render the HUD on a transparent SubViewport so the root
  viewport is HUD-less, then call the two setters) is **not included** — in crater-zombie-fps that is a
  large, RTX-only-verifiable change (the HUD is a CanvasLayer stack whose elements reach
  `get_viewport().get_camera_3d()` and whose menus route input through the root viewport).

### Fixed — Frame Generation device-lost from per-frame token mismatch (RTX 5080)

- Present-only FG crashed with `VK_ERROR_DEVICE_LOST`: each subsystem minted its own Streamline frame
  token (sl.reflex markers/sleep, the SR `slSetConstants`/`slEvaluateFeature`, the FG present), so SR
  set the "common" constants under one frame id and the FG present looked them up under another —
  Streamline logged `Unable to find 'common' constants for frame N` every frame, disabled FG, skipped
  the out-of-order present, and the pacer's semaphore wait timed out. Fix: a single
  `reflex::FrameTokenSource` (owned by `dlss::Streamline` when the FG plugins load) mints **one token
  per frame** — `advance()` at the Reflex sleep (frame start), `current()` for the rest — and it is
  threaded through the sl.reflex markers/sleep (via `SlReflexApi::token_source`), the SR `evaluate()`
  and the FG present. Targets Godot's default thread model; the separate render thread is not
  supported for FG. FG off and the Reflex-only / DLSS-SR-only paths keep their previous token
  behaviour (byte-identical). Tests: one token minted per frame, shared by the markers, the sleep and
  what the SR evaluate / FG use.

### Milestone H5 — drive present-only Frame Generation

- Frame Generation is now actually driven (present-only / Option A), guarded off by default. When the
  hardware supports it, `frame_generation` is On, a DLSS Super Resolution / DLAA upscaler is active,
  and VSync is off, `dlss::Streamline` calls `slDLSSGSetOptions(viewport, {eOn, numFramesToGenerate,
  eRetainResourcesWhenOff})` inside its SR `evaluate()` on the upscaled viewport — reusing the depth +
  motion vectors SR already tagged (the backbuffer is intercepted in interposer mode). The decision is
  the pure `dlss::fg_availability()`; `numFramesToGenerate` comes from `dlss::fg_num_frames_to_generate()`
  (2x->1, 3x->2, 4x->3, Auto->the runtime ceiling). `LatencyUpscaling` pushes the request each frame
  only when it changed, through `Upscaler::set_frame_generation()`.
- `is_frame_generation_active()` / `get_frame_generation_status()` now reflect the live
  `slDLSSGGetState`: `"active (Nx, WxH -> M presented)"` when generating; `"off: needs a DLSS or DLAA
  upscaler"` when FG is on but SR isn't; `"unavailable: Frame Generation needs VSync off"`;
  `"off: Frame Generation is off"`; `"unavailable: <reason>"` on non-RTX. `get_status()["upscaling"]
  ["frame_generation"]` gains `status`, `num_frames_to_generate`, `num_frames_presented`.
- Present-only: the UI ghosts on generated frames (no HUD-less tagging yet — H6). FG off is
  byte-identical (the per-frame request update early-outs when FG is off). Validation nits from the
  5080 FG-on run (vkCmdDraw-09600 / vkCmdCopyImage-06663 / VkImageMemoryBarrier-01213 /
  vkQueueSubmit2-03868) are Streamline-internal or inherent to present-only without HUD-less tagging,
  deferred to H6 (docs/PHASE3_FG.md).
- Tests: `fg_availability()` (the drive decision), `fg_num_frames_to_generate()` (the multiplier
  mapping), and the status categories.

### Fixed — Frame Generation device-creation crash (RTX 5080 bring-up)

- With Frame Generation on, the engine crashed at `vkCreateDevice`. Root cause (5080 diagnostic): a
  **queue collision** — Godot 4.7 blanket-creates one queue in every family, including the
  optical-flow family (one queue), and Streamline's interposer appends its own optical-flow queue
  there downstream → two entries, 2 > 1 → invalid create (VUID-...-queueFamilyIndex-02802 /
  VUID-...-pQueueCreateInfos-06755). The fix: in Frame Generation mode the addon removes Godot's
  optical-flow-family queue from the forwarded `pQueueCreateInfos` (`DeviceRequest::drop_optical_flow_queue`
  set by `Streamline::request_device`; the removal + the family lookup in `DeviceCreatePatch`), so the
  interposer's is the only one; and the dispatch layer remaps the host's `vkGetDeviceQueue` for that
  family to index 0 so Godot's held (unused) handle stays valid. A separate 01387 (the interposer's
  `VK_NV_low_latency2` for sl.reflex missing `VK_KHR_present_id`) is fixed by
  `ReflexFeature::request_device` enabling `VK_KHR_present_id` + its feature in Frame Generation mode
  (no `VK_NV_low_latency2`, no `Ll2Device`). `device_patch` keeps a defensive dedupe/cap pass. The
  Frame-Generation-off path is byte-identical. See `docs/ISSUES.md` #2.
- `get_max_frame_generation_multiplier()` now derives from `slDLSSGGetState().numFramesToGenerateMax`
  (`+1`, so 5 -> 6x) when Frame Generation is available at runtime, falling back to the architecture
  guess otherwise; the dev-box value (unavailable -> 1) is unchanged.
- Tests: the device-create assembly (optical-flow dependency present, no duplicate `(family, flags)`,
  `queueCount` capped) and the multiplier mapping / runtime override.

### Milestone H4 — Reflex through Streamline's sl.reflex (for Frame Generation)

- `reflex::ReflexDevice` is now an interface the Godot singleton drives, with two implementations:
  `Ll2Device` (VK_NV_low_latency2, unchanged, used whenever Frame Generation is off) and the new
  `reflex::SlReflexDevice` (`src/reflex/sl_reflex_device.*`), which drives Reflex through Streamline's
  sl.reflex / sl.pcl. `SlReflexDevice` reuses `reflex::FrameSequencer` for the six-marker ordering
  (each marker → `slPCLSetMarker`, INPUT_SAMPLE dropped as PCL has no equivalent), does one
  `slReflexSleep` per frame, maps the mode + FPS cap to `slReflexSetOptions`, reads the 64-frame
  statistics from `slReflexGetState`, and threads one Streamline frame token per frame through that
  frame's sleep and markers.
- When Frame Generation is permitted, bootstrap keeps VK_NV_low_latency2 out of the device
  (`ReflexFeature::request_device` declines) and the LL2 submit/present hooks off, so the present
  path falls straight through to Streamline's interposer — what Frame Generation needs. Streamline,
  once it has resolved sl.reflex / sl.pcl on the device (RTX only), hands the functions to
  `ReflexFeature::enable_streamline_reflex`, which creates the `SlReflexDevice`; it is released at
  shutdown. With Frame Generation off the whole path is inert and the LL2 behaviour is unchanged.
- Tests: `SlReflexDevice` against a fake Streamline Reflex/PCL API — mode mapping, one sleep per
  frame with the upcoming frame's token, the six markers in order (INPUT_SAMPLE dropped), the frame
  token threaded through each frame, and the statistics mapping; the existing `Ll2Device` fake-driver
  tests are unchanged.

### Milestone H3 — load the Streamline Frame Generation plugins, ship them with exports

- `dlss::streamline_features_to_load(fg_permitted)` (pure, unit-tested): with Frame Generation off,
  slInit loads exactly `kFeatureDLSS` (byte-identical to before Phase 3); with it permitted, also
  `kFeatureDLSS_G`, `kFeatureReflex` and `kFeaturePCL`. `fg_permitted` is threaded from bootstrap
  into `Streamline::load()`.
- `Streamline` resolves the Frame Generation plugins' functions (slDLSSGSetOptions / slDLSSGGetState,
  slReflex*, slPCL*) into its `Api` once the device exists (slGetFeatureFunction requires it), when
  Frame Generation is permitted. They are never called to drive rendering and the Reflex backend is
  not switched (a later milestone). If a plugin is missing or unsupported (e.g. forced on here on the
  GTX 1080) it logs, leaves Frame Generation not ready, and never fails DLSS or the non-FG path.
- Windows exports now carry the Frame Generation DLLs (`sl.dlss_g.dll`, `sl.reflex.dll`,
  `sl.pcl.dll`, `nvngx_dlssg.dll`): added to `nvidia_runtime.json` `export_files` (and a labelled
  `fg_files` set); they are already pinned in `nvidia_runtime.lock.json`. The licence manifest lists
  `nvngx_dlssg.dll` under the NVIDIA RTX SDKs licence (the sl.* DLLs are MIT, Streamline). They ride
  along but are only loaded at runtime when Frame Generation is enabled on a supported GPU.
- Tests: the feature-list gating (C++ unit test: fg off → only DLSS; fg on → the three added) and the
  export manifest (Python: `fg_files`/`export_files`/`files` carry the four FG DLLs, licence names).

### Milestone H2 — real capability detection

- `bootstrap.cpp::resolve_fg_caps()` fills `FgCaps` with the real GPU the plugin sees at start-up,
  from the `gpu_precheck` adapter probe (`dlss::check_adapters`, reused — the same temporary-instance
  enumeration DLSS uses, through the real Vulkan loader, reading the adapter Godot picks by default),
  the driver decoded with `dlss::nvidia_driver_version`, and the architecture from the new pure
  `dlss::classify_nvidia_architecture()` (RTX 50xx Blackwell, RTX 40xx / "Ada" names Ada, RTX
  30xx/20xx and every GTX pre-Ada, non-NVIDIA / unplaceable unknown → conservative). No NVIDIA
  runtime is loaded and the Reflex backend is untouched.
- The driver reason below 527.64 now notes that older drivers offer Frame Generation only through
  Direct3D interop (this Vulkan addon needs native Vulkan optical flow). The OS / HAGS requirement is
  deferred (`os_known` false), so it does not refuse a capable GPU yet.
- On the GTX 1080 dev box Frame Generation stays unavailable, now with the accurate reason
  `"NVIDIA GeForce GTX 1080 does not support Frame Generation (needs a GeForce RTX 40 series GPU or
  newer)"`.
- Unit tests: the architecture classifier for real adapter names (RTX 50/40/30/20, GTX, Ada/Blackwell
  workstation keywords, AMD/Intel, unplaceable), and `check_frame_generation()` with realistic cap
  inputs (Ada/Blackwell supported, Ampere/Turing/Pascal/AMD/Intel unavailable, old-driver interop
  note, headless no-Vulkan, OS known-bad vs. not-probed).

### Milestone H1 — settings, API surface, stubbed detection

### Added

- Settings: `latency_upscaling/fg/enabled` (permit at startup, off by default, restart), `fg/mode`
  (0 Off, 1 On) and `fg/multiplier` (0 Auto, 2 / 3 / 4 for Multi Frame Generation), resolved and
  validated in the pure settings module.
- API: the `frame_generation` and `mfg_multiplier` properties, `is_frame_generation_supported()`,
  `is_frame_generation_active()`, `get_frame_generation_status()` ("active (...)", "off: ...",
  "unavailable: <reason>", worded like `get_dlss_status()`) and `get_max_frame_generation_multiplier()`.
  `get_status()["upscaling"]["frame_generation"]` reports {supported, active, mode, multiplier,
  max_multiplier, reason}.
- `dlss::FgFeature` and the pure `dlss::check_frame_generation()` capability logic: refuses
  non-NVIDIA, pre-Ada, old-driver, no-Vulkan-device and unsupported-OS GPUs with a player-actionable
  reason. The capability is not probed in H1, so it is unavailable everywhere (on this GTX 1080:
  "needs a GeForce RTX 40 series GPU").
- Bootstrap resolves the Frame Generation settings and logs whether it is permitted, beside the DLSS
  block, without loading any runtime or switching the Reflex backend.
- `docs/PHASE3_FG.md`: the Phase 3 design, milestones (H1-H6 hardware-independent, R1-R6 RTX-only)
  and risks.
- Unit tests: the new `fg/*` settings (valid and invalid), and Frame Generation capability detection
  with injected fake caps (RTX 40 / 50 supported, GTX 1080 / non-NVIDIA / old driver / unsupported
  OS unavailable with the right reason).

## [0.2.0] - 2026-09-25

Phase 2: DLSS Super Resolution / DLAA.

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

- History resets on camera cuts: a switch to another camera, or a turn / move past
  `dlss/cut_angle_degrees` (60) / `dlss/cut_distance` (10 m) in one frame, resets DLSS (and the
  reference upscaler) on exactly that frame; `reset_dlss_history()` for cuts the detection cannot
  see; `upscaling.history_resets` counts resets by reason.
- Motion vectors for the sky and background: far-plane pixels get the camera's motion with Godot's
  own formula (what its FSR2 reconstructs), so DLSS no longer smears geometry edges into them.
- `latency_upscaling/dlss/project_id`: the project ID NVIDIA's runtime gets, by default derived
  from the project's name (it was one fixed ID for every project).
- The export warns when the editor plugin is disabled and the NVIDIA runtime therefore does not
  ship; the NVIDIA runtime dialog lets the developer pick the download folder (default next to
  the runtime, no longer the system drive) and shows the size first.
- Tools: `medium_launcher` (validation-layer runs where UAC is off), `rtx_remote.py deploy
  --validation-layer` / `session --validation`.
- Tests: cut thresholds from the settings end to end, cuts with Reflex off, the sky fill after a
  resize, the RTX harness's privacy rule (`tests/test_rtx_remote.py`).

### Changed

- The DLSS feature is registered in every game run with the hook (not in the editor); without the
  runtime or a suitable GPU it changes nothing and reports why.
- The licences shown for acceptance are the licence files of the pinned release (pinned to its
  commit and checked against the lock), not texts from GitHub's main branches;
  `licence_acceptance.json` records who accepted, when and how, and the licence files with their
  SHA-256. `fetch_nvidia_sdk.py --download` requires `--accepted-by`.
- The current camera is tracked before the frame renders, so a camera made current during
  processing is upscaled from its first frame (it showed one stale frame).
- `sl_probe` ends right after printing its result (Streamline's teardown blocked over SSH).
- The RTX harness's preflight reduces what it sees on the test PC to two yes/no answers (a
  fullscreen app in front, a known game running) plus the GPU load; no process, app or window name
  is returned, printed or logged.
- The acceptance record says which licence was accepted: the NVIDIA RTX SDKs licence
  (`nvngx_dlss.license.txt`); Streamline's `license.txt` is MIT and is only received.
- The upscaler's camera tracking has its own `frame_pre_draw` handler, independent of Reflex.
- Checks that need NVIDIA's runtime report SKIP without it (run_all, run_export), and
  run_mutations labels their mutations NEEDS RUNTIME.

### Fixed

- After a declined marker recording, Godot's compute state is bound again before the marker
  dispatch runs.
- DLSS's output (`color_upscaled`) is created with copy-to usage: DLSS clears it with
  `vkCmdClearColorImage` (validation error on the RTX 5080).
- A clean clone passes `run_all.py`: NVIDIA's real archive is an allowed skip.

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
