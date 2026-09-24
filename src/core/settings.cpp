#include "core/settings.h"

#include <cmath>
#include <cstdio>

namespace nvdlss {
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

ResolvedSettings resolve_settings(const std::vector<SettingsLayer> &layers) {
	ResolvedSettings out;
	for (const SettingsLayer &layer : layers) {
		for (const auto &[key, value] : layer.values) {
			bool accepted = true;
			if (key == keys::ENABLED || key == keys::ENABLE_IN_EDITOR || key == keys::REFLEX_FLASH_INDICATOR ||
					key == keys::OVERLAY_ENABLED) {
				if (!is_bool_value(value)) {
					out.warnings.push_back(describe(layer, key, value, "true or false"));
					accepted = false;
				} else if (key == keys::ENABLED) {
					out.settings.enabled = value != 0.0;
				} else if (key == keys::ENABLE_IN_EDITOR) {
					out.settings.enable_in_editor = value != 0.0;
				} else if (key == keys::REFLEX_FLASH_INDICATOR) {
					out.settings.reflex_flash_indicator = value != 0.0;
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

} // namespace nvdlss
