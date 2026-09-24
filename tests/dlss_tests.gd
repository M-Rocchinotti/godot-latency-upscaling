extends SceneTree
## DLSS image tests, on an RTX GPU with the NVIDIA runtime installed (debug build):
##
##     python tools/rtx_remote.py session dlss_tests --window-ok -- --script res://tests/dlss_tests.gd
##
## The procedural still scene of the upscaler tests (tests/test_scene.gd) at 1280x720. Checks:
##   active      DLSS is the backend and evaluates every upscaled frame (none failed, none fell back
##               to the reference upscaler)
##   modes       for every quality mode Godot renders at the size DLSS asks for (slDLSSGetOptimalSettings;
##               the width exactly, the height within a pixel), the output is target size, frames are
##               evaluated
##   auto        Auto below 1920x1080 is off and gives the viewport back
##   presets     Default, J, K, L and M evaluate without failures
##   still       against a 2x supersampled truth: DLSS Quality beats a bilinear upscale of the same
##               render size, DLAA beats native rendering without AA, and Godot's jitter convention
##               beats ignored and flipped jitter (so the convention handed to DLSS is right);
##               FSR2 at the same render size and native rendering are measured for comparison
##   motion      with the camera moving, the motion vectors as handed over beat flipped ones
## Ends "dlss tests: <passed> passed, <failed> failed". Captures go to LU_RUN_DIR or build/dlss/.

const TestScene := preload("res://tests/test_scene.gd")
const SIZE := Vector2i(1280, 720)
const CONVERGE_FRAMES := 64
const QUALITY_NAMES := ["Auto", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"]
const PRESET_NAMES := ["Default", "J", "K", "L", "M"]

var _lu: Object
var _camera: Camera3D
var _out_dir := "res://build/dlss"
var _passed := 0
var _failed := 0
var _results := {}


func _initialize() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(SIZE)
	_camera = TestScene.build(root)
	_lu = Engine.get_singleton("LatencyUpscaling") if Engine.has_singleton("LatencyUpscaling") else null
	if OS.get_environment("LU_RUN_DIR") != "":
		_out_dir = OS.get_environment("LU_RUN_DIR")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(_out_dir))
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


func _capture(name: String) -> Image:
	var image := root.get_texture().get_image()
	image.save_png(_out_dir.path_join(name + ".png"))
	return image


func _psnr(image: Image, truth: Image) -> float:
	return float(image.compute_image_metrics(truth, false)["peak_snr"])


# Waits until DLSS runs in the given mode on buffers taken over for it.
func _dlss(mode: int, preset: int = 0) -> bool:
	_lu.dlss_enabled = true
	_lu.dlss_quality = mode
	_lu.dlss_preset = preset
	var takeovers: int = _status()["takeovers"]
	for i in 180:
		await process_frame
		var s := _status()
		if s["active"] and s["resolved_quality"] == QUALITY_NAMES[mode] and s["frames_since_reset"] > 2:
			return true
		if s["takeovers"] > takeovers and s["active"]:
			takeovers = s["takeovers"]
	return false


# A converged still of DLSS in `mode` with the given conventions (+1 = Godot's).
func _dlss_still(mode: int, jitter_x: float, jitter_y: float, motion: float, name: String) -> Image:
	await _dlss(mode)
	_lu._set_upscaler_test_conventions(jitter_x, jitter_y, motion)
	_lu._reset_upscaler_history()
	await _frames(CONVERGE_FRAMES)
	return _capture(name)


# Godot's own viewport modes with the addon's upscaler off.
func _godot_mode(mode: Viewport.Scaling3DMode, scale: float, frames: int, name: String) -> Image:
	_lu.dlss_enabled = false
	await _frames(2)
	root.scaling_3d_mode = mode
	root.scaling_3d_scale = scale
	await _frames(frames)
	return _capture(name)


func _run() -> void:
	if _lu == null or not _lu.has_method("_set_upscaler_test_conventions"):
		_check("debug build with the LatencyUpscaling singleton", false)
		_finish()
		return
	await _frames(5)
	await _test_active()
	if _failed == 0:
		await _test_modes()
		await _test_auto()
		await _test_presets()
		await _test_still()
		await _test_motion()
	_finish()


