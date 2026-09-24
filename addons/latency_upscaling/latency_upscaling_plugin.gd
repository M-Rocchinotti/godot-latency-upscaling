@tool
extends EditorPlugin
## Editor side of the Latency & Upscaling addon.
##
## The GDExtension does its work without this plugin, and its settings live in Project Settings
## (latency_upscaling/...), which are always exported. Enabling the plugin only adds:
## - a status report when the editor starts (and a notice when the addon was added while the
##   editor was running, which needs an editor restart to reach the editor's own Vulkan device);
## - Project > Tools > "Latency & Upscaling: Print Status".

const EXTENSION_PATH := "res://addons/latency_upscaling/latency_upscaling.gdextension"
const SINGLETON := "LatencyUpscaling"
const MENU_ITEM := "Latency & Upscaling: Print Status"


func _enter_tree() -> void:
	add_tool_menu_item(MENU_ITEM, _print_status)
	_report_status.call_deferred()


func _exit_tree() -> void:
	remove_tool_menu_item(MENU_ITEM)


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
	return "Vulkan hook active on %s (driver %s). %s." % [vulkan["gpu"], vulkan["driver_version"], reflex_text]
