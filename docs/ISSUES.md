# Known issues

## 1. Reflex shows no latency reduction on the RTX 5080 (open)

**Symptom.** With the addon on an RTX 5080 (driver 591.86), `get_reflex_status()` is `active` and
every mode (Off / On / On + Boost) reaches the driver (`reflex_mode` follows), but the measured
latency `sim_start -> gpu_end` (`get_latency_stats().sim_start_to_gpu_end_ms`, from
`vkGetLatencyTimingsNV`) is the same in all three modes: about **23 ms**, over two rounds of 360
frames each. Reflex On is expected to lower latency when the GPU is the bottleneck; here it does
not.

**Where the data comes from.** The game Hollow Front (`E:\crater-zombie-fps`), an exported release
build with this addon, run through `tools/rtx_dlss_test.py` (which drives `tools/rtx_remote.py`),
Havana's boulevard, the Ultra graphics tier so the GPU is the bottleneck (GPU ~5.6 ms of ~5.9 ms
frames), V-Sync off, 2026-09-26:

| Reflex | latency sim->gpu-end (ms, median) | p95 | frame interval (ms) |
|---|---|---|---|
| Off | 23.3 | 26.6 | 5.93 |
| On | 23.4 | 24.4 | 5.89 |
| On + Boost | 23.5 | 24.4 | 5.97 |

(Round two agreed within 0.3 ms.) The reference GTX 1080 (README "Reflex results") DID show a gain
there: 50 -> 41 ms (-18 %), and its own breakdown found the dominant interval to be
**present_end -> gpu_start, ~27 ms, cause not established** (README's note). The RTX numbers are
consistent with that same gap dominating and swamping any Reflex sleep, but the stage breakdown was
not captured on the RTX in these runs.

**Caveat — the measurement must be redone at a window no larger than the screen.** These RTX runs
used a 3440x1440 window on a 2560x1080 screen. A window larger than the display can be clamped or
composited in ways that distort present/scanout timing, so these numbers are not a clean basis for
a conclusion. A re-measure at <= the screen (1920x1080 or 2560x1080 borderless), recording the
Reflex sleep time and the full stage breakdown (simulation / render submit / present /
present_end->gpu_start / gpu render), is the next step. The probe (`crater-zombie-fps`
`tools/rtx_dlss_probe.gd`, `_reflex`) now records `reflex_sleep_ms`, `present_end_to_gpu_start_ms`
and `breakdown_ms` for exactly this. As of 2026-10-01 that run is pending: the RTX PC's preflight
reported a game running, so no window was opened.

**What it suggests so far.** Not established. The leading hypothesis from the GTX 1080 — a large,
unexplained present_end -> gpu_start wait that the Reflex sleep does not remove — would, if it also
holds on RTX, explain the flat latency: the sleep has nothing to shave because the frame is not
waiting where Reflex acts. Whether that gap is DWM composition (windowed), the swapchain present
mode, or the render graph's wait before each frame (`rendering_device_driver_vulkan.cpp` on the
GTX 1080) needs the breakdown above, plus a comparison of windowed vs. the game's borderless
fullscreen path, and ideally a run with V-Sync and VRR off confirmed on that monitor.

**Not a blocker for the game integration.** Reflex is correct where it helps (the GTX 1080); on the
RTX 5080 it is active and harmless. The game's settings expose it as NVIDIA asks and leave it On by
default. This issue is about the addon's latency result, not the integration.

## 2. Frame Generation crashed at vkCreateDevice on the RTX 5080 (fixed, pending RTX re-verify)

**Symptom.** Phase 3 RTX bring-up on an RTX 5080 (driver 591.86): with Frame Generation **off** the
addon is validation-clean and R1 passed (DLSS_G supported, `numFramesToGenerateMax = 5`, i.e. up to
6x). With Frame Generation **on** the engine crashed deterministically at Vulkan device creation,
*before* Streamline initialised — the validation layer caught a spec-invalid `VkDeviceCreateInfo` on
the Frame Generation optical-flow-queue path:

- `VUID-vkCreateDevice-ppEnabledExtensionNames-01387` — a required device-extension dependency was
  missing: `VK_NV_optical_flow` was enabled without its dependencies.
- `VUID-VkDeviceCreateInfo-queueFamilyIndex-02802` — two `VkDeviceQueueCreateInfo` for the same
  `(queueFamilyIndex, flags)`.
- `VUID-VkDeviceCreateInfo-pQueueCreateInfos-06755` — a family's `queueCount` exceeded its limit.

**Cause.** The addon did not set the device up for Frame Generation itself, so the optical-flow
queue / `VK_NV_optical_flow` ended up added without its dependency extensions, duplicated against an
existing family entry, and over the family's queue limit.

**Fix (`src/vk/device_patch.cpp`, `src/dlss/streamline.cpp`).**
- The addon now requests the optical-flow queue itself, but only when the device actually offers an
  optical-flow queue family and `VK_NV_optical_flow` with its dependency extensions
  (`Streamline::request_device`, gated on `fg_permitted`). If the device cannot satisfy them, Frame
  Generation is skipped (unavailable) and DLSS Super Resolution is unaffected — never an invalid
  create.
- When the optical-flow queue is requested, `DeviceCreatePatch` adds `VK_NV_optical_flow` **and its
  <1.3 device-extension dependencies** `VK_KHR_format_feature_flags2` and `VK_KHR_synchronization2`
  (vk.xml `depends`; Godot 4.7 creates a Vulkan 1.2 device, so none are core) — fixes 01387.
- `DeviceCreatePatch::add_queues` now hardens the result: it **merges** any entries that share
  `(queueFamilyIndex, flags)` (fixes 02802) and **caps** each family's `queueCount` to
  `VkQueueFamilyProperties::queueCount` (fixes 06755). Correct placements never trigger either pass;
  the default Frame-Generation-off path is byte-identical.

**Also fixed alongside.** `get_max_frame_generation_multiplier()` was hardcoded to the
architecture guess (Blackwell = 4). When Frame Generation is available at runtime it now derives
from `slDLSSGGetState().numFramesToGenerateMax` (`numFramesToGenerateMax + 1`, so 5 -> 6x), falling
back to the architecture guess when the runtime value is not available. The dev-box behaviour
(unavailable -> 1) is unchanged.

**Status.** Fixed and unit-tested (the device-create assembly: dependency added, no duplicate
`(family, flags)`, `queueCount` capped; the multiplier mapping). Build + full suite green on the
GTX 1080 with Frame Generation off (pass-through byte-identical, validation clean). The RTX windowed
Frame-Generation-on probe is to be re-run to confirm the crash is gone (the fix's runtime path is
RTX-only).
