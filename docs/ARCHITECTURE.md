# Architecture

How the addon gets NVIDIA features into a stock Godot 4.7 without engine changes, and how it
stays out of the way when it cannot. Phase 1 (this version) implements the Vulkan hook layer and
NVIDIA Reflex; Phase 2 (DLSS Super Resolution / DLAA) and Phase 3 (Frame Generation) build on the
same layer. The research behind every design decision, with file and line references into Godot
4.7.2, Streamline and the DLSS SDK, is in the separate research document (`RESEARCH.md` of the
research phase); this file references it as "research §x".

## 1. The problem

Godot 4.7 has no public extension point to add Vulkan device extensions, to record native
commands, or to replace the temporal upscaler (research §2.1). Two verified facts open a door:

1. GDExtensions are initialised at the CORE level **before** the Vulkan instance and device
   exist (`main/main.cpp:2282` loads and initialises extensions, `:3368` creates the
   DisplayServer, which creates the device).
2. Godot resolves **every** Vulkan function through one `LoadLibraryA("vulkan-1.dll")` plus
   `GetProcAddress(module, "vkGetInstanceProcAddr")` in volk (`thirdparty/volk/volk.c:87,91`),
   called from `godot.exe` itself.

So an extension can put its own Vulkan dispatch layer in front of Godot, in process, with no files
next to the executable, no environment variables and no registry entries.

## 2. Components

```
                      godot.exe                                  this addon (one DLL, pinned)
  volk: LoadLibraryA("vulkan-1.dll") ──IAT──► hook/loader_hook.cpp ──► vk::on_loader_requested()
        GetProcAddress(m, "vkGetInstanceProcAddr") ──────────────────► exported proxy (vk/dispatch.cpp)
  vkCreateInstance / vkCreateDevice ────────────────► hooks: features add extensions + feature bits
  vkCreateSwapchainKHR / vkQueueSubmit / vkQueuePresentKHR ─► QueueHooks (reflex/reflex_ll2.cpp)
  everything else ──────────────────────────────────► real vulkan-1.dll (untouched pointers)

  RenderingServer frame_pre_draw + the GDExtension frame callback ─► godot/latency_upscaling.cpp (markers, sleep)
  GDExtension frame callback (end of iteration) ────► godot/latency_upscaling.cpp (Reflex sleep)
```

| Module | Role | Godot-free (unit-tested) |
|---|---|---|
| `src/register_types.cpp` | Entry point; CORE / SCENE levels; startup and frame callbacks | |
| `src/godot/bootstrap.cpp` | CORE start-up: settings, safety checks, hook installation | |
| `src/godot/latency_upscaling.cpp` | `LatencyUpscaling` singleton (GDScript API), frame-loop wiring, project settings | |
| `src/hook/iat_patch.cpp` | Generic Import Address Table patching of one PE module | yes |
| `src/hook/loader_name.cpp` | Recognises a Vulkan loader request | yes |
| `src/hook/loader_hook.cpp` | Patches godot.exe's `LoadLibraryA/W/ExA/ExW`, pins the DLL | |
| `src/vk/dispatch.cpp` | The proxy `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` and the hooks | |
| `src/vk/device_patch.cpp` | Adds extensions and feature bits to a `VkDeviceCreateInfo` | yes |
| `src/vk/vk_feature.h` | Extension point: what a feature needs from instance and device | |
| `src/reflex/reflex_ll2.cpp` | Reflex via `VK_NV_low_latency2`: sleep, markers, timings, swapchain / present / submit | |
| `src/reflex/frame_sequencer.cpp` | Frame IDs and marker order rules | yes |
| `src/reflex/latency_stats.cpp` | Frame reports to averages | yes |
| `src/core/settings.cpp` | Settings layers and validation | yes |
| `src/dlss/jitter.cpp` | Phase 2: Godot's camera jitter reproduced on the CPU | yes |
| `src/dlss/upscaler_provider.h` | Phase 2: interface of the DLSS backend | |

## 3. The hook

### 3.1 Installation (CORE level)

`bootstrap_core()` decides, in this order, and records the first reason that stops it:

