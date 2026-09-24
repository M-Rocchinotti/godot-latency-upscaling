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


## Installs the manifest's files from the release zip into `target` and records the acceptance.
## Refuses unless `accepted` (the developer ticked the licence box). Nothing is written unless every
## file is in the archive and, with a `pinned` lock (normally lock()), the archive and every file
## match its SHA-256. Returns "" on success, otherwise what went wrong.
static func install_from_zip(zip_path: String, target: String, accepted: bool, accepted_by: String,
		pinned: Dictionary = {}) -> String:
	if not accepted:
		return "the NVIDIA licences were not accepted: nothing was installed"
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
		if not pinned.is_empty():
			var expected := str((pinned.get("files", {}) as Dictionary).get(file_name, {}).get("sha256", ""))
			if sha256_bytes(data) != expected:
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
	var licences := []
	for lic: Dictionary in m["licences"]:
		licences.append({"name": lic["name"], "page": lic["page"]})
	var record := {
		"accepted_at": Time.get_datetime_string_from_system(true) + "Z",
		"accepted_by": accepted_by,
		"licences": licences,
		"repository": m["repository"],
		"tag": m["tag"],
		"asset": m["asset"],
		"sha256": sha256_file(zip_path),
		"files": files,
	}
	var record_file := FileAccess.open(target_abs.path_join(str(m["acceptance_record"])), FileAccess.WRITE)
	if record_file == null:
		return "the files were installed, but the acceptance could not be recorded"
	record_file.store_string(JSON.stringify(record, "  ") + "\n")
	return ""
