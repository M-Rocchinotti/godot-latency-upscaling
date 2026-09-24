extends SceneTree
## Headed integration tests of the Vulkan hook and Reflex (needs a window: swapchain, present).
##
##     python tools/godot.py --script res://tests/reflex_tests.gd
##
## Ends with "reflex tests: <passed> passed, <failed> failed". Every check prints PASS/FAIL.

const WARMUP_FRAMES := 30
const TIMESTAMP_TOLERANCE_US := 1500

var _dlss: Object
var _passed := 0
var _failed := 0


func _initialize() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	# A real scene: with an empty one Godot runs at thousands of frames per second and the
	# driver reports only a few complete frames (measured on driver 582.28).
	var demo: Node = load("res://demo/demo_main.tscn").instantiate()
	demo.gpu_load = 300
	root.add_child(demo)
	_run.call_deferred()


func _run() -> void:
	if not Engine.has_singleton("LatencyUpscaling"):
		_check("singleton registered", false, "LatencyUpscaling missing")
		_finish()
		return
	_dlss = Engine.get_singleton("LatencyUpscaling")
	await _frames(WARMUP_FRAMES)
	await _test_device_came_through_the_layer()
	await _test_markers_are_sent_every_frame()
	await _test_flash_marker()
	await _test_timings_are_plausible()
	await _test_mode_switching()
	await _test_fps_limit()
	await _test_swapchain_recreation()
	await _test_minimized_window()
	await _test_overlay()
	_finish()


func _frames(count: int) -> void:
	for i in count:
		await process_frame


func _check(name: String, ok: bool, detail: String = "") -> void:
	if ok:
		_passed += 1
		print("PASS %s" % name)
	else:
		_failed += 1
		print("FAIL %s  %s" % [name, detail])


func _finish() -> void:
	print("reflex tests: %d passed, %d failed" % [_passed, _failed])
	quit(1 if _failed > 0 else 0)


func _counters() -> Dictionary:
	return _dlss.get_status()["reflex"].get("counters", {})


func _test_device_came_through_the_layer() -> void:
	var status: Dictionary = _dlss.get_status()
	var vulkan: Dictionary = status["vulkan"]
	print("instance layers: %s" % str(vulkan["instance_layers"]))
	_check("hook active", status["hook"]["active"], str(status["hook"]))
	_check("VK_NV_low_latency2 enabled on the device", "VK_NV_low_latency2" in vulkan["device_extensions"], str(vulkan["device_extensions"]))
	_check("VK_KHR_present_id enabled on the device", "VK_KHR_present_id" in vulkan["device_extensions"])
	# get_driver_resource() must run on the render thread (a separate one in that thread model).
	var queried := [0]
	RenderingServer.call_on_render_thread(func() -> void:
		queried[0] = RenderingServer.get_rendering_device().get_driver_resource(RenderingDevice.DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0))
	await _frames(3)
	var godot_device: int = queried[0]
	_check("Godot renders with the device created through the layer", godot_device != 0 and godot_device == vulkan["device_handle"],
			"godot %d vs layer %d" % [godot_device, vulkan["device_handle"]])
	_check("Reflex supported", _dlss.is_reflex_supported(), _dlss.get_reflex_status())
	_check("Reflex active", _dlss.is_reflex_active(), _dlss.get_reflex_status())
	_check("reflex status text", _dlss.get_reflex_status() == "active", _dlss.get_reflex_status())


func _test_markers_are_sent_every_frame() -> void:
	var before := _counters()
	await _frames(20)
	var after := _counters()
	for marker in ["SIMULATION_START", "SIMULATION_END", "RENDERSUBMIT_START", "RENDERSUBMIT_END", "PRESENT_START", "PRESENT_END", "INPUT_SAMPLE"]:
		var sent: int = after[marker] - before[marker]
		_check("%s sent once per frame" % marker, sent >= 18 and sent <= 22, "%d in 20 frames" % sent)
	var presents: int = after["presents_stamped"] - before["presents_stamped"]
	_check("presents carry a present ID", presents >= 18, "%d" % presents)
	var submits: int = after["submits_tagged"] - before["submits_tagged"]
	_check("submissions carry the frame's present ID", submits >= 18, "%d" % submits)
	var sleeps: int = after["sleeps"] - before["sleeps"]
	_check("Reflex sleep called every frame", sleeps >= 18 and sleeps <= 22, "%d" % sleeps)
	_check("no sleep timeouts", after["sleep_timeouts"] == 0, "%d" % after["sleep_timeouts"])


func _test_flash_marker() -> void:
	var before: int = _counters()["TRIGGER_FLASH"]
	_dlss.trigger_flash()
	await _frames(3)
	var sent: int = _counters()["TRIGGER_FLASH"] - before
	_check("trigger_flash() sends one TRIGGER_FLASH marker", sent == 1, "%d" % sent)