| Check | Result when it fails |
|---|---|
| `enabled` setting (addon config, project setting, `GODOT_LATENCY_UPSCALING_DISABLE=1`) | no hook: "disabled by ..." |
| Godot major.minor is 4.7 | no hook: "Godot x.y.z is not supported" (4.8 may change the internals the hook relies on) |
| No `RenderingServer` exists yet | no hook, dormant: "loaded after the Vulkan device was created; restart" (the editor loads a freshly added addon at run time) |
| godot.exe imports `LoadLibraryA/W/ExA/ExW` from a DLL | no hook: "unsupported build" |

Installation patches every import slot of those four functions in `godot.exe` (the official
4.7.2 build imports `LoadLibraryA`, `LoadLibraryW`, `LoadLibraryExW` from `KERNEL32.dll` and
`LoadLibraryExA` from a second `kernel32.dll` descriptor; all four are patched). Each hook
compares the file name with `vulkan-1.dll` / `vulkan-1` (any case, any directory) and otherwise
calls the previous slot value, so other hooks installed earlier (overlays) keep working. Loads
made by other modules (Streamline, NGX, overlays) are not affected: IAT patching is per module.

The DLL pins itself (`GET_MODULE_HANDLE_EX_FLAG_PIN`) at CORE initialisation. Once Godot holds
function pointers into it, it must never be unloaded, including when the addon folder is deleted
while the editor runs.

### 3.2 The proxy

For a `vulkan-1.dll` request the hook repeats the host's original call (same function, same
arguments, so the same search path), then returns **this DLL's** module handle, with a reference
that balances volk's later `FreeLibrary`. The DLL exports `vkGetInstanceProcAddr`
(`#pragma comment(linker, "/EXPORT:...")`), so volk's `GetProcAddress` finds the proxy. The
extension itself never links against `vulkan-1.dll` (`VK_NO_PROTOTYPES`).

The proxy returns the loader's own answer for every name except these:

| Function | Why |
|---|---|
| `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr` | stay in front for later queries |
| `vkCreateInstance`, `vkDestroyInstance` | remember the instance's API version; add instance extensions features ask for (none in Phase 1) |
| `vkCreateDevice`, `vkDestroyDevice` | add device extensions and feature bits; tell features about the device |
| `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkDestroySurfaceKHR`, `vkQueueSubmit`, `vkQueuePresentKHR` | Reflex (only intercepted when Reflex is enabled for the process) |

A `NULL` answer from the loader stays `NULL`. Every hook ends in the loader's exported
trampoline for that function, which dispatches through the handle, so validation layers,
overlays and the driver see exactly the calls they would see without the addon. Godot loads
most device functions through `vkGetInstanceProcAddr` (`volkLoadInstance`, no `volkLoadDevice`),
and the swapchain functions through `vkGetDeviceProcAddr`
(`rendering_device_driver_vulkan.cpp:1511-1515`); both paths are covered.

Handles are associated with their device through the loader's dispatch key (the first pointer
of every dispatchable handle; a device and its queues share it).

### 3.3 Device creation

`hook_CreateDevice` gathers the physical device's properties, extension list and the two
feature bits it needs (`timelineSemaphore`, `presentId`), asks every registered
`VulkanFeature` what it needs, and merges the requests. `DeviceCreatePatch` then builds the
create info Godot's call continues with:

- its own extension array: Godot's list plus the missing requested names (no duplicates);
- its own feature structures prepended to the chain (`VkPhysicalDeviceTimelineSemaphoreFeatures`,
  `VkPhysicalDevicePresentIdFeaturesKHR`); Godot 4.7 chains the promoted per-feature structures
  and `VkPhysicalDeviceVulkan11Features`, never `...Vulkan12Features`, so the prepended
  structures are valid next to Godot's;
- when a host chain already contains a structure holding a requested bit (for example
  `VkPhysicalDeviceVulkan12Features`, which must not coexist with the promoted structures), the
  bit is set in place for the duration of the call and restored before returning to the host.

