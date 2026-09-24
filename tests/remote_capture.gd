extends SceneTree
## Saves captures into the run folder of tools/rtx_remote.py (environment LU_RUN_DIR), so the
## harness can collect them. Screenshots come from inside Godot, never from the user's desktop.
##
##     tools/rtx_remote.py run NAME -- --headless --script res://tests/remote_capture.gd -- --frames=60
##
## With a window: runs the demo scene and saves the viewport after --frames frames (frame.png).
## Headless (nothing is rendered): saves a generated test image instead, which proves the
## capture-and-collect path. Also writes capture.json with what was saved.

var _frames := 60
var _left := 0


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--frames="):
			_frames = int(arg.get_slice("=", 1))
	_left = _frames
	if DisplayServer.get_name() != "headless":
		root.add_child(load("res://demo/demo_main.tscn").instantiate())


func _process(_delta: float) -> bool:
	_left -= 1
	if _left > 0:
		return false
	var dir := OS.get_environment("LU_RUN_DIR")
	if dir.is_empty():
		dir = OS.get_user_data_dir()
	var image: Image
	var kind := "viewport"
	if DisplayServer.get_name() == "headless":
		kind = "generated"
		image = Image.create_empty(64, 32, false, Image.FORMAT_RGB8)
		for y in 32:
			for x in 64:
				image.set_pixel(x, y, Color(x / 63.0, y / 31.0, 0.5))
	else:
		image = root.get_texture().get_image()
	var path := dir.path_join("frame.png")
	var err := image.save_png(path)
	var info := {"kind": kind, "path": path, "size": [image.get_width(), image.get_height()], "error": err,
			"frames": _frames, "display": DisplayServer.get_name()}
	var file := FileAccess.open(dir.path_join("capture.json"), FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(info))
	print("CAPTURE " + JSON.stringify(info))
	quit(0 if err == OK else 1)
	return true
