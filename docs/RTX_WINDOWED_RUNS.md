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
