#include "upscale/quality.h"

#include <algorithm>
#include <cmath>

namespace lu::upscale {

bool resolve_auto(Size output, Quality &mode) {
	const uint64_t pixels = static_cast<uint64_t>(output.width) * output.height;
	if (pixels < 1920ull * 1080ull) {
		return false; // NVIDIA: DLSS disabled below 1920x1080
	}
	if (pixels <= 2560ull * 1440ull) {
		mode = Quality::Quality;
	} else if (pixels <= 3840ull * 2160ull) {
		mode = Quality::Performance;
	} else {
		mode = Quality::UltraPerformance;
	}
	return true;
}

double scaling_ratio(Quality mode) {
	switch (mode) {
		case Quality::UltraPerformance:
			return 3.0;
		case Quality::Performance:
			return 2.0;
		case Quality::Balanced:
			return 1.724;
		case Quality::Quality:
			return 1.5;
		case Quality::DLAA:
		case Quality::Auto:
			return 1.0;
	}
	return 1.0;
}

float render_scale(Quality mode) {
	return static_cast<float>(1.0 / scaling_ratio(mode));
}

Size render_size(Size output, Quality mode) {
	const double ratio = scaling_ratio(mode);
	Size out;
	out.width = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(output.width / ratio)));
	out.height = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(output.height / ratio)));
	return out;
}

const char *quality_name(Quality mode) {
	switch (mode) {
		case Quality::Auto:
			return "Auto";
		case Quality::DLAA:
			return "DLAA";
		case Quality::Quality:
			return "Quality";
		case Quality::Balanced:
			return "Balanced";
		case Quality::Performance:
			return "Performance";
		case Quality::UltraPerformance:
			return "Ultra Performance";
	}
	return "unknown";
}

const char *preset_name(Preset preset) {
	switch (preset) {
		case Preset::Default:
			return "Default";
		case Preset::J:
			return "J";
		case Preset::K:
			return "K";
		case Preset::L:
			return "L";
		case Preset::M:
			return "M";
	}
	return "unknown";
}

} // namespace lu::upscale
