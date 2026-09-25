#!/usr/bin/env python
"""Tests of tools/fetch_nvidia_sdk.py against synthetic archives.

No NVIDIA file is involved: the archives are built here and hold small text files that merely
carry the manifest's file names. Nothing is downloaded (the one download test serves the archive
from memory).

    python tests/test_fetch_nvidia_sdk.py
"""
from __future__ import annotations

import hashlib
import io
import json
import re
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import fetch_nvidia_sdk as fetch  # noqa: E402

FAKE = b"synthetic test file, not an NVIDIA binary\n"


def make_zip(path: Path, members: dict[str, bytes]) -> Path:
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in members.items():
            archive.writestr(name, data)
    return path


def release_members(prefix: str = "") -> dict[str, bytes]:
    members = {prefix + name: FAKE + name.encode() for name in fetch.manifest()["files"]}
    members[prefix + "bin/x64/sl.unrelated.dll"] = FAKE  # not in the manifest: must not be extracted
    members[prefix + "include/sl.h"] = FAKE
    return members


class FetchTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="lu_fetch_test_")
        self.tmp = Path(self._tmp.name)
        self.target = self.tmp / "target"
        self.m = fetch.manifest()
        # The repository's lock pins NVIDIA's real files; these tests use synthetic ones.
        self._lock = mock.patch.object(fetch, "LOCK_PATH", self.tmp / "nvidia_runtime.lock.json")
        self._lock.start()

    def tearDown(self) -> None:
        self._lock.stop()
        self._tmp.cleanup()

    def run_main(self, *args: str) -> int:
        with mock.patch("sys.stdout", new=io.StringIO()), mock.patch("sys.stderr", new=io.StringIO()):
            return fetch.main(list(args))

    def test_download_is_refused_without_accepting_the_licences(self) -> None:
        archive = make_zip(self.tmp / "release.zip", release_members())
        code = self.run_main("--download", "--zip", str(archive), "--target", str(self.target))
        self.assertEqual(code, 2)
        self.assertFalse(self.target.exists())

    def test_accepted_download_extracts_exactly_the_manifest_files_and_records_it(self) -> None:
        archive = make_zip(self.tmp / "release.zip", release_members())
        code = self.run_main("--download", "--accept-nvidia-licences", "--accepted-by", "Test Person", "--zip", str(archive), "--target", str(self.target))
        self.assertEqual(code, 0)
        expected = sorted(Path(f).name for f in self.m["files"])
        installed = sorted(p.name for p in self.target.iterdir() if p.name != self.m["acceptance_record"])
        self.assertEqual(installed, expected)
        for path in self.m["files"]:
            self.assertEqual((self.target / Path(path).name).read_bytes(), FAKE + path.encode())
        self.assertEqual((self.target / "license.txt").read_bytes(), FAKE + b"license.txt")
        record = json.loads((self.target / self.m["acceptance_record"]).read_text(encoding="utf-8"))
        self.assertEqual(record["files"], expected)
        self.assertEqual(record["tag"], self.m["tag"])
        self.assertEqual(record["sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())
        # Who accepted, when and how, as given: the tool never claims the acceptance for itself.
        self.assertEqual(record["accepted_by"], "Test Person")
        self.assertRegex(record["accepted_on"], r"^\d{4}-\d{2}-\d{2}$")
        self.assertIn("--accept-nvidia-licences", record["how"])
        self.assertEqual(record["recorded_with"], "tools/fetch_nvidia_sdk.py")
        self.assertEqual(record["commit"], self.m["commit"])
        # The licences accepted are the installed licence files, with their SHA-256.
        self.assertEqual([lic["file"] for lic in record["licences"]], [lic["file"] for lic in self.m["licences"]])
        for lic in record["licences"]:
            self.assertEqual(lic["sha256"], hashlib.sha256((self.target / lic["file"]).read_bytes()).hexdigest())
        self.assertFalse(any(p.name.startswith(".staging_") for p in self.target.iterdir()))

    def test_the_acceptance_names_who_when_and_how(self) -> None:
        archive = make_zip(self.tmp / "release.zip", release_members())
        base = ["--download", "--accept-nvidia-licences", "--zip", str(archive), "--target", str(self.target)]
        self.assertEqual(self.run_main(*base), 2)  # nobody named: refused
        self.assertFalse(self.target.exists())
        self.assertEqual(self.run_main(*base, "--accepted-by", "  "), 2)
        self.assertEqual(self.run_main(*base, "--accepted-by", "X", "--accepted-on", "24.09.2026"), 2)  # not a date
        self.assertFalse(self.target.exists())
        code = self.run_main(*base, "--accepted-by", "the project owner", "--accepted-on", "2026-09-24",
                             "--how", "in the conversation, relayed by the CEO agent")
        self.assertEqual(code, 0)
        record = json.loads((self.target / self.m["acceptance_record"]).read_text(encoding="utf-8"))
        self.assertEqual((record["accepted_by"], record["accepted_on"], record["how"]),
                         ("the project owner", "2026-09-24", "in the conversation, relayed by the CEO agent"))
        self.assertNotEqual(record["recorded_at"][:10], "")

    def test_the_licences_are_the_pinned_release_files(self) -> None:
        self.assertRegex(self.m["commit"], r"^[0-9a-f]{40}$")
        names = {Path(f).name for f in self.m["files"]}
        for lic in self.m["licences"]:
            self.assertIn(lic["file"], names)  # a file of the archive, checked against the lock
            self.assertIn(self.m["commit"], lic["text"])  # never a moving branch
            self.assertIn(self.m["commit"], lic["page"])
            self.assertNotIn("/main/", lic["text"])

    def test_licence_texts_are_shown_only_when_they_match_the_lock(self) -> None:
        lic = self.m["licences"][0]
        good = b"the licence text of the pinned release\n"
        lock = {"files": {lic["file"]: {"sha256": hashlib.sha256(good).hexdigest()}}}
        with mock.patch.object(fetch, "http_get", lambda url, accept="*/*": io.BytesIO(good)):
            self.assertEqual(fetch.pinned_licence_text(self.m, lic, lock), good.decode())
        with mock.patch.object(fetch, "http_get", lambda url, accept="*/*": io.BytesIO(b"another text")):
            with self.assertRaises(ValueError):
                fetch.pinned_licence_text(self.m, lic, lock)

    def test_the_repository_lock_pins_the_licence_files(self) -> None:
        real_lock = ROOT / "addons" / "latency_upscaling" / "nvidia_runtime.lock.json"
        if not real_lock.exists():
            self.skipTest("no lock yet")
        lock = json.loads(real_lock.read_text(encoding="utf-8"))
        for lic in self.m["licences"]:
            self.assertRegex(lock["files"][lic["file"]]["sha256"], r"^[0-9a-f]{64}$")

    def test_files_under_a_top_level_folder_are_found(self) -> None:
        archive = make_zip(self.tmp / "release.zip", release_members("streamline-sdk-v2.14.1/"))
        self.assertEqual(list(fetch.extract(archive, self.target, self.m)), sorted(Path(f).name for f in self.m["files"]))

    def test_an_incomplete_archive_writes_nothing(self) -> None:
        members = release_members()
        del members["bin/x64/nvngx_dlss.dll"]
        archive = make_zip(self.tmp / "release.zip", members)
        code = self.run_main("--download", "--accept-nvidia-licences", "--accepted-by", "Test Person", "--zip", str(archive), "--target", str(self.target))
        self.assertEqual(code, 1)
        self.assertEqual([p for p in self.target.glob("*") if p.is_file()], [])

    def test_deeper_files_with_the_same_name_are_not_taken(self) -> None:
        # The real archive has license.txt at its root and external/ngx-sdk/license.txt.
        for prefix in ("", "streamline-sdk-v2.14.1/"):
            members = release_members(prefix)
            members[prefix + "external/ngx-sdk/license.txt"] = b"another licence"
            archive = make_zip(self.tmp / "nested.zip", members)
            target = self.tmp / ("nested" + str(len(prefix)))
            fetch.extract(archive, target, self.m)
            self.assertEqual((target / "license.txt").read_bytes(), FAKE + b"license.txt")

    def test_an_ambiguous_archive_is_rejected(self) -> None:
        members = release_members()
        members["other/bin/x64/sl.dlss.dll"] = FAKE
        archive = make_zip(self.tmp / "release.zip", members)
        with self.assertRaises(LookupError):
            fetch.extract(archive, self.target, self.m)

    def test_archive_paths_never_reach_the_disk(self) -> None:
        # A hostile member name ("../" as the top-level folder): only the manifest's own file name is
        # ever written, inside the target.
        members = {("../" + name): FAKE for name in self.m["files"]}
        archive = make_zip(self.tmp / "release.zip", members)
        fetch.extract(archive, self.target, self.m)
        self.assertFalse((self.tmp / "bin").exists())
        self.assertFalse((self.target.parent / "bin").exists())
        self.assertTrue((self.target / "sl.dlss.dll").exists())

    def test_download_checks_size_and_the_release_digest(self) -> None:
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr("x", FAKE)
        payload = data.getvalue()
        good = {"browser_download_url": "https://example.invalid/r.zip", "size": len(payload),
                "digest": "sha256:" + hashlib.sha256(payload).hexdigest()}

        def fake_get(url: str, accept: str = "*/*") -> io.BytesIO:
            return io.BytesIO(payload)

        with mock.patch.object(fetch, "http_get", fake_get):
            self.assertEqual(fetch.download(good, self.tmp / "a.zip"), hashlib.sha256(payload).hexdigest())
            with self.assertRaises(ValueError):
                fetch.download({**good, "digest": "sha256:" + "0" * 64}, self.tmp / "b.zip")
            with self.assertRaises(ValueError):
                fetch.download({**good, "size": len(payload) + 1}, self.tmp / "c.zip")

    def test_the_super_resolution_files_checked_by_the_addon_are_fetched(self) -> None:
        fetched = {Path(f).name for f in self.m["files"]}
        self.assertTrue(set(self.m["super_resolution_files"]) <= fetched)
        # The C++ availability check (src/dlss/runtime_files.cpp) looks for exactly these.
        source = (ROOT / "src" / "dlss" / "runtime_files.cpp").read_text(encoding="utf-8")
        listed = re.findall(r'L"([^"]+\.dll)"', source)
        self.assertEqual(sorted(listed), sorted(self.m["super_resolution_files"]))

    def test_the_lock_is_written_and_then_enforced(self) -> None:
        archive = make_zip(self.tmp / "release.zip", release_members())
        code = self.run_main("--download", "--accept-nvidia-licences", "--accepted-by", "Test Person", "--write-lock", "--zip", str(archive),
                             "--target", str(self.target))
        self.assertEqual(code, 0)
        lock = json.loads(fetch.LOCK_PATH.read_text(encoding="utf-8"))
        self.assertEqual(lock["sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertEqual(lock["tag"], self.m["tag"])
        self.assertIn(self.m["tag"], lock["url"])
        self.assertEqual(sorted(lock["files"]), sorted(Path(f).name for f in self.m["files"]))
        name = "sl.dlss.dll"
        self.assertEqual(lock["files"][name]["sha256"], hashlib.sha256(FAKE + b"bin/x64/" + name.encode()).hexdigest())
        # The same archive installs against the lock.
        self.assertEqual(self.run_main("--download", "--accept-nvidia-licences", "--accepted-by", "Test Person", "--zip", str(archive),
                                       "--target", str(self.tmp / "again")), 0)
        # A different archive does not, and nothing of it is written.
        members = release_members()
        members["bin/x64/sl.dlss.dll"] = b"tampered"
        tampered = make_zip(self.tmp / "tampered.zip", members)
        other = self.tmp / "other"
        self.assertEqual(self.run_main("--download", "--accept-nvidia-licences", "--accepted-by", "Test Person", "--zip", str(tampered), "--target", str(other)), 1)
        self.assertFalse(other.exists() and any(p.suffix == ".dll" for p in other.iterdir()))
        # Even with the archive hash made to match, a file that differs from the lock is refused.
        lock["sha256"] = hashlib.sha256(tampered.read_bytes()).hexdigest()
        fetch.LOCK_PATH.write_text(json.dumps(lock), encoding="utf-8")
        self.assertEqual(self.run_main("--download", "--accept-nvidia-licences", "--accepted-by", "Test Person", "--zip", str(tampered), "--target", str(other)), 1)
        self.assertFalse(other.exists() and any(p.suffix == ".dll" for p in other.iterdir()))

    def test_a_lock_for_another_release_is_an_error(self) -> None:
        fetch.LOCK_PATH.write_text(json.dumps({"repository": self.m["repository"], "tag": "v0.0.1", "asset": "x.zip"}), encoding="utf-8")
        with self.assertRaises(ValueError):
            fetch.load_lock(self.m)

    def test_the_repository_lock_matches_the_manifest(self) -> None:
        real_lock = ROOT / "addons" / "latency_upscaling" / "nvidia_runtime.lock.json"
        if not real_lock.exists():
            self.skipTest("no lock yet")
        lock = json.loads(real_lock.read_text(encoding="utf-8"))
        self.assertEqual((lock["repository"], lock["tag"], lock["asset"]), (self.m["repository"], self.m["tag"], self.m["asset"]))
        self.assertEqual(sorted(lock["files"]), sorted(Path(f).name for f in self.m["files"]))
        self.assertRegex(lock["sha256"], r"^[0-9a-f]{64}$")
        for info in lock["files"].values():
            self.assertRegex(info["sha256"], r"^[0-9a-f]{64}$")

    def test_dry_runs_do_not_download(self) -> None:
        with mock.patch.object(fetch, "download", side_effect=AssertionError("downloaded")):
            self.assertEqual(self.run_main("--offline"), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
