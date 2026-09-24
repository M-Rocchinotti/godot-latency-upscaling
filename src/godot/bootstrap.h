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

inline constexpr const char *PROJECT_SETTING_PREFIX = "latency_upscaling/";

} // namespace lu
