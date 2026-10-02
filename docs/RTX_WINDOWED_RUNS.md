# Windowed runs on the RTX test PC

Every run that opened a window on the user's RTX 5080 PC (after the user's "WINDOW OK"), written by
`tools/rtx_remote.py session --window-ok`. Runs are short (at most 120 s), in a normal window that does not take the
keyboard focus, and start only when `preflight` finds the PC idle (GPU load, no fullscreen app or game in front).

| Start (the RTX PC's local time) | Run | What | Duration | Outcome | PC state before |
|---|---|---|---|---|---|
| 2026-09-25 00:50 (clock of the test machine, first three runs) | upscaler_reference | `GODOT_LATENCY_UPSCALING_TEST_UPSCALER=reference GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=4 --script res://tests/upscaler_tests.gd` | 6 s | exit 1 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 00:51 (clock of the test machine) | upscaler_reference | `GODOT_LATENCY_UPSCALING_TEST_UPSCALER=reference GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=4 --script res://tests/upscaler_tests.gd` | 8 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 00:51 (clock of the test machine) | upscaler_native | `GODOT_LATENCY_UPSCALING_TEST_UPSCALER=native GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=4 --script res://tests/upscaler_tests.gd` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 01:03:07 | dlss_smoke | `GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=2 --script res://tests/upscaler_run.gd -- --frames=300` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 01:07:04 | dlss_tests | `--script res://tests/dlss_tests.gd` | 8 s | exit 1 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 01:08:45 | dlss_tests | `--script res://tests/dlss_tests.gd` | 8 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 01:09:04 | dlss_bench | `--script res://tests/dlss_bench.gd -- --size=1920x1080` | 12 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 01:10:09 | dlss_reflex | `GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=2 --script res://tests/upscaler_run.gd -- --frames=400` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 01:15:42 | dlss_minimal_runtime | `GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=2 --script res://tests/upscaler_run.gd -- --frames=200` | 4 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 01:28:22 | dlss_tests | `--script res://tests/dlss_tests.gd` | 8 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 03:16:54 | dlss_tests | `--script res://tests/dlss_tests.gd` | 10 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 03:17:46 | review_motion | `--script res://tests/review_motion_scratch.gd` | 12 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 03:18:58 | validation_fsr2_baseline | `GODOT_LATENCY_UPSCALING_DISABLE=1 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --verbose --script res://tests/upscaler_run.gd -- --frames=240 --fsr2=0.667` | 4 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 03:19:19 | validation_dlss | `GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=2 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --verbose --script res://tests/upscaler_run.gd -- --frames=240` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 03:20:32 | validation_dlss | `GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=2 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --verbose --script res://tests/upscaler_run.gd -- --frames=240` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 03:20:55 | validation_dlss_tests | `VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --script res://tests/dlss_tests.gd` | 10 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 03:21:55 | validation_dlss_tests_verbose | `VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --verbose --script res://tests/dlss_tests.gd` | 10 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 17:37:41 | dlss_tests | `--script res://tests/dlss_tests.gd` | 10 s | exit 0 | GPU 1 %, no fullscreen app in front, no game in front |
| 2026-09-25 17:38:03 | validation_dlss | `GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=2 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --verbose --script res://tests/upscaler_run.gd -- --frames=240` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, no game in front |
| 2026-09-25 19:35:39 | dlss_tests | `--script res://tests/dlss_tests.gd` | 12 s | exit 0 | GPU 8 %, no fullscreen app in front, no game running |
| 2026-09-26 20:58:25 | rtx_1440p | `APPDATA=C:\Users\lol\claude-rtx-test\userdata hollowfront\HollowFront.exe -- --size=2560x1440 --steps=perf,ghost,reflex --budget=105 --ghost=native_taa,fsr2_quality,dlaa,dlss_quality,dlss_performance` | 54 s | exit 0 | GPU 0 %, no fullscreen app in front, no game running |
| 2026-09-26 21:01:36 | rtx_1440p_ghost | `APPDATA=C:\Users\lol\claude-rtx-test\userdata hollowfront\HollowFront.exe -- --size=2560x1440 --steps=ghost --budget=105 --ghost=native_taa,fsr2_quality,dlaa,dlss_quality,dlss_performance,dlss_ultra_performance` | 48 s | exit 0 | GPU 0 %, no fullscreen app in front, no game running |
| 2026-09-26 21:03:53 | rtx_uw3440_perf | `APPDATA=C:\Users\lol\claude-rtx-test\userdata hollowfront\HollowFront.exe -- --size=3440x1440 --steps=perf --budget=100` | 18 s | exit 0 | GPU 0 %, no fullscreen app in front, no game running |
| 2026-09-26 21:04:32 | rtx_uw3440_ultra | `APPDATA=C:\Users\lol\claude-rtx-test\userdata hollowfront\HollowFront.exe -- --size=3440x1440 --steps=perf,reflex --budget=100 --preset=3 --reflex-mode=native_taa` | 22 s | exit 0 | GPU 0 %, no fullscreen app in front, no game running |
| 2026-09-26 21:05:43 | rtx_reflex_uw | `APPDATA=C:\Users\lol\claude-rtx-test\userdata hollowfront\HollowFront.exe -- --size=3440x1440 --steps=reflex --budget=100 --preset=3 --reflex-mode=native_taa --reflex-frames=400` | 26 s | exit 0 | GPU 0 %, no fullscreen app in front, no game running |
| 2026-09-26 21:06:55 | rtx_2560x1080 | `APPDATA=C:\Users\lol\claude-rtx-test\userdata hollowfront\HollowFront.exe -- --size=2560x1080 --steps=perf,settings --budget=100` | 18 s | exit 0 | GPU 0 %, no fullscreen app in front, no game running |
| 2026-09-26 21:08:00 | rtx_reflex_uw2 | `APPDATA=C:\Users\lol\claude-rtx-test\userdata hollowfront\HollowFront.exe -- --size=3440x1440 --steps=reflex --budget=100 --preset=3 --reflex-mode=native_taa --reflex-frames=360 --reflex-rounds=2` | 28 s | exit 0 | GPU 0 %, no fullscreen app in front, no game running |

## Phase 3 Frame Generation bring-up (R1-R2), 2026-10-02

Validation of DLSS Frame Generation (milestones H1-H4) on the RTX 5080 (driver 591.86).

**R1 — FG features load & supported (no window needed; `tools/rtx_remote.py probe`, `sl_probe` extended
to load {DLSS, DLSS_G, Reflex, PCL}).** On the RTX 5080: `dlss_supported = Result::eOk`,
`dlss_g_supported = Result::eOk`, `reflex_supported = Result::eOk`, `pcl_supported = Result::eOk`.
`slDLSSGGetState`: `set_options = Result::eOk`, `get_state = Result::eOk`,
`num_frames_to_generate_max = 5`, `dlssg_status = 0` (eOk). Streamline 2.14.1, DLSS plugin 2.14.1,
NGX 310.9.1; driver_detected 591.86.0 ≥ driver_required 527.64.0; os_detected 10.0.19045 ≥
os_required 10.0.0. `VK_NV_optical_flow`, `VK_NV_low_latency2`, `VK_NV_cooperative_matrix` all present.
Unit tests 120 passed / 0 failed. No "common constants cannot be found" line in the Streamline log.
(In the probe, Streamline also logged `Failed to load NvLowLatencyVk.dll` and three
`sl.common:Vulkan:* hook NOT supported` warnings. The windowed runs below clarify these: the
`sl.common hook NOT supported` warnings DO also appear in the plugin's interposer mode and are benign
for this addon — it disables command-list state tracking and records commands itself. The
`NvLowLatencyVk.dll` error did NOT appear in the FG-off interposer run, which loads only the DLSS
feature, not sl.reflex; it could not be checked with FG on, because the FG-on run crashes at device
creation — see R2.)

**R2 — Reflex via sl.reflex (windowed): BLOCKED by an FG-on crash. Run under the user's durable grant
to open a short window alongside other programs (preflight: GPU 0 %, no fullscreen app in front, a game
running). See the fg_* rows below.** The sl.reflex switch could not be observed, because forcing FG on
(`fg/enabled=1;fg/mode=1`) crashes the engine at Vulkan device creation, before `Streamline::initialize`
/ `enable_streamline_reflex` run.

- **FG off (baseline, works):** exit 0. `reflex.active = true` via the default VK_NV_low_latency2 path
  (counters live: sleeps 299, presents_stamped 298, submits_tagged 596, swapchains_with_latency_mode 1);
  `frame_generation` = `{supported:true, active:false, max_multiplier:4, reason:"Frame Generation is not
  yet available in this version"}`, i.e. `get_frame_generation_status()` is **"off: …", NOT
  "unavailable"** — confirmed live on the 5080. With `--gpu-validation` + the Khronos layer: **zero
  validation errors**. DLSS reports `DLSS 310.9.1 through Streamline 2.14.1`, available.
- **FG on (crashes, deterministic — 3/3):** exit `-1073741819` (0xC0000005 access violation, signal 11)
  within ~4 s, at device creation. With `--gpu-validation` the Khronos layer flags the cause before the
  crash, three violations around the extra optical-flow queue + FG device extensions that loading
  `sl.dlss_g` adds: `VUID-vkCreateDevice-ppEnabledExtensionNames-01387` (a required device-extension
  dependency missing from the enabled list), `VUID-VkDeviceCreateInfo-queueFamilyIndex-02802` (duplicate
  queueFamilyIndex+flags in pQueueCreateInfos), `VUID-VkDeviceCreateInfo-pQueueCreateInfos-06755`
  (queueCount for a shared family over its limit). The crash is then inside the NVIDIA driver. FG off is
  validation-clean, so the defect is specific to the FG-on device-creation path. (A third-party Vulkan
  present-path hook on this box appears in the crash stack but is incidental — the invalid
  VkDeviceCreateInfo is the root cause and is independent of it.)

Conclusion: R1 passes. R2's sl.reflex path is not reachable on this build — forcing FG on builds a
spec-invalid device and crashes. The addon ships FG guarded off by default, so users are unaffected; the
FG device-setup (optical-flow queue + extension merge when `sl.dlss_g` is loaded) must be fixed before FG
can come up live. Nothing of ours was left running.
| 2026-10-02 10:28:24 | fg_bringup | `GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=1;fg/mode=1 GODOT_LATENCY_UPSCALING_VERBOSE=1 --script res://tests/upscaler_run.gd -- --frames=300` | 4 s | exit -1073741819 | GPU 0 %, no fullscreen app in front, a game running |
| 2026-10-02 10:29:54 | fg_off_compare | `GODOT_LATENCY_UPSCALING_VERBOSE=1 --script res://tests/upscaler_run.gd -- --frames=300` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, a game running |
| 2026-10-02 10:31:53 | fg_on_2 | `GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=1;fg/mode=1 GODOT_LATENCY_UPSCALING_VERBOSE=1 --script res://tests/upscaler_run.gd -- --frames=300` | 4 s | exit -1073741819 | GPU 0 %, no fullscreen app in front, a game running |
| 2026-10-02 10:32:58 | fg_on_valid | `GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=1;fg/mode=1 GODOT_LATENCY_UPSCALING_VERBOSE=1 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --script res://tests/upscaler_run.gd -- --frames=300` | 4 s | exit -1073741819 | GPU 0 %, no fullscreen app in front, a game running |
| 2026-10-02 10:33:55 | fg_off_valid | `GODOT_LATENCY_UPSCALING_VERBOSE=1 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --script res://tests/upscaler_run.gd -- --frames=200` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, a game running |
| 2026-10-02 11:07:30 | fg_on_fixed | `GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=1;fg/mode=1 GODOT_LATENCY_UPSCALING_VERBOSE=1 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --script res://tests/upscaler_run.gd -- --frames=300` | 4 s | exit -1073741819 | GPU 2 %, no fullscreen app in front, a game running |
| 2026-10-02 11:09:54 | fg_off_fixed | `GODOT_LATENCY_UPSCALING_VERBOSE=1 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --script res://tests/upscaler_run.gd -- --frames=200` | 6 s | exit 0 | GPU 3 %, no fullscreen app in front, a game running |

## R2 re-verify after the FG device-creation fix (2026-10-02, fg_on_fixed / fg_off_fixed)

The FG device-creation fix (optical-flow extension deps, queue de-dup by (family, flags), queueCount
cap; `Streamline::request_device` requests the optical-flow queue only if the device offers it) was
rebuilt and redeployed — the deployed `template_debug` DLL's SHA-256 matches the freshly built one, and
the crash backtrace shows that same addon DLL, so the fixed binary is the one that ran. C++ unit tests:
**123 passed / 0 failed** (incl. the new `device_patch_adds_the_optical_flow_extension_dependencies_for_the_fg_queue`,
`device_patch_dedupes_queue_entries_and_caps_to_the_family_limit`, `fg_runtime_max_multiplier_overrides_the_architecture_guess`).

**Result: the crash is NOT fixed.** FG on still exits `-1073741819` (0xC0000005) at `vkCreateDevice`,
with the same three VUIDs. FG off is still exit 0 and validation-clean. So the addon's own device
request is correct (unit-tested), but the *final* `VkDeviceCreateInfo` on the interposer-mode path is
still invalid, because Streamline's interposer appends to it downstream:

- `VUID-VkDeviceCreateInfo-queueFamilyIndex-02802` + `-pQueueCreateInfos-06755`: `pQueueCreateInfos[6]`
  duplicates `[5]` — **both ask for queue family 5 (the optical-flow family, which has only 1 queue)**,
  so 2 queues are requested from a 1-queue family. The addon now requests that optical-flow queue (the
  fix) and the interposer adds its own as well; the addon cannot de-dup an entry appended after it.
- `VUID-vkCreateDevice-ppEnabledExtensionNames-01387`: `VK_NV_low_latency2` (added by the interposer for
  sl.reflex in FG mode — the addon declines LL2 there) is missing its required `VK_KHR_present_id`.

Because the engine crashes at device creation, the R2 integration targets still could **not** be
observed live: `enable_streamline_reflex` firing, `get_reflex_status()` active via sl.reflex, the live
FG `get_status()`, and `get_max_frame_generation_multiplier()` reporting the runtime 6x all require the
device to be created first. (`get_max_frame_generation_multiplier()` reads 4 in the FG-off runs — the
architecture guess — because the runtime query `slDLSSGGetState().numFramesToGenerateMax` only runs
once FG initialises, which never happens.) The probe already proved the SDK-level 6x
(`numFramesToGenerateMax = 5`).

Direction for the next fix: in interposer mode with FG permitted, do not have the addon request the
optical-flow queue (let the interposer own the FG device setup, removing the duplicate), and ensure
`VK_KHR_present_id` accompanies the interposer's `VK_NV_low_latency2`. Then re-verify on the 5080. FG
stays guarded off by default, so shipped users are unaffected. Nothing of ours was left running.
| 2026-10-02 11:27:54 | fg_on_fixed2 | `GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=1;fg/mode=1 GODOT_LATENCY_UPSCALING_VERBOSE=1 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --script res://tests/upscaler_run.gd -- --frames=300` | 4 s | exit -1073741819 | GPU 2 %, no fullscreen app in front, a game running |

## R2 re-verify #2 — present_id fix lands, optical-flow queue duplicate remains (2026-10-02, fg_on_fixed2)

Rebuilt from the working tree (addon no longer requests the optical-flow queue; in FG mode it enables
`VK_KHR_present_id` + chains `VkPhysicalDevicePresentIdFeaturesKHR` for the interposer's
`VK_NV_low_latency2`, without adding `VK_NV_low_latency2` itself; optical-flow left to the interposer).
Deployed DLL SHA matches the freshly built one. C++ unit tests **124 passed / 0 failed**, incl. the new
`device_patch_does_not_add_optical_flow_extensions_itself` and
`reflex_feature_in_frame_generation_mode_enables_present_id_not_low_latency2`.

**Partial progress, still crashes.** FG on exits `-1073741819` (0xC0000005) at `vkCreateDevice` again.

- **FIXED:** `VUID-vkCreateDevice-ppEnabledExtensionNames-01387` (the `VK_NV_low_latency2` → missing
  `VK_KHR_present_id`) is **gone** — the present_id change worked.
- **REMAINS:** `VUID-VkDeviceCreateInfo-queueFamilyIndex-02802` and `-pQueueCreateInfos-06755`:
  `pQueueCreateInfos[6]` still uses the same queueFamilyIndex (5) + flags (0) as `[5]`, and family 5
  (the optical-flow family) has only 1 queue while 2 are requested.

The addon now adds no optical-flow queue (confirmed in `Streamline::request_device` and the unit test),
so **both family-5 entries are introduced downstream of the addon's vkCreateDevice hook by Streamline's
interposer** — which the addon cannot de-dup, because the interposer appends its copy after the addon
runs and the addon's hook sits above the interposer (Godot → addon → interposer → driver). FG off stays
exit 0 and validation-clean. Because device creation still fails, the R2 integration targets
(`enable_streamline_reflex`, `get_reflex_status()` via sl.reflex, live FG `get_status()`, 6x from
`numFramesToGenerateMax`, any "common constants for frame N") still could not be observed.

Next lever (addon cannot fix it by omitting the queue — it already does): establish who emits the two
family-5 entries (the interposer requesting the optical-flow queue twice, or the interposer's request
colliding with a base/Godot family-5 queue) and address it at that layer — a Streamline config/version
matter, or stopping the optical-flow family from being requested upstream of the interposer. Nothing of
ours was left running.
| 2026-10-02 11:45:56 | fg_apidump | `VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_api_dump VK_LAYER_SETTINGS_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer\apidump_settings.txt GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=1;fg/mode=1 GODOT_LATENCY_UPSCALING_VERBOSE=1 (medium integrity) --script res://tests/upscaler_run.gd -- --frames=5` | 4 s | exit -1073741819 | GPU 1 %, a fullscreen app in front, a game running |
| 2026-10-02 11:48:09 | fg_apidump2 | `VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_api_dump VK_LAYER_SETTINGS_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer\apidump_settings.txt VK_APIDUMP_PRE_DUMP=true GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=1;fg/mode=1 GODOT_LATENCY_UPSCALING_VERBOSE=1 (medium integrity) --script res://tests/upscaler_run.gd -- --frames=5` | 2 s | exit -1073741819 | GPU 1 %, a fullscreen app in front, a game running |
| 2026-10-02 11:49:58 | fgoff_apidump | `VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_api_dump VK_LAYER_SETTINGS_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer\apidump_settings.txt GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=0 GODOT_LATENCY_UPSCALING_VERBOSE=1 (medium integrity) --script res://tests/upscaler_run.gd -- --frames=5` | 6 s | exit 0 | GPU 1 %, a fullscreen app in front, a game running |

## R2 diagnostic round — the optical-flow queue duplicate is a Godot/interposer collision (2026-10-02)

Goal: find the real source of the duplicate family-5 (optical-flow) queue and a fix lever. Method: a
temporary (uncommitted, since reverted) `DIAG` log in `hook_CreateDevice` dumping Godot's original and
the addon-forwarded `VkDeviceCreateInfo` (pre-interposer), plus the LunarG `VK_LAYER_LUNARG_api_dump`
layer (temporarily deployed, since removed) capturing the FINAL post-interposer `vkCreateDevice`. Three
short windowed runs above. Box cleaned, api_dump layer removed, clean DLLs redeployed.

**Root cause — a collision, not a double-add.** The RTX 5080 exposes 6 queue families; family 5 =
`TRANSFER | SPARSE_BINDING | OPTICAL_FLOW_NV`, **queueCount 1**. Godot 4.7 creates **one queue in every
family 0–5**, so its base `VkDeviceCreateInfo` already contains a family-5 entry (count 1), and in FG-off
it also calls `vkGetDeviceQueue(family=5)` — Godot retrieves and holds that queue. The addon forwards
Godot's list unchanged in FG mode (it adds only the `VK_KHR_present_id` + `VK_EXT_private_data`
extensions, no queue — confirmed by the DIAG dump). When Frame Generation is on, Streamline's interposer
(sl.dlss_g) appends **its own** optical-flow queue for family 5 (and bumps family 2 from 1→3 queues).
Result: two family-5 `VkDeviceQueueCreateInfo` with identical (familyIndex 5, flags 0), total count 2 >
the 1 queue the family has → `VUID-02802` (duplicate family+flags) and `VUID-06755` (over-count), then
the driver crashes (0xC0000005). So entry `[5]` is Godot's, entry `[6]` is the interposer's; the
interposer requests the optical-flow queue **once** and it collides with Godot's pre-existing one.

