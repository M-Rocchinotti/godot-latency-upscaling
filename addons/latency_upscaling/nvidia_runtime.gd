@tool
extends RefCounted
## The NVIDIA runtime for DLSS on the developer's machine: which files are needed
## (nvidia_runtime.json), whether they are there, and installing them from NVIDIA's release zip
## once the developer has accepted NVIDIA's licences (nvidia_runtime_dialog.gd does the asking and
## the download; tools/fetch_nvidia_sdk.py is the command-line twin).
##
## The files are licensed by NVIDIA and are never part of this addon; they land in the
## git-ignored bin/nvidia/ folder next to the addon's own libraries.

const MANIFEST_PATH := "res://addons/latency_upscaling/nvidia_runtime.json"
## SHA-256 of NVIDIA's release archive and of every file taken from it (tools/fetch_nvidia_sdk.py
## --write-lock); an archive or a file that does not match is not installed.
const LOCK_PATH := "res://addons/latency_upscaling/nvidia_runtime.lock.json"


static func manifest() -> Dictionary:
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(MANIFEST_PATH))
	return parsed if parsed is Dictionary else {}


static func lock() -> Dictionary:
	if not FileAccess.file_exists(LOCK_PATH):
		return {}
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(LOCK_PATH))
	return parsed if parsed is Dictionary else {}


## Bytes of NVIDIA's release archive and of the files taken from it, as the lock records them
## (0 without a lock): what a download needs before anything is fetched.
static func archive_size() -> int:
	return int(lock().get("size", 0))


static func installed_size() -> int:
	var total := 0
	for info: Dictionary in (lock().get("files", {}) as Dictionary).values():
		total += int(info.get("size", 0))
	return total


static func sha256_bytes(data: PackedByteArray) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(data)
	return context.finish().hex_encode()


static func target_dir() -> String:
	return str(manifest().get("target", "res://addons/latency_upscaling/bin/nvidia"))


## The files DLSS Super Resolution needs that are not in `dir` (default: the manifest's target).
static func missing_files(dir: String = "") -> PackedStringArray:
	var folder := dir if dir != "" else target_dir()
	var missing := PackedStringArray()
	for name: String in manifest().get("super_resolution_files", []):
		if not FileAccess.file_exists(folder.path_join(name)):
			missing.append(name)
	return missing


static func is_installed(dir: String = "") -> bool:
	return missing_files(dir).is_empty()


## What a Windows export ships next to the game (the Super Resolution DLLs and NVIDIA's licence
## files), as paths inside `dir`, and the folder they go to beside the executable.
static func export_files(dir: String = "") -> PackedStringArray:
	var folder := dir if dir != "" else target_dir()
	var files := PackedStringArray()
	for name: String in manifest().get("export_files", []):
		files.append(folder.path_join(name))
	return files


static func export_folder() -> String:
	return str(manifest().get("export_folder", "nvidia"))


## The recorded acceptance ({} when the runtime was never installed through this addon).
static func acceptance(dir: String = "") -> Dictionary:
	var folder := dir if dir != "" else target_dir()
	var path := folder.path_join(str(manifest().get("acceptance_record", "licence_acceptance.json")))
	if not FileAccess.file_exists(path):
		return {}
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	return parsed if parsed is Dictionary else {}


