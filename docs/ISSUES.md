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

**Cause (corrected after the RTX re-verify).** Streamline runs as an **interposer**: its own
`vkCreateDevice`, downstream of the addon, adds everything its loaded features need — including Frame
Generation's optical-flow queue and `VK_NV_optical_flow`, and `VK_NV_low_latency2` for sl.reflex. The
precise validation causes were:

- **02802 + 06755:** `pQueueCreateInfos[6]` duplicated `[5]`, both queue family 5 (the optical-flow
  family, which has one queue). The addon requested an optical-flow queue **and** the interposer
  added its own downstream — a duplicate the addon cannot de-dup (the interposer appends its copy
  after the addon has run). The first fix attempt (addon requests the optical-flow queue) went the
  wrong direction and caused this.
- **01387:** `VK_NV_low_latency2` (added by the interposer for sl.reflex, since the addon declines
  LL2 in Frame Generation mode) was missing its required `VK_KHR_present_id` in
  `ppEnabledExtensionNames` — the interposer does not add present_id itself.

**Fix (`src/dlss/streamline.cpp`, `src/reflex/reflex_ll2.cpp`, `src/vk/device_patch.cpp`).**
- The addon does **not** request the optical-flow queue (`Streamline::request_device` reverted) and
  `device_patch` does **not** add `VK_NV_optical_flow` — the interposer owns the whole optical-flow
  setup (queue + extension + its deps). Reasoning: a `VK_QUEUE_OPTICAL_FLOW_BIT_NV` queue cannot be
  requested without `VK_NV_optical_flow` enabled, so the interposer, which adds the queue, must enable
  the extension; adding it in the addon only risks a conflict. (If the RTX re-verify shows a 01387 for
  `VK_NV_optical_flow` itself, the extension — not the queue — would be re-added; not expected.)
- When Frame Generation is permitted, `ReflexFeature::request_device` now enables `VK_KHR_present_id`
  and chains `VkPhysicalDevicePresentIdFeaturesKHR` (reusing the LL2 plumbing) but **not**
  `VK_NV_low_latency2`, and `on_device_created` creates **no** `Ll2Device` — so the interposer's
  sl.reflex `VK_NV_low_latency2` has present_id present (fixes 01387) while the addon's LL2 path stays
  inert. If the device lacks present_id, Frame Generation's Reflex is declined cleanly.
- `DeviceCreatePatch::add_queues` keeps a defensive hardening pass: it **merges** entries that share
  `(queueFamilyIndex, flags)` and **caps** each family's `queueCount` to its limit. It cannot see the
  interposer's downstream additions, so it does not fix the interposer duplicate — removing the
  addon's own optical-flow queue request does. Correct placements never trigger the pass; the
  Frame-Generation-off path is byte-identical.

**Also fixed alongside.** `get_max_frame_generation_multiplier()` was hardcoded to the
architecture guess (Blackwell = 4). When Frame Generation is available at runtime it now derives
from `slDLSSGGetState().numFramesToGenerateMax` (`numFramesToGenerateMax + 1`, so 5 -> 6x), falling
back to the architecture guess when the runtime value is not available. The dev-box behaviour
(unavailable -> 1) is unchanged.

**Status.** Corrected fix in place and unit-tested: `ReflexFeature::request_device` in Frame
Generation mode enables present_id and not `VK_NV_low_latency2` with no `Ll2Device`; `device_patch`
adds no optical-flow extensions; the generic dedupe-by-`(family, flags)` and `queueCount`-cap
hardening still holds. Build + full suite green on the GTX 1080 with Frame Generation off
(pass-through byte-identical, validation clean). The fix's runtime path is RTX-only (interposer
present), so the RTX windowed Frame-Generation-on probe is to be re-run to confirm the crash is gone.
