# Latency & Upscaling for Godot 4.7 (Phase 1: NVIDIA Reflex)

Adds NVIDIA Reflex to Godot 4.7 Vulkan projects on Windows, with no project setup. DLSS Super
Resolution / DLAA and Frame Generation are planned. Version 0.1.0.

## Install

1. Copy this folder to `res://addons/latency_upscaling/` (keep the path) with its `bin/` DLLs.
2. Open or restart the editor. The GDExtension loads by itself; Reflex is On in games.
3. Optional: enable the plugin **Latency & Upscaling** for a status report and a Tools menu entry.
   The addon works without the plugin.

Requirements: Windows 10/11 x64, Godot 4.7.x with the Vulkan driver (Forward+ or Mobile), an
NVIDIA GPU whose driver exposes `VK_NV_low_latency2` (a 2024 or newer NVIDIA driver). On anything
else the addon does nothing and says why.

## Use

```gdscript
LatencyUpscaling.reflex_mode = LatencyUpscaling.REFLEX_MODE_ON   # OFF, ON, ON_BOOST
LatencyUpscaling.reflex_fps_limit = 0.0                          # 0 = none, else 10..1000
LatencyUpscaling.overlay_visible = true                          # latency overlay
print(LatencyUpscaling.get_reflex_status())                      # "active" or "unavailable: ..."
print(LatencyUpscaling.get_latency_stats())                      # measured latency, last 64 frames
```

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
