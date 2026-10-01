@tool
extends EditorExportPlugin
## Ships the NVIDIA runtime for DLSS with Windows exports: the Super Resolution DLLs, the Frame
## Generation DLLs (Phase 3; they ride along but are only loaded at runtime when Frame Generation is
## enabled on a supported GPU) and NVIDIA's licence files go into nvidia/ beside the game executable,
## where the addon looks for them (the folder of its own library). The exact set is
## nvidia_runtime.json "export_files". Nothing is added when the runtime is not installed; then the
## export says so, because the game will run without DLSS.

const Runtime := preload("res://addons/latency_upscaling/nvidia_runtime.gd")
const DLSS_ENABLED_SETTING := "latency_upscaling/dlss/enabled"


func _get_name() -> String:
	return "LatencyUpscalingNvidiaRuntime"


func _export_begin(features: PackedStringArray, _is_debug: bool, _path: String, _flags: int) -> void:
	if not features.has("windows") or not ProjectSettings.get_setting(DLSS_ENABLED_SETTING, true):
		return
	if not Runtime.is_installed():
		push_warning("Latency & Upscaling: the NVIDIA DLSS runtime is not installed, so this export has no DLSS "
				+ "(Project > Tools > \"Latency & Upscaling: Get the NVIDIA DLSS Runtime\").")
		return
	for file in Runtime.export_files():
		if FileAccess.file_exists(file):
			add_shared_object(file, PackedStringArray(), Runtime.export_folder())
		else:
			push_warning("Latency & Upscaling: %s is missing from the NVIDIA runtime; fetch it again." % file)
