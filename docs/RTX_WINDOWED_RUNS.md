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
