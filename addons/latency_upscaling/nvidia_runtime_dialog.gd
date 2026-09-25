@tool
extends ConfirmationDialog
## Asks the developer to read and accept NVIDIA's licences, then downloads the NVIDIA runtime for
## DLSS from NVIDIA's own GitHub release and installs it (nvidia_runtime.gd).
##
## The licences shown are the licence files of the pinned release (nvidia_runtime.json "licences":
## the same files at the release's commit), shown only after their SHA-256 matched the lock's, so
## the text accepted is the text that governs the pinned DLLs. Nothing is downloaded before every
## licence text is verified, the developer ticks the acceptance box and presses Download; the
## Download button stays disabled until then. The archive's size is shown up front, and the
## developer picks the folder it is downloaded to (default: next to the runtime's folder, as
## tools/fetch_nvidia_sdk.py does; it is deleted after the install). Built from plain editor
## controls so it follows the editor's theme.

signal finished(ok: bool, message: String)

const Runtime := preload("res://addons/latency_upscaling/nvidia_runtime.gd")
const USER_AGENT := "godot-latency-upscaling"
const SPACE_MARGIN := 64 * 1024 * 1024 # bytes kept free beyond the archive and the installed files

## Tests: use this local zip instead of downloading the release, and this text (counted as verified)
## instead of fetching the licences. Empty: the real sources.
var source_zip_override := ""
var licence_text_override := ""
## Tests: fetch the licence texts from these URLs instead of the manifest's, and verify them against
## this lock instead of nvidia_runtime.lock.json.
var licence_url_overrides := PackedStringArray()
var pinned_override := {}
## Where to install (default: the manifest's target).
var target_override := ""
## Tests: a local stand-in for https://api.github.com (the download path without the network).
var api_base_override := ""
## Tests with synthetic archives turn this off; otherwise the archive and every file must match
## nvidia_runtime.lock.json.
var verify_lock := true

var _manifest := {}
var _accept: CheckBox
var _status: Label
var _progress: ProgressBar
var _folder: LineEdit
var _folder_dialog: FileDialog
var _licence_views: Array[TextEdit] = []
var _licence_notes: Array[Label] = []
var _verified: Array[bool] = []
var _busy := false


func _init() -> void:
	title = "NVIDIA DLSS runtime"
	dialog_hide_on_ok = false
	min_size = Vector2i(780, 700)
	_manifest = Runtime.manifest()
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	add_child(box)

	var intro := Label.new()
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.custom_minimum_size.x = 740
	intro.text = ("DLSS needs NVIDIA's runtime (Streamline %s and the DLSS library). It is licensed by NVIDIA, is not "
			+ "part of this addon, and comes from NVIDIA's GitHub release: %s, %s. It is installed into %s (%s, kept out "
			+ "of version control). Below are the licence files of exactly that release: downloading the runtime means "
			+ "you accept them.") % [_manifest.get("tag", "?"), _manifest.get("asset", "?"), _megabytes(Runtime.archive_size()),
				_manifest.get("target", "?"), _megabytes(Runtime.installed_size())]
	box.add_child(intro)

	for lic: Dictionary in _manifest.get("licences", []):
		var header := HBoxContainer.new()
		var name_label := Label.new()
		name_label.text = "%s: %s" % [lic["name"], lic.get("file", "")]
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		header.add_child(name_label)
		var link := LinkButton.new()
		link.text = "Open in browser"
		link.uri = str(lic["page"]) # pinned to the release's commit
		header.add_child(link)
		box.add_child(header)
		var view := TextEdit.new()
		view.editable = false
		view.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
		view.custom_minimum_size = Vector2(740, 160)
		view.size_flags_vertical = Control.SIZE_EXPAND_FILL
		view.text = "Loading %s of %s %s ..." % [lic.get("file", ""), _manifest.get("repository", ""), _manifest.get("tag", "")]
		box.add_child(view)
		_licence_views.append(view)
		var note := Label.new()
		note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		note.custom_minimum_size.x = 740
		box.add_child(note)
		_licence_notes.append(note)
		_verified.append(false)

	var folder_row := HBoxContainer.new()
	var folder_label := Label.new()
	folder_label.text = "Download the archive to:"
	folder_row.add_child(folder_label)
	_folder = LineEdit.new()
	_folder.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_folder.text_changed.connect(func(_text: String) -> void: _update_buttons())
	folder_row.add_child(_folder)
	var browse := Button.new()
	browse.text = "Browse..."
	browse.pressed.connect(_on_browse)
	folder_row.add_child(browse)
	box.add_child(folder_row)

	_accept = CheckBox.new()
	_accept.text = "I have read and accept the licences above"
	_accept.toggled.connect(func(_on: bool) -> void: _update_buttons())
	box.add_child(_accept)
	_progress = ProgressBar.new()
	_progress.visible = false
	box.add_child(_progress)
	_status = Label.new()
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.custom_minimum_size.x = 740
	box.add_child(_status)

	_folder_dialog = FileDialog.new()
	_folder_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_folder_dialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_folder_dialog.dir_selected.connect(func(dir: String) -> void:
			_folder.text = dir
			_update_buttons())
	add_child(_folder_dialog)

	get_ok_button().text = "Download"
	confirmed.connect(_on_download)


