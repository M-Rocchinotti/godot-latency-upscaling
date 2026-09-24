@tool
extends ConfirmationDialog
## Asks the developer to read and accept NVIDIA's licences, then downloads the NVIDIA runtime for
## DLSS from NVIDIA's own GitHub release and installs it (nvidia_runtime.gd).
##
## Nothing is downloaded before the developer ticks the acceptance box and presses Download; the
## Download button stays disabled until then. Built from plain editor controls so it follows the
## editor's theme.

signal finished(ok: bool, message: String)

const Runtime := preload("res://addons/latency_upscaling/nvidia_runtime.gd")
const USER_AGENT := "godot-latency-upscaling"

## Tests: use this local zip instead of downloading the release, and these texts instead of
## fetching the licences. Empty: the real sources.
var source_zip_override := ""
var licence_text_override := ""
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
var _licence_views: Array[TextEdit] = []
var _busy := false


func _init() -> void:
	title = "NVIDIA DLSS runtime"
	dialog_hide_on_ok = false
	min_size = Vector2i(760, 620)
	_manifest = Runtime.manifest()
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	add_child(box)

	var intro := Label.new()
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.custom_minimum_size.x = 720
	intro.text = ("DLSS needs NVIDIA's runtime (Streamline %s and the DLSS library). It is licensed by NVIDIA, "
			+ "is not part of this addon, and is downloaded from NVIDIA's GitHub release into %s (kept out of version "
			+ "control). Read the licences below: downloading the runtime means you accept them.") % [
				_manifest.get("tag", "?"), _manifest.get("target", "?")]
	box.add_child(intro)

	for lic: Dictionary in _manifest.get("licences", []):
		var header := HBoxContainer.new()
		var name_label := Label.new()
		name_label.text = str(lic["name"])
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		header.add_child(name_label)
		var link := LinkButton.new()
		link.text = "Open in browser"
		link.uri = str(lic["page"])
		header.add_child(link)
		box.add_child(header)
		var view := TextEdit.new()
		view.editable = false
		view.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
		view.custom_minimum_size = Vector2(720, 170)
		view.size_flags_vertical = Control.SIZE_EXPAND_FILL
		view.text = "Loading the licence text from %s ..." % lic["text"]
		box.add_child(view)
		_licence_views.append(view)

	_accept = CheckBox.new()
	_accept.text = "I have read and accept the licences above"
	_accept.toggled.connect(func(_on: bool) -> void: _update_buttons())
	box.add_child(_accept)
	_progress = ProgressBar.new()
	_progress.visible = false
	box.add_child(_progress)
	_status = Label.new()
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.custom_minimum_size.x = 720
	box.add_child(_status)

	get_ok_button().text = "Download"
	confirmed.connect(_on_download)


func _ready() -> void:
	_update_buttons()
	_load_licence_texts.call_deferred()


func is_download_enabled() -> bool:
	return not get_ok_button().disabled


func set_accepted(accepted: bool) -> void:
	_accept.button_pressed = accepted
	_update_buttons()


func _update_buttons() -> void:
	get_ok_button().disabled = _busy or not _accept.button_pressed


func _target() -> String:
	return target_override if target_override != "" else Runtime.target_dir()


func _set_status(text: String) -> void:
	_status.text = text


# --- licences -------------------------------------------------------------------------------

func _load_licence_texts() -> void:
	var licences: Array = _manifest.get("licences", [])
	for i in licences.size():
		if licence_text_override != "":
			_licence_views[i].text = licence_text_override
			continue
		var result := await _http_get(str(licences[i]["text"]))
		if result["ok"]:
			_licence_views[i].text = (result["body"] as PackedByteArray).get_string_from_utf8()
		else:
			_licence_views[i].text = "The licence text could not be loaded (%s). Read it with \"Open in browser\": %s" % [
					result["error"], licences[i]["page"]]


# --- download and install -------------------------------------------------------------------

func _on_download() -> void:
	if _busy or not _accept.button_pressed:
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


func _download_and_install() -> String:
	var zip_path := source_zip_override
	if zip_path == "":
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
		zip_path = OS.get_user_data_dir().path_join("latency_upscaling_nvidia").path_join(str(_manifest["asset"]))
		DirAccess.make_dir_recursive_absolute(zip_path.get_base_dir())
		_set_status("Downloading %s (%.0f MB) ..." % [asset["name"], float(asset["size"]) / 1e6])
		_progress.visible = true
		var downloaded := await _http_get(str(asset["browser_download_url"]), [], zip_path, int(asset["size"]))
		if not downloaded["ok"]:
			return "the download failed (%s)" % downloaded["error"]
		var size := _file_size(zip_path)
		if size != int(asset["size"]):
			return "downloaded %d bytes, the release lists %d" % [size, int(asset["size"])]
		var digest := str(asset.get("digest", ""))
		if digest.begins_with("sha256:") and digest.substr(7).to_lower() != Runtime.sha256_file(zip_path):
			return "the download does not match the release's SHA-256"
	_set_status("Installing ...")
	var message := Runtime.install_from_zip(zip_path, _target(), _accept.button_pressed, "editor dialog (licence box ticked)",
			Runtime.lock() if verify_lock else {})
	if source_zip_override == "":
		DirAccess.remove_absolute(zip_path)
	return message


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