If `vkCreateDevice` fails with the patch, it is retried unpatched and the features are told they
are unavailable ("created without them"). The host never sees a failure the addon caused.

### 3.4 Safety net

| Situation | Behaviour |
|---|---|
| Godot not 4.7.x, `enabled = false`, `GODOT_LATENCY_UPSCALING_DISABLE=1` | no hook installed; status says why |
| Added while the editor runs | no hook, no classes registered (dormant) until restart; the editor plugin shows "restart the editor to activate". Games started from that editor are new processes and work immediately |
| Compatibility renderer, D3D12 driver, headless | hook installed but the Vulkan loader is never requested: nothing happens; status names the driver |
| Non-NVIDIA GPU, driver without `VK_NV_low_latency2` (or only its revision 1, whose `vkGetLatencyTimingsNV` has another signature), no `VK_KHR_present_id`, no timeline semaphores | the device is created exactly as Godot asked (no extension added); status names what is missing |
| Loader lacks an export the layer needs | the host gets the real loader: pure pass-through |
| Device or swapchain creation fails with the addon's additions | retried without them (a swapchain without `oldSwapchain`, which the failed call retired) |
| The sleep semaphore is not signalled within 250 ms three frames in a row | sleep suspended until the window swapchain is re-created; markers continue; status "markers only, sleep stopped: ..." |
| `vkLatencySleepNV` or the wait returns an error | sleep disabled for the session, markers continue |
| No present since the last sleep (minimised window, frame not drawn) | no sleep that frame |
| Editor process | Reflex off by default (`enable_in_editor`): the layer only passes calls through |

"Pass-through" is tested, not assumed: `tests/run_all.py` renders a frame without the addon, with
the layer in front but nothing added, and with Reflex active, and requires identical pixels.

**Test coverage of the layer.** Three levels, each claim backed by a mutation in
`tests/run_mutations.py` that a suite really catches:

- *Godot suites* (`reflex` in both thread models, `pass-through`, `fallback`, `editor`, `validation`)
  run the real engine and driver. A debug-only `GODOT_LATENCY_UPSCALING_TEST_FAULT` switch forces
  the device-extension, first-swapchain and loader-incomplete failure paths; a real second native
  window checks that only the main window is tracked.
- *Fake-driver unit tests* (`test_reflex_ll2.cpp`) drive `Ll2Device` where the engine cannot:
  Godot 4.7 never passes `oldSwapchain` (it releases the old swapchain first), so only a fake create
  that fails shows the retry drops the retired handle; and Godot's separate render thread usually
  presents *before* the main thread's frame callback sleeps, so an engine run sees the present hook
  blocked behind the sleep only occasionally (1-17 of 60 capped presents with the lock taken
  exclusively), while a fake `vkLatencySleepNV` that blocks shows it every time. The engine run
  still asserts the bound (no present waits > 1 ms under the cap).
- *Unit tests against the real loader* (`test_dispatch.cpp`) check that the proxy keeps the
  loader's null answers for hooked names; `test_iat_patch.cpp` covers IAT patching and
  `select_chain_target`.

One path is defensive only: `ReflexFeature::destroy_surface` clears the tracked surface so a new
window can be tracked afresh — a single-window Godot game never destroys and recreates its main
surface mid-session, so no test can observe it; `run_mutations.py` lists that mutation as
review-only instead of claiming it.

## 4. Reflex (Phase 1)

Reflex runs through the Khronos-registered extension `VK_NV_low_latency2` (spec version 2). No
NVIDIA SDK code or DLL is involved, so the NVIDIA RTX SDKs licence does not apply to Phase 1.

### 4.1 Requirements the layer satisfies

