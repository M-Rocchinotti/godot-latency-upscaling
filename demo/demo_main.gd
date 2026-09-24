extends Node3D
## Demo of the Latency & Upscaling addon (Phase 1: Reflex).
##
## A moving scene, a mouse-look camera (hold the right mouse button) to feel input latency, and a
## "GPU load" control that makes the GPU the bottleneck, which is when Reflex lowers latency.
## The panel talks to the LatencyUpscaling singleton; nothing in this project configures the addon.

const GRID := 20
const SPACING := 2.2
const MODE_NAMES := ["Off", "On", "On + Boost"]

## Iterations of the full-screen load shader per pixel (0 = no extra GPU load).
@export_range(0, 6000, 50) var gpu_load := 0:
	set(value):
		gpu_load = value
		if _load_material != null:
			_load_material.set_shader_parameter("iterations", value)

var _cubes: MultiMesh
var _camera: Camera3D
var _load_material: ShaderMaterial
var _time := 0.0
var _yaw := 0.0
var _pitch := -0.35
var _status_label: Label
var _mode_button: OptionButton
var _dlss: Object


var _probe_frames := -1  # >=0 when the export probe is active


func _ready() -> void:
	_dlss = Engine.get_singleton("LatencyUpscaling") if Engine.has_singleton("LatencyUpscaling") else null
	# Export probe: tests/run_export.py runs this exported main scene with LU_EXPORT_PROBE=1 to read
	# the addon status from inside a real exported game. (A `--script` SceneTree main loop hangs in
	# an exported build on Godot 4.7.2, so the probe rides the main scene instead.)
	if OS.get_environment("LU_EXPORT_PROBE") == "1":
		_probe_frames = 5
	_build_world()
	_build_panel()
	gpu_load = gpu_load


func _probe_tick() -> void:
	_probe_frames -= 1
	if _probe_frames > 0:
		return
	var status := {}
	if Engine.has_singleton("LatencyUpscaling"):
		status = Engine.get_singleton("LatencyUpscaling").get_status()
	else:
		status["error"] = "LatencyUpscaling singleton not registered"
	print("LU_STATUS " + JSON.stringify(status))
	get_tree().quit()


func _process(delta: float) -> void:
	if _probe_frames >= 0:
		_probe_tick()
	_time += delta
	for i in _cubes.instance_count:
		var x := i % GRID
		var z := i / GRID
		var basis := Basis(Vector3.UP, _time * 0.8 + float(i) * 0.1).rotated(Vector3.RIGHT, _time * 0.5 + float(x) * 0.2)
		var origin := Vector3((x - GRID * 0.5) * SPACING, 1.0 + sin(_time + float(i) * 0.37) * 0.5, (z - GRID * 0.5) * SPACING)
		_cubes.set_instance_transform(i, Transform3D(basis, origin))
	_camera.basis = Basis.from_euler(Vector3(_pitch, _yaw, 0.0))


func _unhandled_input(event: InputEvent) -> void:
	var motion := event as InputEventMouseMotion
	if motion != null and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT):
		_yaw -= motion.relative.x * 0.004
		_pitch = clampf(_pitch - motion.relative.y * 0.004, -1.3, 1.3)


