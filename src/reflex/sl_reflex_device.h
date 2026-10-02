// NVIDIA Reflex through Streamline's sl.reflex / sl.pcl plugins, the sibling of Ll2Device used when
// DLSS Frame Generation is on (Phase 3). Frame Generation requires Reflex to run through Streamline
// (so its frame scheduler and Reflex agree on pacing), so the VK_NV_low_latency2 path is kept out of
// the device and this drives Reflex instead.
//
// It implements the same ReflexDevice contract and reuses reflex::FrameSequencer for the marker
// ordering (the sequencer is marker-API-agnostic): each marker becomes an slPCLSetMarker, the
// per-frame sleep an slReflexSleep, the options an slReflexSetOptions, and the statistics come from
// slReflexGetState. The coupling is the Streamline frame token: one token per frame index threads
// through that frame's sleep and markers (a mismatch makes Streamline report "common constants
// cannot be found for frame N").
//
// The Streamline functions are injected as a struct of pointers (SlReflexApi), resolved from the
// loaded plugins by dlss::Streamline. Unit-tested with a fake SlReflexApi (no NVIDIA runtime).
#pragma once

#include "reflex/reflex_device.h"

#include <sl.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace lu::reflex {

// One Streamline frame token per frame, shared by every subsystem that touches a frame: the
// sl.reflex markers / slReflexSleep, the Super Resolution slSetConstants / slEvaluateFeature, and
// the Frame Generation present. Streamline's commonInterface keys a frame's constants on this token,
// so if different subsystems minted their own, the FG present would look the constants up under the
// wrong id ("Unable to find 'common' constants for frame N") and disable per-frame. advance() at
// frame start mints the next monotonic token; current() returns it for the rest of that frame.
//
// dlss::Streamline owns one instance (when the Frame Generation plugins are loaded) and threads it
// into evaluate() and into SlReflexDevice via SlReflexApi. Thread-safe; targets Godot's default
// thread model (render on the main thread), where a frame's calls are sequential.
class FrameTokenSource {
public:
	explicit FrameTokenSource(PFun_slGetNewFrameToken *mint) :
			mint_(mint) {}

	sl::FrameToken *advance(); // frame start: mint the next frame's token and make it current
	sl::FrameToken *current(); // this frame's token (mints the first lazily)

private:
	sl::FrameToken *advance_locked(); // mutex_ held
	std::mutex mutex_;
	PFun_slGetNewFrameToken *mint_ = nullptr;
	uint32_t next_index_ = 0;
	sl::FrameToken *current_ = nullptr;
};

// The Streamline functions SlReflexDevice needs: the shared per-frame token source plus the
// sl.reflex and sl.pcl plugin functions. dlss::Streamline fills these once the plugins are resolved
// on the device; unit tests pass fakes.
struct SlReflexApi {
	FrameTokenSource *token_source = nullptr; // the one token per frame (shared with SR + FG)
	PFun_slReflexSetOptions *set_options = nullptr;
	PFun_slReflexSleep *sleep = nullptr;
	PFun_slReflexGetState *get_state = nullptr;
	PFun_slPCLSetMarker *set_marker = nullptr;

	bool complete() const { return token_source && set_options && sleep && get_state && set_marker; }
};

// Maps a reflex::Marker to Streamline's PCLMarker. Returns false for markers PCL has no equivalent
// for (INPUT_SAMPLE is deprecated in PCL and is dropped).
bool to_pcl_marker(Marker marker, sl::PCLMarker &out);
// Maps the Reflex mode to Streamline's ReflexMode.
sl::ReflexMode to_sl_reflex_mode(ReflexMode mode);

class SlReflexDevice final : public MarkerSink, public ReflexDevice {
public:
	// `mode` is applied immediately (slReflexSetOptions).
	SlReflexDevice(const SlReflexApi &api, const SleepMode &mode);

	// --- ReflexDevice ---
	FrameSequencer &sequencer() override { return sequencer_; }
	bool sleep() override; // slReflexSleep for the upcoming frame's token
	void set_sleep_mode(const SleepMode &mode) override;
	std::vector<FrameReport> latency_reports() override; // from slReflexGetState's 64-frame report
	DeviceState state() override;

	// --- MarkerSink ---
	void emit_marker(uint64_t frame_id, Marker marker) override; // slPCLSetMarker with the frame's shared token

private:
	void apply_options_locked();

	SlReflexApi api_;
	std::mutex mutex_;
	SleepMode mode_;
	bool options_applied_ = false;
	std::string options_error_;
	Counters counters_;
	FrameSequencer sequencer_{ *this };
};

} // namespace lu::reflex