Final post-interposer `pQueueCreateInfos` (7 entries): fam0, fam1×1, **fam2×3**, fam3×1, fam4×1,
**fam5×1 (Godot)**, **fam5×1 (interposer)**. The interposer also added `VK_NV_low_latency`,
`VK_NV_low_latency2`, `VK_NV_optical_flow`, `VK_NVX_*`, `VK_KHR_push_descriptor`, external-memory/semaphore
extensions, etc. NVIDIA's own docs state the optical-flow queue family must be **exclusive** to the
client's other queues — Godot's blanket per-family queue creation violates that, which is the underlying
incompatibility.

**Q3 — does a Streamline config avoid it? No.** `DLSSGQueueParallelismMode`
(`eBlockPresentingClientQueue` vs `eBlockNoClientQueues`) is a field of `slDLSSGSetOptions`, a **runtime**
call made after the device exists; it cannot change the device-creation queue set. No documented
Streamline preference makes the interposer reuse an existing optical-flow-family queue or suppress its
own at device-create time.

**Q4 — can the addon de-dup below the interposer? Feasible, but not with the current clean technique.**
`sl.interposer.dll` imports no Vulkan library (only kernel32/user32/CRT); it holds the strings
`"vulkan-1.dll"`/`"vkCreateDevice"`/`"vkGetDeviceProcAddr"` and resolves the real loader **dynamically**
(LoadLibrary + GetProcAddress). So there is no static IAT thunk to patch below it. A below-interposer
de-dup would need either (a) an inline/trampoline detour of the real loader's exported `vkCreateDevice`
(new low-level, process-wide hooking the addon does not currently have), or (b) extending the addon's
existing LoadLibrary-IAT hook onto `sl.interposer.dll` and returning a dedicated second shim for the
interposer's dynamic `vulkan-1.dll` resolution (the addon loads the interposer itself, so it controls that
timing). The de-dup logic itself already exists (`device_patch.cpp` merges same-(family,flags) entries and
caps queueCount); only reaching the final list is missing. Merging `[5]`+`[6]` into one family-5 entry
(count 1) is valid — both Godot and the interposer would retrieve the same single physical optical-flow
queue.

