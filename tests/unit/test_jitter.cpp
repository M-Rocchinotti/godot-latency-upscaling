#include "dlss/jitter.h"
#include "test_framework.h"

using namespace nvdlss::dlss;

TEST(jitter_halton_matches_godot) {
	// Godot's get_halton_value (renderer_scene_cull.cpp:55-64) maps the radical inverse to [-1, 1].
	CHECK_NEAR(halton(0, 2), -1.0, 1e-7);
	CHECK_NEAR(halton(1, 2), 0.0, 1e-7);
	CHECK_NEAR(halton(2, 2), -0.5, 1e-7);
	CHECK_NEAR(halton(3, 2), 0.5, 1e-7);
	CHECK_NEAR(halton(5, 2), 0.25, 1e-7); // 101b -> 0.101b = 0.625
	CHECK_NEAR(halton(1, 3), -1.0 / 3.0, 1e-6);
	CHECK_NEAR(halton(2, 3), 1.0 / 3.0, 1e-6);
	CHECK_NEAR(halton(4, 3), -1.0 / 9.0, 1e-6); // 11 (base 3) -> 1/3 + 1/9 = 4/9
	CHECK_NEAR(halton(7, 3), 2.0 * (1.0 / 3.0 + 2.0 / 9.0) - 1.0, 1e-6); // 21 (base 3)
}

TEST(jitter_phase_count_matches_godot) {
	// renderer_viewport.cpp:269-277: 8 * (target / render)^2 for temporal upscalers, 16 for TAA.
	CHECK_EQ(jitter_phase_count(1920, 1280, true, false), 18u); // FSR2 Quality (1.5x)
	CHECK_EQ(jitter_phase_count(3840, 1920, true, false), 32u); // Performance (2x)
	CHECK_EQ(jitter_phase_count(1920, 1920, true, false), 8u); // native (DLAA)
	CHECK_EQ(jitter_phase_count(1920, 1920, false, true), 16u); // TAA
	CHECK_EQ(jitter_phase_count(1920, 1920, false, false), 0u);
	CHECK_EQ(jitter_phase_count(1920, 0, true, true), 16u); // degenerate render size falls back
}

TEST(jitter_in_pixels_is_half_the_halton_value) {
	const JitterPixels first = jitter_in_pixels(0, 18);
	CHECK_NEAR(first.x, -0.5, 1e-7);
	CHECK_NEAR(first.y, -0.5, 1e-7);
	const JitterPixels third = jitter_in_pixels(2 + 18 * 5, 18); // frame numbers wrap by the phase count
	CHECK_NEAR(third.x, -0.25, 1e-7);
	CHECK_NEAR(third.y, 1.0 / 6.0, 1e-6);
	const JitterPixels none = jitter_in_pixels(7, 0);
	CHECK_EQ(none.x, 0.0f);
	CHECK_EQ(none.y, 0.0f);
	for (uint64_t frame = 0; frame < 64; ++frame) {
		const JitterPixels j = jitter_in_pixels(frame, 32);
		CHECK(j.x >= -0.5f && j.x <= 0.5f && j.y >= -0.5f && j.y <= 0.5f);
	}
}
