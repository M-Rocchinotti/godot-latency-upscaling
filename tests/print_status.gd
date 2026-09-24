extends SceneTree
## Prints the addon status as one JSON line ("NVDLSS_STATUS {...}") after a number of frames,
## then quits. Used by the fallback tests and handy for manual checks:
##     python tools/godot.py --script res://tests/print_status.gd -- --frames=60

var _frames_left := 60


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--frames="):
			_frames_left = int(arg.get_slice("=", 1))


func _process(_delta: float) -> bool:
	_frames_left -= 1
	if _frames_left > 0:
		return false
	var status := {}
	if Engine.has_singleton("NvidiaDLSS"):
		status = Engine.get_singleton("NvidiaDLSS").get_status()
		var rd := RenderingServer.get_rendering_device()
		if rd != null:
			# The VkDevice Godot renders with, to compare with the one created through the layer.
			status["godot_vk_device"] = rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0)
	else:
		status["error"] = "NvidiaDLSS singleton not registered"
	print("NVDLSS_STATUS " + JSON.stringify(status))
	return true
