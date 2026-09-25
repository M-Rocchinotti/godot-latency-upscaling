@tool
extends EditorExportPlugin
## Warns when a Windows export will not carry the NVIDIA runtime for DLSS because the addon's editor
## plugin is disabled (nvidia_runtime_export.gd, which ships the runtime, is only registered by the
## enabled plugin). The GDExtension registers this check in every editor session, plugin or not
## (src/godot/latency_upscaling.cpp); with the plugin enabled it stays silent.

const Runtime := preload("res://addons/latency_upscaling/nvidia_runtime.gd")
const PLUGIN_CFG := "res://addons/latency_upscaling/plugin.cfg"
const ENABLED_SETTING := "latency_upscaling/general/enabled"
const DLSS_ENABLED_SETTING := "latency_upscaling/dlss/enabled"


func _get_name() -> String:
	return "LatencyUpscalingNvidiaRuntimeCheck"


## Why this export goes without the runtime although the project would use it ("" when it does not).
static func missing_runtime_reason(features: PackedStringArray) -> String:
	if not features.has("windows") or not ProjectSettings.get_setting(ENABLED_SETTING, true) \
			or not ProjectSettings.get_setting(DLSS_ENABLED_SETTING, true) or not Runtime.is_installed():
		return ""
	var plugins: PackedStringArray = ProjectSettings.get_setting("editor_plugins/enabled", PackedStringArray())
	if plugins.has(PLUGIN_CFG):
		return ""
	return ("the Latency & Upscaling editor plugin is disabled, so this Windows export does not carry the NVIDIA DLSS "
			+ "runtime and the game will run without DLSS. Enable the plugin (Project Settings > Plugins) and export again.")


func _export_begin(features: PackedStringArray, _is_debug: bool, _path: String, _flags: int) -> void:
	var reason := missing_runtime_reason(features)
	if reason != "":
		push_warning("Latency & Upscaling: " + reason)
