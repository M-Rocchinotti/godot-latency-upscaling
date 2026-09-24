# NVIDIA DLSS addon for Godot 4.7 (Phase 1: Reflex)

Brings NVIDIA Reflex to Godot 4.7 Vulkan projects on Windows, with no project setup. DLSS Super
Resolution / DLAA and Frame Generation are planned. Version 0.1.0.

## Install

1. Copy this folder to `res://addons/nvidia_dlss/` (keep the path) with its `bin/` DLLs.
2. Open or restart the editor. The GDExtension loads by itself; Reflex is On in games.
3. Optional: enable the plugin **NVIDIA DLSS** (status report, Tools menu entry, and it packs
   `nvidia_dlss.cfg` into exports; without it exports use the same built-in defaults).

Requirements: Windows 10/11 x64, Godot 4.7.x with the Vulkan driver (Forward+ or Mobile), an
NVIDIA GPU whose driver exposes `VK_NV_low_latency2` (551.70 or newer). On anything else the addon
does nothing and says why.

## Use

```gdscript
NvidiaDLSS.reflex_mode = NvidiaDLSS.REFLEX_MODE_ON        # OFF, ON, ON_BOOST
NvidiaDLSS.reflex_fps_limit = 0.0                         # 0 = none, else 10..1000
NvidiaDLSS.overlay_visible = true                         # latency overlay
print(NvidiaDLSS.get_reflex_status())                     # "active" or "unavailable: ..."
print(NvidiaDLSS.get_latency_stats())                     # measured latency, last 64 frames
```

Defaults live in `nvidia_dlss.cfg`; Project Settings `nvidia_dlss/...` override them per project.
`GODOT_NVIDIA_DLSS_DISABLE=1` turns the addon off.

## Notes

- The addon installs an in-process Vulkan layer at start-up by patching `godot.exe`'s
  `LoadLibrary` imports in memory. It depends on Godot 4.7 internals and refuses other versions.
- No NVIDIA binary is included. NVIDIA, GeForce, RTX, DLSS and NVIDIA Reflex are trademarks of
  NVIDIA Corporation; this addon is not affiliated with or endorsed by NVIDIA.
- Licence: MIT (`LICENSE`); third-party notices and NVIDIA terms: `THIRD_PARTY_NOTICES.md`.
- Full documentation: the godot-nvidia-dlss repository (README.md, docs/ARCHITECTURE.md);
  planned location https://github.com/M-Rocchinotti/godot-nvidia-dlss.
