extends SceneTree
## Image tests of the Phase 2 plumbing with the reference upscaler (debug builds):
##
##     python tools/godot.py --env GODOT_LATENCY_UPSCALING_TEST_UPSCALER=reference \
##         --env "GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=4" --script res://tests/upscaler_tests.gd
##
## With GODOT_LATENCY_UPSCALING_TEST_UPSCALER=native every check below runs on the native path: the
## layer replaces the reference pipeline's dispatch in Godot's command buffer with its own recording.
##
## A procedural, static scene with fine detail (a checkerboard floor, thin bars) at 1280x720,
## upscaled from Performance (2x per axis). Checks:
##   slot        the viewport runs in the FSR2 slot, Godot's buffers are taken over once, output is
##               target size and render size is half of it
##   jitter      phase count 8 * ratio^2; the CPU Halton phase, the jittered projection and Godot's
##               own scene UBO (taa_jitter, taa_frame_count) agree every frame
##   still       against a 2x supersampled ground truth, the converged reference upscaler beats
##               bilinear and every deliberately wrong jitter convention (so Godot's convention is
##               the one in use), and is in the range of Godot's own FSR2
##   velocity    a camera yaw / pitch of known size gives the expected centre velocity: current ->
##               previous, UV units, Y down (the convention DLSS and FSR2 expect)
##   fill        the sky (no geometry, Godot writes no motion there after the takeover) gets the
##               camera's motion from Godot's own formula (godot/velocity_fill.h): the expected
##               centre velocity for a yaw and a pitch with the centre on the sky; derived from depth
##               on static geometry, the formula equals Godot's vectors under a turn and a strafe;
##               elsewhere the fill copies Godot's vectors unchanged
##   motion      with the camera moving, correct motion vectors beat flipped ones
##   cuts        history resets on exactly the frame of a 150 degree turn, a 20 m jump, a switch to
##               another camera (whose first frame is upscaled too) and reset_dlss_history(); none
##               under fast normal motion, none for a turn with the detection off
##   native      (native backend) every marker dispatch is replaced by the native recording, which
##               equals Godot's own recording of the same shader from the same jitter phase, and is
##               what reaches the screen; (reference backend) nothing is intercepted
## Ends "upscaler tests: <passed> passed, <failed> failed". Captures go to build/upscaler/ (native
## backend: build/upscaler_native/).

const TestScene := preload("res://tests/test_scene.gd")
const SIZE := Vector2i(1280, 720)
const CONVERGE_FRAMES := 96
var OUT_DIR := "res://build/upscaler"

var _lu: Object
var _camera: Camera3D
var _passed := 0
var _failed := 0
var _results := {}
var _truth: Image # the supersampled still, from _test_still


func _initialize() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(SIZE)
	_build_scene()
	_lu = Engine.get_singleton("LatencyUpscaling") if Engine.has_singleton("LatencyUpscaling") else null
	if OS.get_environment("GODOT_LATENCY_UPSCALING_TEST_UPSCALER") == "native":
		OUT_DIR = "res://build/upscaler_native"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUT_DIR))
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
	return _lu.get_status()["upscaling"]


# --- scene --------------------------------------------------------------------------------------

func _build_scene() -> void:
	_camera = TestScene.build(root)


func _reset_camera() -> void:
	TestScene.reset_camera(_camera)


func _capture(name: String) -> Image:
	var image := root.get_texture().get_image()
	image.save_png(OUT_DIR.path_join(name + ".png"))
	return image


func _psnr(image: Image, truth: Image) -> float:
	return float(image.compute_image_metrics(truth, false)["peak_snr"])


# Native viewport modes with the addon's upscaler off (it restores the viewport when disabled).
func _native(mode: Viewport.Scaling3DMode, scale: float, frames: int, name: String) -> Image:
	_lu.dlss_enabled = false
	await _frames(2)
	root.scaling_3d_mode = mode
	root.scaling_3d_scale = scale
	await _frames(frames)
	return _capture(name)


