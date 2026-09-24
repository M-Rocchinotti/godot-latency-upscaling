# Cross-platform no-op stub

The full library (`godot_latency_upscaling.windows.*.x86_64.dll`) is **Windows x86_64 only** — it
works by patching Godot's Vulkan loader on Windows, which has no equivalent on the other platforms.

Without a library entry for a platform, Godot prints **"No GDExtension library found"** when a game
that ships this addon is exported to it. To avoid that, `latency_upscaling_stub.c` is a tiny library
built for the other desktop platforms. It uses only the GDExtension C ABI (no godot-cpp, no libc), so
one source file compiles for every target. It registers the same `LatencyUpscaling` singleton, whose
methods report that the platform is not supported:

| method | returns |
|---|---|
| `is_reflex_supported()` | `false` |
| `is_reflex_active()` | `false` |
| `get_reflex_status()` | `"not supported on this platform"` |
| `get_version()` | the version string |
| `get_status()` | `{ "platform_supported": false, "reason": "not supported on this platform" }` |

A game can therefore query the singleton the same way on every platform and degrade gracefully.

## Building

```
python stub/build_stubs.py     # into addons/latency_upscaling/bin/
```

Requires [zig](https://ziglang.org) (set `ZIG`, put `zig.exe` on `PATH`, or `E:\zig\zig.exe`). zig
ships the cross sysroots — including macOS — so all targets build from Windows with no other SDK.
`tools/package.py` runs this automatically. The built stubs, like the DLLs, are git-ignored build
artifacts bundled into the release zip.

## Platforms covered

| feature tag | triple | output |
|---|---|---|
| `linux.x86_64` | `x86_64-linux-gnu` | `.so` |
| `linux.arm64` | `aarch64-linux-gnu` | `.so` |
| `macos.x86_64` | `x86_64-macos` | `.dylib` |
| `macos.arm64` | `aarch64-macos` | `.dylib` |
| `windows.arm64` | `aarch64-windows-gnu` | `.dll` |

## Verification

`stub/test_stub.py` builds the stub as a **Windows x86_64** DLL, swaps it in for the real debug DLL,
and runs a headless Godot probe asserting the singleton reports "not supported". The GDExtension C
ABI is identical across operating systems and the stub makes no OS calls, so this Windows-x64 run
validates the exact same source that the Linux/macOS/Windows-arm64 stubs are built from. Those
targets are **not run on hardware here**; only Windows x86_64 is runtime-verified.

macOS entries are per-architecture. A **universal** macOS export (both architectures in one binary)
needs a universal stub — build the two `.dylib`s and combine them with `lipo` on a Mac — or export
for a single architecture.