func _build_world() -> void:
	var sky_material := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_material
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	var sun := DirectionalLight3D.new()
	sun.rotation = Vector3(-0.9, 0.6, 0.0)
	sun.shadow_enabled = true
	add_child(sun)

	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(80, 80)
	var floor_node := MeshInstance3D.new()
	floor_node.mesh = floor_mesh
	add_child(floor_node)

	var box := BoxMesh.new()
	box.size = Vector3.ONE
	var box_material := StandardMaterial3D.new()
	box_material.albedo_color = Color(0.55, 0.72, 0.95)
	box_material.roughness = 0.35
	box.material = box_material
	_cubes = MultiMesh.new()
	_cubes.transform_format = MultiMesh.TRANSFORM_3D
	_cubes.mesh = box
	_cubes.instance_count = GRID * GRID
	var cubes_node := MultiMeshInstance3D.new()
	cubes_node.multimesh = _cubes
	add_child(cubes_node)

	_camera = Camera3D.new()
	_camera.position = Vector3(0, 9, 26)
	add_child(_camera)

	# Full-screen quad in front of the camera whose fragment shader loops `iterations` times.
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, depth_test_disabled, blend_mix;
uniform int iterations = 0;
void vertex() {
	POSITION = vec4(VERTEX.xy * 2.0, 0.0, 1.0);
}
void fragment() {
	vec2 p = UV;
	float acc = 0.0;
	for (int i = 0; i < iterations; i++) {
		acc += sin(p.x * float(i) * 0.013 + acc) * cos(p.y * 1.7 - acc);
		p = p.yx * 0.999 + vec2(0.001, 0.002);
	}
	ALBEDO = vec3(0.0);
	ALPHA = clamp(abs(acc) * 1e-6, 0.0, 0.01);
}
"""
	_load_material = ShaderMaterial.new()
	_load_material.shader = shader
	var quad := QuadMesh.new()
	quad.size = Vector2.ONE
	var load_node := MeshInstance3D.new()
	load_node.mesh = quad
	load_node.material_override = _load_material
	load_node.extra_cull_margin = 16384.0
	load_node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_camera.add_child(load_node)


func _build_panel() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _glass_panel())
	panel.anchor_left = 1.0
	panel.anchor_right = 1.0
	panel.offset_left = -330
	panel.offset_right = -16
	panel.offset_top = 16
	layer.add_child(panel)
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 8)
	panel.add_child(column)

	var title := Label.new()
	title.text = "NVIDIA Reflex demo"
	title.add_theme_font_size_override("font_size", 17)
	column.add_child(title)

	_mode_button = OptionButton.new()
	for name in MODE_NAMES:
		_mode_button.add_item("Reflex Low Latency: %s" % name)
	_mode_button.item_selected.connect(func(index: int) -> void:
		if _dlss != null:
			_dlss.reflex_mode = index)
	column.add_child(_mode_button)

	column.add_child(_labeled_slider("Frame-rate limit (0 = none)", 0, 240, 0,
			func(v: float) -> void:
				if _dlss != null:
					_dlss.reflex_fps_limit = 0.0 if v < 10.0 else v))
	column.add_child(_labeled_slider("GPU load", 0, 6000, gpu_load, func(v: float) -> void: gpu_load = int(v)))

	var vsync := CheckButton.new()
	vsync.text = "VSync"
	vsync.button_pressed = DisplayServer.window_get_vsync_mode() != DisplayServer.VSYNC_DISABLED
	vsync.toggled.connect(func(on: bool) -> void:
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_ENABLED if on else DisplayServer.VSYNC_DISABLED))
	column.add_child(vsync)

	var overlay := CheckButton.new()
	overlay.text = "Latency overlay"
	overlay.toggled.connect(func(on: bool) -> void:
		if _dlss != null:
			_dlss.overlay_visible = on)
	column.add_child(overlay)

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.custom_minimum_size = Vector2(290, 0)
	_status_label.add_theme_color_override("font_color", Color(1, 1, 1, 0.78))
	column.add_child(_status_label)

	var hint := Label.new()
	hint.text = "Hold the right mouse button to look around."
	hint.add_theme_color_override("font_color", Color(1, 1, 1, 0.55))
	column.add_child(hint)

	if _dlss == null:
		_mode_button.disabled = true
		overlay.disabled = true
		_status_label.text = "The Latency & Upscaling addon is not loaded."
	else:
		_mode_button.select(_dlss.reflex_mode)
		overlay.button_pressed = _dlss.overlay_visible
		# NVIDIA's UI guideline: Reflex controls are disabled when Reflex is unavailable.
		_mode_button.disabled = not _dlss.is_reflex_supported()
		_status_label.text = _dlss.get_reflex_status()
		var timer := Timer.new()
		timer.wait_time = 0.5
		timer.autostart = true
		timer.timeout.connect(func() -> void:
			_mode_button.disabled = not _dlss.is_reflex_supported()
			_status_label.text = "Reflex: %s" % _dlss.get_reflex_status())
		add_child(timer)


func _labeled_slider(text: String, min_value: float, max_value: float, value: float, on_change: Callable) -> Control:
	var box := VBoxContainer.new()
	var label := Label.new()
	box.add_child(label)
	var slider := HSlider.new()
	slider.min_value = min_value
	slider.max_value = max_value
	slider.step = 10 if max_value > 1000 else 1
	slider.value = value
	var update := func(v: float) -> void:
		label.text = "%s: %d" % [text, int(v)]
		on_change.call(v)
	slider.value_changed.connect(update)
	label.text = "%s: %d" % [text, int(value)]
	box.add_child(slider)
	return box


static func _glass_panel() -> StyleBoxFlat:
	var box := StyleBoxFlat.new()
	box.bg_color = Color(0.07, 0.08, 0.10, 0.62)
	box.border_color = Color(1, 1, 1, 0.16)
	box.set_border_width_all(1)
	box.set_corner_radius_all(18)
	box.shadow_color = Color(0, 0, 0, 0.28)
	box.shadow_size = 14
	box.shadow_offset = Vector2(0, 4)
	box.content_margin_left = 16
	box.content_margin_right = 16
	box.content_margin_top = 14
	box.content_margin_bottom = 14
	box.anti_aliasing = true
	return box
