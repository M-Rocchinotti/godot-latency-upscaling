extends CanvasLayer
## Small latency overlay of the Latency & Upscaling addon.
##
## Created by the LatencyUpscaling singleton when its overlay_visible property is set
## (Engine.get_singleton("LatencyUpscaling").set("overlay_visible", true)) or the setting
## latency_upscaling/overlay/enabled is on; it never needs to be added to a scene by hand. It reads
## get_latency_stats() four times a second, so its own cost is negligible. The glass
## panel deliberately has no backdrop blur: a blur pass would add GPU work to the very frames
## whose latency it reports.

const REFRESH_SECONDS := 0.25
const MODE_NAMES := ["Off", "On", "On + Boost"]

var _label: Label
var _since_refresh := 0.0


func _ready() -> void:
	layer = 128 # above typical game UI
	process_mode = Node.PROCESS_MODE_ALWAYS
	var panel := PanelContainer.new()
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	panel.position = Vector2(16, 16)
	panel.add_theme_stylebox_override("panel", _glass_panel())
	_label = Label.new()
	_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_label.add_theme_font_size_override("font_size", 13)
	_label.add_theme_color_override("font_color", Color(1.0, 1.0, 1.0, 0.94))
	_label.add_theme_color_override("font_shadow_color", Color(0.0, 0.0, 0.0, 0.45))
	_label.add_theme_constant_override("shadow_offset_y", 1)
	panel.add_child(_label)
	add_child(panel)
	_refresh()


func _process(delta: float) -> void:
	_since_refresh += delta
	if _since_refresh >= REFRESH_SECONDS:
		_since_refresh = 0.0
		_refresh()


func _refresh() -> void:
	if not Engine.has_singleton("LatencyUpscaling"):
		_label.text = "Latency & Upscaling addon not loaded"
		return
	var dlss: Object = Engine.get_singleton("LatencyUpscaling")
	var lines := PackedStringArray()
	lines.append("NVIDIA Reflex  %s" % MODE_NAMES[clampi(dlss.get_reflex_mode(), 0, 2)])
	if not dlss.is_reflex_active():
		lines.append(dlss.get_reflex_status())
	else:
		var s: Dictionary = dlss.get_latency_stats()
		if s["frames"] == 0:
			lines.append("waiting for frame reports")
		else:
			var interval: float = s["frame_interval_ms"]
			lines.append("Latency  %5.1f ms  (simulation start to GPU end)" % s["sim_start_to_gpu_end_ms"])
			lines.append("Frame    %5.1f ms  %4d fps" % [interval, roundi(1000.0 / interval) if interval > 0.0 else 0])
			lines.append("GPU      %5.1f ms" % s["gpu_render_ms"])
			lines.append("Sleep    %5.1f ms" % s["reflex_sleep_ms"])
	_label.text = "\n".join(lines)


static func _glass_panel() -> StyleBoxFlat:
	var box := StyleBoxFlat.new()
	box.bg_color = Color(0.07, 0.08, 0.10, 0.62)
	box.border_color = Color(1.0, 1.0, 1.0, 0.16)
	box.set_border_width_all(1)
	box.set_corner_radius_all(14)
	box.shadow_color = Color(0.0, 0.0, 0.0, 0.28)
	box.shadow_size = 10
	box.shadow_offset = Vector2(0, 3)
	box.content_margin_left = 14
	box.content_margin_right = 14
	box.content_margin_top = 10
	box.content_margin_bottom = 10
	box.anti_aliasing = true
	return box