# The reference upscaler with the given conventions (+1 = Godot's), converged on the still scene.
func _upscaled(jitter_x: float, jitter_y: float, motion: float, name: String) -> Image:
	_lu.dlss_enabled = true
	_lu._set_upscaler_test_conventions(jitter_x, jitter_y, motion)
	var takeovers: int = _status()["takeovers"]
	for i in 120: # re-enabling changes the viewport mode: Godot reconfigures, one takeover follows
		if _status()["takeovers"] > takeovers and _status()["active"]:
			break
		await process_frame
	_lu._reset_upscaler_history()
	await _frames(CONVERGE_FRAMES)
	return _capture(name)


# --- tests --------------------------------------------------------------------------------------

func _run() -> void:
	if _lu == null or not _lu.has_method("_set_upscaler_test_conventions"):
		_check("debug build with the LatencyUpscaling singleton", false)
		_finish()
		return
	await _frames(10)
	for i in 300:
		if _status()["active"] and _status()["frames_upscaled"] > 10:
			break
		await process_frame
	await _test_slot()
	await _test_jitter()
	await _test_still()
	await _test_velocity()
	await _test_velocity_fill()
	await _test_motion()
	await _test_cuts()
	await _test_native()
	_finish()


func _finish() -> void:
	print("UPSCALER_RESULTS " + JSON.stringify(_results))
	print("upscaler tests: %d passed, %d failed" % [_passed, _failed])
	quit(1 if _failed > 0 else 0)


func _test_slot() -> void:
	var s := _status()
	_check("reference upscaler active", s["active"] and s["backend"] == "reference", _lu.get_dlss_status())
	_check("viewport runs in the FSR2 slot at the Performance scale",
			root.scaling_3d_mode == Viewport.SCALING_3D_MODE_FSR2 and is_equal_approx(root.scaling_3d_scale, 0.5),
			"mode %d scale %.3f" % [root.scaling_3d_mode, root.scaling_3d_scale])
	_check("Godot's buffers taken over once, after one FSR2 frame", s["takeovers"] == 1 and s["fsr2_frames"] >= 1,
			"takeovers %d, fsr2 frames %d" % [s["takeovers"], s["fsr2_frames"]])
	var target: Vector2i = s["target_size"]
	var render: Vector2i = s["render_size"]
	_check("output is target size", target == Vector2i(root.get_visible_rect().size), "target %s" % target)
	_check("render size is half the target (Performance, 2x)", abs(render.x * 2 - target.x) <= 1 and abs(render.y * 2 - target.y) <= 1,
			"render %s target %s" % [render, target])
	var image := _capture("slot")
	_check("captured frame is target size", image.get_size() == target, "%s" % image.get_size())
	_check("frame is not empty", image.compute_image_metrics(Image.create_empty(image.get_width(), image.get_height(), false, image.get_format()), false)["mean"] > 10.0)
	_results["render_size"] = [render.x, render.y]
	_results["target_size"] = [target.x, target.y]


func _test_jitter() -> void:
	_lu._request_upscaler_readbacks(40)
	await _frames(60)
	var j: Dictionary = _status()["jitter"]
	var s := _status()
	var ratio := float(s["target_size"].x) / float(s["render_size"].x)
	_check("jitter phase count is 8 * ratio^2 (Godot's, DLSS guide 3.7.1)", j["phase_count"] == int(8.0 * ratio * ratio),
			"%d phases at ratio %.3f" % [j["phase_count"], ratio])
	_check("CPU Halton phase matches the jittered projection every frame", j["checks"] > 50 and j["mismatches"] == 0,
			"%d checks, %d mismatches" % [j["checks"], j["mismatches"]])
	_check("Godot's scene UBO (taa_jitter, taa_frame_count) matches every frame", j["ubo_checks"] >= 30 and j["ubo_mismatches"] == 0,
			"%d UBO read-backs, %d mismatches" % [j["ubo_checks"], j["ubo_mismatches"]])
	var last: Vector2 = j["last_pixels"]
	_check("jitter is within +-0.5 render pixels (DLSS guide 3.7.3)", abs(last.x) <= 0.5 and abs(last.y) <= 0.5, "%s" % last)
	_results["jitter"] = j


