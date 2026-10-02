// The Reflex contract the Godot singleton drives, independent of how Reflex is implemented.
//
// Two implementations:
//   - Ll2Device    — NVIDIA Reflex through the Khronos extension VK_NV_low_latency2 (no NVIDIA DLLs);
//                    the default, used whenever Frame Generation is off.
//   - SlReflexDevice — NVIDIA Reflex through Streamline's sl.reflex / sl.pcl plugins, required when
//                    DLSS Frame Generation is on (Phase 3); the LL2 path is then inert.
//
// The singleton (godot/latency_upscaling.cpp) only ever calls the methods below, so it does not care
// which backend drives Reflex. Pure C++: the shared types are unit-tested.
#pragma once

#include "core/settings.h" // ReflexMode
#include "reflex/frame_sequencer.h" // Marker, MARKER_COUNT, FrameSequencer, MarkerSink
#include "reflex/latency_stats.h" // FrameReport

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lu::reflex {

struct SleepMode {
	ReflexMode mode = ReflexMode::On;
	uint32_t minimum_interval_us = 0;
};

struct Counters {
	std::array<uint64_t, MARKER_COUNT> markers{};
	uint64_t sleeps = 0;
	uint64_t sleeps_skipped = 0; // no present since the previous sleep (minimised window, no draw)
	uint64_t sleep_timeouts = 0;
	uint64_t presents_stamped = 0; // LL2 only
	uint64_t submits_tagged = 0; // LL2 only
	uint64_t swapchains_with_latency_mode = 0; // LL2 only
	uint64_t other_swapchains = 0; // LL2 only: swapchains of other windows, passed through
	// LL2 only: the present hook's wait for the swapchain lock.
	uint64_t present_lock_waits_over_1ms = 0;
	uint64_t present_lock_wait_max_us = 0;
	uint64_t latency_sleep_call_max_us = 0; // how long the sleep call itself blocked, at most
	double last_sleep_ms = 0.0;
	double average_sleep_ms = 0.0; // exponential moving average
};

// A snapshot of a Reflex device for the status API and the latency statistics. The LL2-specific
// fields (swapchain, latency mode) are left at their defaults by the Streamline backend.
struct DeviceState {
	bool has_swapchain = false;
	bool latency_mode_enabled = false;
	bool sleep_enabled = true;
	std::string sleep_disabled_reason;
	std::string swapchain_note;
	std::string sleep_mode_error;
	Counters counters;
};

class ReflexDevice {
public:
	virtual ~ReflexDevice() = default;

	// Godot side (frame-loop placement in godot/latency_upscaling.cpp).
	virtual FrameSequencer &sequencer() = 0;
	virtual bool sleep() = 0; // once per frame, on the main thread; false when skipped / disabled
	virtual void set_sleep_mode(const SleepMode &mode) = 0;
	virtual std::vector<FrameReport> latency_reports() = 0;
	virtual DeviceState state() = 0;
};

} // namespace lu::reflex
