// CORE-level start-up: settings, safety checks and hook installation.
//
// Runs from the GDExtension CORE initialisation, which Godot performs before it creates the
// DisplayServer and with it the Vulkan instance and device (main/main.cpp:2282 vs :3368 in
// 4.7.2). Everything decided here is kept for the status API.
#pragma once

#include "core/settings.h"

#include <map>
#include <string>
#include <vector>

namespace lu {

struct BootState {
	bool ran = false;
	ResolvedSettings resolved;
	Settings addon_defaults; // the built-in defaults, without project settings (for the editor UI)
	bool editor = false;
	std::string godot_version; // "4.7.2"
	bool late_load = false; // loaded while a RenderingServer already existed
	bool hook_installed = false;
	std::string hook_reason; // why the hook is not installed
	std::vector<std::string> patched_imports;
	bool reflex_enabled = false; // Reflex may modify this process' Vulkan device
	std::string reflex_reason;
	std::string dlss_runtime_dir; // where the NVIDIA runtime for DLSS is looked for (UTF-8)
	std::vector<std::string> dlss_runtime_missing; // its Super Resolution files not found there
	bool dlss_streamline = false; // the NVIDIA runtime is loaded and Vulkan goes through Streamline
	std::string dlss_runtime_reason; // why the NVIDIA runtime was not loaded ("" when it was)
	std::string dlss_project_id; // the project ID handed to NGX (dlss/project_id.h)
	std::string dlss_project_id_source; // "project setting" or "derived from application/config/name"
	// Phase 3, Frame Generation (guarded off in milestone H1). fg_permitted: the settings would
	// permit Frame Generation at startup; nothing of the runtime is loaded yet, so it is only logged.
	bool fg_permitted = false;
	std::string fg_reason; // why Frame Generation is not permitted / not loaded (for the log)
	std::vector<std::string> hidden_extensions; // debug-build test knob
	// Debug-build test knob: "reference" runs the reference upscaler through RenderingDevice,
	// "native" records it natively into Godot's command buffer (marker interception).
	std::string test_upscaler;
	int test_readback_frames = 0; // debug-build test knob: frames of UBO / velocity read-backs
};

void bootstrap_core();
const BootState &boot_state();

// Environment variables.
inline constexpr const char *ENV_DISABLE = "GODOT_LATENCY_UPSCALING_DISABLE"; // "1": no hook at all
inline constexpr const char *ENV_SETTINGS = "GODOT_LATENCY_UPSCALING_SETTINGS"; // "key=value;...": overrides
inline constexpr const char *ENV_VERBOSE = "GODOT_LATENCY_UPSCALING_VERBOSE"; // "1": informational log lines
// Debug builds only, for the fallback tests:
inline constexpr const char *ENV_TEST_HIDE_EXTENSIONS = "GODOT_LATENCY_UPSCALING_TEST_HIDE_EXTENSIONS"; // comma list
inline constexpr const char *ENV_TEST_GODOT_VERSION = "GODOT_LATENCY_UPSCALING_TEST_GODOT_VERSION"; // e.g. "4.8.0"
inline constexpr const char *ENV_TEST_FAULT = "GODOT_LATENCY_UPSCALING_TEST_FAULT"; // comma list (see core/testing.h)
inline constexpr const char *ENV_TEST_UPSCALER = "GODOT_LATENCY_UPSCALING_TEST_UPSCALER"; // "reference" / "native"
inline constexpr const char *ENV_TEST_READBACK_FRAMES = "GODOT_LATENCY_UPSCALING_TEST_READBACK_FRAMES"; // e.g. "40"
inline constexpr const char *ENV_TEST_FORCE_STREAMLINE = "GODOT_LATENCY_UPSCALING_TEST_FORCE_STREAMLINE"; // "1"
inline constexpr const char *ENV_TEST_RUNTIME_DIR = "GODOT_LATENCY_UPSCALING_TEST_RUNTIME_DIR"; // another runtime folder

inline constexpr const char *PROJECT_SETTING_PREFIX = "latency_upscaling/";
// A string project setting, outside the numeric settings of core/settings.h.
inline constexpr const char *PROJECT_ID_SETTING = "latency_upscaling/dlss/project_id";

} // namespace lu
