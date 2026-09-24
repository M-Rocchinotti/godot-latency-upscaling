#include "reflex/latency_stats.h"

#include <algorithm>

namespace lu::reflex {
namespace {

bool is_complete(const FrameReport &r) {
	return r.present_id != 0 && r.sim_start_us != 0 && r.gpu_render_end_us != 0 && r.gpu_render_end_us >= r.sim_start_us;
}

// Running mean of end - start over the pairs where both timestamps exist and are ordered.
struct Mean {
	double sum_us = 0.0;
	int count = 0;
	void add(uint64_t start, uint64_t end) {
		if (start != 0 && end != 0 && end >= start) {
			sum_us += static_cast<double>(end - start);
			++count;
		}
	}
	double ms() const { return count > 0 ? sum_us / count / 1000.0 : 0.0; }
};

} // namespace

LatencyStats compute_latency_stats(const std::vector<FrameReport> &reports) {
	std::vector<FrameReport> frames;
	frames.reserve(reports.size());
	for (const FrameReport &r : reports) {
		if (is_complete(r)) {
			frames.push_back(r);
		}
	}
	LatencyStats stats;
	if (frames.empty()) {
		return stats;
	}
	std::sort(frames.begin(), frames.end(),
			[](const FrameReport &a, const FrameReport &b) { return a.present_id < b.present_id; });

	Mean total, input, render_to_gpu, sim, submit, present, driver, queue, gpu, interval, gpu_interval;
	for (size_t i = 0; i < frames.size(); ++i) {
		const FrameReport &f = frames[i];
		total.add(f.sim_start_us, f.gpu_render_end_us);
		input.add(f.input_sample_us, f.gpu_render_end_us);
		render_to_gpu.add(f.render_submit_start_us, f.gpu_render_end_us);
		sim.add(f.sim_start_us, f.sim_end_us);
		submit.add(f.render_submit_start_us, f.render_submit_end_us);
		present.add(f.present_start_us, f.present_end_us);
		driver.add(f.driver_start_us, f.driver_end_us);
		queue.add(f.os_render_queue_start_us, f.os_render_queue_end_us);
		gpu.add(f.gpu_render_start_us, f.gpu_render_end_us);
		if (i > 0) {
			interval.add(frames[i - 1].sim_start_us, f.sim_start_us);
			gpu_interval.add(frames[i - 1].gpu_render_end_us, f.gpu_render_end_us);
		}
	}
	stats.frames = static_cast<int>(frames.size());
	stats.latest_present_id = frames.back().present_id;
	stats.sim_start_to_gpu_end_ms = total.ms();
	stats.input_sample_to_gpu_end_ms = input.ms();
	stats.render_submit_start_to_gpu_end_ms = render_to_gpu.ms();
	stats.simulation_ms = sim.ms();
	stats.render_submit_ms = submit.ms();
	stats.present_ms = present.ms();
	stats.driver_ms = driver.ms();
	stats.os_render_queue_ms = queue.ms();
	stats.gpu_render_ms = gpu.ms();
	stats.frame_interval_ms = interval.ms();
	stats.gpu_frame_interval_ms = gpu_interval.ms();
	return stats;
}

} // namespace lu::reflex
