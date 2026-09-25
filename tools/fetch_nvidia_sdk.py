#!/usr/bin/env python
"""Fetch the NVIDIA runtime for DLSS (Streamline + the DLSS DLLs) after the developer accepted
NVIDIA's licences: the command-line twin of the editor plugin's download dialog.

The DLLs are licensed by NVIDIA (the DLSS runtime under the NVIDIA RTX SDKs licence, which
forbids redistributing it stand-alone), so they are never part of this MIT repository or of the
addon: they come from NVIDIA's own GitHub release, onto the developer's machine, into the
git-ignored addons/latency_upscaling/bin/nvidia/. What is fetched and from where is pinned in
addons/latency_upscaling/nvidia_runtime.json (shared with the editor plugin), and the SHA-256 of
the archive and of every file taken from it in addons/latency_upscaling/nvidia_runtime.lock.json:
a download that does not match the lock is not installed.

    python tools/fetch_nvidia_sdk.py                  # dry run: resolve the release, print the plan
    python tools/fetch_nvidia_sdk.py --offline        # the pinned plan, without network access
    python tools/fetch_nvidia_sdk.py --show-licences  # print the licence texts of the pinned release
    python tools/fetch_nvidia_sdk.py --download --accept-nvidia-licences --accepted-by "Your Name"
                                                      # download, verify, extract, record the acceptance
Options for --download:
    --accepted-by WHO    required: the person (or company) accepting NVIDIA's licences
    --accepted-on DATE   when they accepted (YYYY-MM-DD; default: today), e.g. when you run the tool
                         for someone who accepted earlier
    --how TEXT           how they accepted (default: by running this command after reading the texts)
    --zip PATH           use a local copy of the release zip instead of downloading it
    --target DIR         extract somewhere else than the manifest's target (tests)
    --write-lock         (maintainers) record the archive's and the files' SHA-256 in the lock file
                         instead of checking them against it

The licence texts shown (--show-licences, and the editor dialog) are the licence files of the pinned
release: fetched from the release's commit on GitHub and checked against the SHA-256 the lock
records for the same files in NVIDIA's archive (nvngx_dlss.license.txt, license.txt).

--download refuses to run without --accept-nvidia-licences and --accepted-by: passing them states
that the person named has read and accepts every licence --show-licences prints. The tool never
accepts anything itself: licence_acceptance.json next to the DLLs records who accepted, when and
how, and which licence files (with their SHA-256) of which release. Exit codes: 0 done, 1 failure,
2 refused (licences not accepted).
"""
from __future__ import annotations

import datetime
import hashlib
import json
import shutil
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = ROOT / "addons" / "latency_upscaling" / "nvidia_runtime.json"
LOCK_PATH = ROOT / "addons" / "latency_upscaling" / "nvidia_runtime.lock.json"
USER_AGENT = "godot-latency-upscaling"


def manifest() -> dict:
    return json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))


def load_lock(m: dict) -> dict | None:
    """The lock for the manifest's release, None when there is none. A lock for another release is
    an error (the manifest was changed without re-locking)."""
    if not LOCK_PATH.exists():
        return None
    lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    if lock.get("repository") != m["repository"] or lock.get("tag") != m["tag"] or lock.get("asset") != m["asset"]:
        raise ValueError(f"{LOCK_PATH.name} is for {lock.get('repository')} {lock.get('tag')}, not {m['repository']} {m['tag']}")
    return lock


def res_to_path(res_path: str) -> Path:
    if not res_path.startswith("res://"):
        raise ValueError(f"expected a res:// path, got {res_path}")
    return ROOT / res_path[len("res://"):]


def http_get(url: str, accept: str = "*/*"):
    request = urllib.request.Request(url, headers={"Accept": accept, "User-Agent": USER_AGENT})
    return urllib.request.urlopen(request, timeout=60)


