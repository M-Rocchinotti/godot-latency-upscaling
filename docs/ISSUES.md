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

## 2. Frame Generation crashed at vkCreateDevice on the RTX 5080 (fixed, verified on the RTX 5080)

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

**True cause (5080 diagnostic).** It is a **collision**, not an interposer double-add:

- Godot 4.7's `RenderingDeviceDriverVulkan` **blanket-creates one queue in every queue family 0-5**,
  including the optical-flow family (family 5 = `TRANSFER | SPARSE | OPTICAL_FLOW_NV`, `queueCount`
  1). It even calls `vkGetDeviceQueue(family 5)` and holds the handle, but never submits real work
  there (it has no optical-flow rendering — it just creates one per family).
- In Frame Generation mode Streamline's interposer (sl.dlss_g) appends **its own** optical-flow queue
  for family 5 in its downstream `vkCreateDevice`. Now there are two family-5 entries — Godot's and
  the interposer's — and 2 > 1 available → `VUID-...-queueFamilyIndex-02802` /
  `VUID-...-pQueueCreateInfos-06755` → the invalid `VkDeviceCreateInfo` → 0xC0000005. NVIDIA's docs
  say the optical-flow family is meant to be exclusive to Streamline's queue; Godot's blanket
  creation violates that. Streamline config cannot fix it (`DLSSGQueueParallelismMode` is a runtime
  `slDLSSGSetOptions` field, after device creation).
- A separate `01387`: `VK_NV_low_latency2` (added by the interposer for sl.reflex, since the addon
  declines LL2 in Frame Generation mode) was missing its required `VK_KHR_present_id`.

