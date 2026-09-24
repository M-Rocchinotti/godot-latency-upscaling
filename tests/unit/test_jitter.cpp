#include "dlss/jitter.h"
#include "test_framework.h"

#include <cmath>

using namespace lu::dlss;

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

namespace {

// Godot's Projection layout and product (core/math/projection.cpp): columns[c][r],
// (a * b).columns[j][i] = sum_k a.columns[k][i] * b.columns[j][k].
struct Mat {
	float c[4][4] = {};
};
Mat mul(const Mat &a, const Mat &b) {
	Mat out;
	for (int j = 0; j < 4; ++j) {
		for (int i = 0; i < 4; ++i) {
			for (int k = 0; k < 4; ++k) {
				out.c[j][i] += a.c[k][i] * b.c[j][k];
			}
		}
	}
	return out;
}
Mat perspective(float fovy_rad, float aspect, float znear, float zfar) { // Projection::set_perspective
	Mat p;
	const float cot = 1.0f / std::tan(fovy_rad * 0.5f);
	p.c[0][0] = cot / aspect;
	p.c[1][1] = cot;
	p.c[2][2] = -(zfar + znear) / (zfar - znear);
	p.c[2][3] = -1.0f;
	p.c[3][2] = -2.0f * znear * zfar / (zfar - znear);
	return p;
}
Mat orthogonal(float half, float aspect, float znear, float zfar) { // Projection::set_orthogonal
	Mat p;
	p.c[0][0] = 1.0f / (half * aspect);
	p.c[1][1] = 1.0f / half;
	p.c[2][2] = -2.0f / (zfar - znear);
	p.c[3][2] = -(zfar + znear) / (zfar - znear);
	p.c[3][3] = 1.0f;
	return p;
}
// RenderSceneDataRD::get_cam_projection(): correction.set_depth_correction(flip_y) then
// correction.add_jitter_offset(taa_jitter), times the camera projection.
Mat godot_cam_projection(const Mat &cam, float jx, float jy, bool flip_y) {
	Mat correction;
	correction.c[0][0] = 1.0f;
	correction.c[1][1] = flip_y ? -1.0f : 1.0f;
	correction.c[2][2] = 0.5f; // remap_z (Projection::set_depth_correction default)
	correction.c[3][2] = 0.5f;
	correction.c[3][3] = 1.0f;
	correction.c[3][0] += jx;
	correction.c[3][1] += jy;
	return mul(correction, cam);
}

} // namespace

TEST(jitter_is_read_back_from_the_jittered_projection) {
	const uint32_t w = 1280, h = 720, phases = 18;
	for (uint32_t frame = 0; frame < 2 * phases; ++frame) {
		const JitterPixels expected = jitter_in_pixels(frame, phases);
		// Godot: taa_jitter = halton / render size (NDC), i.e. pixels * 2 / size.
		const float jx = expected.x * 2.0f / w, jy = expected.y * 2.0f / h;
		for (const bool flip_y : { false, true }) {
			const Mat persp = godot_cam_projection(perspective(1.2f, 16.0f / 9.0f, 0.05f, 4000.0f), jx, jy, flip_y);
			const JitterPixels got = ndc_to_pixels(jitter_ndc_from_projection(persp.c), w, h);
			CHECK_NEAR(got.x, expected.x, 1e-4);
			CHECK_NEAR(got.y, expected.y, 1e-4);
			const Mat ortho = godot_cam_projection(orthogonal(5.0f, 16.0f / 9.0f, 0.05f, 100.0f), jx, jy, flip_y);
			const JitterPixels got_ortho = ndc_to_pixels(jitter_ndc_from_projection(ortho.c), w, h);
			CHECK_NEAR(got_ortho.x, expected.x, 1e-4);
			CHECK_NEAR(got_ortho.y, expected.y, 1e-4);
		}
	}
}

TEST(jitter_phase_is_found_for_every_frame) {
	for (const uint32_t phases : { 8u, 18u, 23u, 32u, 72u }) {
		for (uint32_t frame = 0; frame < phases; ++frame) {
			CHECK_EQ(find_phase(jitter_in_pixels(frame, phases), phases), static_cast<int>(frame));
		}
	}
	CHECK_EQ(find_phase(JitterPixels{ 0.31f, -0.42f }, 18), -1); // not a Halton point
	CHECK_EQ(find_phase(jitter_in_pixels(3, 18), 0), -1); // no jitter at all
}