def release_asset(m: dict) -> dict:
    url = f"https://api.github.com/repos/{m['repository']}/releases/tags/{m['tag']}"
    with http_get(url, "application/vnd.github+json") as response:
        release = json.load(response)
    for asset in release.get("assets", []):
        if asset.get("name") == m["asset"]:
            return asset
    raise LookupError(f"release {m['tag']} of {m['repository']} has no asset {m['asset']}")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(asset: dict, destination: Path) -> str:
    """Streams the asset to `destination`; checks its size and GitHub's SHA-256 digest when the
    API provides one. Returns the SHA-256."""
    expected_size = int(asset.get("size", 0))
    with http_get(asset["browser_download_url"]) as response, open(destination, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)
    size = destination.stat().st_size
    if expected_size and size != expected_size:
        raise ValueError(f"downloaded {size} bytes, the release lists {expected_size}")
    sha = sha256_of(destination)
    digest = str(asset.get("digest") or "")
    if digest.startswith("sha256:") and digest[len("sha256:"):].lower() != sha:
        raise ValueError(f"SHA-256 {sha} does not match the release's {digest}")
    return sha


def find_member(names: list[str], wanted: str) -> str:
    """The zip member for a manifest path like bin/x64/sl.dlss.dll: that exact path, at the archive
    root or under one top-level folder (never deeper: external/ngx-sdk/license.txt is not license.txt)."""
    depth = wanted.count("/")
    matches = [n for n in names if n == wanted or (n.endswith("/" + wanted) and n.count("/") == depth + 1)]
    if len(matches) != 1:
        raise LookupError(f"{wanted}: {'not in the archive' if not matches else 'more than once in the archive'}")
    return matches[0]


def extract(zip_path: Path, target: Path, m: dict, lock: dict | None = None) -> dict[str, dict]:
    """Extracts exactly the manifest's files (flat, by file name) into `target`. Nothing is written
    unless every file is present and, with a lock, matches its SHA-256. Returns {file name:
    {"sha256", "size"}}."""
    with zipfile.ZipFile(zip_path) as archive:
        names = archive.namelist()
        members = {Path(wanted).name: find_member(names, wanted) for wanted in m["files"]}
        target.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".staging_", dir=target))
        try:
            hashes = {}
            for file_name, member in members.items():
                # Written under the manifest's own file name: archive paths never reach the disk.
                with archive.open(member) as source, open(staging / file_name, "wb") as out:
                    shutil.copyfileobj(source, out)
                hashes[file_name] = {"sha256": sha256_of(staging / file_name), "size": (staging / file_name).stat().st_size}
                if lock is not None:
                    locked = lock.get("files", {}).get(file_name, {})
                    if locked.get("sha256") != hashes[file_name]["sha256"]:
                        raise ValueError(f"{file_name} does not match {LOCK_PATH.name} (SHA-256 {hashes[file_name]['sha256']})")
            for file_name in members:
                (staging / file_name).replace(target / file_name)
        finally:
            shutil.rmtree(staging, ignore_errors=True)
    return dict(sorted(hashes.items()))


def write_lock(m: dict, asset_url: str, asset_sha: str, asset_size: int, files: dict[str, dict]) -> Path:
    lock = {
        "comment": "SHA-256 of the NVIDIA release archive and of every file the addon takes from it, recorded when "
                   "the runtime was first fetched (python tools/fetch_nvidia_sdk.py --download --accept-nvidia-licences "
                   "--write-lock). Downloads that do not match are not installed.",
        "repository": m["repository"],
        "tag": m["tag"],
        "asset": m["asset"],
        "url": asset_url,
        "sha256": asset_sha,
        "size": asset_size,
        "files": files,
    }
    LOCK_PATH.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
    return LOCK_PATH


def requires_acceptance(lic: dict) -> bool:
    return lic.get("acceptance") == "required"


def accepted_licences(m: dict, files: dict[str, dict]) -> list[dict]:
    """The licence files of the installed release: those that need acceptance marked accepted,
    the others (Streamline's MIT licence) with the manifest's note instead."""
    out = []
    for lic in m["licences"]:
        entry = {"name": lic["name"], "file": lic["file"], "sha256": files.get(lic["file"], {}).get("sha256", ""),
                 "text": lic["text"], "accepted": requires_acceptance(lic)}
        if not requires_acceptance(lic):
            entry["note"] = lic.get("acceptance", "")
        out.append(entry)
    return out


