extends SceneTree
## Headless tests of the NVIDIA-runtime installer of the editor plugin (nvidia_runtime.gd and
## nvidia_runtime_dialog.gd), with synthetic archives only:
##
##     python tools/godot.py --headless --script res://tests/nvidia_runtime_tests.gd
##
## No NVIDIA file is involved and nothing leaves the machine: the "release" is a zip of small text
## files built here, served by a tiny HTTP server inside this process for the download path.
## Everything is installed below user://, never into the addon's bin/nvidia/.
## With GODOT_LATENCY_UPSCALING_TEST_RUNTIME_ZIP=<path of NVIDIA's release zip> (a local copy; the
## tests never download it) the real archive is also installed against the repository's lock.
## Ends "nvidia runtime tests: <passed> passed, <failed> failed" (and "SKIP real archive" without it).

const Runtime := preload("res://addons/latency_upscaling/nvidia_runtime.gd")
const RuntimeDialog := preload("res://addons/latency_upscaling/nvidia_runtime_dialog.gd")
const FAKE := "synthetic test file, not an NVIDIA binary\n"
const WORK := "user://lu_nvidia_runtime_test"
const ACCEPTANCE := {"accepted_by": "test", "accepted_on": "2026-09-24", "how": "a test"}

var _passed := 0
var _failed := 0


func _initialize() -> void:
	_run.call_deferred()


func _check(name: String, ok: bool, detail: String = "") -> void:
	print(("PASS " if ok else "FAIL ") + name + ("  " + detail if detail != "" else ""))
	if ok:
		_passed += 1
	else:
		_failed += 1


func _abs(path: String) -> String:
	return ProjectSettings.globalize_path(path)


func _wipe(path: String) -> void:
	var dir := _abs(path)
	if not DirAccess.dir_exists_absolute(dir):
		return
	for sub in DirAccess.get_directories_at(dir):
		_wipe(path.path_join(sub))
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)


func _make_zip(name: String, prefix: String, skip: String = "", extra: Dictionary = {}) -> String:
	var path := _abs(WORK.path_join(name))
	var packer := ZIPPacker.new()
	packer.open(path)
	for file: String in Runtime.manifest()["files"]:
		if file.get_file() == skip:
			continue
		packer.start_file(prefix + file)
		packer.write_file((FAKE + file).to_utf8_buffer())
		packer.close_file()
	for member: String in extra:
		packer.start_file(member)
		packer.write_file(str(extra[member]).to_utf8_buffer())
		packer.close_file()
	packer.close()
	return path


func _dlls(dir: String) -> PackedStringArray:
	var out := PackedStringArray()
	if DirAccess.dir_exists_absolute(_abs(dir)):
		for file in DirAccess.get_files_at(_abs(dir)):
			if file.ends_with(".dll"):
				out.append(file)
	out.sort()
	return out


func _expected_dlls() -> PackedStringArray:
	var out := PackedStringArray()
	for file: String in Runtime.manifest()["files"]:
		if file.ends_with(".dll"):
			out.append(file.get_file())
	out.sort()
	return out


func _run() -> void:
	_wipe(WORK)
	DirAccess.make_dir_recursive_absolute(_abs(WORK))
	_test_manifest()
	_test_install()
	_test_lock()
	_test_real_archive()
	await _test_dialog()
	await _test_download_path()
	_wipe(WORK)
	print("nvidia runtime tests: %d passed, %d failed" % [_passed, _failed])
	quit(1 if _failed > 0 else 0)


func _test_manifest() -> void:
	var m := Runtime.manifest()
	_check("manifest names the release, files and licences", m.get("tag", "") != "" and (m.get("files", []) as Array).size() >= 4
			and (m.get("licences", []) as Array).size() == 2, "%s %d files" % [m.get("tag"), (m.get("files", []) as Array).size()])
	_check("the runtime is looked for in the git-ignored bin/nvidia/", Runtime.target_dir() == "res://addons/latency_upscaling/bin/nvidia")
	var empty := WORK.path_join("empty")
	DirAccess.make_dir_recursive_absolute(_abs(empty))
	_check("an empty folder misses every Super Resolution file",
			Runtime.missing_files(empty) == PackedStringArray(m["super_resolution_files"]) and not Runtime.is_installed(empty))


