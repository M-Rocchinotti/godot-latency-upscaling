#include "dlss/jitter.h"
#include "test_framework.h"
#include "upscale/quality.h"

#include <cstring>

using namespace lu::upscale;

TEST(quality_auto_follows_nvidias_output_resolution_table) {
	Quality mode = Quality::DLAA;
	CHECK(!resolve_auto(Size{ 1280, 720 }, mode)); // below 1920x1080: DLSS off
	CHECK(mode == Quality::DLAA); // untouched
	CHECK(resolve_auto(Size{ 1920, 1080 }, mode));
	CHECK(mode == Quality::Quality);
	CHECK(resolve_auto(Size{ 2560, 1440 }, mode));
	CHECK(mode == Quality::Quality);
	CHECK(resolve_auto(Size{ 3440, 1440 }, mode));
	CHECK(mode == Quality::Performance);
	CHECK(resolve_auto(Size{ 3840, 2160 }, mode));
	CHECK(mode == Quality::Performance);
	CHECK(resolve_auto(Size{ 5120, 2880 }, mode));
	CHECK(mode == Quality::UltraPerformance);
}

TEST(quality_render_sizes_use_the_dlss_scaling_ratios) {
	const Size uhd{ 3840, 2160 };
	CHECK_EQ(render_size(uhd, Quality::DLAA).width, 3840u);
	CHECK_EQ(render_size(uhd, Quality::Quality).width, 2560u);
	CHECK_EQ(render_size(uhd, Quality::Quality).height, 1440u);
	CHECK_EQ(render_size(uhd, Quality::Balanced).width, 2227u); // 3840 / 1.724
	CHECK_EQ(render_size(uhd, Quality::Performance).width, 1920u);
	CHECK_EQ(render_size(uhd, Quality::UltraPerformance).width, 1280u);
	CHECK_EQ(render_size(uhd, Quality::UltraPerformance).height, 720u);
	CHECK_NEAR(render_scale(Quality::Quality), 2.0 / 3.0, 1e-6);
	CHECK_NEAR(render_scale(Quality::Balanced), 1.0 / 1.724, 1e-6);
	CHECK_EQ(render_size(Size{ 1, 1 }, Quality::UltraPerformance).width, 1u); // never 0
}

TEST(quality_godot_jitter_phases_match_dlss_guidance) {
	// DLSS guide 3.7.1: phases = 8 * ratio^2 (UP 72, P 32, Q 18, DLAA 8; Balanced 23.8 -> guide
	// rounds up to 24, Godot's uint32 truncation gives 23).
	using lu::dlss::jitter_phase_count;
	CHECK_EQ(jitter_phase_count(3840, 1280, true, false), 72u);
	CHECK_EQ(jitter_phase_count(3840, 1920, true, false), 32u);
	CHECK_EQ(jitter_phase_count(3840, 2560, true, false), 18u);
	CHECK_EQ(jitter_phase_count(3840, 3840, true, false), 8u);
	CHECK_EQ(jitter_phase_count(3840, 2227, true, false), 23u);
}

TEST(quality_and_preset_names) {
	CHECK(std::strcmp(quality_name(Quality::Auto), "Auto") == 0);
	CHECK(std::strcmp(quality_name(Quality::UltraPerformance), "Ultra Performance") == 0);
	CHECK(std::strcmp(preset_name(Preset::Default), "Default") == 0);
	CHECK(std::strcmp(preset_name(Preset::M), "M") == 0);
}