func _ready() -> void:
	# Default: the folder that holds the runtime's folder, like tools/fetch_nvidia_sdk.py (the
	# system drive may be short of space for the archive).
	if _folder.text == "":
		_folder.text = ProjectSettings.globalize_path(_target()).get_base_dir()
	_update_buttons()
	_load_licence_texts.call_deferred()


func is_download_enabled() -> bool:
	return not get_ok_button().disabled


func set_accepted(accepted: bool) -> void:
	_accept.button_pressed = accepted
	_update_buttons()


func set_download_folder(folder: String) -> void:
	_folder.text = folder
	_update_buttons()


func licences_verified() -> bool:
	return not _verified.is_empty() and not _verified.has(false)


func _update_buttons() -> void:
	get_ok_button().disabled = _busy or not _accept.button_pressed or not licences_verified() or _folder.text.strip_edges() == ""


func _target() -> String:
	return target_override if target_override != "" else Runtime.target_dir()


func _set_status(text: String) -> void:
	_status.text = text


func _on_browse() -> void:
	_folder_dialog.current_dir = _folder.text
	_folder_dialog.popup_centered_ratio(0.6)


static func _megabytes(bytes: int) -> String:
	return "%.0f MB" % (bytes / 1e6) if bytes > 0 else "size unknown"


# --- licences -------------------------------------------------------------------------------

func _load_licence_texts() -> void:
	var licences: Array = _manifest.get("licences", [])
	var pinned := pinned_override if not pinned_override.is_empty() else Runtime.lock()
	for i in licences.size():
		if licence_text_override != "":
			_licence_views[i].text = licence_text_override
			_licence_notes[i].text = "Test text (not fetched)."
			_verified[i] = true
			continue
		var url := licence_url_overrides[i] if i < licence_url_overrides.size() else str(licences[i]["text"])
		var result := await _http_get(url)
		if not result["ok"]:
			_licence_views[i].text = "The licence text could not be loaded (%s)." % result["error"]
			_licence_notes[i].text = "Read it with \"Open in browser\" (%s). Download stays off until the text is shown here." % licences[i]["page"]
			continue
		var body: PackedByteArray = result["body"]
		var problem := Runtime.licence_text_problem(body, licences[i], pinned)
		if problem != "":
			_licence_views[i].text = "Not shown: " + problem
			_licence_notes[i].text = "Download stays off: the text accepted must be the text that ships with the DLLs."
			continue
		_licence_views[i].text = body.get_string_from_utf8()
		_licence_notes[i].text = "This is %s of %s (%s %s, commit %s): SHA-256 verified against %s." % [
				licences[i]["file"], _manifest.get("asset", ""), _manifest.get("repository", ""), _manifest.get("tag", ""),
				str(_manifest.get("commit", "")).left(12), Runtime.LOCK_PATH.get_file()]
		_verified[i] = true
	_update_buttons()


# --- download and install -------------------------------------------------------------------

func _on_download() -> void:
	if _busy or not _accept.button_pressed or not licences_verified():
		return # the button is disabled then; never download without the acceptance
	_busy = true
	_update_buttons()
	var message := await _download_and_install()
	_busy = false
	_update_buttons()
	_progress.visible = false
	var ok := message == ""
	_set_status("Installed into %s. Restart the editor (and running games) to use DLSS." % _target() if ok else "Failed: " + message)
	finished.emit(ok, message)


## Who accepted, when and how: the developer using this editor, who ticked the box after the
## verified licence files were shown.
func _acceptance() -> Dictionary:
	var user := OS.get_environment("USERNAME")
	if user == "":
		user = OS.get_environment("USER")
	return {
		"accepted_by": "the developer using the Godot editor" + (" (user %s)" % user if user != "" else ""),
		"accepted_on": Time.get_date_string_from_system(true),
		"how": ("ticked \"I have read and accept the licences above\" in the Latency & Upscaling plugin's dialog, "
				+ "which showed the licence files of the pinned release (SHA-256 verified), and pressed Download"),
		"recorded_with": "the editor dialog of the Latency & Upscaling plugin",
	}