func _test_install() -> void:
	var zip := _make_zip("release.zip", "streamline-sdk-v2.14.1/", "", {"streamline-sdk-v2.14.1/bin/x64/sl.unrelated.dll": FAKE})
	var target := WORK.path_join("target_install")
	var refused := Runtime.install_from_zip(zip, target, false, ACCEPTANCE)
	_check("install refuses without the licence acceptance", refused != "" and _dlls(target).is_empty(), refused)
	var message := Runtime.install_from_zip(zip, target, true, ACCEPTANCE)
	_check("install writes exactly the manifest's files, flat", message == "" and _dlls(target) == _expected_dlls(),
			"%s %s" % [message, _dlls(target)])
	_check("installed files are the archive's", FileAccess.get_file_as_string(target.path_join("sl.dlss.dll")) == FAKE + "bin/x64/sl.dlss.dll"
			and FileAccess.get_file_as_string(target.path_join("license.txt")) == FAKE + "license.txt")
	var record := Runtime.acceptance(target)
	var licences_ok := (record.get("licences", []) as Array).size() == 2
	for lic: Dictionary in record.get("licences", []):
		licences_ok = licences_ok and lic.get("sha256") == Runtime.sha256_file(target.path_join(str(lic.get("file"))))
	_check("the acceptance is recorded (who, when, how, which licence files, which archive)",
			record.get("accepted_by") == "test" and record.get("accepted_on") == "2026-09-24" and record.get("how") == "a test"
			and licences_ok and record.get("sha256") == Runtime.sha256_file(zip) and record.get("tag") == Runtime.manifest()["tag"]
			and record.get("commit") == Runtime.manifest()["commit"], "%s" % record)
	for incomplete: Dictionary in [{"accepted_on": "2026-09-24", "how": "x"}, {"accepted_by": "x", "accepted_on": "2026-09-24"},
			{"accepted_by": "x", "accepted_on": "24.09.2026", "how": "x"}]:
		var other := WORK.path_join("target_unnamed")
		var why := Runtime.install_from_zip(zip, other, true, incomplete)
		_check("install refuses an acceptance without who, how or a date (%s)" % incomplete, why != "" and _dlls(other).is_empty(), why)
	_check("the runtime counts as installed afterwards", Runtime.is_installed(target) and Runtime.missing_files(target).is_empty())
	_check("no staging folder is left behind", not DirAccess.dir_exists_absolute(_abs(target).path_join(".staging")))

	# The real archive also has external/ngx-sdk/license.txt: only the root license.txt is taken.
	var nested := _make_zip("nested.zip", "streamline/", "", {"streamline/external/ngx-sdk/license.txt": "another licence"})
	var target_nested := WORK.path_join("target_nested")
	var nested_error := Runtime.install_from_zip(nested, target_nested, true, ACCEPTANCE)
	_check("a deeper file with the same name is not taken", nested_error == ""
			and FileAccess.get_file_as_string(target_nested.path_join("license.txt")) == FAKE + "license.txt", nested_error)

	var incomplete := _make_zip("incomplete.zip", "", "nvngx_dlss.dll")
	var target2 := WORK.path_join("target_incomplete")
	var error := Runtime.install_from_zip(incomplete, target2, true, ACCEPTANCE)
	_check("an incomplete archive installs nothing", error.contains("nvngx_dlss.dll") and _dlls(target2).is_empty(), error)

	var hostile := _make_zip("hostile.zip", "../") # "../" as the top-level folder
	var target3 := WORK.path_join("deep/deeper/target_hostile")
	Runtime.install_from_zip(hostile, target3, true, ACCEPTANCE)
	_check("archive paths never reach the disk", _dlls(target3) == _expected_dlls()
			and not DirAccess.dir_exists_absolute(_abs(WORK).path_join("deep/bin"))
			and not DirAccess.dir_exists_absolute(_abs(WORK).path_join("deep/deeper/bin")))


