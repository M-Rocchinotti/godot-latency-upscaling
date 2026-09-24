extends SceneTree
## Runs the demo scene for --frames frames (default 240) with whatever upscaler backend the
## environment selects, then prints the upscaling status as one line ("UPSCALE_STATUS {...}") and
## the whole status like tests/print_status.gd ("LU_STATUS {...}").
## Used by the validation suite (the upscaler's frames must add no validation message) and by the
## upscaler fallback cases of tests/run_all.py.
## --fsr2=SCALE puts the viewport in Godot's own FSR2 at that scale (the baseline to compare with).

var _left := 240


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--frames="):
			_left = int(arg.get_slice("=", 1))
		elif arg.begins_with("--fsr2="):
			root.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
			root.scaling_3d_scale = float(arg.get_slice("=", 1))
	root.add_child(load("res://demo/demo_main.tscn").instantiate())


func _process(_delta: float) -> bool:
	_left -= 1
	if _left > 0:
		return false
	var status := {}
	if Engine.has_singleton("LatencyUpscaling"):
		status = Engine.get_singleton("LatencyUpscaling").get_status()
		var rd := RenderingServer.get_rendering_device()
		if rd != null:
			status["godot_vk_device"] = rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0)
	print("UPSCALE_STATUS " + JSON.stringify(status.get("upscaling", {})))
	print("LU_STATUS " + JSON.stringify(status))
	quit()
	return true