def record_acceptance(target: Path, m: dict, sha: str, files: dict[str, dict], acceptance: dict) -> Path:
    """licence_acceptance.json: who accepted which licence files of which release, when and how.
    `acceptance` holds "accepted_by", "accepted_on" and "how" as the person gave them."""
    record = {
        "accepted_by": acceptance["accepted_by"],
        "accepted_on": acceptance["accepted_on"],
        "how": acceptance["how"],
        "licences": accepted_licences(m, files),
        "repository": m["repository"],
        "tag": m["tag"],
        "commit": m["commit"],
        "asset": m["asset"],
        "sha256": sha,
        "files": sorted(files),
        "recorded_at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "recorded_with": "tools/fetch_nvidia_sdk.py",
    }
    path = target / m["acceptance_record"]
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return path


def acceptance_from(argv: list[str]) -> dict:
    """Who accepted, when and how, from the command line. Raises ValueError when --accepted-by is
    missing or --accepted-on is not a date."""
    who = (value_of(argv, "--accepted-by") or "").strip()
    if not who:
        raise ValueError("--accepted-by is required: name the person (or company) accepting NVIDIA's licences")
    on = value_of(argv, "--accepted-on") or datetime.datetime.now(datetime.timezone.utc).date().isoformat()
    datetime.date.fromisoformat(on)  # ValueError when it is not YYYY-MM-DD
    how = (value_of(argv, "--how") or "").strip() or (
        "ran tools/fetch_nvidia_sdk.py --download --accept-nvidia-licences after reading the licence files of the "
        "pinned release (--show-licences), accepting the NVIDIA RTX SDKs licence")
    return {"accepted_by": who, "accepted_on": on, "how": how}


def print_plan(m: dict, offline: bool) -> int:
    print(f"Release : {m['repository']} {m['tag']}")
    if offline:
        print(f"Asset   : {m['asset']} (not checked: --offline)")
    else:
        try:
            asset = release_asset(m)
        except (urllib.error.URLError, LookupError, TimeoutError, json.JSONDecodeError) as error:
            print(f"Could not resolve the release asset: {error}", file=sys.stderr)
            return 1
        print(f"Asset   : {asset['name']}, {asset['size'] / 1e6:.0f} MB")
        print(f"URL     : {asset['browser_download_url']}")
    print(f"Target  : {m['target']} (git-ignored)")
    print("Files   :")
    for name in m["files"]:
        print(f"  {name}")
    print_licence_list(m)
    print("Dry run: nothing was downloaded. Download with --download --accept-nvidia-licences --accepted-by \"Your Name\".")
    return 0


def print_licence_list(m: dict) -> None:
    print("Licences you must read first (python tools/fetch_nvidia_sdk.py --show-licences):")
    for lic in m["licences"]:
        need = "to accept" if requires_acceptance(lic) else "no acceptance needed (MIT)"
        print(f"  {lic['name']}: {lic['file']} of {m['asset']}, {need} ({lic['page']})")


def pinned_licence_text(m: dict, lic: dict, lock: dict | None) -> str:
    """The licence text of the pinned release: fetched from the release's commit and, with a lock,
    checked against the SHA-256 of the same file in NVIDIA's archive. Raises ValueError otherwise."""
    with http_get(lic["text"]) as response:
        data = response.read()
    if lock is not None:
        expected = lock.get("files", {}).get(lic["file"], {}).get("sha256")
        actual = hashlib.sha256(data).hexdigest()
        if actual != expected:
            raise ValueError(f"the text at {lic['text']} (SHA-256 {actual}) is not {lic['file']} of {m['asset']} ({expected})")
    return data.decode("utf-8", errors="replace")