func _download_and_install() -> String:
	var zip_path := source_zip_override
	var work_dir := ""
	if zip_path == "":
		work_dir = _folder.text.strip_edges().path_join(".lu_nvidia_download")
		var parent := DirAccess.open(_folder.text.strip_edges())
		if parent == null:
			return "the download folder %s does not exist" % _folder.text
		_set_status("Looking up %s %s ..." % [_manifest["repository"], _manifest["tag"]])
		var api_base := api_base_override if api_base_override != "" else "https://api.github.com"
		var api := "%s/repos/%s/releases/tags/%s" % [api_base, _manifest["repository"], _manifest["tag"]]
		var release := await _http_get(api, ["Accept: application/vnd.github+json"])
		if not release["ok"]:
			return "the release could not be looked up (%s)" % release["error"]
		var parsed: Variant = JSON.parse_string((release["body"] as PackedByteArray).get_string_from_utf8())
		var asset := {}
		if parsed is Dictionary:
			for candidate: Dictionary in parsed.get("assets", []):
				if candidate.get("name") == _manifest["asset"]:
					asset = candidate
		if asset.is_empty():
			return "the release has no %s" % _manifest["asset"]
		# The archive and, if the runtime's folder is on the same drive, the files taken from it.
		var needed := int(asset["size"]) + Runtime.installed_size() + SPACE_MARGIN
		if parent.get_space_left() < needed:
			return "%s has %s free, the download needs %s" % [_folder.text, _megabytes(parent.get_space_left()), _megabytes(needed)]
		DirAccess.make_dir_recursive_absolute(work_dir)
		zip_path = work_dir.path_join(str(_manifest["asset"]))
		_set_status("Downloading %s (%s) into %s ..." % [asset["name"], _megabytes(int(asset["size"])), _folder.text])
		_progress.visible = true
		var downloaded := await _http_get(str(asset["browser_download_url"]), [], zip_path, int(asset["size"]))
		var problem := ""
		if not downloaded["ok"]:
			problem = "the download failed (%s)" % downloaded["error"]
		elif _file_size(zip_path) != int(asset["size"]):
			problem = "downloaded %d bytes, the release lists %d" % [_file_size(zip_path), int(asset["size"])]
		else:
			var digest := str(asset.get("digest", ""))
			if digest.begins_with("sha256:") and digest.substr(7).to_lower() != Runtime.sha256_file(zip_path):
				problem = "the download does not match the release's SHA-256"
		if problem != "":
			_remove_download(work_dir)
			return problem
	_set_status("Installing ...")
	var message := Runtime.install_from_zip(zip_path, _target(), _accept.button_pressed, _acceptance(),
			Runtime.lock() if verify_lock else {})
	if work_dir != "":
		_remove_download(work_dir)
	return message


static func _remove_download(work_dir: String) -> void:
	for file in DirAccess.get_files_at(work_dir):
		DirAccess.remove_absolute(work_dir.path_join(file))
	DirAccess.remove_absolute(work_dir)


static func _file_size(path: String) -> int:
	var file := FileAccess.open(path, FileAccess.READ)
	return file.get_length() if file != null else -1


# One HTTP GET: {ok, body, error}. Each request gets its own HTTPRequest, so a licence text still
# loading never blocks the download. With `download_to`, the body goes to that file and the
# progress bar follows it (_process).
func _http_get(url: String, headers: PackedStringArray = [], download_to: String = "", expected_size: int = 0) -> Dictionary:
	var http := HTTPRequest.new()
	http.use_threads = true
	http.download_file = download_to
	add_child(http)
	var request_headers := PackedStringArray(["User-Agent: " + USER_AGENT])
	request_headers.append_array(headers)
	if http.request(url, request_headers) != OK:
		http.queue_free()
		return {"ok": false, "body": PackedByteArray(), "error": "the request could not be started"}
	if download_to != "":
		_download = http
		_expected_size = expected_size
	var done: Array = await http.request_completed # result, response code, headers, body
	if _download == http:
		_download = null
		_expected_size = 0
	http.queue_free()
	var result: int = done[0]
	var code: int = done[1]
	if result != HTTPRequest.RESULT_SUCCESS or code < 200 or code >= 300:
		return {"ok": false, "body": done[3], "error": "result %d, HTTP %d" % [result, code]}
	return {"ok": true, "body": done[3], "error": ""}


var _download: HTTPRequest
var _expected_size := 0


func _process(_delta: float) -> void:
	if _download != null and _expected_size > 0:
		_progress.value = 100.0 * _download.get_downloaded_bytes() / _expected_size