func _test_still() -> void:
	var truth := await _native(Viewport.SCALING_3D_MODE_BILINEAR, 2.0, 12, "truth_ssaa2x")
	var bilinear := await _native(Viewport.SCALING_3D_MODE_BILINEAR, 0.5, 8, "bilinear_0.5")
	var fsr2 := await _native(Viewport.SCALING_3D_MODE_FSR2, 0.5, CONVERGE_FRAMES, "fsr2_0.5")
	_truth = truth
	var reference := await _upscaled(1, 1, 1, "reference")
	var no_jitter := await _upscaled(0, 0, 1, "reference_jitter_ignored")
	var flip_x := await _upscaled(-1, 1, 1, "reference_jitter_flip_x")
	var flip_y := await _upscaled(1, -1, 1, "reference_jitter_flip_y")
	var p := {
		"bilinear": _psnr(bilinear, truth), "fsr2": _psnr(fsr2, truth), "reference": _psnr(reference, truth),
		"jitter_ignored": _psnr(no_jitter, truth), "jitter_flip_x": _psnr(flip_x, truth), "jitter_flip_y": _psnr(flip_y, truth),
	}
	_results["still_psnr"] = p
	var detail := "PSNR dB: reference %.2f, bilinear %.2f, FSR2 %.2f, jitter ignored %.2f, flip x %.2f, flip y %.2f" % [
		p["reference"], p["bilinear"], p["fsr2"], p["jitter_ignored"], p["jitter_flip_x"], p["jitter_flip_y"]]
	print("STILL " + detail)
	_check("converged reference upscaler beats bilinear", p["reference"] > p["bilinear"] + 2.0, detail)
	_check("Godot's jitter convention beats ignoring the jitter", p["reference"] > p["jitter_ignored"] + 1.0, detail)
	_check("Godot's jitter convention beats a flipped X", p["reference"] > p["jitter_flip_x"] + 1.0, detail)
	_check("Godot's jitter convention beats a flipped Y", p["reference"] > p["jitter_flip_y"] + 1.0, detail)
	# A toy upscaler is not FSR2, but a working temporal path closes much of the gap to it.
	_check("reference upscaler closes at least 40% of the bilinear-to-FSR2 gap",
			p["reference"] > p["bilinear"] + 0.4 * (p["fsr2"] - p["bilinear"]), detail)


# Centre velocity for a known camera rotation per frame. Current -> previous in UV units, Y down.
func _velocity_for(axis: String, step: float) -> Vector2:
	_reset_camera()
	await _frames(4)
	_lu._request_upscaler_readbacks(24)
	var start: float = _camera.rotation.y if axis == "yaw" else _camera.rotation.x
	for i in 30:
		if axis == "yaw":
			_camera.rotation.y = start + step * (i + 1)
		else:
			_camera.rotation.x = start + step * (i + 1)
		await process_frame
	var median := _median_tail(_status()["velocity_samples"])
	_reset_camera()
	return median


# Per-component median of the last 12 read-backs.
func _median_tail(samples: Array) -> Vector2:
	var tail := samples.slice(max(0, samples.size() - 12))
	if tail.is_empty():
		return Vector2(INF, INF)
	var xs: Array = tail.map(func(v: Vector2) -> float: return v.x)
	var ys: Array = tail.map(func(v: Vector2) -> float: return v.y)
	xs.sort()
	ys.sort()
	return Vector2(xs[xs.size() / 2], ys[ys.size() / 2])


