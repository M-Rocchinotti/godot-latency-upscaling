// GDExtension entry point.
//
// CORE level  (before the DisplayServer and the Vulkan device exist): settings, safety checks,
//             hook installation (godot/bootstrap.cpp).
// SCENE level (the Vulkan device exists): the LatencyUpscaling class and engine singleton, project
//             settings in the editor.
// Main-loop callbacks: startup (every engine singleton is registered, the main loop exists:
//             frame signals, overlay) and frame (end of every main-loop iteration: Reflex sleep).
#include "core/log.h"
#include "godot/bootstrap.h"
#include "godot/latency_upscaling.h"

#include <gdextension_interface.h>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace {

constexpr const char *SINGLETON_NAME = "LatencyUpscaling";
lu::LatencyUpscaling *g_singleton = nullptr;

void initialize(ModuleInitializationLevel level) {
	if (level == MODULE_INITIALIZATION_LEVEL_CORE) {
		lu::bootstrap_core();
		return;
	}
	if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	if (lu::boot_state().late_load) {
		// Loaded into a running editor (the addon was just added): the Vulkan device already exists,
		// so nothing can be activated before a restart. Registering classes now is also unsafe:
		// Godot 4.7.2 crashes when a `--headless --import` run exits right after loading an extension
		// that registered classes (the editor's documentation pass still uses them). Stay dormant;
		// the editor plugin reports "restart the editor to activate".
		return;
	}
	GDREGISTER_CLASS(lu::LatencyUpscaling);
	g_singleton = memnew(lu::LatencyUpscaling);
	Engine::get_singleton()->register_singleton(SINGLETON_NAME, g_singleton);
	if (Engine::get_singleton()->is_editor_hint()) {
		lu::LatencyUpscaling::register_project_settings();
	}
}

void uninitialize(ModuleInitializationLevel level) {
	if (level == MODULE_INITIALIZATION_LEVEL_SCENE && g_singleton != nullptr) {
		g_singleton->disconnect_frame_signals();
		Engine::get_singleton()->unregister_singleton(SINGLETON_NAME);
		memdelete(g_singleton);
		g_singleton = nullptr;
	} else if (level == MODULE_INITIALIZATION_LEVEL_CORE) {
		// The Vulkan layer stays in place (the DLL is pinned) but must stop printing through an
		// engine that is shutting down.
		lu::log::set_sink(nullptr);
	}
}

void on_startup() {
	if (g_singleton != nullptr) {
		g_singleton->on_startup();
	}
}

void on_frame() {
	if (g_singleton != nullptr) {
		g_singleton->on_frame();
	}
}

} // namespace

extern "C" {

GDExtensionBool GDE_EXPORT godot_latency_upscaling_init(GDExtensionInterfaceGetProcAddress get_proc_address,
		GDExtensionClassLibraryPtr library, GDExtensionInitialization *initialization) {
	GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
	init.register_initializer(initialize);
	init.register_terminator(uninitialize);
	init.register_startup_callback(on_startup);
	init.register_frame_callback(on_frame);
	// CORE: the earliest level, the only one that runs before the Vulkan device is created.
	init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_CORE);
	return init.init();
}
}
