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
##   cut         a 150 degree camera cut: the history is reset on exactly the first frame of the new
##               view, and that frame and the next show no ghost of the old view (bars) in the plain
##               background, for DLSS Quality and DLAA; with neither the cut detection nor the sky
##               motion (the control: the state the review found) the ghost is there, so the metric
##               sees it
##   sky turn    a camera turn with the thin bars over the plain background: the trail metric (pixels
##               of the plain background that differ from a supersampled still of the final pose) of
##               DLSS Quality is at most FSR2's at the same render size; without the far-plane motion
##               vectors (the control) it is worse
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
		await _test_cut()
		await _test_sky_turn()
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


# --- ghosts and trails in the plain background -------------------------------------------------------

# Indices (y * width + x) of the plain-background pixels of `truth` in rows [top, bottom): the
# background colour (the truth's pixel at (2, 2)) within 5 levels, and so for `radius` pixels around,
# so edges and their anti-aliasing never count.
func _background_pixels(truth: Image, top: int, bottom: int, radius: int = 2) -> PackedInt32Array:
	var t := truth.duplicate() as Image
	t.convert(Image.FORMAT_RGB8)
	var w := t.get_width()
	var data := t.get_data()
	var o0 := (2 * w + 2) * 3
	var bg := [data[o0], data[o0 + 1], data[o0 + 2]]
	var rows := bottom - top
	var plain := PackedByteArray()
	plain.resize(w * rows)
	for y in rows:
		var row := (top + y) * w * 3
		for x in w:
			var o := row + x * 3
			plain[y * w + x] = 1 if absi(data[o] - bg[0]) <= 5 and absi(data[o + 1] - bg[1]) <= 5 and absi(data[o + 2] - bg[2]) <= 5 else 0
	# Erosion by a (2 radius + 1) square: a horizontal, then a vertical minimum.
	var h := PackedByteArray()
	h.resize(w * rows)
	for y in rows:
		for x in w:
			var keep := 1
			for d in range(-radius, radius + 1):
				var xx := x + d
				if xx < 0 or xx >= w or plain[y * w + xx] == 0:
					keep = 0
					break
			h[y * w + x] = keep
	var out := PackedInt32Array()
	for y in range(radius, rows - radius):
		for x in w:
			var keep := true
			for d in range(-radius, radius + 1):
				if h[(y + d) * w + x] == 0:
					keep = false
					break
			if keep:
				out.append((top + y) * w + x)
	return out


# Of the given pixels: how many differ from the truth by more than `threshold` levels (the largest
# channel difference), their fraction, and the mean difference (0..1).
func _trail(image: Image, truth: Image, pixels: PackedInt32Array, threshold: int = 20) -> Dictionary:
	var a := image.duplicate() as Image
	a.convert(Image.FORMAT_RGB8)
	var b := truth.duplicate() as Image
	b.convert(Image.FORMAT_RGB8)
	var da := a.get_data()
	var db := b.get_data()
	var bad := 0
	var sum := 0.0
	for p in pixels:
		var o := p * 3
		var e := maxi(absi(da[o] - db[o]), maxi(absi(da[o + 1] - db[o + 1]), absi(da[o + 2] - db[o + 2])))
		sum += e
		if e > threshold:
			bad += 1
	var n := maxi(pixels.size(), 1)
	return {"pixels": pixels.size(), "bad": bad, "fraction": float(bad) / n, "mean": sum / n / 255.0}


func _resets() -> Dictionary:
	return _status()["history_resets"].duplicate()


# A 150 degree cut: settled on the bars (the reset pose), then turned away in one frame. Returns the
# first two frames of the new view and the status of the first.
func _cut(mode: int, detection: bool, sky_motion: bool, name: String) -> Array:
	await _dlss(mode)
	_lu._set_upscaler_test_cut_thresholds(60.0 if detection else 0.0, 10.0 if detection else 0.0)
	_lu._set_upscaler_test_velocity_fill(0 if sky_motion else 2)
	TestScene.reset_camera(_camera)
	await _frames(CONVERGE_FRAMES)
	var before := _resets()
	_camera.rotation.y = deg_to_rad(150.0)
	await process_frame
	var s := _status()
	var first := _capture(name + "_cut1")
	await process_frame
	var second := _capture(name + "_cut2")
	_lu._set_upscaler_test_cut_thresholds(60.0, 10.0)
	_lu._set_upscaler_test_velocity_fill(0)
	var added := {}
	for key: String in s["history_resets"]:
		var n: int = int(s["history_resets"][key]) - int(before.get(key, 0))
		if n != 0:
			added[key] = n
	TestScene.reset_camera(_camera)
	return [first, second, s, added]