func _test_velocity() -> void:
	_lu.dlss_enabled = true
	_lu._set_upscaler_test_conventions(1, 1, 1)
	await _frames(20)
	var step := 0.004 # radians per frame
	var target: Vector2i = _status()["target_size"]
	var aspect := float(target.x) / float(target.y)
	var tan_half_v := tan(deg_to_rad(_camera.fov) * 0.5)
	var tan_half_h := tan_half_v * aspect
	# Turning left (yaw +, about world Y): the point now at the centre was left of it -> smaller u ->
	# velocity.x < 0. With the camera pitched by p, a yaw of s moves the centre by s * cos(p).
	var expected_x := -tan(step) * cos(_camera.rotation.x) / (2.0 * tan_half_h)
	# Looking up (pitch +): the point now at the centre was above it -> smaller v (Y down) -> velocity.y < 0.
	var expected_y := -tan(step) / (2.0 * tan_half_v)
	var yaw := await _velocity_for("yaw", step)
	var pitch := await _velocity_for("pitch", step)
	_results["velocity"] = {"yaw": [yaw.x, yaw.y], "pitch": [pitch.x, pitch.y], "expected": [expected_x, expected_y]}
	var detail_x := "yaw: measured %.6f, expected %.6f (UV per frame)" % [yaw.x, expected_x]
	var detail_y := "pitch: measured %.6f, expected %.6f (UV per frame)" % [pitch.y, expected_y]
	print("VELOCITY " + detail_x + "; " + detail_y)
	_check("velocity X: current -> previous, UV units (camera yaw)", abs(yaw.x - expected_x) < abs(expected_x) * 0.03, detail_x)
	_check("velocity Y: current -> previous, UV units, Y down (camera pitch)", abs(pitch.y - expected_y) < abs(expected_y) * 0.03, detail_y)
	_check("a yaw moves only X and a pitch only Y", abs(yaw.y) < abs(expected_x) * 0.1 and abs(pitch.x) < abs(expected_y) * 0.1,
			"yaw.y %.6f pitch.x %.6f" % [yaw.y, pitch.x])


# Centre velocity (Godot's, filled) while the camera, pitched by `pitch`, turns by `step` per frame
# about `axis` ("yaw" or "pitch") and strafes by `strafe` per frame.
func _fill_velocity_for(pitch: float, axis: String, step: float, strafe: float = 0.0) -> Array:
	_reset_camera()
	_camera.rotation.x = pitch
	await _frames(4)
	_lu._request_upscaler_readbacks(24)
	var start_yaw := _camera.rotation.y
	var start_pitch := _camera.rotation.x
	var start_x := _camera.position.x
	for i in 30:
		if axis == "yaw":
			_camera.rotation.y = start_yaw + step * (i + 1)
		else:
			_camera.rotation.x = start_pitch + step * (i + 1)
		_camera.position.x = start_x + strafe * (i + 1)
		await process_frame
	var s := _status()
	var result := [_median_tail(s["velocity_samples"]), _median_tail(s["filled_velocity_samples"])]
	_reset_camera()
	return result


