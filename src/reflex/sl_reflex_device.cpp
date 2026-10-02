#include "reflex/sl_reflex_device.h"

#include <chrono>

namespace lu::reflex {

bool to_pcl_marker(Marker marker, sl::PCLMarker &out) {
	switch (marker) {
		case Marker::SimulationStart:
			out = sl::PCLMarker::eSimulationStart;
			return true;
		case Marker::SimulationEnd:
			out = sl::PCLMarker::eSimulationEnd;
			return true;
		case Marker::RenderSubmitStart:
			out = sl::PCLMarker::eRenderSubmitStart;
			return true;
		case Marker::RenderSubmitEnd:
			out = sl::PCLMarker::eRenderSubmitEnd;
			return true;
		case Marker::PresentStart:
			out = sl::PCLMarker::ePresentStart;
			return true;
		case Marker::PresentEnd:
			out = sl::PCLMarker::ePresentEnd;
			return true;
		case Marker::TriggerFlash:
			out = sl::PCLMarker::eTriggerFlash;
			return true;
		case Marker::InputSample:
			return false; // deprecated in PCL: dropped
	}
	return false;
}

sl::ReflexMode to_sl_reflex_mode(ReflexMode mode) {
	switch (mode) {
		case ReflexMode::Off:
			return sl::ReflexMode::eOff;
		case ReflexMode::On:
			return sl::ReflexMode::eLowLatency;
		case ReflexMode::OnBoost:
			return sl::ReflexMode::eLowLatencyWithBoost;
	}
	return sl::ReflexMode::eOff;
}

SlReflexDevice::SlReflexDevice(const SlReflexApi &api, const SleepMode &mode) :
		api_(api), mode_(mode) {
	std::lock_guard<std::mutex> lock(mutex_);
	apply_options_locked();
}

sl::FrameToken *SlReflexDevice::token_for_locked(uint64_t frame_id) {
	if (api_.get_new_frame_token == nullptr) {
		return nullptr;
	}
	sl::FrameToken *token = nullptr;
	// The Streamline frame index is the sequencer's per-frame ID; the same ID always maps to the
	// same token, so this frame's sleep and markers share one token.
	const uint32_t index = static_cast<uint32_t>(frame_id);
	if (api_.get_new_frame_token(token, &index) != sl::Result::eOk) {
		return nullptr;
	}
	return token;
}

void SlReflexDevice::apply_options_locked() {
	if (api_.set_options == nullptr) {
		options_error_ = "the sl.reflex set-options function is missing";
		options_applied_ = false;
		return;
	}
	sl::ReflexOptions options{};
	options.mode = to_sl_reflex_mode(mode_.mode);
	options.frameLimitUs = mode_.minimum_interval_us; // Reflex's own FPS cap (driver-aware)
	const sl::Result r = api_.set_options(options);
	options_applied_ = r == sl::Result::eOk;
	options_error_ = options_applied_ ? std::string() : std::string("slReflexSetOptions failed");
}

void SlReflexDevice::set_sleep_mode(const SleepMode &mode) {
	std::lock_guard<std::mutex> lock(mutex_);
	mode_ = mode;
	apply_options_locked();
}

bool SlReflexDevice::sleep() {
	PFun_slReflexSleep *sleep_fn = nullptr;
	sl::FrameToken *token = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		sleep_fn = api_.sleep;
		// The sleep belongs to the frame begin_simulation() is about to open, so it uses that
		// frame's token — the same token its markers will use.
		token = token_for_locked(sequencer_.peek_next_id());
		if (sleep_fn == nullptr || token == nullptr) {
			counters_.sleeps_skipped++;
			return false;
		}
	}
	// Outside the lock: slReflexSleep can block for a frame and must not stall the render thread's
	// marker calls (slReflexSleep and slPCLSetMarker are both thread-safe in Streamline).
	const auto start = std::chrono::steady_clock::now();
	const sl::Result r = sleep_fn(*token);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	std::lock_guard<std::mutex> lock(mutex_);
	counters_.sleeps++;
	counters_.last_sleep_ms = ms;
	counters_.average_sleep_ms = counters_.sleeps == 1 ? ms : counters_.average_sleep_ms * 0.95 + ms * 0.05;
	return r == sl::Result::eOk;
}

void SlReflexDevice::emit_marker(uint64_t frame_id, Marker marker) {
	sl::PCLMarker pcl;
	if (!to_pcl_marker(marker, pcl)) {
		return; // INPUT_SAMPLE: no PCL equivalent
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (api_.set_marker == nullptr) {
		return;
	}
	sl::FrameToken *token = token_for_locked(frame_id);
	if (token == nullptr) {
		return;
	}
	api_.set_marker(pcl, *token);
	counters_.markers[static_cast<size_t>(marker)]++;
}

std::vector<FrameReport> SlReflexDevice::latency_reports() {
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<FrameReport> out;
	if (api_.get_state == nullptr) {
		return out;
	}
	sl::ReflexState rs{};
	if (api_.get_state(rs) != sl::Result::eOk) {
		return out;
	}
	out.reserve(sl::kReflexFrameReportCount);
	for (int i = 0; i < sl::kReflexFrameReportCount; ++i) {
		const sl::ReflexReport &r = rs.frameReport[i];
		if (r.frameID == 0 && r.simStartTime == 0 && r.gpuRenderEndTime == 0) {
			continue; // empty slot
		}
		FrameReport f;
		f.present_id = r.frameID;
		f.input_sample_us = r.inputSampleTime;
		f.sim_start_us = r.simStartTime;
		f.sim_end_us = r.simEndTime;
		f.render_submit_start_us = r.renderSubmitStartTime;
		f.render_submit_end_us = r.renderSubmitEndTime;
		f.present_start_us = r.presentStartTime;
		f.present_end_us = r.presentEndTime;
		f.driver_start_us = r.driverStartTime;
		f.driver_end_us = r.driverEndTime;
		f.os_render_queue_start_us = r.osRenderQueueStartTime;
		f.os_render_queue_end_us = r.osRenderQueueEndTime;
		f.gpu_render_start_us = r.gpuRenderStartTime;
		f.gpu_render_end_us = r.gpuRenderEndTime;
		out.push_back(f);
	}
	return out;
}

DeviceState SlReflexDevice::state() {
	std::lock_guard<std::mutex> lock(mutex_);
	DeviceState s;
	s.has_swapchain = true; // Streamline drives the present path when Frame Generation is on
	s.latency_mode_enabled = mode_.mode != ReflexMode::Off;
	s.sleep_enabled = true;
	s.sleep_mode_error = options_error_;
	s.counters = counters_;
	return s;
}

} // namespace lu::reflex
