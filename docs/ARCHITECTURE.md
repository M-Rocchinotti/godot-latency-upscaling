# Architecture

How the addon gets NVIDIA features into a stock Godot 4.7 without engine changes, and how it
stays out of the way when it cannot. Phase 1 implements the Vulkan hook layer and NVIDIA Reflex;
Phase 2 adds DLSS Super Resolution / DLAA (section 7); Phase 3 (Frame Generation) will build on the
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
  vkCmdBindPipeline / ...DescriptorSets / ...PushConstants / vkCmdDispatch
                                (only when an upscaler records natively) ─► vk/marker_interceptor.cpp
  everything else ──────────────────────────────────► real vulkan-1.dll (untouched pointers), or
                                Streamline's sl.interposer.dll in front of it when DLSS runs

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
| `src/vk/marker_interceptor.cpp` | Phase 2: replaces a marker compute dispatch in Godot's command buffer, re-binds Godot's state | yes |
| `src/reflex/reflex_ll2.cpp` | Reflex via `VK_NV_low_latency2`: sleep, markers, timings, swapchain / present / submit | |
| `src/reflex/frame_sequencer.cpp` | Frame IDs and marker order rules | yes |
| `src/reflex/latency_stats.cpp` | Frame reports to averages | yes |
| `src/core/settings.cpp` | Settings layers and validation | yes |
| `src/dlss/jitter.cpp` | Phase 2: Godot's camera jitter reproduced on the CPU | yes |
| `src/dlss/upscaler_provider.h` | Phase 2: interface of the DLSS backend, evaluation inputs | |
| `src/dlss/dlss_feature.cpp` | Phase 2: DLSS's `VulkanFeature`: device checks, reasons, backend lifetime | yes |
| `src/dlss/streamline.cpp` | Phase 2: Streamline 2.14.1 (load, signature, `slInit`, evaluation); the chain feature | builds in the tests, runs only with NVIDIA's runtime |
| `src/dlss/dlss_recorder.cpp` | Phase 2: the marker recorder that has DLSS evaluate | yes |
| `src/dlss/gpu_precheck.cpp`, `runtime_files.cpp` | Phase 2: GPUs before Godot's instance; the runtime folder | |
| `src/upscale/quality.cpp` | Phase 2: quality modes, scaling ratios, Auto, presets | yes |
| `src/upscale/upscale_feature.cpp` | Phase 2: the upscaler's `VulkanFeature` (`VK_KHR_push_descriptor`), native objects' lifetime | yes |
| `src/upscale/native_reference.cpp` | Phase 2: the reference upscaler recorded natively (push descriptors) | yes |
| `src/godot/upscaler.cpp` | Phase 2: the FSR2 slot takeover, jitter, the reference backend, the marker | |

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
| `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkCmdBindPipeline`, `vkCmdBindDescriptorSets`, `vkCmdPushConstants`, `vkCmdDispatch` | Phase 2 marker interception (only handed out when an upscaler records natively; enabled at CORE initialisation, before Godot resolves them) |

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

## 7. Phase 2: DLSS Super Resolution / DLAA

Implemented and tested on an RTX 5080 (driver 591.86, DLSS 310.9.1 through Streamline 2.14.1),
with the plumbing also tested on the GTX 1080 through a reference upscaler.

### 7.1 The FSR2 slot

Godot 4.7 has no API to replace its temporal upscaler, but its FSR2 mode does everything an
upscaler needs from the renderer (`src/godot/upscaler.cpp`):

