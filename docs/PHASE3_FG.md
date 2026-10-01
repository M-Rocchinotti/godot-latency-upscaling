# Phase 3: DLSS Frame Generation

Design, milestones and risks for DLSS Frame Generation (FG) and Multi Frame Generation (MFG) in
godot-latency-upscaling. Phase 3 builds on the same in-process Vulkan dispatch layer as Reflex
(Phase 1) and DLSS Super Resolution (Phase 2); it adds no engine fork.

> Status: **Phase 3 in progress.** Milestone **H1** (settings, public API, stubbed detection) and
> **H2** (real capability detection of the GPU the plugin sees) are landed, **all guarded off**. No
> Streamline Frame Generation feature is loaded, the Reflex backend is not switched, and FG reports
> "unavailable" on every non-Ada GPU. Everything is hardware-independent and builds and tests on the
> GTX 1080 dev box.

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
| **H3** | The Reflex -> `sl.reflex` / `sl.pcl` switch plumbing behind a flag, off by default; direct Reflex path unchanged when FG off. | H2 |
| **H4** | Present-path scheduling scaffolding and the VSync-off / Reflex-on guards, with FG still inert. | H3 |
| **H5** | Degraded present-only FG path (interpolating the final image including the HUD), guarded. | H4 |
| **H6** | HUD-less-colour input contract and a game-side UI-layer hook for a clean FG image. | H5 |
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

Phase 3 ships as **0.3.0** once the FG path works end to end on RTX hardware. **1.0.0** follows after
RTX verification of the whole addon (Reflex + DLSS SR + FG) on the supported GPUs. Milestone H1 does
**not** bump the version (it adds only guarded-off surface); `src/core/version.h` stays `0.2.0`.