func _test_cut() -> void:
	TestScene.reset_camera(_camera)
	_camera.rotation.y = deg_to_rad(150.0)
	var truth := await _godot_mode(Viewport.SCALING_3D_MODE_BILINEAR, 2.0, 12, "cut_truth_ssaa2x")
	TestScene.reset_camera(_camera)
	var pixels := _background_pixels(truth, 0, SIZE.y / 2)
	var results := {"background_pixels": pixels.size()}
	var runs := [[2, true, true, "dlss_quality"], [1, true, true, "dlaa"], [2, false, true, "dlss_quality_no_cut_detection"],
		[2, false, false, "dlss_quality_before_the_fixes"]]
	for run: Array in runs:
		var r: Array = await _cut(run[0], run[1], run[2], run[3])
		results[run[3]] = {"first": _trail(r[0], truth, pixels), "second": _trail(r[1], truth, pixels),
				"added_resets": r[3], "frames_since_reset": r[2]["frames_since_reset"]}
	_results["cut"] = results
	print("CUT " + JSON.stringify(results))
	var enough := pixels.size() > SIZE.x * SIZE.y / 8
	for name: String in ["dlss_quality", "dlaa"]:
		var r: Dictionary = results[name]
		_check("%s: a 150 degree cut resets the history on the first frame of the new view" % name,
				r["added_resets"] == {"rotation": 1} and r["frames_since_reset"] == 1,
				"added %s, frames since reset %d" % [r["added_resets"], r["frames_since_reset"]])
		_check("%s: no ghost of the old view after the cut (first and second frame, < 0.05%% of %d background pixels)" % [name, pixels.size()],
				enough and r["first"]["fraction"] < 0.0005 and r["second"]["fraction"] < 0.0005,
				"first %s, second %s" % [r["first"], r["second"]])
	var control: Dictionary = results["dlss_quality_before_the_fixes"]
	_check("control: without the reset and the sky motion the old view shows (so the metric sees a ghost)",
			control["first"]["bad"] > 10 * maxi(results["dlss_quality"]["first"]["bad"], 1),
			"before the fixes %s, with them %s; cut detection off, sky motion on: %s" % [control["first"],
				results["dlss_quality"]["first"], results["dlss_quality_no_cut_detection"]["first"]])


# The camera turns 1.2 degrees per frame for 40 frames and stops on the bars (the reset pose) with the
# plain background between and above them. `setup` selects the upscaler; returns the last frame.
func _turn(setup: Callable, name: String) -> Image:
	await setup.call()
	TestScene.reset_camera(_camera)
	var end_yaw := _camera.rotation.y
	_camera.rotation.y = end_yaw + deg_to_rad(1.2 * 40)
	await _frames(CONVERGE_FRAMES)
	for i in range(39, -1, -1):
		_camera.rotation.y = end_yaw + deg_to_rad(1.2 * i)
		await process_frame
	var image := _capture(name)
	TestScene.reset_camera(_camera)
	return image


func _test_sky_turn() -> void:
	TestScene.reset_camera(_camera)
	var truth := await _godot_mode(Viewport.SCALING_3D_MODE_BILINEAR, 2.0, 12, "turn_truth_ssaa2x")
	# Above the floor's far edge (row ~279 at 720 p) there are only the bars and the background.
	var pixels := _background_pixels(truth, 0, 260)
	await _dlss(2)
	var quality_scale := float(_status()["render_size"].x + 0.5) / float(SIZE.x)
	var fsr2 := await _turn(func() -> void: await _godot_mode(Viewport.SCALING_3D_MODE_FSR2, quality_scale, 2, "turn_fsr2_setup"),
			"turn_fsr2_quality_size")
	var dlss := await _turn(func() -> void: await _dlss(2), "turn_dlss_quality")
	var dlaa := await _turn(func() -> void: await _dlss(1), "turn_dlaa")
	_lu._set_upscaler_test_velocity_fill(2)
	var no_fill := await _turn(func() -> void: await _dlss(2), "turn_dlss_quality_no_sky_motion")
	_lu._set_upscaler_test_velocity_fill(0)
	var t := {"background_pixels": pixels.size(), "fsr2_quality_size": _trail(fsr2, truth, pixels),
		"dlss_quality": _trail(dlss, truth, pixels), "dlaa": _trail(dlaa, truth, pixels),
		"dlss_quality_no_sky_motion": _trail(no_fill, truth, pixels)}
	_results["sky_turn"] = t
	print("SKY_TURN " + JSON.stringify(t))
	var enough := pixels.size() > SIZE.x * 100
	_check("sky turn: DLSS Quality trails no more than FSR2 at the same render size",
			enough and t["dlss_quality"]["bad"] <= t["fsr2_quality_size"]["bad"],
			"trail pixels of %d: DLSS Quality %d, FSR2 %d, DLAA %d, DLSS without sky motion %d" % [pixels.size(),
				t["dlss_quality"]["bad"], t["fsr2_quality_size"]["bad"], t["dlaa"]["bad"], t["dlss_quality_no_sky_motion"]["bad"]])
	_check("control: without the far-plane motion vectors DLSS trails more (so the metric sees trails)",
			t["dlss_quality_no_sky_motion"]["bad"] > t["dlss_quality"]["bad"],
			"DLSS Quality %d, without sky motion %d" % [t["dlss_quality"]["bad"], t["dlss_quality_no_sky_motion"]["bad"]])


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