1. **Main thread**, end of every iteration: the root viewport runs in `SCALING_3D_MODE_FSR2` at the
   quality mode's scale, so Godot computes the render size, the Halton jitter (`8 * ratio^2`
   phases), the mip bias (`log2(scale)` plus the viewport's own) and switches its TAA off. With
   DLSS the scale comes from Streamline's optimal render size (`slDLSSGetOptimalSettings`): Godot
   truncates `target * scale` per axis (`renderer_viewport.cpp`), so the width matches exactly and
   the height can be one pixel off (inside DLSS's render-size range); DLAA is exactly 1. The
   current camera gets a `POST_TRANSPARENT` `CompositorEffect` with `needs_motion_vectors`, merged
   with its own or the WorldEnvironment's compositor (restored on shutdown). The camera is tracked
   at `frame_pre_draw`, before the frame renders, so a camera made current during processing is
   upscaled from its first frame (and the end of the iteration checks again).
2. **Render thread**, the frame after Godot (re)configures the render buffers (FSR2 runs once): a
   takeover queued behind that frame's draw re-configures them identically but with
   `SCALING_3D_MODE_METALFX_TEMPORAL`, which is "no upscaler" on Vulkan
   (`render_forward_clustered.cpp:1789-1800`), and creates the target-size `color_upscaled` texture
   that tonemapping reads whenever it exists (`renderer_scene_render_rd.cpp:473`). Lost in the
   re-configuration: `use_debanding` (not exposed).
3. **Render thread**, every frame after that: the effect dispatches the upscaler, which reads colour,
   depth and velocity at render size and writes `color_upscaled` at target size.
4. **History resets.** The history is thrown away (DLSS's `reset`, the reference upscaler's reset
   flag) on the frame where it no longer describes the view: after a takeover or a resize, when
   the game calls `reset_dlss_history()`, and on camera cuts (`src/upscale/camera_cut.cpp`, pure and
   unit-tested; NVIDIA's guide asks for a reset on cuts): another camera became current (the main
   thread counts switches at `frame_pre_draw`, the render thread compares), or the camera turned
   further than `dlss/cut_angle_degrees` (60) or moved further than `dlss/cut_distance` (10 m) since
   the previous frame. The angle is that of the relative rotation (yaw, pitch and roll alike, scale
   removed, a mirrored basis counts as 180 degrees); 0 disables a test. The thresholds follow the
   post-effects addon's cut detection; the distance is larger because DLSS reprojects translation
   with the motion vectors and a needless reset costs a frame of anti-aliasing. The status counts
   resets by reason (`upscaling.history_resets`, `last_reset_reason`).
5. **Motion for the sky** (`src/godot/velocity_fill.cpp`). In the FSR2 slot Godot clears velocity to
   (-1, -1) and FSR2 reconstructs the motion of every pixel no geometry wrote from its depth and the
   camera matrices (`render_forward_clustered.cpp:2229-2239`, `motion_vector_inc.glsl`). After the
   takeover the opaque pass clears velocity to 0 instead, so the sky and the background claimed to
   stand still while the camera turned, and DLSS smeared geometry edges into them. Before the
   upscaler runs, a compute pass copies Godot's velocity into a texture of the addon's
   (RGBA16F, in the render buffers) and gives every far-plane pixel (reverse-Z depth exactly 0)
   Godot's formula: `prev = reprojection * (uv * 2 - 1, depth, 1)`, velocity
   `prev.xy / prev.w * 0.5 + 0.5 - uv`, with `reprojection` = clip to previous clip, both without
   jitter (the matrices DLSS gets too). Both upscalers read that copy. Tested: the filled sky
   vectors equal the analytic camera motion within 3 % (yaw and pitch), the formula equals Godot's
   own vectors on static geometry under a turn and a strafe (debug mode that derives every pixel),
   and on the RTX a turn over thin bars against a plain background leaves 0 trail pixels with DLSS
   Quality (FSR2 at the same size: 0; DLSS without the fill: 12).

Conventions, measured with image tests (`tests/upscaler_tests.gd`, `tests/dlss_tests.gd`):

| | Convention | Evidence |
|---|---|---|
| Jitter | `RenderSceneData.get_cam_projection()` is the jittered projection; jitter in render pixels = NDC offset * size / 2, exactly what Godot hands FSR2 (`taa_jitter * internal_size * 0.5`); +1/+1 in texture orientation | projection, CPU Halton sequence (calibrated once) and Godot's scene UBO (`taa_jitter`, `taa_frame_count`, read back) agree every frame; ignored or flipped jitter scores lower with both the reference upscaler and DLSS |
| Motion vectors | current -> previous, UV units, Y down; DLSS / Streamline get `mvecScale` 1, 1 | a camera yaw / pitch of known size gives the expected centre velocity within 0.1 %; flipped vectors score lower under camera motion with both upscalers |
| Depth | reverse Z (`depthInverted`) | Godot 4.7 renders reverse Z |

### 7.2 Recording into Godot's command buffer: the marker

An upscaler has to record into the command buffer Godot is recording, at the point where colour,
depth and velocity are final and `color_upscaled` has not been read yet, and with those images in
known layouts. Instead of hooking Godot's render graph, the addon lets the graph do the ordering:

1. The effect dispatches a **marker**: an ordinary RenderingDevice compute list with the reference
   upscaler's pipeline and a uniform set naming colour, depth and velocity (sampled) and
   `color_upscaled` (storage). The render graph therefore orders the dispatch after the scene and
   before tonemapping, and transitions the images to `SHADER_READ_ONLY_OPTIMAL` (sampled) and
   `GENERAL` (storage) (`rendering_device_graph.cpp:136-140`). The push constants carry a job id.
2. The layer hands Godot hooks for `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkCmdBindPipeline`,
   `vkCmdBindDescriptorSets`, `vkCmdPushConstants` and `vkCmdDispatch` (only in runs that record
   natively; they must be in place before Godot resolves its functions, i.e. at CORE
   initialisation). `MarkerInterceptor` (`src/vk/marker_interceptor.cpp`) tracks the compute state
   bound while a registered marker pipeline (its `VkPipeline` from `get_driver_resource`) is bound
   and, on its `vkCmdDispatch`, hands the command buffer to the marker's recorder instead.
3. After the recorder, the marker, its descriptor sets (with their dynamic offsets) and push
   constants are bound again, so whatever Godot records next sees the state it set up (Streamline's
   rule when it does not track command-list state). When the recorder recorded, the marker dispatch
   is dropped; when it declines, the marker dispatch runs on that re-bound state (a recorder may
   have bound its own pipeline before giving up) and the reference upscaler fills the output.

Recorders: `NativeReferenceRecorder` (the reference shader through the addon's own pipeline with
push descriptors, `VK_KHR_push_descriptor`; debug test backend, proves the mechanism on any GPU:
bit-identical to Godot's own recording of the same shader) and `DlssRecorder` (DLSS evaluation).

### 7.3 DLSS through Streamline

`src/dlss/streamline.cpp`, `src/godot/bootstrap.cpp`:

1. **Decision at CORE initialisation** (the only moment the host's Vulkan calls can still be
   routed): DLSS enabled in the settings, a Vulkan run (not headless, not Compatibility, not
   D3D12), the runtime files present (`<addon library folder>/nvidia/`, checked by existence), then
   a temporary Vulkan instance through the real loader lists the GPUs. Each goes through the
   addon's checks (`dlss::check_device`: NVIDIA, driver 512.15+, `VK_NVX_binary_import` and
   `VK_NVX_image_view_handle`); only then is `sl.interposer.dll` loaded, after its NVIDIA signature
   is verified (Streamline's own `sl_security.h`; an unsigned one is refused before it is loaded,
   `upscaling.dlss.runtime_reason` says so), `slInit` called (Vulkan, DLSS only, no OTA, no
   command-list state tracking, frame-based tagging, no log files, the project's own ID) and
   Streamline asked (`slIsFeatureSupported`). Without a supported GPU Streamline is unloaded again.
   The project ID NGX gets is GUID-like as NVIDIA requires for the custom engine type:
   `latency_upscaling/dlss/project_id`, or else a version-8 UUID from the SHA-256 of the project's
   name (`src/dlss/project_id.cpp`), the same on every machine and in every export.
2. **Interposer mode**, as NVIDIA's own Godot integration uses it: the layer's `next` functions come
   from `sl.interposer.dll` instead of `vulkan-1.dll` (`vk::set_loader_override`). Streamline's
   `vkCreateInstance` / `vkCreateDevice` add DLSS's requirements (on the RTX 5080: instance
   `VK_KHR_get_physical_device_properties2`, `VK_KHR_external_memory_capabilities`,
   `VK_KHR_external_semaphore_capabilities`; device `VK_KHR_push_descriptor`,
   `VK_NVX_binary_import`, `VK_NVX_image_view_handle`, buffer device address; Vulkan 1.2 features
   `timelineSemaphore`, `descriptorIndexing`, `bufferDeviceAddress`; no extra queues), and its
   present path runs every frame. The addon's own hooks (Reflex, the marker) sit in front of it.
   Streamline creates private-data slots without asking for the feature, so
   `StreamlineChainFeature` enables `privateData` whenever Vulkan goes through Streamline.
3. **Evaluation** at the marker: `DlssRecorder` looks up the job and `Streamline::evaluate` records
   `slDLSSSetOptions` (mode, output size, presets, HDR colour, auto exposure),
   `slSetConstants` (jitter, `mvecScale`, reverse Z, reset, unjittered camera matrices),
   `slSetTagForFrame` (colour, depth, motion vectors, output: `VkImage`, view, format, size, usage
   derived from Godot's texture usage, layout as the graph left it) and `slEvaluateFeature` into
   Godot's command buffer.
4. **Shutdown**: `slShutdown` right before the device is destroyed (the layer's `vkDestroyDevice`
   hook), also when DLSS did not come up; the module stays loaded for the rest of the process.

Streamline 2.14.1 logs three warnings at start-up in every Vulkan run, `Hook sl.common:Vulkan:
CmdBindPipeline / CmdBindDescriptorSets / BeginCommandBuffer is NOT supported, plugin will not
function properly`. They are expected: `sl.common` asks for these three hooks to track compute
state, but Streamline's plugin manager only maps swapchain, present, surface and device-wait hooks
for Vulkan (`source/core/sl.plugin-manager/pluginManager.cpp:1587-1594` at v2.14.1), so that
tracking does not exist on Vulkan; the addon switches it off (`eDisableCLStateTracking`) and
re-binds Godot's state itself (7.2).

Every "no" is reported: `get_dlss_status()` returns `unavailable: <reason>` (not an NVIDIA GPU,
driver older than 512.15, the NGX extensions missing, the runtime not installed, DLSS disabled at
startup, Streamline's own verdict), `off: <reason>` (disabled at run time, Auto below 1920x1080) or
`active (<mode>, <render> -> <output>)`.

### 7.4 The NVIDIA runtime

NVIDIA's binaries are never in this repository or the release zip. The developer fetches them after
accepting NVIDIA's licences:

- in the editor: when the plugin is enabled with DLSS on and the runtime missing (and under
  Project > Tools), a dialog shows the licence files of the pinned release, `nvngx_dlss.license.txt`
  (NVIDIA RTX SDKs licence) and `license.txt` (Streamline, MIT), fetched from the release's commit
  (`NVIDIA-RTX/Streamline` 2122257e, tag v2.14.1) and shown only when their SHA-256 is the one the
  lock records for the same files in NVIDIA's archive, so the text accepted is the text that ships
  with the DLLs. It shows the archive's size (276 MB) and the installed size before anything is
  fetched, lets the developer pick the download folder (default: next to the runtime's folder, as
  the command-line tool does; the free space is checked, the archive removed afterwards), keeps
  Download disabled until every text is verified and the acceptance box is ticked, downloads
  `streamline-sdk-v2.14.1.zip` from NVIDIA's GitHub release, checks size, the release's SHA-256
  digest and the pinned hashes, and installs the listed files into the git-ignored
  `addons/latency_upscaling/bin/nvidia/`;
- or `tools/fetch_nvidia_sdk.py --show-licences` (the same verified texts), then `--download
  --accept-nvidia-licences --accepted-by "NAME"` (`--accepted-on DATE`, `--how TEXT` to record an
  acceptance given earlier or elsewhere).

Only the NVIDIA RTX SDKs licence (`nvngx_dlss.license.txt`) needs acceptance; Streamline's
`license.txt` is the MIT licence for `sl.*.dll` (the NSight Perf SDK licence it also names covers only
`sl_nvperf.*`, which this runtime does not include), shown and shipped but not accepted
(`nvidia_runtime.json` "acceptance"). Either way `licence_acceptance.json` next to the DLLs records
**who accepted, when and how**, as the person gave it (the dialog: the developer using the editor,
who ticked the box), both licence files with their SHA-256 and which of them was accepted, the
release (tag, commit, archive SHA-256) and when and with what the record was written. The tools
never name themselves as the party accepting. For the runtime used by this project's own tests the
record says: in the conversation with the CEO agent on 2026-09-24 the project owner accepted
"NVIDIAs RTX-SDK-Lizenz" ("ja ich akzeptiere"), the NVIDIA RTX SDK licence as presented, relayed by
the CEO agent; it covers `nvngx_dlss.license.txt`; `license.txt` (MIT) is not recorded as accepted.

`addons/latency_upscaling/nvidia_runtime.json` names the release and the files;
`nvidia_runtime.lock.json` pins the SHA-256 of the archive and of every file taken from it.
Windows exports ship the four DLLs DLSS Super Resolution needs (`sl.interposer`, `sl.common`,
`sl.dlss`, `nvngx_dlss`: verified to be enough on the RTX 5080) and NVIDIA's licence files in
`nvidia/` beside the executable (`nvidia_runtime_export.gd`, registered by the editor plugin). With
the plugin disabled that export plugin does not exist, so the extension registers
`nvidia_runtime_export_check.gd` in every editor session, which warns at export time that the
export goes without the runtime (tested by `tests/run_export.py`).

### 7.5 Limits and costs

- One frame of Godot's FSR2 whenever Godot re-configures the render buffers (resize, quality
  change), then the takeover.
- `use_debanding` is lost in the re-configuration.
- Only the root viewport is upscaled; sub-viewports and multiview (XR) buffers are left alone.
- The sky's motion is reconstructed from the camera (7.1, 5.), so it is exact for the sky and for
  anything at the far plane; transparent surfaces drawn over the sky without depth get the sky's
  motion, as with Godot's own FSR2.
- The velocity fill costs one full-screen compute pass at render size and an RGBA16F texture.
- Everything depends on Godot 4.7 internals (the FSR2 / MetalFX mode mapping, the render-graph
  layouts, `color_upscaled`); other Godot versions are refused by the version check.
- Tests that need an RTX GPU run on a remote PC in short windowed runs (`tools/rtx_remote.py`,
  log in `docs/RTX_WINDOWED_RUNS.md`). UAC is off on that PC, so every process is elevated and the
  Vulkan loader ignores `VK_ADD_LAYER_PATH`; without touching its settings or registry,
  `deploy --validation-layer` copies the Khronos layer into the test folder and `session
  --validation` starts Godot through `medium_launcher.exe`, which lowers a copy of its own token to
  medium integrity. Result (validation layer 1.4.357, DLSS 310.9.1): a DLSS Quality run of the demo
  (240 frames) reports nothing from the addon or DLSS; the only messages are the loader's note
  about a broken third-party layer manifest on that PC and Godot's own
  `Undefined-Value-StorageImage-FormatMismatch-ImageView` warnings, both also in the run without the
  addon. The first run found `VUID-vkCmdClearColorImage-image-00002` (DLSS clears its output, which
  needed `VK_IMAGE_USAGE_TRANSFER_DST_BIT`; fixed). Switching modes or presets at run time
  (`tests/dlss_tests.gd`) additionally shows `VUID-vkCmdDraw-None-09600` for images named
  `nv.ngx.dlss.resource`: DLSS's internal images of a newly created feature, first used in
  `GENERAL` while still `UNDEFINED`. They are NVIDIA's (created and used inside the NGX
  evaluation), appear only on the frame a feature is re-created, and cannot be changed by the
  addon.

## 8. Phase 3: Frame Generation (outline)

Streamline's DLSS-G owns the present path: the eight swapchain / surface functions go to its
proxies, Reflex switches from `VK_NV_low_latency2` to `sl.reflex` + `sl.pcl` (required by DLSS-G),
and the frame ID of section 4.2 becomes Streamline's frame token. Limits on Vulkan: no HUD-less
colour (Godot draws the UI into the same target), no VSync with FG, no dynamic MFG (research
§2.5). Needs RTX 40 (FG) / RTX 50 (MFG) with a real display.
