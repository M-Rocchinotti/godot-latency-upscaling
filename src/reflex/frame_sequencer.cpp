#include "reflex/frame_sequencer.h"

namespace lu::reflex {

const char *marker_name(Marker marker) {
	switch (marker) {
		case Marker::SimulationStart:
			return "SIMULATION_START";
		case Marker::SimulationEnd:
			return "SIMULATION_END";
		case Marker::RenderSubmitStart:
			return "RENDERSUBMIT_START";
		case Marker::RenderSubmitEnd:
			return "RENDERSUBMIT_END";
		case Marker::PresentStart:
			return "PRESENT_START";
		case Marker::PresentEnd:
			return "PRESENT_END";
		case Marker::InputSample:
			return "INPUT_SAMPLE";
		case Marker::TriggerFlash:
			return "TRIGGER_FLASH";
	}
	return "UNKNOWN";
}

void FrameSequencer::close_open_simulation() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (sim_id_ != 0) {
		emit(sim_id_, Marker::SimulationEnd);
		sim_id_ = 0;
		flash_pending_ = false;
	}
}

uint64_t FrameSequencer::begin_simulation() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (sim_id_ != 0) {
		emit(sim_id_, Marker::SimulationEnd);
	}
	sim_id_ = next_id_++;
	flash_pending_ = false;
	emit(sim_id_, Marker::SimulationStart);
	emit(sim_id_, Marker::InputSample);
	return sim_id_;
}

void FrameSequencer::request_flash() {
	std::lock_guard<std::mutex> lock(mutex_);
	flash_pending_ = sim_id_ != 0;
}

uint64_t FrameSequencer::end_simulation() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (sim_id_ == 0) {
		return 0;
	}
	const uint64_t id = sim_id_;
	if (flash_pending_) {
		emit(id, Marker::TriggerFlash);
		flash_pending_ = false;
	}
	emit(id, Marker::SimulationEnd);
	sim_id_ = 0;
	return id;
}

void FrameSequencer::render_begin(uint64_t frame_id) {
	if (frame_id == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (render_id_ != 0 && !render_end_sent_) {
		// The previous draw never finished through render_end() (e.g. its frame_post_draw was
		// deferred past this point in the multi-threaded model): close it first.
		emit(render_id_, Marker::RenderSubmitEnd);
	}
	render_id_ = frame_id;
	render_end_sent_ = false;
	emit(frame_id, Marker::RenderSubmitStart);
}

uint64_t FrameSequencer::submission_id() const {
	std::lock_guard<std::mutex> lock(mutex_);
	// Once RENDERSUBMIT_END is out the frame's submissions are done; later ones (uploads from
	// worker threads, script submissions) belong to no frame.
	return render_end_sent_ ? 0 : render_id_;
}

uint64_t FrameSequencer::present_begin() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (render_id_ == 0 || render_id_ <= last_presented_) {
		return 0;
	}
	if (!render_end_sent_) {
		emit(render_id_, Marker::RenderSubmitEnd);
		render_end_sent_ = true;
	}
	emit(render_id_, Marker::PresentStart);
	last_presented_ = render_id_;
	return render_id_;
}

void FrameSequencer::present_end(uint64_t present_id) {
	if (present_id == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	emit(present_id, Marker::PresentEnd);
}

void FrameSequencer::render_end(uint64_t frame_id) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (frame_id != 0 && frame_id == render_id_ && !render_end_sent_) {
		emit(frame_id, Marker::RenderSubmitEnd);
		render_end_sent_ = true;
	}
}

void FrameSequencer::reset_present_history() {
	std::lock_guard<std::mutex> lock(mutex_);
	last_presented_ = 0;
}

uint64_t FrameSequencer::current_simulation_id() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return sim_id_;
}

uint64_t FrameSequencer::last_presented_id() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return last_presented_;
}

uint64_t FrameSequencer::peek_next_id() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return next_id_;
}

} // namespace lu::reflex
