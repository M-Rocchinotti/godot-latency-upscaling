#include "dlss/jitter.h"

#include <cmath>

namespace lu::dlss {

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

JitterNdc jitter_ndc_from_projection(const float columns[4][4]) {
	// correction = identity + depth correction + columns[3].xy = jitter. For a column v of the
	// projection, correction * v adds v.w * jitter to v.xy. Perspective: column 2 has w = -1
	// (and xy = 0 when symmetric); orthogonal: column 3 has w = 1 (and xy = 0).
	JitterNdc out;
	const float w2 = columns[2][3];
	if (w2 != 0.0f) {
		out.x = columns[2][0] / w2;
		out.y = columns[2][1] / w2;
	} else if (columns[3][3] != 0.0f) {
		out.x = columns[3][0] / columns[3][3];
		out.y = columns[3][1] / columns[3][3];
	}
	return out;
}

JitterPixels ndc_to_pixels(JitterNdc ndc, uint32_t render_width, uint32_t render_height) {
	JitterPixels out;
	out.x = ndc.x * static_cast<float>(render_width) * 0.5f;
	out.y = ndc.y * static_cast<float>(render_height) * 0.5f;
	return out;
}

int find_phase(JitterPixels observed, uint32_t phase_count, float tolerance) {
	for (uint32_t i = 0; i < phase_count; ++i) {
		const JitterPixels j = jitter_in_pixels(i, phase_count);
		if (std::fabs(j.x - observed.x) <= tolerance && std::fabs(j.y - observed.y) <= tolerance) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

} // namespace lu::dlss
