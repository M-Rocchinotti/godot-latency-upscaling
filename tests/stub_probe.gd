extends SceneTree
## Queries the LatencyUpscaling singleton and prints one JSON line ("STUB_PROBE {...}"), then
## quits. Used by stub/test_stub.py to validate the no-op stub (loaded in place of the real DLL);
## on the stub every method reports that the platform is not supported.

var _frames_left := 3


func _process(_delta: float) -> bool:
	_frames_left -= 1
	if _frames_left > 0:
		return false
	var out := {}
	out["has_singleton"] = Engine.has_singleton("LatencyUpscaling")
	if out["has_singleton"]:
		var s := Engine.get_singleton("LatencyUpscaling")
		out["is_reflex_supported"] = s.is_reflex_supported()
		out["is_reflex_active"] = s.is_reflex_active()
		out["get_reflex_status"] = s.get_reflex_status()
		out["get_version"] = s.get_version()
		out["get_status"] = s.get_status()
	print("STUB_PROBE " + JSON.stringify(out))
	quit()
	return true