**Fix (`src/vk/device_patch.cpp`, `src/vk/dispatch.cpp`, `src/dlss/streamline.cpp`, `src/reflex/reflex_ll2.cpp`).**
- **Drop the collision.** When Frame Generation is permitted, `Streamline::request_device` sets
  `DeviceRequest::drop_optical_flow_queue`, and `DeviceCreatePatch` **removes Godot's
  `VkDeviceQueueCreateInfo` for the optical-flow family** from the forwarded `pQueueCreateInfos`
  (found from the physical device's `VkQueueFamilyProperties`). After the interposer appends its own,
  there is exactly one family-5 entry (count 1) → valid. Strictly guarded: only when
  `fg_permitted`, Streamline is the backend, and the family exists; Frame Generation off → the
  forwarded list is unchanged (byte-identical).
- **Keep Godot's held handle valid.** The dispatch layer wraps `vkGetDeviceQueue`
  (`enable_optical_flow_queue_remap`, handed out only when Frame Generation wants it): a request for
  the dropped optical-flow family is remapped to index 0 — the one queue that exists there (the
  interposer's). Godot does not submit to it, so this is purely defensive; every other family and
  Frame Generation off is a pass-through.
- **present_id for sl.reflex.** `ReflexFeature::request_device` enables `VK_KHR_present_id` and chains
  `VkPhysicalDevicePresentIdFeaturesKHR` in Frame Generation mode (but not `VK_NV_low_latency2`, no
  `Ll2Device`), so the interposer's `VK_NV_low_latency2` has its dependency (fixes 01387).
- `DeviceCreatePatch::add_queues` keeps the defensive dedupe-by-`(queueFamilyIndex, flags)` and
  `queueCount`-cap hardening. (The addon does not add `VK_NV_optical_flow`; the interposer enables it
  itself for its queue.)

**Also fixed alongside.** `get_max_frame_generation_multiplier()` was hardcoded to the
architecture guess (Blackwell = 4). When Frame Generation is available at runtime it now derives
from `slDLSSGGetState().numFramesToGenerateMax` (`numFramesToGenerateMax + 1`, so 5 -> 6x), falling
back to the architecture guess when the runtime value is not available. The dev-box behaviour
(unavailable -> 1) is unchanged.

**Status.** Collision fix in place and unit-tested: `device_patch` drops Godot's optical-flow-family
queue when `drop_optical_flow_queue` is set and reports the family; the dispatch layer remaps
`vkGetDeviceQueue` for it; `ReflexFeature` enables present_id (not LL2) in Frame Generation mode. The
generic dedupe/cap hardening still holds. Build + full suite green on the GTX 1080 with Frame
Generation off (pass-through byte-identical, validation clean). The fix's runtime path is RTX-only
(the interposer must be present), so the RTX windowed Frame-Generation-on probe is to be re-run — this
should finally let `vkCreateDevice` succeed with Frame Generation on.

**Verified (RTX 5080, driver 591.86, v0.3.0 FG milestone).** The windowed Frame-Generation-on run
succeeds: the device creates cleanly, Frame Generation is active (up to 6x), Reflex runs through
`sl.reflex`, DLSS Super Resolution stays intact, UI recomposition engages, and there is **no crash
and no device loss**. On the non-FG GTX 1080 the pass-through is byte-identical to Frame Generation
off. The crash is fixed; what remains is the low-severity validation-layer polish in issue 3 below.

## 3. Validation-layer messages under Frame Generation on the RTX 5080 (open, low severity)

**Symptom.** With Frame Generation **on** on an RTX 5080 (driver 591.86), a validation-layer run
reports a small, stable set of messages inside the Frame Generation path. They are **not** crash- or
device-level: the device creates cleanly, Frame Generation generates, DLSS Super Resolution and the
UI recomposition work, and there is no device loss (issue 2). They do **not** appear with Frame
Generation off (that path is validation-clean), and they were present **before** the HUD-less / UI
recomposition fix and are unchanged by it — so they are **independent of the HUD path**, not caused
by the HUD-less / UI-layer tagging.

The messages, and where each resource comes from:

- **`vkCmdDispatch` `VUID-vkCmdDispatch-magFilter-04553`** — a sampler with `magFilter = LINEAR` is
  used to sample an `R8G8_UINT` image (a motion / ID buffer) in a compute dispatch, and integer
  formats are not linearly filterable; alongside it a **storage-image format mismatch** is flagged
  (an image view as `R16G16B16A16_UNORM` over an `R16G16B16A16_SFLOAT` resource).
  - *Attribution.* The addon's own tagged motion vectors are the filled velocity copy, created as
    `R16G16B16A16_SFLOAT` (`src/godot/velocity_fill.cpp`, `src/godot/upscaler.cpp`), **not**
    `R8G8_UINT`, and the addon runs no compute dispatch that samples an `R8G8_UINT` buffer with a
    LINEAR sampler (its reference-upscaler dispatch samples the colour; DLSS-G replaces the marker
    dispatch with its own evaluation). The `R8G8_UINT` buffer and the dispatch are therefore
    **internal to NVIDIA's Streamline / DLSS-G** (`sl.dlss_g`) and **not fixable here**; the stated
    fix (a NEAREST sampler, or a filterable format) would be inside NVIDIA's code.
  - The storage-image format mismatch is the one item that **might** be touchable from the plugin
    side: the addon reports each tagged resource's `nativeFormat` from Godot's data format
    (`src/dlss/streamline.cpp`), so if DLSS-G builds its storage view from that, matching the format
    the addon reports could resolve it. Whether the mismatched view is the addon's reported format or
    a DLSS-G-internal view is **not established** from a static read; it needs a validation run that
    captures the offending `VkImageView`'s object chain. Treated as uncertain, not yet actionable.
- **`vkCmdCopyImage` `VUID-vkCmdCopyImage-aspect-06663`** paired with **`VkImageMemoryBarrier`
  `VUID-VkImageMemoryBarrier-oldLayout-01213`** — a copy target was created without
  `VK_IMAGE_USAGE_TRANSFER_DST_BIT` (so the copy region's aspect and the image's old layout do not
  validate for a transfer destination).
  - *Attribution.* The addon's **own** Super Resolution output already carries
    `VK_IMAGE_USAGE_TRANSFER_DST_BIT` (created with `TEXTURE_USAGE_CAN_COPY_TO_BIT`,
    `src/godot/upscaler.cpp`, added earlier to satisfy `VUID-vkCmdClearColorImage-image-00002`), so
    the faulting copy target is **not** the addon's SR output. Because these messages are present
    without the HUD path, the copy target is **internal to DLSS-G's Frame Generation present path**
    (NVIDIA's `sl.dlss_g` / the interposer) rather than a game-provided HUD-less / UI layer, and is
    therefore **not fixable here**. (Were it ever traced to a game-provided `SubViewport` texture, the
    fix would be game-side — create that texture with copy-to usage — still not the addon's.)

**Severity and status.** Low. No crash, no device loss, no wrong output beyond the documented
present-only HUD ghosting; Frame Generation off is validation-clean. These are the remaining Frame
Generation polish items on the road to **1.0**: the storage-image format mismatch is the only
candidate plugin-side fix and is unconfirmed; the rest are internal to NVIDIA's Streamline / DLSS-G.
A validation run that captures the offending object handles is the next step to confirm the
attribution before 1.0.
