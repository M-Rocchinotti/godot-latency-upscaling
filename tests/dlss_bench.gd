extends SceneTree
## DLSS performance on the demo scene, on an RTX GPU with the NVIDIA runtime installed:
##
##     python tools/rtx_remote.py session dlss_bench --window-ok -- --script res://tests/dlss_bench.gd -- --size=1920x1080
##
## At the output size (default 1920x1080, a normal window, VSync off) and for two GPU loads (the
## demo's full-screen load shader, which runs at the render resolution: 0 and --load iterations) it
## measures, per configuration, the GPU time of the main viewport (RenderingServer's measured render
## time, which includes the upscaler) and the frame interval, as medians over --frames frames:
##   native (no AA), FSR2 at 1.0 (Godot's own temporal AA), FSR2 Quality (render scale 1/1.5),
##   DLAA, DLSS Quality, Balanced, Performance, Ultra Performance.
## Prints one "BENCH {json}" line per configuration and "dlss bench: done".

var _size := Vector2i(1920, 1080)
var _frames := 200
var _load := 1500
var _lu: Object
var _demo: Node

const SETTLE_FRAMES := 60
const CONFIGS := [
	{"name": "native, no AA", "godot_mode": Viewport.SCALING_3D_MODE_BILINEAR, "scale": 1.0},
	{"name": "FSR2 1.0 (Godot TAA)", "godot_mode": Viewport.SCALING_3D_MODE_FSR2, "scale": 1.0},
	{"name": "FSR2 Quality", "godot_mode": Viewport.SCALING_3D_MODE_FSR2, "scale": 1.0 / 1.5},
	{"name": "DLAA", "dlss": 1},
	{"name": "DLSS Quality", "dlss": 2},
	{"name": "DLSS Balanced", "dlss": 3},
	{"name": "DLSS Performance", "dlss": 4},
	{"name": "DLSS Ultra Performance", "dlss": 5},
]


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		var value := arg.get_slice("=", 1)
		if arg.begins_with("--size="):
			_size = Vector2i(int(value.get_slice("x", 0)), int(value.get_slice("x", 1)))
		elif arg.begins_with("--frames="):
			_frames = int(value)
		elif arg.begins_with("--load="):
			_load = int(value)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(_size)
	_demo = load("res://demo/demo_main.tscn").instantiate()
	root.add_child(_demo)
	_lu = Engine.get_singleton("LatencyUpscaling") if Engine.has_singleton("LatencyUpscaling") else null
	RenderingServer.viewport_set_measure_render_time(root.get_viewport_rid(), true)
	_run.call_deferred()


func _median(values: Array) -> float:
	if values.is_empty():
		return -1.0
	var sorted := values.duplicate()
	sorted.sort()
	return float(sorted[sorted.size() / 2])


func _apply(config: Dictionary) -> bool:
	if config.has("dlss"):
		_lu.dlss_enabled = true
		_lu.dlss_quality = config["dlss"]
		for i in 180:
			await process_frame
			var s: Dictionary = _lu.get_status()["upscaling"]
			if s["active"] and s["backend"] == "dlss" and s["frames_since_reset"] > 2:
				return true
		return false
	_lu.dlss_enabled = false
	await process_frame
	await process_frame
	root.scaling_3d_mode = config["godot_mode"]
	root.scaling_3d_scale = config["scale"]
	return true


func _measure(config: Dictionary, gpu_load: int) -> Dictionary:
	_demo.set("gpu_load", gpu_load)
	var ok := await _apply(config)
	for i in SETTLE_FRAMES:
		await process_frame
	var gpu_ms := []
	var frame_ms := []
	var last := Time.get_ticks_usec()
	for i in _frames:
		await process_frame
		var now := Time.get_ticks_usec()
		frame_ms.append((now - last) / 1000.0)
		last = now
		gpu_ms.append(RenderingServer.viewport_get_measured_render_time_gpu(root.get_viewport_rid()))
	var s: Dictionary = _lu.get_status()["upscaling"]
	var render: Vector2i = s["render_size"] if config.has("dlss") else Vector2i(Vector2(_size) * float(config["scale"]))
	var result := {
		"config": config["name"], "load": gpu_load, "applied": ok, "output": [_size.x, _size.y], "render": [render.x, render.y],
		"gpu_ms": snappedf(_median(gpu_ms), 0.001), "frame_ms": snappedf(_median(frame_ms), 0.001),
		"dlss_failed": s["dlss_frames"]["failed"] if config.has("dlss") else 0,
	}
	print("BENCH " + JSON.stringify(result))
	return result


func _run() -> void:
	if _lu == null:
		print("dlss bench: the LatencyUpscaling singleton is missing")
		quit(1)
		return
	for i in 30:
		await process_frame
	print("DLSS runtime: %s; GPU %s" % [_lu.get_status()["upscaling"]["dlss_frames"].get("runtime", "?"), _lu.get_status()["vulkan"]["gpu"]])
	for gpu_load in [0, _load]:
		for config: Dictionary in CONFIGS:
			await _measure(config, gpu_load)
	print("dlss bench: done")
	quit(0)
