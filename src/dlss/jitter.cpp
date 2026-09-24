#include "dlss/jitter.h"

#include <cmath>

namespace nvdlss::dlss {

float halton(int index, int base) {
	float f = 1;
	float r = 0;
	while (index > 0) {
		f = f / static_cast<float>(base);
		r = r + f * static_cast<float>(index % base);
		index = index / base;
	}
	return r * 2.0f - 1.0f;
}

uint32_t jitter_phase_count(uint32_t target_width, uint32_t render_width, bool temporal_upscaler, bool taa) {
	if (temporal_upscaler && render_width > 0) {
		return static_cast<uint32_t>(8.0f * std::pow(static_cast<float>(target_width) / static_cast<float>(render_width), 2.0f));
	}
	return taa ? 16u : 0u;
}

JitterPixels jitter_in_pixels(uint64_t frame_number, uint32_t phase_count) {
	JitterPixels out;
	if (phase_count == 0) {
		return out;
	}
	const int index = static_cast<int>(frame_number % phase_count);
	// Godot: NDC offset = halton / viewport_size; FSR2 pixels = NDC * size * 0.5 = halton * 0.5.
	out.x = halton(index, 2) * 0.5f;
	out.y = halton(index, 3) * 0.5f;
	return out;
}

} // namespace nvdlss::dlss