func _finish() -> void:
	print("DLSS_RESULTS " + JSON.stringify(_results))
	print("dlss tests: %d passed, %d failed" % [_passed, _failed])
	quit(1 if _failed > 0 else 0)


func _test_active() -> void:
	var ready := await _dlss(2)
	var s := _status()
	_check("DLSS is the upscaler", ready and s["backend"] == "dlss", "%s; %s" % [_lu.get_dlss_status(), s.get("reason", "")])
	if not ready:
		return
	var before: Dictionary = s["dlss_frames"]
	var upscaled_before: int = s["frames_upscaled"]
	await _frames(60)
	s = _status()
	var after: Dictionary = s["dlss_frames"]
	var frames: int = s["frames_upscaled"] - upscaled_before
	var evaluated: int = after["evaluated"] - before["evaluated"]
	_check("every upscaled frame is evaluated by DLSS in Godot's command buffer",
			frames >= 55 and abs(evaluated - frames) <= 1 and after["failed"] == 0 and after["missing_job"] == 0,
			"%d frames, %d evaluated, %d failed (%s)" % [frames, evaluated, after["failed"], after["last_error"]])
	_results["runtime"] = after.get("runtime", "")
	print("DLSS runtime: %s" % after.get("runtime", "?"))


func _test_modes() -> void:
	var sizes := {}
	for mode in [1, 2, 3, 4, 5]:
		var ready := await _dlss(mode)
		await _frames(10)
		var s := _status()
		var frames: Dictionary = s["dlss_frames"]
		var render: Vector2i = s["render_size"]
		var target: Vector2i = s["target_size"]
		var optimal: Vector2i = frames.get("optimal_render_size", Vector2i(-1, -1))
		sizes[QUALITY_NAMES[mode]] = [render.x, render.y]
		# Godot applies one scale to both axes (renderer_viewport.cpp), DLSS rounds each axis: the width
		# is matched exactly, the height can be one pixel off (still inside DLSS's render-size range).
		_check("%s: renders at DLSS's optimal size (height within 1 px), output at target size" % QUALITY_NAMES[mode],
				ready and render.x == optimal.x and abs(render.y - optimal.y) <= 1 and target == SIZE and frames["failed"] == 0,
				"render %s, DLSS optimal %s, target %s, %d failed" % [render, optimal, target, frames["failed"]])
	_results["render_sizes"] = sizes


# Auto follows NVIDIA's table by output size: below 1920x1080 it means off, and the addon gives the
# viewport back to Godot's own settings.
func _test_auto() -> void:
	_lu.dlss_enabled = true
	_lu.dlss_quality = 0
	await _frames(10)
	var status: String = _lu.get_dlss_status()
	_check("Auto at 1280x720 is off and gives the viewport back",
			status.begins_with("off: Auto") and not _lu.is_dlss_active() and root.scaling_3d_mode != Viewport.SCALING_3D_MODE_FSR2,
			"%s; viewport mode %d" % [status, root.scaling_3d_mode])


func _test_presets() -> void:
	for preset in PRESET_NAMES.size():
		await _dlss(2, preset)
		var before: Dictionary = _status()["dlss_frames"]
		await _frames(20)
		var after: Dictionary = _status()["dlss_frames"]
		_check("preset %s evaluates" % PRESET_NAMES[preset],
				after["evaluated"] - before["evaluated"] >= 15 and after["failed"] == before["failed"], "%s" % after["last_error"])
	_lu.dlss_preset = 0


