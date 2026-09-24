#include "reflex/latency_stats.h"
#include "test_framework.h"

using namespace lu::reflex;

namespace {

// A frame starting at `offset` (us, after an arbitrary epoch): 1 ms simulation, 2 ms render submission, 0.5 ms present,
// GPU 4 ms starting 1 ms after submission ended; input sampled 0.2 ms after the start.
FrameReport frame(uint64_t id, uint64_t offset) {
	const uint64_t t = 5000000 + offset; // driver timestamps are never 0
	FrameReport r;
	r.present_id = id;
	r.sim_start_us = t;
	r.input_sample_us = t + 200;
	r.sim_end_us = t + 1000;
	r.render_submit_start_us = t + 1000;
	r.render_submit_end_us = t + 3000;
	r.present_start_us = t + 3000;
	r.present_end_us = t + 3500;
	r.driver_start_us = t + 1100;
	r.driver_end_us = t + 3400;
	r.os_render_queue_start_us = t + 3100;
	r.os_render_queue_end_us = t + 8000;
	r.gpu_render_start_us = t + 4000;
	r.gpu_render_end_us = t + 8000;
	return r;
}

} // namespace

TEST(latency_stats_average_complete_frames) {
	// Deliberately out of order: the driver returns its ring buffer as is.
	const std::vector<FrameReport> reports = { frame(3, 20000), frame(1, 0), frame(2, 10000) };
	const LatencyStats s = compute_latency_stats(reports);
	CHECK_EQ(s.frames, 3);
	CHECK_EQ(s.latest_present_id, 3ull);
	CHECK_NEAR(s.sim_start_to_gpu_end_ms, 8.0, 1e-9);
	CHECK_NEAR(s.input_sample_to_gpu_end_ms, 7.8, 1e-9);
	CHECK_NEAR(s.render_submit_start_to_gpu_end_ms, 7.0, 1e-9);
	CHECK_NEAR(s.simulation_ms, 1.0, 1e-9);
	CHECK_NEAR(s.render_submit_ms, 2.0, 1e-9);
	CHECK_NEAR(s.present_ms, 0.5, 1e-9);
	CHECK_NEAR(s.driver_ms, 2.3, 1e-9);
	CHECK_NEAR(s.os_render_queue_ms, 4.9, 1e-9);
	CHECK_NEAR(s.gpu_render_ms, 4.0, 1e-9);
	CHECK_NEAR(s.frame_interval_ms, 10.0, 1e-9);
	CHECK_NEAR(s.gpu_frame_interval_ms, 10.0, 1e-9);
}

TEST(latency_stats_skip_frames_still_in_flight) {
	FrameReport pending = frame(4, 30000);
	pending.gpu_render_end_us = 0; // GPU not done yet
	FrameReport empty; // unused slot of the driver's ring buffer
	const LatencyStats s = compute_latency_stats({ frame(1, 0), pending, empty });
	CHECK_EQ(s.frames, 1);
	CHECK_EQ(s.latest_present_id, 1ull);
	CHECK_NEAR(s.frame_interval_ms, 0.0, 1e-12); // needs two frames
}

TEST(latency_stats_ignore_missing_optional_timestamps) {
	FrameReport no_input = frame(1, 0);
	no_input.input_sample_us = 0;
	const LatencyStats s = compute_latency_stats({ no_input, frame(2, 10000) });
	CHECK_EQ(s.frames, 2);
	CHECK_NEAR(s.input_sample_to_gpu_end_ms, 7.8, 1e-9); // averaged over the one frame that has it
}

TEST(latency_stats_reject_reversed_timestamps) {
	FrameReport reversed = frame(1, 10000);
	reversed.gpu_render_end_us = reversed.sim_start_us - 1; // before the simulation start: corrupt
	const LatencyStats s = compute_latency_stats({ reversed });
	CHECK_EQ(s.frames, 0);
	CHECK_NEAR(s.sim_start_to_gpu_end_ms, 0.0, 1e-12);
}

TEST(latency_stats_empty_input) {
	const LatencyStats s = compute_latency_stats({});
	CHECK_EQ(s.frames, 0);
	CHECK_EQ(s.latest_present_id, 0ull);
}
