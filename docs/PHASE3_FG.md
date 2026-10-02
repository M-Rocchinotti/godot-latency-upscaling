# Phase 3: DLSS Frame Generation

Design, milestones and risks for DLSS Frame Generation (FG) and Multi Frame Generation (MFG) in
godot-latency-upscaling. Phase 3 builds on the same in-process Vulkan dispatch layer as Reflex
(Phase 1) and DLSS Super Resolution (Phase 2); it adds no engine fork.

> Status: **Phase 3 shipped as 0.3.0.** Milestones **H1-H6 (plugin side)** are landed and **verified
> on an RTX 5080** (driver 591.86): the device creates cleanly, Frame Generation is active (up to 6x),
> Reflex runs through sl.reflex, DLSS Super Resolution is intact, and the HUD-less + UI recomposition
> path **engages** (clean UI on real and generated frames), with no crash or device loss. Frame
> Generation is **off by default and gated on real FG hardware**, so with it off — or permitted on a
> non-FG GPU — the Reflex / DLSS / pass-through behaviour and the Vulkan device are byte-identical
> (verified on the GTX 1080). Real generation runs only on an RTX 40 series+. Without the game-side
> HUD-less input the fallback is present-only (the UI ghosts). The matching **game-side** HUD
> restructuring (render the HUD on a transparent SubViewport, call the two setters) is a separate,
> larger, per-game change and is **still deferred** in crater-zombie-fps (see H6). A few low-severity
> validation-layer messages under FG remain (`docs/ISSUES.md` #3).

## What Frame Generation is

DLSS Frame Generation inserts AI-generated frames between rendered frames: FG on an RTX 40 series
(Ada) GPU generates one frame per rendered frame (2x); Multi Frame Generation on an RTX 50 series
(Blackwell) GPU generates up to three (3x / 4x). It raises the displayed frame rate but **not**
input responsiveness, so NVIDIA requires Reflex to be on whenever FG is on, to keep latency in
check. FG trades a little latency and VRAM for smoothness; it is most useful when the base frame
rate is already reasonable (NVIDIA recommends roughly 60 fps base before FG).

## How it fits this addon

The addon already installs a Vulkan dispatch layer in front of Godot at start-up and intercepts the
present path (Phase 1) and the DLSS evaluation at the upscaler's marker dispatch (Phase 2). Frame
Generation reuses that foundation:

- **The interposer is already in the present path.** Streamline's interposer sits in front of the
  Vulkan loader whenever the NVIDIA runtime is loaded (Phase 2). FG hooks the same present path to
  schedule the generated frames; no new hook point is invented.
- **Load `sl.dlss_g`, `sl.reflex`, `sl.pcl`.** FG needs Streamline's Frame Generation plugin
  (`sl.dlss_g`), and it requires Reflex and PC Latency markers to go through Streamline's own
  plugins (`sl.reflex`, `sl.pcl`) rather than the addon's direct `VK_NV_low_latency2` path. These
  are fetched with the rest of the NVIDIA runtime (never shipped in this repo).
- **Mandatory Reflex -> sl.reflex switch when FG is on.** With FG on, the addon must stop driving
  Reflex directly through `VK_NV_low_latency2` and hand the Reflex markers and sleep to
  `sl.reflex` / `sl.pcl`, because Streamline's FG scheduler and Reflex must agree on frame pacing.
  When FG is off the addon keeps its existing direct Reflex path (unchanged from Phase 1/2). This
  switch is the single riskiest behavioural change of Phase 3.
- **HUD / UI layer.** FG interpolates the whole presented image, including any UI drawn into it,
  which smears moving HUD elements. A clean result needs the game to render its UI on a separate
  layer that FG is told to leave alone (Streamline's UI/HUD-less colour input). Godot does not
  expose that by default, so a game-side UI-layer change is required for a clean FG image;
  present-only FG (interpolating the final image including the HUD) is a **degraded** fallback the
  addon can offer without engine or game changes.
- **VSync off on Vulkan.** FG schedules its own present timing; on Vulkan it requires VSync off
  (the swapchain must not block the generated presents). The addon will refuse FG, with a reason,
  when VSync is on, rather than fight the swapchain.

## Public API (milestone H1, landed)

On the `LatencyUpscaling` singleton (reach it through `Engine`, as in the README):

| Member | Meaning |
|---|---|
| `frame_generation` (`FRAME_GENERATION_OFF`, `FRAME_GENERATION_ON`) | FG mode (property) |
| `mfg_multiplier: int` | Multi Frame Generation multiplier: `0` Auto, else `2` / `3` / `4` (property) |
| `is_frame_generation_supported() -> bool` | a GeForce RTX 40 series GPU or newer could run FG |
| `is_frame_generation_active() -> bool` | FG is generating frames now (always false in H1) |
| `get_frame_generation_status() -> String` | `"active (...)"`, `"off: ..."` or `"unavailable: <reason>"`, worded like `get_dlss_status()` |
| `get_max_frame_generation_multiplier() -> int` | the hardware's ceiling (1 = none, 2 = Ada, 4 = Blackwell) |
| `set_hudless_source(texture: RID)` (H6) | the 3D scene without the HUD (root viewport), for clean-UI FG; empty RID clears |
| `set_ui_layer(texture: RID)` (H6) | the HUD on its own transparent layer (colour + alpha); empty RID clears |

Both H6 setters are no-ops unless DLSS Frame Generation is driving and both are set; see "Clean UI on
generated frames" below.

Project settings (`latency_upscaling/...`), read at CORE init like the rest:

| Setting | Default | Meaning |
|---|---|---|
| `fg/enabled` | `false` | permit FG at startup (restart needed; the runtime is only loaded when on) |
| `fg/mode` | `0` (Off) | 0 Off, 1 On |
| `fg/multiplier` | `0` (Auto) | 0 Auto, 2 / 3 / 4 (Multi Frame Generation) |

`get_status()["upscaling"]["frame_generation"]` reports `{supported, active, mode, multiplier,
max_multiplier, reason}`. In H1 on every GPU: `supported=false`, `active=false`, `max_multiplier=1`,
with the reason (on this GTX 1080) `"needs a GeForce RTX 40 series GPU"`.

## Capability detection

`dlss::check_frame_generation(const FgCaps &)` is the real capability logic (pure C++, unit-tested
with injected fake caps): it refuses non-NVIDIA GPUs, anything older than Ada, drivers below the FG
minimum, a missing Vulkan device, and a known-bad OS / HAGS state, each with a player-actionable
reason.

**Milestone H2** fills `FgCaps` with the real GPU the plugin sees at start-up:
`bootstrap.cpp::resolve_fg_caps()` runs the `gpu_precheck` adapter probe (`dlss::check_adapters`,
the same temporary-instance enumeration DLSS uses, through the real Vulkan loader, before Godot's
device exists), reads the first adapter (Godot's default), and classifies the architecture with the
pure `dlss::classify_nvidia_architecture(vendor, name, device_id)` — GeForce RTX 50xx -> Blackwell
(MFG, max 4), RTX 40xx or an "Ada" name -> Ada (FG, max 2), RTX 30xx/20xx and every GTX -> pre-Ada,
non-NVIDIA or an unplaceable NVIDIA name -> unknown (conservative, unsupported). The driver is
decoded with `dlss::nvidia_driver_version`; below 527.64 the reason notes that older drivers offer
FG only through Direct3D interop, which this Vulkan addon cannot use. The OS / HAGS requirement is
**not probed yet** (`os_known` false): its real gate belongs to the milestone that loads FG, and
leaving it unknown avoids falsely refusing a capable GPU. No NVIDIA runtime is loaded and the Reflex
backend is untouched. On the GTX 1080 dev box the probe classifies Pascal, so FG stays unavailable:
`"NVIDIA GeForce GTX 1080 does not support Frame Generation (needs a GeForce RTX 40 series GPU or
newer)"`.

## Driving Frame Generation (H5, present-only / Option A)

Frame Generation is driven only when the hardware supports it, `frame_generation` is On, a DLSS
Super Resolution / DLAA upscaler is active, and VSync is off (`fg_availability()`, pure and
unit-tested; `LatencyUpscaling::update_frame_generation_request()` evaluates it each frame and pushes
the request to the backend only when it changed). The backend (`dlss::Streamline`) drives it inside
its SR `evaluate()` on the viewport it already upscales: `slDLSSGSetOptions(viewport, {mode eOn,
numFramesToGenerate, flags eRetainResourcesWhenOff})`, where `numFramesToGenerate` comes from the MFG
multiplier (`fg_num_frames_to_generate`: 2x->1, 3x->2, 4x->3, Auto->`numFramesToGenerateMax`). The
depth and motion vectors SR tagged that frame are the mandatory pair; the backbuffer is intercepted
in interposer mode — no extra tagging. `slDLSSGGetState` feeds the live status.

Status strings (`get_frame_generation_status()`): `"unavailable: <reason>"` (non-RTX etc.),
`"off: Frame Generation is off (set frame_generation to On)"`, `"off: needs a DLSS or DLAA upscaler
(Frame Generation runs on the Super Resolution path)"`, `"unavailable: Frame Generation needs VSync
off"`, and `"active (Nx, WxH -> M presented)"` once generating (N = multiplier, WxH = render size,
M = `numFramesActuallyPresented`).

**One frame token per frame (the coupling the plan flagged).** Streamline's commonInterface keys a
frame's constants on the frame token, so every subsystem that touches a frame must use the *same*
token: the sl.reflex markers / slReflexSleep, the SR `slSetConstants` / `slEvaluateFeature`, and the
FG present. The first H5 attempt let each mint its own, so SR set the constants under one id and the
FG present looked them up under another — Streamline logged `Unable to find 'common' constants for
frame N` every frame, disabled FG per-frame, skipped the out-of-order present, and the pacer's
semaphore wait timed out into `VK_ERROR_DEVICE_LOST`. The fix is one `reflex::FrameTokenSource`
(owned by `dlss::Streamline` when the FG plugins load): `advance()` mints the next monotonic token at
the Reflex sleep (frame start), and `current()` returns it for the rest of the frame — the reflex
markers/sleep (via `SlReflexApi::token_source`), the SR `evaluate()`, and FG all use it. This targets
Godot's default thread model (render on the main thread), where a frame's SL calls are sequential;
Frame Generation under the experimental separate render thread is not supported. (If the out-of-order
present / semaphore timeout persists once the token is unified and FG is actually generating, it is a
present-path issue for the HUD-less restructuring, not the token.)

**Present-only means the UI ghosts on generated frames.** H5 does not tag a HUD-less colour buffer
or a separate UI layer, so Streamline interpolates the final image including the HUD. That is the
documented Option-A trade-off; H6 adds the HUD-less input for a clean image.

**Validation nits deferred to H6 / inherent to present-only** (seen on the 5080 with FG on): the
`vkCmdDraw-09600` / `vkCmdCopyImage-06663` / `VkImageMemoryBarrier-01213` / `vkQueueSubmit2-03868`
messages arise inside Streamline's own Frame Generation present path and from interpolating an image
whose UI was not tagged HUD-less. They are not from the addon's SR tagging or options; a clean
HUD-less tag path (H6) is expected to resolve the UI-related ones, and the Streamline-internal ones
are NVIDIA's. FG off is validation-clean.

## Clean UI on generated frames (H6, HUD-less + UI recomposition)

**Plugin side — landed.** Two engine-API methods let a game hand FG a clean UI instead of the
present-only composite:

- `LatencyUpscaling.set_hudless_source(texture: RID)` — the 3D scene without the HUD (the root
  viewport, post-tonemap), as a RenderingServer texture RID.
- `LatencyUpscaling.set_ui_layer(texture: RID)` — the HUD on its own (a transparent SubViewport),
  colour + alpha, as a RenderingServer texture RID. An empty RID clears either one.

They are stored on the main thread (`Upscaler::set_hudless_source` / `set_ui_layer`, on the snapshot)
and resolved to Vulkan images on the render thread (`game_evaluate_image`: a RenderingServer texture
RID -> its backing RenderingDevice texture via `texture_get_rd_texture` -> `evaluate_image`; a RID
that does not resolve leaves the image invalid). When Frame Generation is driving **and** both images
resolved, `dlss::Streamline::evaluate()` tags `kBufferTypeHUDLessColor` from the HUD-less source and
`kBufferTypeUIColorAndAlpha` from the UI layer alongside the four Super Resolution tags, and
`drive_frame_generation()` sets `DLSSGOptions.enableUserInterfaceRecomposition = true`, so DLSS-G
interpolates the HUD-less colour and recomposes the UI cleanly onto real **and** generated frames.

The decision is the pure, unit-tested `fg_ui_recomposition(fg_driving, hud_less_valid, ui_valid)`
(`src/dlss/fg_feature.{h,cpp}`): all three required, else present-only (four tags, recomposition off)
**unchanged** — a game that provides neither, or only one, is never regressed, and FG off stays
byte-identical. This is expected to resolve the UI-related present-only validation nits above
(`vkCmdDraw-09600` / `vkCmdCopyImage-06663` / `VkImageMemoryBarrier-01213`), which stem from
interpolating an untagged composited image. **(RTX-only to verify; the dev box cannot run FG.)**

**Game side — not done yet (blocker, see below).** For the plugin API to do anything a game must
render its HUD on a transparent SubViewport so the root viewport is HUD-less, keep the weapon
view-model in the 3D viewport, and call the two setters while FG/DLSS is active. In crater-zombie-fps
the in-game HUD is a `CanvasLayer` stack (hosting the pause / results / loading menu layers and a
workbench sheet on a higher layer) whose elements (`hud_minimap`, `hud_damage_indicators`,
`hud_blood_overlay`, `hud_vignette`) reach the player camera with `get_viewport().get_camera_3d()` —
which returns null inside a SubViewport — and whose interactive menus rely on the root viewport for
GUI/mouse input (29 UI files touch `get_viewport()` / `get_window()`). Moving the HUD into a
SubViewport is therefore a large, high-risk restructuring that cannot be visually verified on the
GTX 1080 dev box (FG needs an RTX 40+). Per the plan's guidance ("keep present-only FG rather than
break the HUD") the game-side move is deferred; the plugin API is in place for when it is done.

## Requirements (target)

| Feature | GPU | Driver | Other |
|---|---|---|---|
| Frame Generation | GeForce RTX 40 series (Ada) or newer | 520+ (527.64+ for native Vulkan optical flow) | Windows 10 20H1+, hardware-accelerated GPU scheduling on, VSync off |
| Multi Frame Generation | GeForce RTX 50 series (Blackwell) | as FG | as FG |

## Milestones

Hardware-independent milestones (**H**) build and test on the GTX 1080 dev box; RTX-only milestones
(**R**) need an RTX 40/50 GPU and are verified on the remote RTX box (`tools/rtx_remote.py`).

| ID | Scope | Depends on |
|---|---|---|
| **H1** | Settings, public API surface, stubbed support detection, status dict, docs. All guarded off; behaviour byte-identical to Phase 2. **Landed.** | — |
| **H2** | Real capability detection of the GPU the plugin sees (vendor / architecture / driver via the gpu_precheck probe), still guarded off; OS / HAGS deferred. **Landed.** | H1 |
| **H3** | Load the Streamline Frame Generation plugins (`sl.dlss_g`, `sl.reflex`, `sl.pcl`) into slInit when FG is permitted and resolve their functions on the device; ship the FG DLLs with Windows exports. Guarded off: no rendering uses them and the Reflex backend is not switched. **Landed.** | H2 |
| **H4** | The Reflex backend switch: `SlReflexDevice` drives Reflex through sl.reflex / sl.pcl (same contract as `Ll2Device`, reusing `FrameSequencer`), chosen over VK_NV_low_latency2 when FG is permitted. LL2 kept out of the device; LL2 path byte-identical when FG off. **Landed.** | H3 |
| **H5** | Drive present-only Frame Generation: `slDLSSGSetOptions(eOn, numFramesToGenerate)` on the SR viewport (reusing SR's depth + motion-vector tags, backbuffer auto-intercepted), the VSync-off / SR-active guards, and the live `slDLSSGGetState` status. UI ghosts on generated frames (no HUD-less tagging yet). **Landed.** | H4 |
| **H6** | Clean UI on generated frames. **Plugin side landed:** `set_hudless_source` / `set_ui_layer` API, HUD-less + UI recomposition tagging (`kBufferTypeHUDLessColor` / `kBufferTypeUIColorAndAlpha`, `enableUserInterfaceRecomposition`), pure unit-tested `fg_ui_recomposition`; FG off byte-identical, present-only unchanged when the game provides neither. **Game side deferred** (crater-zombie-fps HUD is a CanvasLayer stack whose elements reach `get_viewport().get_camera_3d()` and whose menus route input through the root viewport; the SubViewport move is a large, RTX-only-verifiable change). | H5 |
| **R1-R6** | The RTX-only counterparts: load `sl.dlss_g`, activate FG, verify the Reflex switch, measure latency/throughput, MFG multipliers, image quality. | H1-H6 |

## Risks

- **Reflex backend switch (highest).** Moving Reflex from the addon's direct `VK_NV_low_latency2`
  path to `sl.reflex` / `sl.pcl` when FG turns on, and back when it turns off, must not regress the
  Phase 1/2 latency behaviour. Mitigated by keeping the direct path the default and gating the
  switch behind FG-on, with a clean fallback.
- **HUD smearing.** Without a game-side UI layer, FG smears moving HUD elements. Present-only FG is
  offered as an honest, degraded fallback; the clean path needs a game change this addon cannot make
  by itself.
- **Latency and VRAM cost.** FG adds latency and memory; it only helps above a reasonable base frame
  rate. The addon reports status and reasons rather than promising a win.
- **Swapchain / VSync.** FG needs VSync off on Vulkan; the addon refuses FG with a reason rather than
  fighting the swapchain.
- **Hardware access.** Activation and all measurements need RTX 40/50 hardware, verified on the
  remote RTX box; the H milestones keep as much as possible hardware-independent and testable here.

## Versioning

Phase 3 ships as **0.3.0**: the Frame Generation path works end to end and is verified on RTX 5080
hardware (`src/core/version.h` is `0.3.0`). **1.0.0** follows once the remaining Frame Generation
validation-layer polish (`docs/ISSUES.md` #3) and the open Reflex-latency question on the RTX 5080
(#1) are resolved across the whole addon (Reflex + DLSS SR + FG) on the supported GPUs.
