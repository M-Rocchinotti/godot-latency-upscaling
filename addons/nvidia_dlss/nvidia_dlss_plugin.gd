@tool
extends EditorPlugin
## Editor side of the NVIDIA DLSS addon.
##
## The GDExtension does its work without this plugin. Enabling the plugin adds:
## - a status report when the editor starts (and a notice when the addon was added while the
##   editor was running, which needs an editor restart to reach the editor's own Vulkan device);
## - Project > Tools > "NVIDIA DLSS: Print Status";
## - packing of addons/nvidia_dlss/nvidia_dlss.cfg into exported games (Godot does not export
##   .cfg files by default; without it exports use the identical built-in defaults).

const CONFIG_PATH := "res://addons/nvidia_dlss/nvidia_dlss.cfg"
const EXTENSION_PATH := "res://addons/nvidia_dlss/nvidia_dlss.gdextension"
const SINGLETON := "NvidiaDLSS"
const MENU_ITEM := "NVIDIA DLSS: Print Status"

var _export_plugin: EditorExportPlugin


func _enter_tree() -> void:
	_export_plugin = _ConfigExportPlugin.new()
	add_export_plugin(_export_plugin)
	add_tool_menu_item(MENU_ITEM, _print_status)
	_report_status.call_deferred()


func _exit_tree() -> void:
	remove_tool_menu_item(MENU_ITEM)
	remove_export_plugin(_export_plugin)
	_export_plugin = null


func _report_status() -> void:
	if not Engine.has_singleton(SINGLETON):
		if GDExtensionManager.is_extension_loaded(EXTENSION_PATH):
			# Loaded into this running editor: it stays dormant until the editor restarts.
			print("NVIDIA DLSS: loaded into the running editor; restart the editor to activate it.")
			EditorInterface.get_editor_toaster().push_toast(
					"NVIDIA DLSS: restart the editor to activate the addon. Games started with Run already use it.",
					EditorToaster.SEVERITY_WARNING)
		else:
			push_warning("NVIDIA DLSS: the GDExtension is not loaded. Check that addons/nvidia_dlss/bin/ contains the DLL for this platform.")
		return
	var status: Dictionary = Engine.get_singleton(SINGLETON).get_status()
	var hook: Dictionary = status["hook"]
	if not hook["installed"]:
		push_warning("NVIDIA DLSS: inactive: %s" % hook["reason"])
	print("NVIDIA DLSS %s: %s" % [status["version"], _summary(status)])


func _print_status() -> void:
	if not Engine.has_singleton(SINGLETON):
		push_warning("NVIDIA DLSS: the GDExtension is not loaded.")
		return
	print(JSON.stringify(Engine.get_singleton(SINGLETON).get_status(), "  "))


static func _summary(status: Dictionary) -> String:
	var hook: Dictionary = status["hook"]
	var vulkan: Dictionary = status["vulkan"]
	if not hook["active"]:
		return "Vulkan hook inactive (%s)." % hook["reason"]
	var reflex: Dictionary = status["reflex"]
	var reflex_text := "Reflex active in this process" if reflex["active"] else "Reflex: %s" % reflex["reason"]
	return "Vulkan hook active on %s (driver %s). %s." % [vulkan["gpu"], vulkan["driver_version"], reflex_text]


class _ConfigExportPlugin extends EditorExportPlugin:
	func _get_name() -> String:
		return "NvidiaDLSSConfig"

	func _export_begin(_features: PackedStringArray, _is_debug: bool, _path: String, _flags: int) -> void:
		if FileAccess.file_exists(CONFIG_PATH):
			add_file(CONFIG_PATH, FileAccess.get_file_as_bytes(CONFIG_PATH), false)