func _test_velocity_fill() -> void:
	_lu.dlss_enabled = true
	_lu._set_upscaler_test_conventions(1, 1, 1)
	_lu._set_upscaler_test_velocity_fill(0)
	await _frames(20)
	var step := 0.004 # radians per frame
	var target: Vector2i = _status()["target_size"]
	var tan_half_v := tan(deg_to_rad(_camera.fov) * 0.5)
	var tan_half_h := tan_half_v * float(target.x) / float(target.y)
	# Pitched up by 35 degrees the centre sees only the sky (the bars end 3 m up). A rotation moves
	# every point the same way whatever its depth, so the expected vectors are those of _test_velocity.
	var sky_pitch := deg_to_rad(35.0)
	var expected_x := -tan(step) * cos(sky_pitch) / (2.0 * tan_half_h)
	var expected_y := -tan(step) / (2.0 * tan_half_v)
	var yaw: Array = await _fill_velocity_for(sky_pitch, "yaw", step)
	var pitch: Array = await _fill_velocity_for(sky_pitch, "pitch", step)
	_results["velocity_fill_sky"] = {"yaw_godot": [yaw[0].x, yaw[0].y], "yaw_filled": [yaw[1].x, yaw[1].y],
			"pitch_godot": [pitch[0].x, pitch[0].y], "pitch_filled": [pitch[1].x, pitch[1].y], "expected": [expected_x, expected_y]}
	var detail := "sky, yaw: Godot %s, filled %s; pitch: Godot %s, filled %s; expected (%.6f, %.6f)" % [
		yaw[0], yaw[1], pitch[0], pitch[1], expected_x, expected_y]
	print("FILL " + detail)
	_check("sky: Godot writes no motion there after the takeover (why the fill exists)",
			yaw[0].length() < abs(expected_x) * 0.02 and pitch[0].length() < abs(expected_y) * 0.02, detail)
	_check("sky: the filled vector of a yaw is the camera's (X, within 3%)",
			abs(yaw[1].x - expected_x) < abs(expected_x) * 0.03 and abs(yaw[1].y) < abs(expected_x) * 0.1, detail)
	_check("sky: the filled vector of a pitch is the camera's (Y down, within 3%)",
			abs(pitch[1].y - expected_y) < abs(expected_y) * 0.03 and abs(pitch[1].x) < abs(expected_y) * 0.1, detail)

	# The formula against Godot's own vectors: every pixel derived from depth (debug mode), static
	# geometry at the centre (the floor), the camera turning and strafing.
	_lu._set_upscaler_test_velocity_fill(1)
	var derived: Array = await _fill_velocity_for(deg_to_rad(-12.0), "yaw", step, 0.02)
	_lu._set_upscaler_test_velocity_fill(0)
	var copied: Array = await _fill_velocity_for(deg_to_rad(-12.0), "yaw", step, 0.02)
	_results["velocity_fill_geometry"] = {"godot": [derived[0].x, derived[0].y], "derived": [derived[1].x, derived[1].y],
			"copied_godot": [copied[0].x, copied[0].y], "copied": [copied[1].x, copied[1].y]}
	var detail_geo := "geometry under turn + strafe: Godot %s, derived from depth %s; far-plane mode: Godot %s, filled %s" % [
		derived[0], derived[1], copied[0], copied[1]]
	print("FILL " + detail_geo)
	_check("the fill's formula equals Godot's vectors on static geometry (within 2%)",
			derived[0].length() > 1e-4 and (derived[1] - derived[0]).length() < derived[0].length() * 0.02 + 2e-5, detail_geo)
	_check("pixels with geometry keep Godot's vectors unchanged", copied[0].length() > 1e-4 and (copied[1] - copied[0]).length() < 1e-6,
			detail_geo)


# The camera strafes at a constant speed; the result at the last pose is compared with a
# supersampled still of that pose.
func _moving(motion: float, name: String) -> Image:
	_lu.dlss_enabled = true
	_lu._set_upscaler_test_conventions(1, 1, motion)
	_reset_camera()
	await _frames(6)
	_lu._reset_upscaler_history()
	for i in 80:
		_camera.position.x = -1.2 + 0.03 * i
		await process_frame
	return _capture(name)


func _test_motion() -> void:
	var correct := await _moving(1, "motion_reference")
	var flipped := await _moving(-1, "motion_flipped_vectors")
	_reset_camera()
	_camera.position.x = -1.2 + 0.03 * 79
	var truth := await _native(Viewport.SCALING_3D_MODE_BILINEAR, 2.0, 12, "motion_truth_ssaa2x")
	_reset_camera()
	var p := {"correct": _psnr(correct, truth), "flipped": _psnr(flipped, truth)}
	_results["motion_psnr"] = p
	var detail := "PSNR dB under camera motion: correct vectors %.2f, flipped %.2f" % [p["correct"], p["flipped"]]
	print("MOTION " + detail)
	_check("Godot's motion vectors, used as current -> previous, beat flipped ones", p["correct"] > p["flipped"] + 2.0, detail)


# --- camera cuts ------------------------------------------------------------------------------------

func _resets() -> Dictionary:
	return _status()["history_resets"].duplicate()


func _total(resets: Dictionary) -> int:
	var sum := 0
	for key: String in resets:
		sum += int(resets[key])
	return sum


# Applies `change` to the camera once and returns the status of the frame that shows it, with the
# resets it added (reason -> count).
func _after_change(change: Callable) -> Array:
	var before := _resets()
	change.call()
	await process_frame # the frame drawn after the change
	var s := _status()
	var added := {}
	for key: String in s["history_resets"]:
		var n: int = int(s["history_resets"][key]) - int(before.get(key, 0))
		if n != 0:
			added[key] = n
	return [s, added]