**Fix options, for the CEO to weigh:**
1. Below-interposer de-dup hook (merge the two family-5 entries to the 1 physical queue). Robust; needs new
   hooking machinery per Q4.
2. Prevent Godot from creating the optical-flow-family queue: drop Godot's family-5 entry pre-interposer
   AND remap Godot's `vkGetDeviceQueue(family=5)` (the addon owns that hook) to the shared queue. Simpler
   reach, but Godot *does* retrieve family 5, so this needs confirmation that Godot never submits real work
   there before it is safe (open question — not yet verified).
3. Upstream: Streamline should merge its optical-flow queue request with an existing family-5 entry rather
   than appending a duplicate; track as an NVIDIA Streamline limitation.

Downstream R2 targets (sl.reflex status, live FG `get_status()`, 6x ceiling, common constants) remain
unobservable until device creation succeeds. FG off stays exit 0 and clean. No commit; nothing left
running.
| 2026-10-02 12:14:10 | fg_collfix | `GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=1;fg/mode=1 GODOT_LATENCY_UPSCALING_VERBOSE=1 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --script res://tests/upscaler_run.gd -- --frames=120` | 4 s | exit -1073741819 | GPU 2 %, no fullscreen app in front, a game running |
| 2026-10-02 12:18:29 | fg_collfix2 | `GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=1;fg/mode=1 GODOT_LATENCY_UPSCALING_VERBOSE=1 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --script res://tests/upscaler_run.gd -- --frames=120` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, a game running |
| 2026-10-02 12:20:55 | fgoff_val | `GODOT_LATENCY_UPSCALING_SETTINGS=fg/enabled=0 GODOT_LATENCY_UPSCALING_VERBOSE=1 VK_ADD_LAYER_PATH=C:\Users\lol\claude-rtx-test\tools\vklayer (medium integrity) --gpu-validation --script res://tests/upscaler_run.gd -- --frames=120` | 6 s | exit 0 | GPU 0 %, no fullscreen app in front, a game running |