func _test_lock() -> void:
	var real := Runtime.lock()
	var m := Runtime.manifest()
	_check("the repository's lock pins the manifest's release and every file",
			real.get("tag") == m["tag"] and real.get("asset") == m["asset"] and str(real.get("sha256", "")).length() == 64
			and (real.get("files", {}) as Dictionary).size() == (m["files"] as Array).size(), "%s %s" % [real.get("tag"), real.get("sha256")])
	# A lock made for a synthetic archive: it installs; a single differing file does not.
	var zip := _make_zip("locked.zip", "")
	var files := {}
	for file: String in m["files"]:
		files[file.get_file()] = {"sha256": Runtime.sha256_bytes((FAKE + file).to_utf8_buffer())}
	var pinned := {"tag": m["tag"], "asset": m["asset"], "sha256": Runtime.sha256_file(zip), "files": files}
	var target := WORK.path_join("target_locked")
	_check("an archive matching the lock installs", Runtime.install_from_zip(zip, target, true, ACCEPTANCE, pinned) == ""
			and _dlls(target) == _expected_dlls())
	var wrong_file := pinned.duplicate(true)
	wrong_file["files"]["sl.dlss.dll"]["sha256"] = "0".repeat(64)
	var target2 := WORK.path_join("target_lock_file_mismatch")
	var error := Runtime.install_from_zip(zip, target2, true, ACCEPTANCE, wrong_file)
	_check("a file that differs from the lock installs nothing", error.contains("sl.dlss.dll") and _dlls(target2).is_empty(), error)
	var wrong_zip := pinned.duplicate(true)
	wrong_zip["sha256"] = "0".repeat(64)
	var target3 := WORK.path_join("target_lock_zip_mismatch")
	var error2 := Runtime.install_from_zip(zip, target3, true, ACCEPTANCE, wrong_zip)
	_check("an archive that differs from the lock installs nothing", error2.contains("does not match") and _dlls(target3).is_empty(), error2)


func _test_real_archive() -> void:
	var zip := OS.get_environment("GODOT_LATENCY_UPSCALING_TEST_RUNTIME_ZIP")
	if zip == "":
		print("SKIP real archive: GODOT_LATENCY_UPSCALING_TEST_RUNTIME_ZIP is not set")
		return
	var target := WORK.path_join("target_real")
	var error := Runtime.install_from_zip(zip, target, true, ACCEPTANCE, Runtime.lock())
	var installed := PackedStringArray()
	for file in DirAccess.get_files_at(_abs(target)):
		installed.append(file)
	var expected := PackedStringArray()
	for file: String in Runtime.manifest()["files"]:
		expected.append(file.get_file())
	expected.append(str(Runtime.manifest()["acceptance_record"]))
	installed.sort()
	expected.sort()
	_check("NVIDIA's release archive installs against the lock (every file's SHA-256 matches)",
			error == "" and installed == expected and Runtime.is_installed(target), "%s %s" % [error, installed])


func _new_dialog(target: String, zip: String) -> ConfirmationDialog:
	var dialog: ConfirmationDialog = RuntimeDialog.new()
	dialog.licence_text_override = "Synthetic licence text for the test."
	dialog.source_zip_override = zip
	dialog.target_override = target
	dialog.verify_lock = false # synthetic archives; the lock itself is tested in _test_lock()
	root.add_child(dialog)
	return dialog


func _test_dialog() -> void:
	var zip := _make_zip("dialog.zip", "")
	var target := WORK.path_join("target_dialog")
	var dialog := _new_dialog(target, zip)
	dialog.popup_centered()
	await process_frame
	await process_frame
	_check("dialog shows each licence", _licence_texts(dialog) == 2)
	_check("Download is disabled until the licences are accepted", not dialog.is_download_enabled())
	# Pressing OK without the acceptance (as if the button were not disabled) installs nothing.
	dialog.confirmed.emit()
	await process_frame
	_check("confirming without the acceptance installs nothing", _dlls(target).is_empty())
	dialog.set_accepted(true)
	_check("accepting enables Download", dialog.is_download_enabled())
	var result := await _download(dialog)
	_check("Download installs the runtime after the acceptance", result.size() == 2 and result[0] == true
			and _dlls(target) == _expected_dlls(), "%s" % [result])
	var record := Runtime.acceptance(target)
	_check("the dialog records the developer's acceptance (who, when, how), not the tool's",
			str(record.get("accepted_by", "")).begins_with("the developer using the Godot editor")
			and str(record.get("how", "")).contains("ticked") and str(record.get("accepted_on", "")).length() == 10
			and str(record.get("recorded_with", "")).contains("dialog"), "%s" % record)
	dialog.queue_free()

	# The download folder: by default next to the runtime's folder (like the command-line tool);
	# without one, Download stays off.
	var dialog2 := _new_dialog(WORK.path_join("target_folder"), zip)
	await process_frame
	_check("the archive is downloaded next to the runtime's folder by default",
			(dialog2.get("_folder") as LineEdit).text == _abs(WORK), (dialog2.get("_folder") as LineEdit).text)
	dialog2.set_accepted(true)
	dialog2.set_download_folder("")
	_check("without a download folder Download stays off", not dialog2.is_download_enabled())
	dialog2.queue_free()