func _test_cuts() -> void:
	var takeovers: int = _status()["takeovers"]
	_lu.dlss_enabled = true # the motion test's truth capture left it off: one takeover follows
	_lu._set_upscaler_test_conventions(1, 1, 1)
	_lu._set_upscaler_test_cut_thresholds(60.0, 10.0)
	_reset_camera()
	for i in 120:
		await process_frame
		if _status()["takeovers"] > takeovers and _status()["active"]:
			break
	await _frames(8) # the takeover's own reset is counted on the next upscaled frame

	# Normal motion, even fast, keeps the history.
	var before := _resets()
	for i in 40:
		_camera.rotation.y += deg_to_rad(1.2)
		_camera.position.x += 0.05
		await process_frame
	var after := _resets()
	_check("a fast turn (1.2 degrees per frame) and a strafe keep the history", _total(after) == _total(before),
			"resets before %s, after %s" % [before, after])

	_reset_camera()
	await _frames(4)
	var r: Array = await _after_change(func() -> void: _camera.rotation.y += deg_to_rad(150.0))
	_check("a 150 degree turn resets the history on that very frame",
			r[1] == {"rotation": 1} and r[0]["frames_since_reset"] == 1 and r[0]["last_reset_reason"] == "rotation",
			"added %s, frames since reset %d" % [r[1], r[0]["frames_since_reset"]])

	_reset_camera()
	await _frames(4)
	r = await _after_change(func() -> void: _camera.position.z += 20.0)
	_check("a 20 m jump resets the history on that very frame",
			r[1] == {"translation": 1} and r[0]["frames_since_reset"] == 1, "added %s, frames since reset %d" % [r[1], r[0]["frames_since_reset"]])

	_reset_camera()
	await _frames(4)
	var other := Camera3D.new()
	other.fov = _camera.fov
	root.add_child(other)
	other.global_transform = _camera.global_transform # the same view: only the switch itself is a cut
	await _frames(2)
	var upscaled: int = _status()["frames_upscaled"]
	r = await _after_change(func() -> void: other.make_current())
	_check("a switch to another camera resets the history on its first frame, which is upscaled",
			r[1] == {"camera_switch": 1} and r[0]["frames_since_reset"] == 1 and r[0]["frames_upscaled"] == upscaled + 1,
			"added %s, frames since reset %d, frames upscaled +%d" % [r[1], r[0]["frames_since_reset"], r[0]["frames_upscaled"] - upscaled])
	r = await _after_change(func() -> void: _camera.make_current())
	_check("switching back is a camera switch too", r[1] == {"camera_switch": 1}, "added %s" % r[1])
	other.queue_free()

	_lu._set_upscaler_test_cut_thresholds(0.0, 0.0)
	await _frames(4)
	r = await _after_change(func() -> void: _camera.rotation.y += deg_to_rad(150.0))
	_lu._set_upscaler_test_cut_thresholds(60.0, 10.0)
	_check("with the thresholds at 0 a 150 degree turn keeps the history", r[1].is_empty() and r[0]["frames_since_reset"] > 1,
			"added %s, frames since reset %d" % [r[1], r[0]["frames_since_reset"]])

	_reset_camera()
	await _frames(4)
	r = await _after_change(func() -> void: _lu.reset_dlss_history())
	_check("reset_dlss_history() resets on the next frame", r[1] == {"api": 1} and r[0]["frames_since_reset"] == 1,
			"added %s, frames since reset %d" % [r[1], r[0]["frames_since_reset"]])
	_results["history_resets"] = _resets()
	_reset_camera()


# --- native recording -------------------------------------------------------------------------------

func _native_status() -> Dictionary:
	return _status().get("native", {})