func _test_timings_are_plausible() -> void:
	await _frames(70) # the driver keeps the last 64 frames
	var reports: Array = _dlss.get_latency_reports()
	_check("vkGetLatencyTimingsNV returns reports", reports.size() > 0, "%d reports" % reports.size())
	var complete := 0
	var ordered := true
	var detail := ""
	var last_id := 0
	var ids_increase := true
	for r: Dictionary in reports:
		if r["sim_start_us"] == 0 or r["gpu_render_end_us"] == 0:
			continue
		complete += 1
		var chain: Array = [r["sim_start_us"], r["sim_end_us"], r["render_submit_start_us"], r["render_submit_end_us"], r["present_start_us"], r["present_end_us"]]
		for i in range(1, chain.size()):
			# The driver's CPU marker timestamps are coarse (about 1 ms steps on driver 582.28)
			# and neighbours can come back a few microseconds out of order although the markers
			# were sent in order; only a real reordering exceeds this tolerance.
			if chain[i] < chain[i - 1] - TIMESTAMP_TOLERANCE_US:
				ordered = false
				detail = "present %d: %s" % [r["present_id"], str(chain)]
		if r["gpu_render_end_us"] < r["gpu_render_start_us"]:
			ordered = false
			detail = "present %d: GPU end before start" % r["present_id"]
		if r["present_id"] <= last_id:
			ids_increase = false
		last_id = r["present_id"]
	_check("complete frame reports", complete >= 30, "%d of %d" % [complete, reports.size()])
	_check("marker timestamps in order", ordered, detail)
	_check("report present IDs increase", ids_increase)
	var stats: Dictionary = _dlss.get_latency_stats()
	_check("latency stats computed", stats["frames"] >= 30, str(stats["frames"]))
	var latency: float = stats["sim_start_to_gpu_end_ms"]
	_check("simulation-to-GPU-end latency plausible", latency > 0.05 and latency < 500.0, "%.3f ms" % latency)
	_check("GPU render time plausible", stats["gpu_render_ms"] > 0.0 and stats["gpu_render_ms"] < 500.0, "%.3f ms" % stats["gpu_render_ms"])


func _test_mode_switching() -> void:
	for mode in [0, 2, 1]:
		_dlss.reflex_mode = mode
		await _frames(10)
		var reflex: Dictionary = _dlss.get_status()["reflex"]
		_check("mode %d applied" % mode, _dlss.reflex_mode == mode and reflex["mode"] == mode and reflex["sleep_mode_error"] == "",
				str(reflex))
		_check("still active in mode %d" % mode, _dlss.is_reflex_active(), _dlss.get_reflex_status())
	var before: int = _dlss.reflex_mode
	_dlss.reflex_mode = 7 # rejected with an error message (expected in the log)
	_check("invalid mode rejected", _dlss.reflex_mode == before)


func _test_fps_limit() -> void:
	_dlss.reflex_fps_limit = 30.0
	await _frames(10)
	var start := Time.get_ticks_usec()
	await _frames(30)
	var interval_ms := (Time.get_ticks_usec() - start) / 30000.0
	# The driver paces the Reflex sleep to the requested minimum interval (33.3 ms at 30 fps).
	_check("Reflex frame-rate limit paces frames", interval_ms > 30.0 and interval_ms < 40.0, "%.2f ms per frame" % interval_ms)
	_dlss.reflex_fps_limit = 0.0
	_dlss.reflex_fps_limit = 5.0 # rejected: below 10 fps
	_check("invalid frame-rate limit rejected", _dlss.reflex_fps_limit == 0.0)
	await _frames(10)


func _test_swapchain_recreation() -> void:
	var before: int = _counters()["swapchains_with_latency_mode"]
	var size := DisplayServer.window_get_size()
	DisplayServer.window_set_size(size + Vector2i(64, 32))
	await _frames(20)
	DisplayServer.window_set_size(size)
	await _frames(20)
	var after := _counters()
	_check("resized window gets a latency-mode swapchain", after["swapchains_with_latency_mode"] > before,
			"%d -> %d" % [before, after["swapchains_with_latency_mode"]])
	_check("Reflex active after swapchain recreation", _dlss.is_reflex_active(), _dlss.get_reflex_status())
	var presents: int = after["presents_stamped"]
	await _frames(10)
	_check("presents stamped after recreation", _counters()["presents_stamped"] > presents)


func _test_minimized_window() -> void:
	# A minimised window draws and presents nothing. The sleep must not wait for a present that
	# never comes (and must not be switched off for the rest of the session because of it).
	var before := _counters()
	DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_MINIMIZED)
	await _frames(40)
	DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_WINDOWED)
	await _frames(30)
	var after := _counters()
	var reflex: Dictionary = _dlss.get_status()["reflex"]
	_check("no sleep timeouts while minimised", after["sleep_timeouts"] == before["sleep_timeouts"],
			"%d -> %d" % [before["sleep_timeouts"], after["sleep_timeouts"]])
	_check("sleep still enabled after minimising", reflex["sleep_enabled"], _dlss.get_reflex_status())
	_check("no sleep without a present while minimised", after["sleeps_skipped"] > before["sleeps_skipped"],
			"%d -> %d skipped" % [before["sleeps_skipped"], after["sleeps_skipped"]])
	var presents: int = after["presents_stamped"]
	var sleeps: int = after["sleeps"]
	await _frames(10)
	_check("frames present and sleep again after restoring", _counters()["presents_stamped"] > presents and _counters()["sleeps"] > sleeps)


func _test_overlay() -> void:
	_dlss.overlay_visible = true
	await _frames(2)
	var overlay := root.get_node_or_null("LatencyUpscalingLatencyOverlay")
	_check("overlay shown", overlay != null and _dlss.overlay_visible)
	_dlss.overlay_visible = false
	await _frames(2)
	_check("overlay removed", root.get_node_or_null("LatencyUpscalingLatencyOverlay") == null and not _dlss.overlay_visible)