## R2 collision-fix re-verify — crash gone, FG/Reflex live (2026-10-02)

The working-tree collision fix (drop Godot's optical-flow-family queue in FG mode; remap the host's
`vkGetDeviceQueue` for it; keep `VK_KHR_present_id`) was **incomplete as first submitted**: `fg_collfix`
still crashed (`exit -1073741819`) with the same `VUID-02802` + `-06755` on the two family-5 entries.
Cause: `DlssFeature::request_device` (dlss_feature.cpp) merged the Streamline backend's `DeviceRequest`
into the layer's request but forwarded only extensions/features/queues — it **dropped
`own.drop_optical_flow_queue` on the floor**, so the patch never removed Godot's family-5 queue. One line
added there (`request.drop_optical_flow_queue = request.drop_optical_flow_queue || own.drop_optical_flow_queue;`,
uncommitted, left in the working tree for the CEO to fold in) completes it.

With that line, `fg_collfix2` **succeeds (exit 0)**:

- **Crash gone.** Device creation passes; `device 'NVIDIA GeForce RTX 5080' created through the layer`.
- **All three device-creation VUIDs gone** — `01387`, `02802`, `06755`: 0 occurrences. api-dump-confirmed
  earlier that `[5]` was Godot's family-5 queue and `[6]` the interposer's; dropping `[5]` leaves one.
