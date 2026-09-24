// Aggregates VK_NV_low_latency2 frame reports (vkGetLatencyTimingsNV) into averages.
//
// Pure C++, unit-tested.
#pragma once

#include <cstdint>
#include <vector>

namespace nvdlss::reflex {

// One VkLatencyTimingsFrameReportNV. All times are driver timestamps in microseconds.
struct FrameReport {
	uint64_t present_id = 0;
	uint64_t input_sample_us = 0;
	uint64_t sim_start_us = 0;
	uint64_t sim_end_us = 0;
	uint64_t render_submit_start_us = 0;
	uint64_t render_submit_end_us = 0;
	uint64_t present_start_us = 0;
	uint64_t present_end_us = 0;
	uint64_t driver_start_us = 0;
	uint64_t driver_end_us = 0;
	uint64_t os_render_queue_start_us = 0;
	uint64_t os_render_queue_end_us = 0;
	uint64_t gpu_render_start_us = 0;
	uint64_t gpu_render_end_us = 0;
};

// Averages over the complete reports. A report counts only when its simulation start and GPU
// render end are both present and ordered; frames still in flight report zeros.
struct LatencyStats {
	int frames = 0; // complete reports used
	uint64_t latest_present_id = 0;
	// Headline number: simulation start to GPU render end. This is the part of the PC latency
	// that Reflex reduces (it excludes the OS input path before and the display after).
	double sim_start_to_gpu_end_ms = 0.0;
	double input_sample_to_gpu_end_ms = 0.0; // only frames with an INPUT_SAMPLE timestamp
	double render_submit_start_to_gpu_end_ms = 0.0;
	double simulation_ms = 0.0; // SIMULATION_START..END
	double render_submit_ms = 0.0; // RENDERSUBMIT_START..END
	double present_ms = 0.0; // PRESENT_START..END
	double driver_ms = 0.0;
	double os_render_queue_ms = 0.0;
	double gpu_render_ms = 0.0; // GPU busy time of the frame
	double frame_interval_ms = 0.0; // between consecutive simulation starts
	double gpu_frame_interval_ms = 0.0; // between consecutive GPU render ends
};

LatencyStats compute_latency_stats(const std::vector<FrameReport> &reports);

} // namespace nvdlss::reflex