func _licence_texts(dialog: Node) -> int:
	var count := 0
	for view in dialog.find_children("*", "TextEdit", true, false):
		if (view as TextEdit).text == "Synthetic licence text for the test.":
			count += 1
	return count


# --- the download path, against a local fake of GitHub's release API ---------------------------

var _server: TCPServer
var _routes := {} # path -> [content type, body bytes]


func _serve() -> void:
	while _server != null and _server.is_connection_available():
		var peer := _server.take_connection()
		var request := ""
		for i in 200:
			peer.poll()
			var available := peer.get_available_bytes()
			if available > 0:
				request += peer.get_utf8_string(available)
			if request.contains("\r\n\r\n"):
				break
			OS.delay_msec(5)
		var path := request.get_slice(" ", 1)
		var route: Array = _routes.get(path, ["text/plain", "not found".to_utf8_buffer()])
		var status := "200 OK" if _routes.has(path) else "404 Not Found"
		var body: PackedByteArray = route[1]
		var header := "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % [status, route[0], body.size()]
		peer.put_data(header.to_utf8_buffer())
		peer.put_data(body)
		peer.disconnect_from_host()


# Accepts, presses Download and waits for the outcome [ok, message] (serving the fake release).
func _download(dialog: ConfirmationDialog) -> Array:
	var outcome := []
	dialog.finished.connect(func(ok: bool, message: String) -> void: outcome.append_array([ok, message]), CONNECT_ONE_SHOT)
	await _await_licences(dialog) # Download only works once the licence texts are shown
	dialog.set_accepted(true)
	dialog.confirmed.emit()
	for i in 600:
		_serve()
		if not outcome.is_empty():
			break
		await process_frame
	return outcome


