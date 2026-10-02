# Latency & Upscaling for Godot 4.7 (NVIDIA Reflex, DLSS Super Resolution / DLAA, Frame Generation)

Adds NVIDIA Reflex, DLSS Super Resolution / DLAA and DLSS Frame Generation / Multi Frame Generation
to Godot 4.7 Vulkan projects on Windows, with no project setup. Version 0.3.0.

## Install

1. Copy this folder to `res://addons/latency_upscaling/` (keep the path) with its `bin/` DLLs.
2. Open or restart the editor. The GDExtension loads by itself; Reflex is On in games.
3. Optional: enable the plugin **Latency & Upscaling** for a status report and a Tools menu entry.
   The addon works without the plugin.

Requirements: Windows 10/11 x64, Godot 4.7.x with the Vulkan driver (Forward+ or Mobile), an
NVIDIA GPU whose driver exposes `VK_NV_low_latency2` (a 2024 or newer NVIDIA driver). On anything
else the addon does nothing and says why.

## Use

Reach the `LatencyUpscaling` singleton through `Engine`, as below: this parses and runs on every
platform and does nothing where the extension is absent. A script that names `LatencyUpscaling`
directly fails to parse wherever the extension is not loaded (see the repository README).

<!-- portable-example -->
```gdscript
extends Node


func _ready() -> void:
	if not Engine.has_singleton("LatencyUpscaling"):
		return # another platform, or the addon excluded from this export
	var lu := Engine.get_singleton("LatencyUpscaling")
	lu.set("reflex_mode", 2) # REFLEX_MODE_OFF 0, ON 1, ON_BOOST 2
	lu.set("reflex_fps_limit", 0.0) # 0 = none, else 10..1000
	lu.set("overlay_visible", true) # latency overlay
	print(lu.get_reflex_status()) # "active" or "unavailable: ..."
	print(lu.get_latency_stats()) # measured latency, last 64 frames
```

## DLSS

DLSS needs a GeForce RTX GPU (20 series or newer) and NVIDIA's runtime (Streamline 2.14.1 and the
DLSS library), which is not part of this addon. Enable the plugin: with DLSS on and the runtime
missing it shows the licence files of the pinned NVIDIA release (checked against
`nvidia_runtime.lock.json`), the download size and folder, and downloads only after you accept the
NVIDIA RTX SDKs licence (Streamline's own is MIT); it records who accepted, when and how in
`bin/nvidia/licence_acceptance.json`. From the command line:
`python tools/fetch_nvidia_sdk.py --download --accept-nvidia-licences --accepted-by "Your Name"`.
Windows exports carry the runtime in `nvidia/` beside the game while the plugin is enabled (an
export without it warns). The addon resets DLSS's history on camera cuts by itself; call
`reset_dlss_history()` on cuts it cannot see (a short teleport). NVIDIA's licence has conditions for
any game given to others (end-user terms, the NVIDIA Marks with NVIDIA's prior approval) and a
notification before a commercial release: see `THIRD_PARTY_NOTICES.md`.

## Frame Generation

DLSS Frame Generation (RTX 40 series, 2x) and Multi Frame Generation (RTX 50 series, up to 6x) run
on the same DLSS path and the same NVIDIA runtime. Permit it in Project Settings with **both**
`latency_upscaling/fg/enabled = true` and `fg/mode = On`; it needs HAGS on and VSync off, and is
gated on real FG hardware, so a non-FG GPU is byte-identical to FG off. For a clean UI on generated
frames the game renders its HUD on a transparent `SubViewport` and hands the addon the HUD-less scene
and the UI layer (`set_hudless_source()` / `set_ui_layer()`); otherwise FG is present-only and the
HUD ghosts. Full details and the status API are in the repository README and `docs/PHASE3_FG.md`.

## Platforms

Windows x86_64 only. For exports to other platforms, exclude the addon: in the export preset,
Resources tab > "Filters to exclude files/folders from project", enter `addons/latency_upscaling/*`. Without that
filter Godot prints its standard "No GDExtension library found" error at start-up there; it is
harmless (the game runs, the singleton is simply absent) but noisy.

Settings are built-in defaults, overridden by Project Settings `latency_upscaling/...` (always
exported, so they reach exported games with or without this plugin), then by the
`GODOT_LATENCY_UPSCALING_SETTINGS` environment variable. `GODOT_LATENCY_UPSCALING_DISABLE=1` turns
the addon off.

## Notes

- The addon installs an in-process Vulkan layer at start-up by patching `godot.exe`'s
  `LoadLibrary` imports in memory. It depends on Godot 4.7 internals and refuses other versions.
- No NVIDIA binary is included. NVIDIA, Reflex and DLSS are trademarks of NVIDIA Corporation; this
  project is not affiliated with or endorsed by NVIDIA.
- Licence: MIT (`LICENSE`); third-party notices and NVIDIA terms: `THIRD_PARTY_NOTICES.md`.
- Full documentation: the godot-latency-upscaling repository (README.md, docs/ARCHITECTURE.md);
  planned location https://github.com/M-Rocchinotti/godot-latency-upscaling.

## AI-usage disclosure

This project is developed with substantial help from an AI coding assistant (Anthropic's Claude,
through Claude Code): research, design, code, tests and documentation were drafted by the
assistant and reviewed, tested and accepted by the maintainer. Measurements come from real runs
on the stated hardware, not from the assistant.