def show_licences(m: dict) -> int:
    lock = load_lock(m)
    failed = False
    for lic in m["licences"]:
        print("=" * 100)
        print(f"{lic['name']}: {lic['file']} of {m['asset']} ({m['repository']} {m['tag']}, commit {m['commit'][:12]})")
        print("=" * 100)
        try:
            text = pinned_licence_text(m, lic, lock)
            print(text)
            print(f"(verified: identical to {lic['file']} in {m['asset']}, SHA-256 from {LOCK_PATH.name})" if lock
                  else "(not verified: no lock file)")
        except (urllib.error.URLError, TimeoutError, ValueError) as error:
            failed = True
            print(f"(could not show the pinned text: {error}; read it at {lic['page']})")
    return 1 if failed else 0


def run_download(m: dict, argv: list[str]) -> int:
    if "--accept-nvidia-licences" not in argv:
        print("Refused: downloading the NVIDIA runtime means accepting NVIDIA's licences.", file=sys.stderr)
        print_licence_list(m)
        print("Read them, then run again with --accept-nvidia-licences --accepted-by \"Your Name\".", file=sys.stderr)
        return 2
    try:
        acceptance = acceptance_from(argv)
    except ValueError as error:
        print(f"Refused: {error}", file=sys.stderr)
        return 2
    target = Path(value_of(argv, "--target")) if value_of(argv, "--target") else res_to_path(m["target"])
    local_zip = value_of(argv, "--zip")
    writing_lock = "--write-lock" in argv
    default_url = f"https://github.com/{m['repository']}/releases/download/{m['tag']}/{m['asset']}"
    try:
        lock = None if writing_lock else load_lock(m)
        if local_zip:
            zip_path = Path(local_zip)
            sha = sha256_of(zip_path)
            size = zip_path.stat().st_size
            url = default_url
            print(f"Using {zip_path} (SHA-256 {sha})")
            check_against_lock(lock, sha)
            files = extract(zip_path, target, m, lock)
        else:
            asset = release_asset(m)
            url = asset["browser_download_url"]
            print(f"Downloading {url} ({asset['size'] / 1e6:.0f} MB)")
            # Next to the target (the system drive may be short of space for the ~280 MB archive).
            target.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix=".lu_nvidia_download_", dir=target.parent) as tmp:
                zip_path = Path(tmp) / m["asset"]
                sha = download(asset, zip_path)
                size = zip_path.stat().st_size
                print(f"Verified against the release: {size} bytes, SHA-256 {sha}")
                check_against_lock(lock, sha)
                files = extract(zip_path, target, m, lock)
    except (urllib.error.URLError, TimeoutError, LookupError, ValueError, OSError, zipfile.BadZipFile) as error:
        print(f"Failed: {error}", file=sys.stderr)
        return 1
    record = record_acceptance(target, m, sha, files, acceptance)
    print(f"Extracted {len(files)} files into {target}:")
    for name, info in files.items():
        print(f"  {name}  {info['size']:>10} bytes  SHA-256 {info['sha256']}")
    if writing_lock:
        print(f"Lock written: {write_lock(m, url, sha, size, files)}")
    elif lock is not None:
        print(f"Every file matches {LOCK_PATH.name}.")
    print(f"Acceptance recorded in {record}: {acceptance['accepted_by']}, {acceptance['accepted_on']}")
    return 0


def check_against_lock(lock: dict | None, sha: str) -> None:
    if lock is not None and lock.get("sha256") != sha:
        raise ValueError(f"the archive (SHA-256 {sha}) does not match {LOCK_PATH.name} ({lock.get('sha256')})")


def value_of(argv: list[str], flag: str) -> str | None:
    if flag in argv:
        index = argv.index(flag)
        if index + 1 < len(argv):
            return argv[index + 1]
    return None


def main(argv: list[str]) -> int:
    m = manifest()
    if "--show-licences" in argv:
        return show_licences(m)
    if "--download" in argv:
        return run_download(m, argv)
    return print_plan(m, "--offline" in argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
