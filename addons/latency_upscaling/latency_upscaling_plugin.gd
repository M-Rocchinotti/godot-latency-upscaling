@tool
extends EditorPlugin
## Editor side of the Latency & Upscaling addon.
##
## The GDExtension does its work without this plugin, and its settings live in Project Settings
## (latency_upscaling/...), which are always exported. Enabling the plugin only adds:
## - a status report when the editor starts (and a notice when the addon was added while the
##   editor was running, which needs an editor restart to reach the editor's own Vulkan device);
## - Project > Tools > "Latency & Upscaling: Print Status";
## - the NVIDIA runtime for DLSS: when the plugin is enabled and DLSS is on but the runtime is
##   missing, a dialog shows NVIDIA's licences and downloads the runtime only after the developer
##   accepted them (also under Project > Tools > "Latency & Upscaling: Get the NVIDIA DLSS Runtime").

const EXTENSION_PATH := "res://addons/latency_upscaling/latency_upscaling.gdextension"
const SINGLETON := "LatencyUpscaling"
const MENU_ITEM := "Latency & Upscaling: Print Status"
const MENU_RUNTIME := "Latency & Upscaling: Get the NVIDIA DLSS Runtime..."
const DLSS_ENABLED_SETTING := "latency_upscaling/dlss/enabled"
const Runtime := preload("res://addons/latency_upscaling/nvidia_runtime.gd")
const RuntimeDialog := preload("res://addons/latency_upscaling/nvidia_runtime_dialog.gd")

var _runtime_dialog: ConfirmationDialog


func _enter_tree() -> void:
	add_tool_menu_item(MENU_ITEM, _print_status)
	add_tool_menu_item(MENU_RUNTIME, _show_runtime_dialog)
	_report_status.call_deferred()


func _exit_tree() -> void:
	remove_tool_menu_item(MENU_ITEM)
	remove_tool_menu_item(MENU_RUNTIME)
	if _runtime_dialog != null:
		_runtime_dialog.queue_free()
		_runtime_dialog = null


# The developer enabled the plugin (Project Settings > Plugins): the first-enable moment to offer
# the NVIDIA runtime, when DLSS is on and the runtime is not installed yet.
func _enable_plugin() -> void:
	if ProjectSettings.get_setting(DLSS_ENABLED_SETTING, true) and not Runtime.is_installed():
		_show_runtime_dialog.call_deferred()


func _show_runtime_dialog() -> void:
	if _runtime_dialog == null:
		_runtime_dialog = RuntimeDialog.new()
		EditorInterface.get_base_control().add_child(_runtime_dialog)
	_runtime_dialog.popup_centered()


func _report_status() -> void:
	if not Engine.has_singleton(SINGLETON):
		if GDExtensionManager.is_extension_loaded(EXTENSION_PATH):
			# Loaded into this running editor: it stays dormant until the editor restarts.
			print("Latency & Upscaling: loaded into the running editor; restart the editor to activate it.")
			EditorInterface.get_editor_toaster().push_toast(
					"Latency & Upscaling: restart the editor to activate the addon. Games started with Run already use it.",
					EditorToaster.SEVERITY_WARNING)
		else:
			push_warning("Latency & Upscaling: the GDExtension is not loaded. Check that addons/latency_upscaling/bin/ contains the DLL for this platform.")
		return
	var status: Dictionary = Engine.get_singleton(SINGLETON).get_status()
	var hook: Dictionary = status["hook"]
	if not hook["installed"]:
		push_warning("Latency & Upscaling: inactive: %s" % hook["reason"])
	print("Latency & Upscaling %s: %s" % [status["version"], _summary(status)])


func _print_status() -> void:
	if not Engine.has_singleton(SINGLETON):
		push_warning("Latency & Upscaling: the GDExtension is not loaded.")
		return
	print(JSON.stringify(Engine.get_singleton(SINGLETON).get_status(), "  "))


static func _summary(status: Dictionary) -> String:
	var hook: Dictionary = status["hook"]
	var vulkan: Dictionary = status["vulkan"]
	if not hook["active"]:
		return "Vulkan hook inactive (%s)." % hook["reason"]
	var reflex: Dictionary = status["reflex"]
	var reflex_text := "Reflex active in this process" if reflex["active"] else "Reflex: %s" % reflex["reason"]
	var runtime_text := "NVIDIA DLSS runtime installed" if Runtime.is_installed() else \
			"NVIDIA DLSS runtime not installed (Project > Tools > %s)" % MENU_RUNTIME
	return "Vulkan hook active on %s (driver %s). %s. %s." % [vulkan["gpu"], vulkan["driver_version"], reflex_text, runtime_text]