static func sha256_file(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return ""
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	while file.get_position() < file.get_length():
		context.update(file.get_buffer(1 << 20))
	return context.finish().hex_encode()


## Whether `text` is the licence file `lic` of the pinned release: "" when its SHA-256 is the one
## `pinned` (normally lock()) records for that file of NVIDIA's archive, otherwise why not.
static func licence_text_problem(text: PackedByteArray, lic: Dictionary, pinned: Dictionary) -> String:
	var file := str(lic.get("file", ""))
	var expected := str((pinned.get("files", {}) as Dictionary).get(file, {}).get("sha256", ""))
	if expected == "":
		return "no SHA-256 for %s in %s" % [file, LOCK_PATH.get_file()]
	var actual := sha256_bytes(text)
	if actual != expected:
		return "the text is not %s of the pinned release (SHA-256 %s, expected %s)" % [file, actual, expected]
	return ""


## Installs the manifest's files from the release zip into `target` and records the acceptance.
## Refuses unless `accepted` (the developer ticked the licence box) and `acceptance` says who
## accepted, when and how: {"accepted_by", "accepted_on" (YYYY-MM-DD), "how", optional
## "recorded_with"}. The record never claims the acceptance for the tool. Nothing is written unless
## every file is in the archive and, with a `pinned` lock (normally lock()), the archive and every
## file match its SHA-256. Returns "" on success, otherwise what went wrong.
static func install_from_zip(zip_path: String, target: String, accepted: bool, acceptance: Dictionary,
		pinned: Dictionary = {}) -> String:
	if not accepted:
		return "the NVIDIA licences were not accepted: nothing was installed"
	if str(acceptance.get("accepted_by", "")).strip_edges() == "" or str(acceptance.get("how", "")).strip_edges() == "":
		return "the acceptance does not say who accepted the licences and how: nothing was installed"
	var accepted_on := str(acceptance.get("accepted_on", ""))
	if not RegEx.create_from_string("^\\d{4}-\\d{2}-\\d{2}$").search(accepted_on):
		return "the acceptance date '%s' is not YYYY-MM-DD: nothing was installed" % accepted_on
	var m := manifest()
	if m.is_empty():
		return "cannot read " + MANIFEST_PATH
	if not pinned.is_empty():
		if pinned.get("tag") != m.get("tag") or pinned.get("asset") != m.get("asset"):
			return "the lock is for %s %s, not %s %s" % [pinned.get("asset"), pinned.get("tag"), m.get("asset"), m.get("tag")]
		var zip_sha := sha256_file(zip_path)
		if zip_sha != str(pinned.get("sha256", "")):
			return "the archive (SHA-256 %s) does not match the pinned %s" % [zip_sha, pinned.get("sha256")]
	var reader := ZIPReader.new()
	if reader.open(zip_path) != OK:
		return "cannot open " + zip_path
	var names := reader.get_files()
	var members := {} # file name -> archive path
	var installed_sha := {} # file name -> SHA-256
	for wanted: String in m["files"]:
		# That exact path, at the archive root or under one top-level folder (never deeper).
		var depth := wanted.count("/")
		var matches := Array(names).filter(func(n: String) -> bool:
				return n == wanted or (n.ends_with("/" + wanted) and n.count("/") == depth + 1))
		if matches.size() != 1:
			reader.close()
			return "%s is %s the archive" % [wanted, "not in" if matches.is_empty() else "more than once in"]
		members[wanted.get_file()] = matches[0]
	var target_abs := ProjectSettings.globalize_path(target)
	var staging := target_abs.path_join(".staging")
	DirAccess.make_dir_recursive_absolute(staging)
	var error := ""
	for file_name: String in members:
		var data := reader.read_file(members[file_name])
		installed_sha[file_name] = sha256_bytes(data)
		if not pinned.is_empty():
			var expected := str((pinned.get("files", {}) as Dictionary).get(file_name, {}).get("sha256", ""))
			if installed_sha[file_name] != expected:
				error = "%s does not match the pinned SHA-256" % file_name
				break
		# Written under the manifest's own file name: archive paths never reach the disk.
		var out := FileAccess.open(staging.path_join(file_name), FileAccess.WRITE)
		if out == null:
			error = "cannot write " + staging.path_join(file_name)
			break
		out.store_buffer(data)
		out.close()
	reader.close()
	if error == "":
		for file_name: String in members:
			var destination := target_abs.path_join(file_name)
			if FileAccess.file_exists(destination):
				DirAccess.remove_absolute(destination)
			if DirAccess.rename_absolute(staging.path_join(file_name), destination) != OK:
				error = "cannot move %s into %s" % [file_name, target_abs]
				break
	for leftover in DirAccess.get_files_at(staging):
		DirAccess.remove_absolute(staging.path_join(leftover))
	DirAccess.remove_absolute(staging)
	if error != "":
		return error
	var files := members.keys()
	files.sort()
	# The licences accepted, as the installed files that hold them (the same schema as
	# tools/fetch_nvidia_sdk.py writes).
	var licences := []
	for lic: Dictionary in m["licences"]:
		licences.append({"name": lic["name"], "file": lic["file"], "sha256": installed_sha.get(lic["file"], ""),
				"text": lic["text"]})
	var record := {
		"accepted_by": str(acceptance["accepted_by"]).strip_edges(),
		"accepted_on": accepted_on,
		"how": str(acceptance["how"]).strip_edges(),
		"licences": licences,
		"repository": m["repository"],
		"tag": m["tag"],
		"commit": m.get("commit", ""),
		"asset": m["asset"],
		"sha256": sha256_file(zip_path),
		"files": files,
		"recorded_at": Time.get_datetime_string_from_system(true) + "Z",
		"recorded_with": str(acceptance.get("recorded_with", "the editor dialog of the Latency & Upscaling plugin")),
	}
	var record_file := FileAccess.open(target_abs.path_join(str(m["acceptance_record"])), FileAccess.WRITE)
	if record_file == null:
		return "the files were installed, but the acceptance could not be recorded"
	record_file.store_string(JSON.stringify(record, "  ") + "\n")
	return ""
