# Third-party notices

The code of this repository is MIT-licensed (see `LICENSE`). This file lists the third-party
components it contains or builds on, and the terms that apply to NVIDIA technology and marks.

## Components compiled into the addon library

### godot-cpp 10.0.0 (MIT)

`thirdparty/godot-cpp` (git submodule), statically linked into
`addons/latency_upscaling/bin/godot_latency_upscaling.*.dll`.

```
Copyright (c) 2017-present Godot Engine contributors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Vulkan-Headers v1.4.357 (Apache-2.0 OR MIT)

`thirdparty/Vulkan-Headers` (git submodule), headers only. Copyright 2015-2026 The Khronos Group
Inc. Used under the MIT option; the licence texts are in `thirdparty/Vulkan-Headers/LICENSES/`.

### Godot Engine (MIT)

`src/dlss/jitter.cpp` reproduces Godot's camera-jitter formula
(`servers/rendering/renderer_scene_cull.cpp`, `renderer_viewport.cpp`, Godot 4.7.2).
Copyright (c) 2014-present Godot Engine contributors; (c) 2007-2014 Juan Linietsky, Ariel
Manzur. MIT licence, same text as godot-cpp above.

### NVIDIA Streamline 2.14.1 headers (MIT)

`thirdparty/streamline/include/` holds the public headers of the Streamline SDK 2.14.1 (from
`streamline-sdk-v2.14.1.zip`, NVIDIA's GitHub release), unmodified, except `sl_nvperf.h`, which is
under the NSight Perf SDK licence and is left out. They declare the Streamline API that the DLSS
backend and `tools/sl_probe` call at run time; no NVIDIA binary is linked. Licence text:
`thirdparty/streamline/LICENSE.txt`.

```
Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### NVIDIA Streamline (MIT), as a reference

`src/reflex/reflex_ll2.cpp` is an independent implementation, but its structure (sleep on a
timeline semaphore with a timeout, a lock around the swapchain handle, the report conversion)
follows Streamline's `source/platforms/sl.chi/vknvll2.cpp` (v2.14.1). No Streamline source is
included. Streamline: Copyright (c) NVIDIA CORPORATION, MIT licence
(https://github.com/NVIDIA-RTX/Streamline/blob/main/license.txt).

## Not included: NVIDIA binaries

No NVIDIA binary (`sl.*.dll`, `nvngx_*.dll`, `NvLowLatencyVk.dll`) is part of this repository or
its releases, and none must be committed (`.gitignore` excludes them). Reflex uses the
Khronos-registered Vulkan extension `VK_NV_low_latency2`, implemented by the NVIDIA driver, and
needs no NVIDIA SDK.

DLSS runs through Streamline. Its binaries come from NVIDIA's own GitHub release
(`NVIDIA-RTX/Streamline` v2.14.1, `streamline-sdk-v2.14.1.zip`) onto the developer's machine, into
the git-ignored `addons/latency_upscaling/bin/nvidia/`, only after the developer has read and
accepted NVIDIA's licences: in the editor (Project > Tools > "Latency & Upscaling: Get the NVIDIA
DLSS Runtime", offered when the plugin is enabled) or with `tools/fetch_nvidia_sdk.py`. Which files
are taken is listed in `addons/latency_upscaling/nvidia_runtime.json`; the SHA-256 of the archive and
of every file is pinned in `addons/latency_upscaling/nvidia_runtime.lock.json`, and anything that
does not match is not installed. NVIDIA's own licence files (`nvngx_dlss.license.txt`,
`license.txt`, `3rd-party-licenses.md`) are installed next to the DLLs and must travel with them.

The DLSS runtime DLLs are licensed under the **NVIDIA RTX SDKs licence**
(https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt). Summary of the clauses that matter for a
game using this addon (a reading, not legal advice):

- The DLLs may only be distributed as part of an application with material additional
  functionality, never stand-alone and never under an open-source licence.
- Before a commercial release (including "a plug-in to a commercial application") the developer
  must notify NVIDIA: https://developer.nvidia.com/sw-notification
- The developer must show NVIDIA attribution and marks as the licence requires (splash screen,
  about box, credits) and submit each proposed use of the marks to NVIDIA for approval.
- Only release (not development / watermarked) DLLs may ship, with NVIDIA's signatures intact.

Streamline itself (`sl.*.dll`) is MIT-licensed (https://github.com/NVIDIA-RTX/Streamline/blob/main/license.txt).

## Trademarks

NVIDIA, GeForce, RTX, DLSS and NVIDIA Reflex are trademarks and/or registered trademarks of NVIDIA
Corporation in the U.S. and other countries. Vulkan is a registered trademark of the Khronos Group
Inc. Godot is a trademark of the Godot Foundation. This project is not affiliated with, sponsored
by or endorsed by NVIDIA, the Khronos Group or the Godot Foundation; the names are used only to
describe what the software works with.