func _test_download_path() -> void:
	_server = TCPServer.new()
	if _server.listen(0, "127.0.0.1") != OK:
		_check("local release server", false, "cannot listen")
		return
	var base := "http://127.0.0.1:%d" % _server.get_local_port()
	var m := Runtime.manifest()
	var zip_bytes := FileAccess.get_file_as_bytes(_make_zip("served.zip", "streamline/"))
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_SHA256)
	hashing.update(zip_bytes)
	var sha := hashing.finish().hex_encode()
	var release_path := "/repos/%s/releases/tags/%s" % [m["repository"], m["tag"]]
	_routes[release_path] = ["application/json", JSON.stringify({"assets": [{"name": m["asset"], "size": zip_bytes.size(),
			"digest": "sha256:" + sha, "browser_download_url": base + "/asset.zip"}]}).to_utf8_buffer()]
	_routes["/asset.zip"] = ["application/zip", zip_bytes]

	var target := WORK.path_join("target_download")
	var dialog := _new_dialog(target, "")
	dialog.api_base_override = base
	var outcome := await _download(dialog)
	_check("download path: release looked up, archive downloaded, size and SHA-256 verified, installed",
			outcome.size() == 2 and outcome[0] == true and _dlls(target) == _expected_dlls()
			and Runtime.acceptance(target).get("sha256") == sha, "%s" % [outcome])
	dialog.queue_free()

	# A download that does not match the release's digest is rejected and installs nothing.
	_routes[release_path] = ["application/json", JSON.stringify({"assets": [{"name": m["asset"], "size": zip_bytes.size(),
			"digest": "sha256:" + "0".repeat(64), "browser_download_url": base + "/asset.zip"}]}).to_utf8_buffer()]
	var target2 := WORK.path_join("target_bad_digest")
	var dialog2 := _new_dialog(target2, "")
	dialog2.api_base_override = base
	var outcome2 := await _download(dialog2)
	_check("download path: a SHA-256 mismatch installs nothing", outcome2.size() == 2 and outcome2[0] == false
			and str(outcome2[1]).contains("SHA-256") and _dlls(target2).is_empty(), "%s" % [outcome2])
	_check("download path: the downloaded archive is removed afterwards, also after a failure",
			not DirAccess.dir_exists_absolute(_abs(WORK).path_join(".lu_nvidia_download")))
	dialog2.queue_free()

	# A download folder chosen by the developer that does not exist: nothing is fetched.
	_routes[release_path] = ["application/json", JSON.stringify({"assets": [{"name": m["asset"], "size": zip_bytes.size(),
			"digest": "sha256:" + sha, "browser_download_url": base + "/asset.zip"}]}).to_utf8_buffer()]
	var target3 := WORK.path_join("target_missing_folder")
	var dialog3 := _new_dialog(target3, "")
	dialog3.api_base_override = base
	dialog3.set_download_folder(_abs(WORK).path_join("no_such_folder"))
	var outcome3 := await _download(dialog3)
	_check("download path: a download folder that does not exist is reported, nothing installed",
			outcome3.size() == 2 and outcome3[0] == false and str(outcome3[1]).contains("does not exist") and _dlls(target3).is_empty(),
			"%s" % [outcome3])
	dialog3.queue_free()

	await _test_licence_verification(base)
	_server.stop()
	_server = null


# The licence texts shown are the pinned release's licence files: a text whose SHA-256 is not the
# lock's is not shown and keeps Download off, even with the box ticked.
func _test_licence_verification(base: String) -> void:
	var licences: Array = Runtime.manifest()["licences"]
	var texts := ["licence text zero\n", "licence text one\n"]
	var pinned := {"files": {}}
	for i in licences.size():
		pinned["files"][licences[i]["file"]] = {"sha256": Runtime.sha256_bytes(texts[i].to_utf8_buffer())}
	_routes["/licence0"] = ["text/plain", texts[0].to_utf8_buffer()]
	_routes["/licence1"] = ["text/plain", "a different text\n".to_utf8_buffer()]
	var dialog := _verifying_dialog(base, pinned)
	await _await_licences(dialog)
	dialog.set_accepted(true)
	var views: Array = dialog.find_children("*", "TextEdit", true, false)
	_check("a licence text that is not the pinned file is not shown and Download stays off",
			not dialog.licences_verified() and not dialog.is_download_enabled() and (views[1] as TextEdit).text.begins_with("Not shown")
			and (views[0] as TextEdit).text == texts[0], "%s | %s" % [(views[0] as TextEdit).text, (views[1] as TextEdit).text])
	dialog.queue_free()
	_routes["/licence1"] = ["text/plain", texts[1].to_utf8_buffer()]
	var dialog2 := _verifying_dialog(base, pinned)
	await _await_licences(dialog2)
	_check("verified licence texts are shown; Download needs the box ticked too",
			dialog2.licences_verified() and not dialog2.is_download_enabled())
	dialog2.set_accepted(true)
	_check("with verified texts and the box ticked Download is on", dialog2.is_download_enabled())
	dialog2.queue_free()


func _verifying_dialog(base: String, pinned: Dictionary) -> ConfirmationDialog:
	var dialog: ConfirmationDialog = RuntimeDialog.new()
	dialog.licence_url_overrides = PackedStringArray([base + "/licence0", base + "/licence1"])
	dialog.pinned_override = pinned
	dialog.target_override = WORK.path_join("target_verification")
	root.add_child(dialog)
	return dialog


func _await_licences(dialog: ConfirmationDialog) -> void:
	for i in 600:
		_serve()
		var loading := false
		for view in dialog.find_children("*", "TextEdit", true, false):
			loading = loading or (view as TextEdit).text.begins_with("Loading")
		if not loading:
			return
		await process_frame