# Converges the still scene from a history reset at jitter phase 0 and captures exactly `frames`
# frames after it, so two runs see the same jitter sequence and can be compared bit for bit.
func _converged_at_phase(native_mode: int, frames: int, name: String) -> Image:
	_lu.dlss_enabled = true
	_lu._set_upscaler_test_conventions(1, 1, 1)
	_lu._set_upscaler_test_native_mode(native_mode)
	await _frames(4)
	_lu._reset_upscaler_history(0)
	for i in 200:
		await process_frame
		var since: int = _status()["frames_since_reset"]
		if since == frames and _status()["jitter"]["phase"] == (frames - 1) % int(_status()["jitter"]["phase_count"]):
			return _capture(name)
		if since > frames:
			break
	return null


func _test_native() -> void:
	var n := _native_status()
	if not n.get("requested", false):
		# The reference backend: Godot records the reference dispatch and the layer hooks nothing.
		_check("reference backend: no command hooks, nothing intercepted",
				not n.get("command_hooks", true) and not n.get("active", true) and n.get("marker_dispatches", -1) == 0, "%s" % n)
		return
	_check("native recording active: vkCmd* hooks resolved, marker registered", n["active"] and n["command_hooks"] and n["error"] == "",
			"%s" % n)

	# Every upscaled frame's marker dispatch is replaced; nothing falls back to Godot's recording.
	_lu._set_upscaler_test_native_mode(0)
	_lu.dlss_enabled = true # the motion test's truth capture left it off
	for i in 120:
		if _status()["active"] and _status()["frames_since_reset"] > 2:
			break
		await process_frame
	var s0 := _status()
	var n0: Dictionary = s0["native"]
	await _frames(30)
	var s1 := _status()
	var n1: Dictionary = s1["native"]
	var frames: int = s1["frames_upscaled"] - s0["frames_upscaled"]
	var substituted: int = n1["substituted"] - n0["substituted"]
	var recorded: int = n1["recorded"] - n0["recorded"]
	_check("every upscaled frame is recorded natively in Godot's command buffer",
			frames >= 25 and abs(substituted - frames) <= 1 and recorded == substituted and n1["declined"] == 0
			and n1["untracked"] == 0 and n1["missing_job"] == 0 and n1["not_created"] == 0,
			"%d frames upscaled, %d substituted, %d recorded; %s" % [frames, substituted, recorded, n1])

	# Same shader, same images, same push constants, same jitter phase: the native recording and
	# Godot's own recording of the marker (the recorder declines) must give the same image.
	var native_image := await _converged_at_phase(0, CONVERGE_FRAMES, "native_phase_aligned")
	var declined_before: int = _native_status()["declined"]
	var godot_image := await _converged_at_phase(1, CONVERGE_FRAMES, "godot_recorded_phase_aligned")
	var declined: int = _native_status()["declined"] - declined_before
	_check("recorder declined: Godot's own dispatch of the marker ran", declined >= CONVERGE_FRAMES, "%d declined" % declined)
	var both := native_image != null and godot_image != null
	var same := _psnr(native_image, godot_image) if both else 0.0
	var identical := both and native_image.get_data() == godot_image.get_data()
	_results["native_vs_godot_psnr"] = same
	_check("native recording equals Godot's recording of the same shader (phase-aligned)", both and (identical or same >= 60.0),
			"identical %s, PSNR %.2f dB" % [identical, same])

	# The displayed image is the native recording (the marker dispatch did not also run): a change
	# made only in the native recording (jitter x negated) must show.
	var negated := await _converged_at_phase(2, CONVERGE_FRAMES, "native_negated_jitter_x")
	_lu._set_upscaler_test_native_mode(0)
	if native_image == null or negated == null or _truth == null:
		_check("the displayed image is the native recording", false, "a capture is missing")
		return
	var p_native := _psnr(native_image, _truth)
	var p_negated := _psnr(negated, _truth)
	_results["native_psnr"] = {"native": p_native, "native_negated_jitter_x": p_negated}
	_check("the displayed image is the native recording (a native-only jitter flip shows)", p_native > p_negated + 1.0,
			"PSNR dB vs truth: native %.2f, native with jitter x negated %.2f" % [p_native, p_negated])
