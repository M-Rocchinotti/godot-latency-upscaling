extends SceneTree
## Renders a deterministic 3D + 2D scene and saves the frame as a PNG (pass-through test).
##
##     python tools/godot.py --resolution 640x360 --script res://tests/capture_frame.gd -- --out=build/captures/a.png
##
## --fsr2=SCALE renders the 3D part with Godot's own FSR2 at that scale, as a project that uses FSR2
## itself would: with the addon's upscaler inactive, the addon must not change those pixels either.
##
## Nothing in the scene depends on time or on the frame count, so two runs on the same machine
## produce the same pixels unless something in the Vulkan path changes them. The capture is
## taken after a warm-up so that shader compilation and shadow atlases have settled.

const WARMUP_FRAMES := 45

var _out := ""
var _frames := 0
var _fsr2_scale := 0.0


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			_out = arg.get_slice("=", 1)
		elif arg.begins_with("--fsr2="):
			_fsr2_scale = float(arg.get_slice("=", 1))
	if _fsr2_scale > 0.0:
		root.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
		root.scaling_3d_scale = _fsr2_scale
	var world := Node3D.new()
	root.add_child(world)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.18, 0.2, 0.24)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.35, 0.37, 0.42)
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	world.add_child(world_env)

	var sun := DirectionalLight3D.new()
	sun.rotation = Vector3(-0.8, 0.5, 0.0)
	sun.shadow_enabled = true
	world.add_child(sun)
	var lamp := OmniLight3D.new()
	lamp.position = Vector3(-2, 2, 2)
	lamp.light_color = Color(1.0, 0.6, 0.3)
	lamp.omni_range = 6.0
	world.add_child(lamp)

	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(20, 20)
	var floor_node := MeshInstance3D.new()
	floor_node.mesh = floor_mesh
	world.add_child(floor_node)
	for i in 5:
		var mesh: PrimitiveMesh = [BoxMesh.new(), SphereMesh.new(), CylinderMesh.new(), TorusMesh.new(), PrismMesh.new()][i]
		var material := StandardMaterial3D.new()
		material.albedo_color = Color.from_hsv(float(i) / 5.0, 0.6, 0.9)
		material.metallic = 0.2 * i
		material.roughness = 0.3 + 0.1 * i
		mesh.material = material
		var node := MeshInstance3D.new()
		node.mesh = mesh
		node.position = Vector3(-4.0 + 2.0 * i, 0.8, 0.0)
		node.rotation = Vector3(0.3 * i, 0.5 * i, 0.0)
		world.add_child(node)

	var camera := Camera3D.new()
	camera.position = Vector3(0, 3, 8)
	camera.rotation = Vector3(-0.3, 0, 0)
	world.add_child(camera)

	var label := Label.new()
	label.text = "pass-through capture"
	label.position = Vector2(12, 8)
	root.add_child(label)


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames < WARMUP_FRAMES:
		return false
	var image := root.get_texture().get_image()
	if _out.is_empty():
		push_error("capture_frame.gd: missing --out=<path>")
		return true
	var path := ProjectSettings.globalize_path("res://").path_join(_out)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var err := image.save_png(path)
	# The raw pixels as well, so the comparison does not depend on PNG encoding.
	var raw := FileAccess.open(path.get_basename() + ".rgba", FileAccess.WRITE)
	if raw == null:
		err = FileAccess.get_open_error()
	else:
		raw.store_buffer(image.get_data())
		raw.close()
	print("CAPTURE %s %dx%d format=%d err=%d" % [_out, image.get_width(), image.get_height(), image.get_format(), err])
	return true