From `vk.xml` (Vulkan-Headers v1.4.357): `VK_NV_low_latency2` depends on
`(VK_VERSION_1_2 or VK_KHR_timeline_semaphore) + (VK_KHR_present_id or VK_KHR_present_id2)`.
The layer enables `VK_NV_low_latency2` and `VK_KHR_present_id` (+ the `presentId` feature), and
the `timelineSemaphore` feature (+ `VK_KHR_timeline_semaphore` when the device is used below
Vulkan 1.2; Godot's instance requests 1.2, so on current drivers it is core).

| Call | Where | Notes |
|---|---|---|
| `VkSwapchainLatencyCreateInfoNV{latencyModeEnable}` | `vkCreateSwapchainKHR` of the window's surface | only the first surface a swapchain is created for (Godot's main window) and its re-creations; other windows pass through |
| `vkSetLatencySleepModeNV` | on every new window swapchain and on every API change | `lowLatencyMode = mode != Off`, `lowLatencyBoost = mode == On + Boost`, `minimumIntervalUs = 1e6 / fps_limit` |
| `vkLatencySleepNV` + `vkWaitSemaphores` | GDExtension frame callback | once per presented frame, in every mode (as NVIDIA requires); own timeline semaphore, 250 ms timeout |
| `vkSetLatencyMarkerNV` | see 4.2 | the present ID is the frame ID |
| `VkPresentIdKHR` | `vkQueuePresentKHR` | ID of the window's swapchain = frame ID; 0 ("none") for other windows |
| `VkLatencySubmissionPresentIdNV` | `vkQueueSubmit` of the Reflex device | only between RENDERSUBMIT_START and _END of a frame; lets the driver attribute the frame's GPU work |
| `vkGetLatencyTimingsNV` | singleton `get_latency_stats()` / `get_latency_reports()` | the driver keeps the last 64 frames |

### 4.2 Marker placement

The frame loop of Godot 4.7.2 (`main/main.cpp` `Main::iteration`, `servers/rendering/
rendering_server_default.cpp`) and where each call goes:

| Step in Godot | Addon | Marker |
|---|---|---|
| end of iteration N-1: GDExtension `frame` callback | `LatencyUpscaling::on_frame()` | SIMULATION_END(N-1) if frame N-1 never drew, then **sleep**, then SIMULATION_START(N), INPUT_SAMPLE(N) |
| OS run loop: `DisplayServer::process_events()` | | input sampled inside the simulation region |
| iteration N: physics, `process_frame`, scripts | | simulation |
| `RenderingServer::draw()` emits `frame_pre_draw` (main thread) | `on_frame_pre_draw()` | [TRIGGER_FLASH(N) on a left click] SIMULATION_END(N); queues `on_render_frame_begin(N)` on the render thread |
| render thread, before `_draw()` | `on_render_frame_begin(N)` | RENDERSUBMIT_START(N) |
| `vkQueueSubmit` (`RenderingDevice::_execute_frame`) | queue hook | submissions tagged with N |
| `vkQueuePresentKHR` (same call chain, right after the submit) | present hook | RENDERSUBMIT_END(N), PRESENT_START(N), present with ID N, PRESENT_END(N) |
| GDExtension `frame` callback (end of iteration N), first | `on_frame()` queues `on_render_frame_end(N)` behind `_draw(N)` | RENDERSUBMIT_END(N) only if no present sent it (a draw without present) |

Justification, against the NVIDIA Reflex SDK Integration Guide (v1.6, §3.4.2, §3.5.1.2) and the
NVIDIA-RTX/godot fork (branch `worktree-dlss-fg-reflex`):

