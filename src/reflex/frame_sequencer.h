// Turns Godot's frame-loop events into a well-formed stream of Reflex latency markers.
//
// VK_NV_low_latency2 identifies a frame by a present ID and expects its markers in the order
// SIMULATION_START < SIMULATION_END < RENDERSUBMIT_START < RENDERSUBMIT_END < PRESENT_START <
// PRESENT_END, with the same ID stamped on that frame's vkQueuePresentKHR (VkPresentIdKHR)
// and submissions (VkLatencySubmissionPresentIdNV). Godot's loop does not always produce
// every event (a frame can skip drawing, a draw can skip presenting, a render thread can run a
// frame behind the main thread), so this class owns the rules:
//   - an ID is opened by begin_simulation() and is strictly increasing;
//   - a simulation that never reached a draw is closed before the next one opens;
//   - RENDERSUBMIT_END is sent once per rendered frame: at present if there is one, else when
//     the draw finishes;
//   - a present only gets an ID (and markers) when that ID is newer than the last presented
//     one, because present IDs must increase per swapchain.
//
// Pure C++ and thread-safe (main thread and render thread may call it concurrently).
#pragma once

#include <array>
#include <cstdint>
#include <mutex>

namespace nvdlss::reflex {

// Values match VkLatencyMarkerNV (VK_LATENCY_MARKER_*_NV).
enum class Marker : uint32_t {
	SimulationStart = 0,
	SimulationEnd = 1,
	RenderSubmitStart = 2,
	RenderSubmitEnd = 3,
	PresentStart = 4,
	PresentEnd = 5,
	InputSample = 6,
	TriggerFlash = 7,
};
inline constexpr size_t MARKER_COUNT = 8;
const char *marker_name(Marker marker);

class MarkerSink {
public:
	virtual ~MarkerSink() = default;
	virtual void emit_marker(uint64_t frame_id, Marker marker) = 0;
};

class FrameSequencer {
public:
	explicit FrameSequencer(MarkerSink &sink) :
			sink_(sink) {}

	// --- Main thread ---------------------------------------------------------------------
	// Closes a simulation that never reached a draw (sends its SIMULATION_END). Called right
	// before the Reflex sleep so that the sleep is not counted as simulation time.
	void close_open_simulation();
	// Opens the next frame after the sleep: SIMULATION_START and INPUT_SAMPLE. Returns its ID.
	uint64_t begin_simulation();
	// frame_pre_draw: [TRIGGER_FLASH] SIMULATION_END for the open frame. Returns the ID the
	// draw belongs to, or 0 when no simulation is open.
	uint64_t end_simulation();
	// A left click was sampled; TRIGGER_FLASH goes out with the next end_simulation().
	void request_flash();

	// --- Render thread (the main thread in Godot's default thread model) -------------------
	// Start of the draw that renders frame `frame_id`: RENDERSUBMIT_START.
	void render_begin(uint64_t frame_id);
	// ID to stamp on the next submission: the frame between its RENDERSUBMIT_START and _END,
	// 0 otherwise.
	uint64_t submission_id() const;
	// Before vkQueuePresentKHR of the tracked swapchain: [RENDERSUBMIT_END] PRESENT_START.
	// Returns the present ID to stamp, or 0 (no markers, no ID) when no frame is being rendered
	// or its ID was already presented.
	uint64_t present_begin();
	// After vkQueuePresentKHR returned: PRESENT_END for the ID present_begin() returned.
	void present_end(uint64_t present_id);
	// End of the draw of `frame_id` (frame_post_draw): RENDERSUBMIT_END if no present sent it.
	void render_end(uint64_t frame_id);

	// A new swapchain accepts any present ID again.
	void reset_present_history();

	uint64_t current_simulation_id() const;
	uint64_t last_presented_id() const;

private:
	void emit(uint64_t id, Marker marker) { sink_.emit_marker(id, marker); }

	MarkerSink &sink_;
	mutable std::mutex mutex_;
	uint64_t next_id_ = 1;
	uint64_t sim_id_ = 0; // open simulation, 0 = none
	bool flash_pending_ = false;
	uint64_t render_id_ = 0; // frame being rendered, 0 = none
	bool render_end_sent_ = true;
	uint64_t last_presented_ = 0;
};

} // namespace nvdlss::reflex
