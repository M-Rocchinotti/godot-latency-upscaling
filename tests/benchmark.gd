extends SceneTree
## Frame-time and Reflex-latency measurement on the demo scene (driven by tests/run_benchmark.py).
##
##     python tools/godot.py --script res://tests/benchmark.gd -- --load=6000 --modes=0,1,2,0,1,2 --frames=600
##
## For every entry of --modes (Reflex mode; "-" = do not touch Reflex, for runs without the
## addon) it lets the scene settle, then measures --frames frames:
##   frame_ms       main-loop frame interval (Time.get_ticks_usec between process frames)
##   latency_ms     simulation start -> GPU render end, from vkGetLatencyTimingsNV (NVIDIA's
##                  "total game-to-render latency"), per frame, deduplicated by present ID
## and prints one line "BENCH {json}" per entry. VSync is off: Reflex lowers latency when the
## GPU is the bottleneck, which --load (full-screen shader iterations) makes it.

const SETTLE_FRAMES := 120
const REPORT_EVERY := 20

var _load := 6000
var _modes: PackedStringArray = ["0", "1", "2"]
var _frames := 600
var _window := "windowed" # or "exclusive" (exclusive fullscreen)
var _dlss: Object

# Consecutive stages of the Reflex timeline; each value is the microsecond field pair whose
# difference is that stage's duration. "present_end -> gpu_start" is the gap the docs single out.
const STAGES := [
	["simulation", "sim_start_us", "sim_end_us"],
	["sim_end -> submit", "sim_end_us", "render_submit_start_us"],
	["render submit", "render_submit_start_us", "render_submit_end_us"],
	["submit -> present", "render_submit_end_us", "present_start_us"],
	["present call", "present_start_us", "present_end_us"],
	["present_end -> gpu_start", "present_end_us", "gpu_render_start_us"],
	["gpu render", "gpu_render_start_us", "gpu_render_end_us"],
]


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		var value := arg.get_slice("=", 1)
		if arg.begins_with("--load="):
			_load = int(value)
		elif arg.begins_with("--modes="):
			_modes = value.split(",")
		elif arg.begins_with("--frames="):
			_frames = int(value)
		elif arg.begins_with("--window="):
			_window = value
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	if _window == "exclusive":
		DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_EXCLUSIVE_FULLSCREEN)
	var demo: Node = load("res://demo/demo_main.tscn").instantiate()
	demo.gpu_load = _load
	root.add_child(demo)
	_dlss = Engine.get_singleton("LatencyUpscaling") if Engine.has_singleton("LatencyUpscaling") else null
	_run.call_deferred()


func _run() -> void:
	for mode in _modes:
		var reflex: bool = _dlss != null and _dlss.is_reflex_active()
		if mode != "-" and reflex:
			_dlss.reflex_mode = int(mode)
		for i in SETTLE_FRAMES:
			await process_frame
		var frame_ms := PackedFloat64Array()
		var latency := {}
		var stages := {} # present_id -> {stage name: ms}
		# Only frames simulated after this point count: the driver's reports reach back 64 frames.
		var first_id: int = _dlss.get_latency_stats()["latest_present_id"] if reflex else 0
		var last := Time.get_ticks_usec()
		for i in _frames:
			await process_frame
			var now := Time.get_ticks_usec()
			frame_ms.append((now - last) / 1000.0)
			last = now
			if reflex and i % REPORT_EVERY == 0:
				_collect(latency, stages, first_id)
		if reflex:
			_collect(latency, stages, first_id)
		var result := {
			"mode": mode,
			"reflex_active": reflex,
			"load": _load,
			"window": _window,
			"frames": _frames,
			"frame_ms": _summary(frame_ms),
			"latency_ms": _summary(PackedFloat64Array(latency.values())),
			"stages_ms": _stage_summary(stages),
			"sleep_ms": _dlss.get_latency_stats()["reflex_sleep_ms"] if reflex else 0.0,
		}
		print("BENCH " + JSON.stringify(result))
	quit()


func _collect(into: Dictionary, stages: Dictionary, first_id: int) -> void:
	for r: Dictionary in _dlss.get_latency_reports():
		if r["present_id"] <= first_id or r["sim_start_us"] <= 0 or r["gpu_render_end_us"] < r["sim_start_us"]:
			continue
		into[r["present_id"]] = (r["gpu_render_end_us"] - r["sim_start_us"]) / 1000.0
		# Per-stage split; only rows whose whole timeline is monotonic (no clamped/missing field).
		var per := {}
		var ok := true
		for stage: Array in STAGES:
			var delta: int = int(r[stage[2]]) - int(r[stage[1]])
			if delta < 0:
				ok = false
				break
			per[stage[0]] = delta / 1000.0
		if ok:
			stages[r["present_id"]] = per


func _stage_summary(stages: Dictionary) -> Dictionary:
	var out := {}
	for stage: Array in STAGES:
		var values := PackedFloat64Array()
		for per: Dictionary in stages.values():
			values.append(per[stage[0]])
		out[stage[0]] = _summary(values)
	return out


static func _summary(values: PackedFloat64Array) -> Dictionary:
	if values.is_empty():
		return {"n": 0}
	var sorted := values.duplicate()
	sorted.sort()
	var total := 0.0
	for v in sorted:
		total += v
	return {
		"n": sorted.size(),
		"mean": total / sorted.size(),
		"median": sorted[sorted.size() / 2],
		"p95": sorted[mini(sorted.size() - 1, int(sorted.size() * 0.95))],
	}