- **Sleep**: the guide places it "at the start of the main / simulation thread's per-frame loop
  … before any mouse or keyboard input is gathered", once per frame, and in every mode. The
  frame callback is the last point an extension reaches before Godot's input pump; the fork
  calls `slReflexSleep` at the end of `Main::iteration` (`main.cpp:5154`, after
  `OS::add_frame_delay`). Ours sits before `add_frame_delay`, which only sleeps when
  `Engine.max_fps` or low-processor mode is set; the addon warns when `max_fps` is set and offers
  `reflex_fps_limit` instead (the guide: the Reflex sleep replaces an engine's own limiter).
- **SIMULATION_START**: "after the call to Sleep … should not include the Sleep call itself".
  Placed directly after the wait. It includes the OS input pump, which the guide counts as
  simulation ("the reading of new inputs"); the fork starts after the pump (start of
  `Main::iteration`, `main.cpp:4867`).
- **SIMULATION_END**: "after all input processing is done and the rendering threads have all
  of the data". `frame_pre_draw` is the first signal after all scripts and physics ran; the fork
  emits it right before `RenderingServer::sync()` (`main.cpp:5023`), which in the default thread
  model is a queue flush. A frame that never draws (low-processor mode without changes) gets its
  SIMULATION_END before the next sleep, so every frame is well-formed.
- **RENDERSUBMIT_START**: "at the start of the render submission region … where actual Vulkan
  runtime API calls are being made". Posted with `RenderingServer.call_on_render_thread`, it
  runs immediately before `_draw()` on whatever thread renders (the main thread by default, the
  render thread in the separate-thread model), with the frame ID captured at `frame_pre_draw`, as
  the guide recommends for engines whose simulation and render threads count frames separately.
  The fork emits it at the end of `RendererCompositorRD::begin_frame` (same point, `_draw()`'s
  first call).
- **RENDERSUBMIT_END**: must not end before the last Vulkan submission. Godot's frame
  submission happens in `RenderingDevice::_execute_frame` directly before the present, so the
  marker goes out in the present hook, before PRESENT_START. The fork emits it at the start of
  `RendererCompositorRD::end_frame`, i.e. before that final `vkQueueSubmit`; ours encloses it.
  For a draw without a present, the frame callback queues the end behind that frame's `_draw()`
  on the render thread. `frame_post_draw` is not used: with a separate render thread Godot
  defers it to the main thread, where it can arrive while the next frame is already rendering.
- **PRESENT_START / PRESENT_END**: "just before the runtime Present call" / "as soon as the
  runtime Present call returns": immediately around the loader's `vkQueuePresentKHR` in the
  hook. Same as the fork (`rendering_device_driver_vulkan.cpp:3344,3362`).
- **INPUT_SAMPLE**: sent right after SIMULATION_START because it was requested. The guide (v1.6,
  §3.5.1.2.7) marks it deprecated ("no longer used"), and Streamline no longer maps it
  (`sl.chi/vknvll2.cpp:49`); the driver reports its time equal to the simulation start.
- **TRIGGER_FLASH**: "anywhere between SIMULATION_START and SIMULATION_END whenever a left mouse
  click occurs … sent unconditionally" (driver R510+ decides whether to draw the indicator).
  The left button is sampled at `frame_pre_draw` and the marker goes out before SIMULATION_END.
  The singleton's `trigger_flash()` sends one manually.
- **PC Latency ping**: has no `VK_NV_low_latency2` marker (Streamline also drops it on Vulkan);
  not sent.

The rules that keep IDs and order valid (monotonic IDs, one RENDERSUBMIT_END per rendered frame,
a present ID only when newer than the last presented one) live in `reflex/frame_sequencer.cpp`
and are unit-tested, including a randomised property test.

### 4.3 Measured behaviour on the reference machine

GTX 1080 (Pascal), driver 582.28, Windows 10 19045, HAGS on. The numbers are in the README
("Reflex results"). Observations:

- The driver exposes and honours `VK_NV_low_latency2` on Pascal: sleep calls return, the
  frame-rate limit paces frames (30 fps limit -> 33 ms frames), and frame reports are complete.
- GPU-bound (about 80 fps, VSync off), Reflex On lowers the simulation-start-to-GPU-end latency
  from about 50 ms to about 41 ms (about 4.0 to 3.2 frames) at about 7 % more frame time; On +
  Boost is the same within noise. The per-stage timeline from `vkGetLatencyTimingsNV` shows the
  dominant interval is between the present call finishing and the GPU starting the next frame
  (present_end -> gpu_start, about 27 ms with Reflex On; the other stages together are ~2 ms plus
  ~12.5 ms of GPU render), which the Reflex sleep does not remove. Its cause is not established.
  Two candidates were considered: windowed (DWM-composited) presentation, and Pascal / the render
  graph's wait on `COLOR_ATTACHMENT_OUTPUT` before each frame's work
  (`rendering_device_driver_vulkan.cpp:3204`). The gap is not removed by exclusive fullscreen
  (about 30 ms there), but that run was at the monitor's resolution rather than 1280x720, so it
  weakens the composition candidate without excluding it. Whether an RTX GPU reaches the usual one-frame latency is untested (NVIDIA's
  Streamline 2.14 dropped Vulkan Reflex before Turing, which suggests Pascal is not a target).
  Note: this remainder was previously attributed to Godot's frame queue with `frame_queue_size = 1`
  as evidence; that was wrong. Godot clamps the queue to at least two frames (`MAX(2U, ...)` in
  `rendering_device.cpp`), so setting it to 1 has no effect and the "measures the same" result
  proves nothing.
- Without the swapchain latency structure or without submission tags, the sleep and the reports
  on this driver behave the same, so those structures are guarded by unit tests
  (`reflex/ll2_patches.cpp`) rather than by run-time checks.
- A minimised window presents nothing: the addon then skips the sleep (the driver itself did
  not block in that case; one sleep per presented frame follows the guide's "once per frame").
- The D3D12 / Compatibility refusal (the hook installs but never sees a Vulkan loader request, so
  it does nothing and the status names the driver) is verified by code review and by the
  `HIDE`/environment fallback tests that drive the same status path. The end-to-end D3D12 run in
  `tests/run_all.py` is **skipped** on the reference machine because Godot's own D3D12 driver
  crashes here even without the addon (exit `0xC0000005`); it is not a regression of this addon.
- The CPU marker timestamps in the reports come in steps of about 1 ms and neighbouring
  timestamps can be a few microseconds out of order although the markers were sent in order;
  GPU timestamps have microsecond resolution. Durations of short CPU regions (simulation,
  render submission) are therefore only accurate to about a millisecond.
- With an almost empty scene (thousands of frames per second) the driver returns only a few
  complete reports; with a real scene it returns all 64.

## 5. Platforms and the stub

The shipped library is Windows x86_64 only: the hook works by patching `godot.exe`'s import table
and wrapping the Windows Vulkan loader, which has no counterpart elsewhere. The `.gdextension`
therefore lists only `windows.debug.x86_64` and `windows.release.x86_64`. Godot picks the library
entry with the most matching feature tags, so the editor, games run from it and debug exports load
the debug DLL and release exports the release DLL; the release zip ships both
(`tests/run_package.py` installs it into a fresh project and checks all four cases).

On every other platform the extension simply does not load:

- an export to such a platform should exclude `addons/latency_upscaling/*` (export preset filter);
  Godot 4.7 then also drops the addon from the exported extension list
  (`EditorExportPlatform::_get_filtered_cache`), so the game starts without any message;
- without the filter, the export succeeds with a "No "x86_64" library found for GDExtension"
  warning and the game logs three ERROR lines at start-up (no library, library not found, error
  loading extension) and then runs normally without the addon;
- the `LatencyUpscaling` singleton is absent, so scripts reach it through `Engine.has_singleton()` /
  `Engine.get_singleton()`; naming it directly is a parse error there.

`tests/run_portable.py` verifies each point, including real Linux exports (built, not run).

`stub/` contains a raw-C no-op GDExtension that would register the singleton on the other
platforms. It is **experimental and not shipped**: it has no ptrcall paths (a typed call crashes),
no properties or constants, and its Linux/macOS builds have never run on those systems. It is
kept for a later release; `stub/README.md` lists what must change first.

## 6. What is automatic

| Automatic | Needs a restart / setting / call |
|---|---|
| The GDExtension loads by being in `addons/` (listed in `.godot/extension_list.cfg`), in the editor, in games started from it and in exported games | Added while the editor runs: the editor itself activates after a restart |
| Hook installation, extension injection, Reflex On by default, markers, sleep | Reflex in the editor process: `latency_upscaling/general/enable_in_editor` |
| `LatencyUpscaling` engine singleton, project settings shown (only saved when changed) | Overlay: the singleton's `overlay_visible` property or `latency_upscaling/overlay/enabled` |
| Fallback with a clear status on every unsupported configuration | A game's own settings menu should set the singleton's `reflex_mode` (through `Engine`, see README Usage) |
| Overrides via Project Settings (`latency_upscaling/...`) reach exported games, read at CORE init, with or without the editor plugin | Defaults changed only by setting them in the editor's Project Settings (saved to `project.godot`) |

## 7. Phase 2: DLSS Super Resolution / DLAA (design)

Nothing of this is active in Phase 1; `src/dlss/` holds the interface and the jitter code.

1. **Backend**: Streamline 2.14.1 in interposer mode behind the proxy (the proxy's
   `vkGetInstanceProcAddr` forwards to `sl.interposer.dll` instead of `vulkan-1.dll`, exactly
   what the NVIDIA fork does with volk, research §2.3), or direct NGX. `UpscalerProvider`
   (`src/dlss/upscaler_provider.h`) is written so either can sit behind it.
2. **Vulkan requirements**: a `VulkanFeature` for DLSS returns NGX's instance and device
   extensions (`VK_NVX_binary_import`, `VK_NVX_image_view_handle`, `VK_EXT_buffer_device_address`,
   `VK_KHR_push_descriptor`, plus the runtime list NGX reports) through the same request path
   Reflex uses. `DeviceRequest` grows a queue list only when Frame Generation needs its
   optical-flow queue (Phase 3).
3. **The FSR2 slot** (research §2.2): the viewport runs in `SCALING_3D_MODE_FSR2` at the DLSS
   render scale, so Godot computes render size, jitter phase count, mip bias and TAA-off. Between
   frames the addon re-configures the viewport's `RenderSceneBuffersRD` with
   `SCALING_3D_MODE_METALFX_TEMPORAL`, which maps to "no upscaler" on Vulkan
   (`render_forward_clustered.cpp:1793-1798`), and creates the target-size `color_upscaled`
   texture that tonemapping reads when it exists (`renderer_scene_render_rd.cpp:473`).
4. **Recording the evaluate**: a `POST_TRANSPARENT` `CompositorEffect` (with
   `needs_motion_vectors`) dispatches a marker compute pipeline whose uniform set names `color`,
   `depth`, `velocity` (sampled) and `color_upscaled` (storage), so Godot's render graph orders
   and transitions them. The layer recognises that dispatch (it knows the pipeline's `VkPipeline`
   from `get_driver_resource`) in `vkCmdBindPipeline` / `vkCmdDispatch`, calls
   `UpscalerProvider::evaluate()` on the same command buffer instead, and restores the bound
   state afterwards.
5. **Jitter**: `dlss::jitter_in_pixels()` reproduces Godot's Halton sequence and phase count
   (unit-tested against the engine's formula). Godot's frame counter must be matched once by a
   scene-UBO readback; motion vectors are current-to-previous in `clip * 0.5` units
   (`scene_forward_clustered.glsl:3106-3112`), converted with `motion_vector_scale`.
6. **NVIDIA binaries**: never in this repository. On first enable the editor plugin shows the
   NVIDIA RTX SDKs licence and, after the developer accepts, fetches `streamline-sdk-v2.14.1.zip`
   from NVIDIA's GitHub release and extracts the eight production DLLs of `bin/x64/` into
   `addons/latency_upscaling/bin/nvidia/` (git-ignored). `tools/fetch_nvidia_sdk.py` is the command-line
   stub of that step: it resolves the release and prints the plan; it does not download.
7. **Known costs** (research §2.2): one frame of FSR2 when Godot re-configures the buffers
   (resize, settings change), the debanding flag has to be restored, and everything must be
   re-validated for each Godot minor version. Needs an RTX 20+ GPU to test.

## 8. Phase 3: Frame Generation (outline)

Streamline's DLSS-G owns the present path: the eight swapchain / surface functions go to its
proxies, Reflex switches from `VK_NV_low_latency2` to `sl.reflex` + `sl.pcl` (required by DLSS-G),
and the frame ID of section 4.2 becomes Streamline's frame token. Limits on Vulkan: no HUD-less
colour (Godot draws the UI into the same target), no VSync with FG, no dynamic MFG (research
§2.5). Needs RTX 40 (FG) / RTX 50 (MFG) with a real display.