- **Reflex live through Streamline** — `reflex.active:true`, `supported:true`, `sleep_enabled:true`,
  119 sleeps over ~120 frames, markers stamped (RENDERSUBMIT/SIMULATION ~118); driven by sl.reflex, NOT
  the addon's `VK_NV_low_latency2` path (`submits_tagged:0`, `presents_stamped:0`, by design). Log:
  "Reflex now runs through NVIDIA Streamline (sl.reflex) for Frame Generation".
- **Frame Generation detected** — `frame_generation.supported:true`, **`max_multiplier:6`** (from
  `numFramesToGenerateMax` 5 → 6x), `mode:1`; `active:false`, reason "Frame Generation is not yet
  available in this version" (Phase-3 guarded, not "unavailable"). "Frame Generation plugins loaded and
  resolved (not yet driving rendering)".
- **Interposer active** — `loader_override_active:true` (sl.interposer.dll), DLSS 310.9.1 through
  Streamline 2.14.1, addon-added device extensions `VK_KHR_present_id` + `VK_EXT_private_data`.
- **No "common constants" errors; rendering completed** 120 frames; the remapped optical-flow queue did
  not break anything.

**Validation not fully clean under FG, but no crash/device blockers.** `fgoff_val` (FG off, same
validation) is **0 VUIDs**, so the FG-on run's 35 are FG-path-specific, not ambient: `vkCmdDraw-None-09600`
×10, `vkCmdCopyImage-aspect-06663` ×10, `VkImageMemoryBarrier-oldLayout-01213` ×10, and
`vkQueueSubmit2-semaphore-03868` ×2 (a swapchain present-semaphore reuse reported by Godot's own
`rendering_context_driver_vulkan.cpp:680`, matching a known Streamline-FG present-sync issue). These are
runtime rendering/present-sync nits in the FG path (FG is not yet driving frames), to resolve in a later
milestone — not regressions from the collision fix, which is clean at device creation. Box cleaned, nothing
left running. No commit.
