#include "core/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace lu {
namespace {

bool is_bool_value(double v) {
	return v == 0.0 || v == 1.0;
}

std::string describe(const SettingsLayer &layer, const std::string &key, double value, const char *expected) {
	char buffer[256];
	std::snprintf(buffer, sizeof(buffer), "%s: '%s' = %g ignored (expected %s)", layer.name.c_str(), key.c_str(),
			value, expected);
	return buffer;
}

} // namespace

const std::vector<const char *> &all_setting_keys() {
	static const std::vector<const char *> list = {
		keys::ENABLED,
		keys::ENABLE_IN_EDITOR,
		keys::REFLEX_MODE,
		keys::REFLEX_FPS_LIMIT,
		keys::REFLEX_FLASH_INDICATOR,
		keys::OVERLAY_ENABLED,
		keys::DLSS_ENABLED,
		keys::DLSS_QUALITY,
		keys::DLSS_PRESET,
		keys::DLSS_CUT_ANGLE,
		keys::DLSS_CUT_DISTANCE,
	};
	return list;
}

bool is_valid_fps_limit(double fps_limit) {
	if (!std::isfinite(fps_limit)) {
		return false;
	}
	return fps_limit == 0.0 || (fps_limit >= MIN_FPS_LIMIT && fps_limit <= MAX_FPS_LIMIT);
}

unsigned int fps_limit_to_interval_us(double fps_limit) {
	if (!is_valid_fps_limit(fps_limit) || fps_limit == 0.0) {
		return 0;
	}
	return static_cast<unsigned int>(std::lround(1000000.0 / fps_limit));
}

const char *reflex_mode_name(ReflexMode mode) {
	switch (mode) {
		case ReflexMode::Off:
			return "Off";
		case ReflexMode::On:
			return "On";
		case ReflexMode::OnBoost:
			return "On + Boost";
	}
	return "unknown";
}

SettingsLayer parse_settings_override(const std::string &name, const std::string &text, std::vector<std::string> &warnings) {
	SettingsLayer layer;
	layer.name = name;
	auto trim = [](std::string s) {
		const size_t first = s.find_first_not_of(" \t");
		const size_t last = s.find_last_not_of(" \t");
		return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
	};
	size_t start = 0;
	while (start <= text.size()) {
		const size_t end = std::min(text.find(';', start), text.size());
		const std::string entry = trim(text.substr(start, end - start));
		start = end + 1;
		if (entry.empty()) {
			continue;
		}
		const size_t equals = entry.find('=');
		const std::string key = equals == std::string::npos ? std::string() : trim(entry.substr(0, equals));
		const std::string value = equals == std::string::npos ? std::string() : trim(entry.substr(equals + 1));
		double number = 0.0;
		char *parse_end = nullptr;
		if (value == "true") {
			number = 1.0;
		} else if (value == "false") {
			number = 0.0;
		} else if (!value.empty()) {
			number = std::strtod(value.c_str(), &parse_end);
		}
		const bool numeric_ok = value == "true" || value == "false" || (parse_end != nullptr && *parse_end == '\0');
		if (key.empty() || !numeric_ok) {
			warnings.push_back(name + ": '" + entry + "' ignored (expected key=true|false|number)");
			continue;
		}
		layer.values[key] = number;
	}
	return layer;
}

ResolvedSettings resolve_settings(const std::vector<SettingsLayer> &layers) {
	ResolvedSettings out;
	for (const SettingsLayer &layer : layers) {
		for (const auto &[key, value] : layer.values) {
			bool accepted = true;
			if (key == keys::ENABLED || key == keys::ENABLE_IN_EDITOR || key == keys::REFLEX_FLASH_INDICATOR ||
					key == keys::OVERLAY_ENABLED || key == keys::DLSS_ENABLED) {
				if (!is_bool_value(value)) {
					out.warnings.push_back(describe(layer, key, value, "true or false"));
					accepted = false;
				} else if (key == keys::ENABLED) {
					out.settings.enabled = value != 0.0;
				} else if (key == keys::ENABLE_IN_EDITOR) {
					out.settings.enable_in_editor = value != 0.0;
				} else if (key == keys::REFLEX_FLASH_INDICATOR) {
					out.settings.reflex_flash_indicator = value != 0.0;
				} else if (key == keys::DLSS_ENABLED) {
					out.settings.dlss_enabled = value != 0.0;
				} else {
					out.settings.overlay_enabled = value != 0.0;
				}
			} else if (key == keys::REFLEX_MODE) {
				if (value != 0.0 && value != 1.0 && value != 2.0) {
					out.warnings.push_back(describe(layer, key, value, "0 (Off), 1 (On) or 2 (On + Boost)"));
					accepted = false;
				} else {
					out.settings.reflex_mode = static_cast<ReflexMode>(static_cast<int>(value));
				}
			} else if (key == keys::DLSS_QUALITY) {
				if (value != std::floor(value) || value < 0.0 || value >= upscale::QUALITY_COUNT) {
					out.warnings.push_back(describe(layer, key, value,
							"0 (Auto), 1 (DLAA), 2 (Quality), 3 (Balanced), 4 (Performance) or 5 (Ultra Performance)"));
					accepted = false;
				} else {
					out.settings.dlss_quality = static_cast<upscale::Quality>(static_cast<int>(value));
				}
			} else if (key == keys::DLSS_PRESET) {
				if (value != std::floor(value) || value < 0.0 || value >= upscale::PRESET_COUNT) {
					out.warnings.push_back(describe(layer, key, value, "0 (Default), 1 (J), 2 (K), 3 (L) or 4 (M)"));
					accepted = false;
				} else {
					out.settings.dlss_preset = static_cast<upscale::Preset>(static_cast<int>(value));
				}
			} else if (key == keys::DLSS_CUT_ANGLE) {
				if (!std::isfinite(value) || value < 0.0 || value > 180.0) {
					out.warnings.push_back(describe(layer, key, value, "0 (off) to 180 degrees"));
					accepted = false;
				} else {
					out.settings.dlss_cut_angle_degrees = value;
				}
			} else if (key == keys::DLSS_CUT_DISTANCE) {
				if (!std::isfinite(value) || value < 0.0) {
					out.warnings.push_back(describe(layer, key, value, "0 (off) or a distance in world units"));
					accepted = false;
				} else {
					out.settings.dlss_cut_distance = value;
				}
			} else if (key == keys::REFLEX_FPS_LIMIT) {
				if (!is_valid_fps_limit(value)) {
					out.warnings.push_back(describe(layer, key, value, "0 or a limit from 10 to 1000"));
					accepted = false;
				} else {
					out.settings.reflex_fps_limit = value;
				}
			} else {
				out.warnings.push_back(layer.name + ": unknown setting '" + key + "' ignored");
				accepted = false;
			}
			if (accepted) {
				out.source_of[key] = layer.name;
			}
		}
	}
	return out;
}

} // namespace lu
