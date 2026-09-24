extends SceneTree
## Image tests of the Phase 2 plumbing with the reference upscaler (debug builds):
##
##     python tools/godot.py --env GODOT_LATENCY_UPSCALING_TEST_UPSCALER=reference \
##         --env "GODOT_LATENCY_UPSCALING_SETTINGS=dlss/quality=4" --script res://tests/upscaler_tests.gd
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
##   motion      with the camera moving, correct motion vectors beat flipped ones
## Ends "upscaler tests: <passed> passed, <failed> failed". Captures go to build/upscaler/.

const SIZE := Vector2i(1280, 720)
const CONVERGE_FRAMES := 96
const OUT_DIR := "res://build/upscaler"

var _lu: Object
var _camera: Camera3D
var _passed := 0
var _failed := 0
var _results := {}


func _initialize() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(SIZE)
	_build_scene()
	_lu = Engine.get_singleton("LatencyUpscaling") if Engine.has_singleton("LatencyUpscaling") else null
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
	var world := Node3D.new()
	root.add_child(world)
	var env := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.45, 0.55, 0.7)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.5, 0.5, 0.5)
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.environment = environment
	world.add_child(env)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 30, 0)
	world.add_child(sun)

	var checker := Image.create_empty(512, 512, true, Image.FORMAT_RGB8)
	for y in 512:
		for x in 512:
			var on := ((x / 8) + (y / 8)) % 2 == 0
			checker.set_pixel(x, y, Color(0.9, 0.9, 0.9) if on else Color(0.1, 0.12, 0.15))
	checker.generate_mipmaps()
	var floor_material := StandardMaterial3D.new()
	floor_material.albedo_texture = ImageTexture.create_from_image(checker)
	floor_material.uv1_scale = Vector3(4, 4, 1)
	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	floor_mesh.mesh = plane
	floor_mesh.material_override = floor_material
	world.add_child(floor_mesh)

	var bar_material := StandardMaterial3D.new()
	bar_material.albedo_color = Color(0.95, 0.35, 0.1)
	for i in 24:
		var bar := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(0.03, 3.0, 0.03)
		bar.mesh = box
		bar.material_override = bar_material
		bar.position = Vector3(-6.0 + i * 0.5, 1.5, -2.0 - (i % 5) * 1.3)
		world.add_child(bar)
	var sphere_material := StandardMaterial3D.new()
	sphere_material.albedo_color = Color(0.2, 0.6, 0.9)
	for i in 5:
		var sphere := MeshInstance3D.new()
		var mesh := SphereMesh.new()
		mesh.radius = 0.5
		mesh.height = 1.0
		sphere.mesh = mesh
		sphere.material_override = sphere_material
		sphere.position = Vector3(-3.0 + i * 1.5, 0.5, 1.0)
		world.add_child(sphere)

	_camera = Camera3D.new()
	_camera.fov = 60.0
	world.add_child(_camera)
	_reset_camera()
	_camera.make_current()


func _reset_camera() -> void:
	_camera.position = Vector3(0, 1.6, 6)
	_camera.rotation = Vector3(deg_to_rad(-12), 0, 0)


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
	await _test_motion()
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
	var samples: Array = _status()["velocity_samples"]
	var tail := samples.slice(max(0, samples.size() - 12))
	var xs: Array = tail.map(func(v: Vector2) -> float: return v.x)
	var ys: Array = tail.map(func(v: Vector2) -> float: return v.y)
	xs.sort()
	ys.sort()
	_reset_camera()
	return Vector2(xs[xs.size() / 2], ys[ys.size() / 2]) if not tail.is_empty() else Vector2(INF, INF)


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
