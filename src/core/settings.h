// Addon settings and how they are resolved from several layers.
//
// Precedence, lowest first:
//   1. built-in defaults (identical to the shipped addons/latency_upscaling/latency_upscaling.cfg),
//   2. addons/latency_upscaling/latency_upscaling.cfg,
//   3. project settings "latency_upscaling/<key>" (only present when a developer changed them),
//   4. the environment: GODOT_LATENCY_UPSCALING_SETTINGS="key=value;..." (e.g. for CI or a quick
//      test of the editor), then GODOT_LATENCY_UPSCALING_DISABLE=1, which forces enabled = false.
// Runtime API calls (LatencyUpscaling.reflex_mode = ...) override the result for the session.
//
// Pure C++: no Godot types, so the resolution rules are unit-tested.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace lu {

enum class ReflexMode : int { Off = 0, On = 1, OnBoost = 2 };

struct Settings {
	bool enabled = true; // false: the Vulkan hook is not installed at all
	bool enable_in_editor = false; // false: the editor process only passes Vulkan calls through
	ReflexMode reflex_mode = ReflexMode::On; // NVIDIA's Reflex QA checklist: default On
	double reflex_fps_limit = 0.0; // 0 = no limit, else frames per second
	bool reflex_flash_indicator = true; // send TRIGGER_FLASH on left mouse clicks
	bool overlay_enabled = false;
};

// Setting keys, shared by the cfg file ("section/key") and the project settings
// ("latency_upscaling/section/key").
namespace keys {
inline constexpr const char *ENABLED = "general/enabled";
inline constexpr const char *ENABLE_IN_EDITOR = "general/enable_in_editor";
inline constexpr const char *REFLEX_MODE = "reflex/mode";
inline constexpr const char *REFLEX_FPS_LIMIT = "reflex/fps_limit";
inline constexpr const char *REFLEX_FLASH_INDICATOR = "reflex/flash_indicator_marker";
inline constexpr const char *OVERLAY_ENABLED = "overlay/enabled";
} // namespace keys

// Every key the addon understands, in display order.
const std::vector<const char *> &all_setting_keys();

// Frame rate limits outside this range are rejected (0 stays "no limit").
inline constexpr double MIN_FPS_LIMIT = 10.0;
inline constexpr double MAX_FPS_LIMIT = 1000.0;

// One source of setting values. All addon settings are booleans or numbers, so a layer
// stores them as doubles (true = 1, false = 0).
struct SettingsLayer {
	std::string name; // for diagnostics, e.g. "project setting"
	std::map<std::string, double> values;
};

struct ResolvedSettings {
	Settings settings;
	std::map<std::string, std::string> source_of; // key -> name of the layer that set it
	std::vector<std::string> warnings; // invalid values that were ignored
};

// Applies the layers in order (later layers win). Invalid values are ignored with a warning,
// unknown keys too.
ResolvedSettings resolve_settings(const std::vector<SettingsLayer> &layers);

// Parses "key=value;key=value" (the GODOT_LATENCY_UPSCALING_SETTINGS environment variable) into a
// layer. Values: true, false or a number. Malformed entries are skipped with a warning; keys
// are validated later by resolve_settings().
SettingsLayer parse_settings_override(const std::string &name, const std::string &text, std::vector<std::string> &warnings);

// Converts a frame-rate limit to VK_NV_low_latency2's minimumIntervalUs (0 = no limit).
unsigned int fps_limit_to_interval_us(double fps_limit);

// Validates an API-provided value; returns false when it is outside the accepted range.
bool is_valid_fps_limit(double fps_limit);

const char *reflex_mode_name(ReflexMode mode);

} // namespace lu