func _test_still() -> void:
	var truth := await _godot_mode(Viewport.SCALING_3D_MODE_BILINEAR, 2.0, 12, "truth_ssaa2x")
	var native := await _godot_mode(Viewport.SCALING_3D_MODE_BILINEAR, 1.0, 8, "native_no_aa")
	await _dlss(2)
	var quality_scale := float(_status()["render_size"].x + 0.5) / float(SIZE.x)
	var bilinear := await _godot_mode(Viewport.SCALING_3D_MODE_BILINEAR, quality_scale, 8, "bilinear_quality_size")
	var fsr2 := await _godot_mode(Viewport.SCALING_3D_MODE_FSR2, quality_scale, CONVERGE_FRAMES, "fsr2_quality_size")
	var fsr2_native := await _godot_mode(Viewport.SCALING_3D_MODE_FSR2, 1.0, CONVERGE_FRAMES, "fsr2_native_aa")
	var dlss_quality := await _dlss_still(2, 1, 1, 1, "dlss_quality")
	var dlaa := await _dlss_still(1, 1, 1, 1, "dlaa")
	var no_jitter := await _dlss_still(2, 0, 0, 1, "dlss_quality_jitter_ignored")
	var flip_x := await _dlss_still(2, -1, 1, 1, "dlss_quality_jitter_flip_x")
	var flip_y := await _dlss_still(2, 1, -1, 1, "dlss_quality_jitter_flip_y")
	_lu._set_upscaler_test_conventions(1, 1, 1)
	var p := {
		"native_no_aa": _psnr(native, truth), "bilinear_quality_size": _psnr(bilinear, truth), "fsr2_quality_size": _psnr(fsr2, truth),
		"fsr2_native_aa": _psnr(fsr2_native, truth), "dlss_quality": _psnr(dlss_quality, truth), "dlaa": _psnr(dlaa, truth),
		"dlss_jitter_ignored": _psnr(no_jitter, truth), "dlss_jitter_flip_x": _psnr(flip_x, truth), "dlss_jitter_flip_y": _psnr(flip_y, truth),
	}
	_results["still_psnr"] = p
	var detail := ("PSNR dB vs 2x SSAA: DLSS Quality %.2f, FSR2 same size %.2f, bilinear same size %.2f, DLAA %.2f, "
			+ "FSR2 native AA %.2f, native no AA %.2f; DLSS jitter ignored %.2f, flip x %.2f, flip y %.2f") % [
				p["dlss_quality"], p["fsr2_quality_size"], p["bilinear_quality_size"], p["dlaa"], p["fsr2_native_aa"],
				p["native_no_aa"], p["dlss_jitter_ignored"], p["dlss_jitter_flip_x"], p["dlss_jitter_flip_y"]]
	print("STILL " + detail)
	_check("DLSS Quality beats a bilinear upscale of the same render size", p["dlss_quality"] > p["bilinear_quality_size"] + 1.0, detail)
	_check("DLAA beats native rendering without anti-aliasing", p["dlaa"] > p["native_no_aa"] + 1.0, detail)
	_check("Godot's jitter convention beats ignoring the jitter", p["dlss_quality"] > p["dlss_jitter_ignored"] + 0.5, detail)
	_check("Godot's jitter convention beats a flipped X", p["dlss_quality"] > p["dlss_jitter_flip_x"] + 0.5, detail)
	_check("Godot's jitter convention beats a flipped Y", p["dlss_quality"] > p["dlss_jitter_flip_y"] + 0.5, detail)


# The camera strafes at a constant speed; the result at the last pose is compared with a
# supersampled still of that pose.
func _moving(motion: float, name: String) -> Image:
	await _dlss(2)
	_lu._set_upscaler_test_conventions(1, 1, motion)
	TestScene.reset_camera(_camera)
	await _frames(6)
	_lu._reset_upscaler_history()
	for i in 80:
		_camera.position.x = -1.2 + 0.03 * i
		await process_frame
	return _capture(name)


func _test_motion() -> void:
	var correct := await _moving(1, "dlss_motion")
	var flipped := await _moving(-1, "dlss_motion_flipped_vectors")
	_lu._set_upscaler_test_conventions(1, 1, 1)
	TestScene.reset_camera(_camera)
	_camera.position.x = -1.2 + 0.03 * 79
	var truth := await _godot_mode(Viewport.SCALING_3D_MODE_BILINEAR, 2.0, 12, "motion_truth_ssaa2x")
	TestScene.reset_camera(_camera)
	var p := {"correct": _psnr(correct, truth), "flipped": _psnr(flipped, truth)}
	_results["motion_psnr"] = p
	var detail := "PSNR dB under camera motion: motion vectors as handed over %.2f, flipped %.2f" % [p["correct"], p["flipped"]]
	print("MOTION " + detail)
	_check("DLSS: Godot's motion vectors, as handed over, beat flipped ones", p["correct"] > p["flipped"] + 1.0, detail)
