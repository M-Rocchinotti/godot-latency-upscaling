extends SceneTree
## Camera cuts end to end under the configuration the run was started with (tests/run_all.py,
## suite "upscaler"), with the reference upscaler:
##
##   --expect-angle=A --expect-distance=D   the thresholds the settings configured (project setting or
##                                          GODOT_LATENCY_UPSCALING_SETTINGS): the status reports them,
##                                          a turn / move just below them keeps the history, one past
##                                          them resets it on that frame
##   --reflex-off                           Reflex is off on this device (its extension hidden, mode Off):
##                                          the upscaler still tracks the camera before each frame renders
## Every run also switches to another camera: the history resets on its first frame, which is upscaled.
## Ends "cut run: <passed> passed, <failed> failed".

const TestScene := preload("res://tests/test_scene.gd")

var _lu: Object
var _camera: Camera3D
var _passed := 0
var _failed := 0
var _expect_angle := -1.0
var _expect_distance := -1.0
var _reflex_off := false


func _initialize() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(Vector2i(1280, 720))
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--expect-angle="):
			_expect_angle = float(arg.get_slice("=", 1))
		elif arg.begins_with("--expect-distance="):
			_expect_distance = float(arg.get_slice("=", 1))
		elif arg == "--reflex-off":
			_reflex_off = true
	_camera = TestScene.build(root)
	_lu = Engine.get_singleton("LatencyUpscaling") if Engine.has_singleton("LatencyUpscaling") else null
	_run.call_deferred()


func _check(name: String, ok: bool, detail: String = "") -> void:
	print(("PASS " if ok else "FAIL ") + name + ("  " + detail if detail != "" else ""))
	if ok:
		_passed += 1
	else:
		_failed += 1


func _frames(n: int) -> void:
	for i in n:
		await process_frame


func _status() -> Dictionary:
	return _lu.get_status()


func _resets() -> Dictionary:
	return _status()["upscaling"]["history_resets"].duplicate()


func _added(before: Dictionary) -> Dictionary:
	var added := {}
	var now := _resets()
	for key: String in now:
		var n: int = int(now[key]) - int(before.get(key, 0))
		if n != 0:
			added[key] = n
	return added


func _run() -> void:
	if _lu == null:
		_check("the LatencyUpscaling singleton exists", false)
		_finish()
		return
	for i in 300:
		await process_frame
		var up: Dictionary = _status()["upscaling"]
		if up["active"] and up["frames_since_reset"] > 4:
			break
	_check("the reference upscaler is active", _status()["upscaling"]["active"], _lu.get_dlss_status())
	var settings: Dictionary = _status()["settings"]
	var angle: float = settings["dlss/cut_angle_degrees"]["value"]
	var distance: float = settings["dlss/cut_distance"]["value"]
	if _expect_angle >= 0.0:
		_check("the status reports the configured cut angle", is_equal_approx(angle, _expect_angle),
				"%s from %s" % [angle, settings["dlss/cut_angle_degrees"]["source"]])
		await _test_turns(angle)
	if _expect_distance >= 0.0:
		_check("the status reports the configured cut distance", is_equal_approx(distance, _expect_distance),
				"%s from %s" % [distance, settings["dlss/cut_distance"]["source"]])
		await _test_moves(distance)
	if _reflex_off:
		var reflex: Dictionary = _status()["reflex"]
		_check("Reflex is off on this device", not reflex["supported"] and not reflex["active"], "%s" % reflex.get("reason", ""))
	await _test_switch()
	_finish()


func _finish() -> void:
	print("cut run: %d passed, %d failed" % [_passed, _failed])
	quit(1 if _failed > 0 else 0)


# Just below the angle every frame keeps the history; just past it resets on every frame.
func _test_turns(angle: float) -> void:
	TestScene.reset_camera(_camera)
	await _frames(4)
	var before := _resets()
	for i in 20:
		_camera.rotation.y += deg_to_rad(angle * 0.8)
		await process_frame
	var added := _added(before)
	_check("turning %.2f degrees per frame (below the configured %.2f) keeps the history" % [angle * 0.8, angle],
			added.is_empty(), "added %s" % added)
	before = _resets()
	for i in 10:
		_camera.rotation.y += deg_to_rad(angle * 1.5)
		await process_frame
	added = _added(before)
	_check("turning %.2f degrees per frame (past it) resets the history every frame" % (angle * 1.5),
			added == {"rotation": 10}, "added %s" % added)
	TestScene.reset_camera(_camera)
	await _frames(4)


func _test_moves(distance: float) -> void:
	TestScene.reset_camera(_camera)
	await _frames(4)
	var before := _resets()
	for i in 10:
		_camera.position.x += distance * 0.8
		await process_frame
	var added := _added(before)
	_check("moving %.2f per frame (below the configured %.2f) keeps the history" % [distance * 0.8, distance],
			added.is_empty(), "added %s" % added)
	before = _resets()
	_camera.position.x += distance * 1.5
	await process_frame
	added = _added(before)
	_check("a jump of %.2f (past it) resets the history on that frame" % (distance * 1.5),
			added == {"translation": 1} and _status()["upscaling"]["frames_since_reset"] == 1, "added %s" % added)
	TestScene.reset_camera(_camera)
	await _frames(4)


# The switch reaches the upscaler before the new camera's first frame renders (frame_pre_draw).
func _test_switch() -> void:
	var other := Camera3D.new()
	other.fov = _camera.fov
	root.add_child(other)
	other.global_transform = _camera.global_transform
	await _frames(3)
	var before := _resets()
	var upscaled: int = _status()["upscaling"]["frames_upscaled"]
	other.make_current()
	await process_frame
	var up: Dictionary = _status()["upscaling"]
	var added := _added(before)
	_check("a camera switch resets the history on the new camera's first frame, which is upscaled",
			added == {"camera_switch": 1} and up["frames_since_reset"] == 1 and up["frames_upscaled"] == upscaled + 1,
			"added %s, frames since reset %d, frames upscaled +%d" % [added, up["frames_since_reset"], up["frames_upscaled"] - upscaled])
	_camera.make_current()
	other.queue_free()
	await _frames(2)
