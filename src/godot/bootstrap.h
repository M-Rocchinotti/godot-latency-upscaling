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

namespace nvdlss {

struct BootState {
	bool ran = false;
	ResolvedSettings resolved;
	Settings addon_defaults; // built-in defaults + nvidia_dlss.cfg, without project settings
	bool editor = false;
	std::string godot_version; // "4.7.2"
	bool late_load = false; // loaded while a RenderingServer already existed
	bool hook_installed = false;
	std::string hook_reason; // why the hook is not installed
	std::vector<std::string> patched_imports;
	bool reflex_enabled = false; // Reflex may modify this process' Vulkan device
	std::string reflex_reason;
	std::vector<std::string> hidden_extensions; // debug-build test knob
};

void bootstrap_core();
const BootState &boot_state();

// Environment variables.
inline constexpr const char *ENV_DISABLE = "GODOT_NVIDIA_DLSS_DISABLE"; // "1": no hook at all
inline constexpr const char *ENV_VERBOSE = "GODOT_NVIDIA_DLSS_VERBOSE"; // "1": informational log lines
// Debug builds only, for the fallback tests:
inline constexpr const char *ENV_TEST_HIDE_EXTENSIONS = "GODOT_NVIDIA_DLSS_TEST_HIDE_EXTENSIONS"; // comma list
inline constexpr const char *ENV_TEST_GODOT_VERSION = "GODOT_NVIDIA_DLSS_TEST_GODOT_VERSION"; // e.g. "4.8.0"

inline constexpr const char *ADDON_CONFIG_PATH = "res://addons/nvidia_dlss/nvidia_dlss.cfg";
inline constexpr const char *PROJECT_SETTING_PREFIX = "nvidia_dlss/";

} // namespace nvdlss
