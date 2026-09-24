#!/usr/bin/env python
"""Phase 2 stub: where the NVIDIA runtime DLLs will come from, without downloading them.

The DLSS runtime DLLs (nvngx_dlss.dll, nvngx_dlssg.dll) are licensed under the NVIDIA RTX SDKs
licence, which forbids redistributing them stand-alone or under an open-source licence, so they
are never committed to this MIT repository. Phase 2 fetches them from NVIDIA's official GitHub
release on the developer's machine, after the developer has read and accepted the licence (in
the editor plugin, on first enable; this script is the command-line equivalent).

Phase 1 needs no NVIDIA binary: Reflex runs through the Khronos extension VK_NV_low_latency2.
This script therefore only resolves and prints what would be fetched (release, asset, size,
licences, target folder) and refuses to download.

    python tools/fetch_nvidia_sdk.py            # dry run: query GitHub, print the plan
    python tools/fetch_nvidia_sdk.py --offline  # print the pinned plan without network access
"""
from __future__ import annotations

import json
import sys
import urllib.error
import urllib.request

# Pinned release (RESEARCH.md §1.1): Streamline 2.14.1, 2026-09-08. The zip contains the
# NVIDIA-signed production binaries under bin/x64/.
REPOSITORY = "NVIDIA-RTX/Streamline"
TAG = "v2.14.1"
ASSET = "streamline-sdk-v2.14.1.zip"
FILES = [
    "bin/x64/sl.interposer.dll",
    "bin/x64/sl.common.dll",
    "bin/x64/sl.dlss.dll",
    "bin/x64/sl.dlss_g.dll",
    "bin/x64/sl.reflex.dll",
    "bin/x64/sl.pcl.dll",
    "bin/x64/nvngx_dlss.dll",
    "bin/x64/nvngx_dlssg.dll",
]
TARGET = "addons/nvidia_dlss/bin/nvidia/"
LICENCES = {
    "Streamline source and sl.* plugins": "https://github.com/NVIDIA-RTX/Streamline/blob/main/license.txt",
    "DLSS runtime (nvngx_*.dll): NVIDIA RTX SDKs licence": "https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt",
}


def release_asset() -> dict:
    url = f"https://api.github.com/repos/{REPOSITORY}/releases/tags/{TAG}"
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "godot-nvidia-dlss"})
    with urllib.request.urlopen(request, timeout=30) as response:
        release = json.load(response)
    for asset in release.get("assets", []):
        if asset.get("name") == ASSET:
            return asset
    raise LookupError(f"release {TAG} of {REPOSITORY} has no asset {ASSET}")


def main(argv: list[str]) -> int:
    if "--download" in argv:
        print("Downloading NVIDIA binaries is part of Phase 2 (DLSS Super Resolution); Phase 1 (Reflex) needs none.",
              file=sys.stderr)
        return 2
    print(f"Release : {REPOSITORY} {TAG}")
    if "--offline" in argv:
        print(f"Asset   : {ASSET} (not checked: --offline)")
    else:
        try:
            asset = release_asset()
        except (urllib.error.URLError, LookupError, TimeoutError, json.JSONDecodeError) as error:
            print(f"Could not resolve the release asset: {error}", file=sys.stderr)
            return 1
        print(f"Asset   : {asset['name']}, {asset['size'] / 1e6:.0f} MB")
        print(f"URL     : {asset['browser_download_url']}")
    print(f"Target  : {TARGET} (git-ignored)")
    print("Files   :")
    for name in FILES:
        print(f"  {name}")
    print("Licences the developer must read and accept first:")
    for what, url in LICENCES.items():
        print(f"  {what}: {url}")
    print("Dry run only: nothing was downloaded.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
