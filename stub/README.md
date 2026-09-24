# Cross-platform no-op stub — EXPERIMENTAL, NOT SHIPPED

This directory holds a tiny raw-C GDExtension (`latency_upscaling_stub.c`) meant, in a later
release, to give the addon a library on the platforms the hook does not support (Linux, macOS,
Windows arm64). **It is not part of this release:** the `.gdextension` does not reference it,
`tools/package.py` does not ship it, and `build_stubs.py` builds into `build/stubs/`, never into the
addon. For this release, non-Windows exports exclude the addon instead (README, "Platforms").

## Why it is not shipped yet

Found in review; all must be fixed before it ships:

- **No ptrcall paths.** Every method registers `ptrcall_func = NULL`. A dynamic call
  (`Engine.get_singleton("LatencyUpscaling").get_status()`) works, but a statically typed call
  goes through ptrcall and **crashes (signal 11)**.
- **No properties or constants.** The real class has `reflex_mode`, `reflex_fps_limit`,
  `overlay_visible` and `REFLEX_MODE_*`; the stub has none, so code written for the real API
  (`LatencyUpscaling.reflex_mode = 1`) fails to parse against it. It must expose the full API.
- **macOS binaries are unverified.** `zig cc` produced dylibs with a minimum OS of 13 and a local
  `install_name`; neither was checked against Godot's macOS export or a real Mac.
- **Nothing is verified on a real OS.** The only runtime check (`test_stub.py`) loads a
  Windows-x64 build of the same source into Godot on Windows. The GDExtension C ABI is the same on
  every OS, which makes that check meaningful for the logic, but not for loading, linking or
  signing on Linux and macOS.

Already fixed: the stub no longer leaks (every temporary String/StringName/Dictionary/Variant is
destroyed; the singleton object and the class are released at SCENE deinit). `test_stub.py` runs
Godot with `--verbose` and fails on any "Leaked instance" or "Orphan StringName" report.

## What it does

Registers the `LatencyUpscaling` singleton (a class extending `Object`) with:

| method | returns |
|---|---|
| `is_reflex_supported()`, `is_reflex_active()` | `false` |
| `get_reflex_status()` | `"not supported on this platform"` |
| `get_version()` | the version string |
| `get_status()` | `{ "platform_supported": false, "reason": "not supported on this platform" }` |

## Building and checking

```
python stub/build_stubs.py     # linux x86_64/arm64, macos x86_64/arm64, windows arm64 -> build/stubs/
python stub/test_stub.py       # Windows-x64 build of the same source, swapped in for the debug DLL
```

Requires [zig](https://ziglang.org) (`ZIG`, `PATH`, or `E:\zig\zig.exe`); zig ships the cross
sysroots, including macOS, so every target builds from Windows. Neither script is part of
`tests/run_all.py`, because nothing here ships.
