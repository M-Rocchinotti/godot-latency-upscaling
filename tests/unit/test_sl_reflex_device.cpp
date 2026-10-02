// SlReflexDevice against a fake Streamline Reflex/PCL API (the way test_reflex_ll2.cpp drives a
// fake LL2 driver): mode mapping, one sleep per frame, the six latency markers in order, the
// per-frame Streamline frame token threaded through a frame's sleep and markers, and the latency
// statistics mapped from slReflexGetState. No NVIDIA runtime is involved.
#include "reflex/sl_reflex_device.h"
#include "test_framework.h"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

using namespace lu;
using namespace lu::reflex;

namespace {

// A concrete sl::FrameToken whose frame index the fake encodes as its value, so a test can see which
// token each Streamline call used.
struct FakeFrameToken : sl::FrameToken {
	uint32_t value = 0;
	operator uint32_t() const override { return value; }
};

std::map<uint32_t, FakeFrameToken> g_tokens; // one stable token per frame index
std::vector<std::pair<sl::PCLMarker, uint32_t>> g_markers; // (marker, token value), in call order
std::vector<uint32_t> g_sleeps; // token value per slReflexSleep call
struct OptionsCall {
	sl::ReflexMode mode;
	uint32_t frame_limit_us;
};
std::vector<OptionsCall> g_options;
sl::ReflexState g_state_to_return;
sl::Result g_get_state_result = sl::Result::eOk;

void reset_fake() {
	g_tokens.clear();
	g_markers.clear();
	g_sleeps.clear();
	g_options.clear();
	g_state_to_return = sl::ReflexState{};
	g_get_state_result = sl::Result::eOk;
}

sl::Result fake_get_new_frame_token(sl::FrameToken *&token, const uint32_t *index) {
	const uint32_t i = index != nullptr ? *index : 0;
	FakeFrameToken &t = g_tokens[i];
	t.value = i;
	token = &t;
	return sl::Result::eOk;
}
sl::Result fake_set_options(const sl::ReflexOptions &options) {
	g_options.push_back({ options.mode, options.frameLimitUs });
	return sl::Result::eOk;
}
sl::Result fake_sleep(const sl::FrameToken &frame) {
	g_sleeps.push_back(static_cast<uint32_t>(frame));
	return sl::Result::eOk;
}
sl::Result fake_get_state(sl::ReflexState &state) {
	state = g_state_to_return;
	return g_get_state_result;
}
sl::Result fake_set_marker(sl::PCLMarker marker, const sl::FrameToken &frame) {
	g_markers.push_back({ marker, static_cast<uint32_t>(frame) });
	return sl::Result::eOk;
}

SlReflexApi fake_api(FrameTokenSource &source) {
	SlReflexApi api;
	api.token_source = &source; // the one shared per-frame token
	api.set_options = &fake_set_options;
	api.sleep = &fake_sleep;
	api.get_state = &fake_get_state;
	api.set_marker = &fake_set_marker;
	return api;
}

} // namespace

TEST(fg_frame_token_source_mints_one_token_per_frame) {
	reset_fake();
	FrameTokenSource source(&fake_get_new_frame_token);
	// current() mints the first lazily; repeated current() returns the same token (shared within a
	// frame by the markers, the SR evaluate and the FG present).
	sl::FrameToken *first = source.current();
	CHECK(first != nullptr && static_cast<uint32_t>(*first) == 0u);
	CHECK(source.current() == first);
	// advance() moves to the next frame's token.
	sl::FrameToken *second = source.advance();
	CHECK(second != nullptr && static_cast<uint32_t>(*second) == 1u && second != first);
	CHECK(source.current() == second);
}

TEST(sl_reflex_mode_maps_to_streamline_and_carries_the_fps_cap) {
	reset_fake();
	FrameTokenSource source(&fake_get_new_frame_token);
	SlReflexDevice device(fake_api(source), SleepMode{ ReflexMode::On, 0 }); // ctor applies the options once
	CHECK_EQ(g_options.size(), size_t(1));
	CHECK(g_options[0].mode == sl::ReflexMode::eLowLatency);
	CHECK_EQ(g_options[0].frame_limit_us, 0u);
	device.set_sleep_mode(SleepMode{ ReflexMode::OnBoost, 16667 });
	CHECK_EQ(g_options.size(), size_t(2));
	CHECK(g_options[1].mode == sl::ReflexMode::eLowLatencyWithBoost);
	CHECK_EQ(g_options[1].frame_limit_us, 16667u); // Reflex's own FPS cap
	device.set_sleep_mode(SleepMode{ ReflexMode::Off, 0 });
	CHECK(g_options.back().mode == sl::ReflexMode::eOff);
	// The free helpers directly.
	CHECK(to_sl_reflex_mode(ReflexMode::Off) == sl::ReflexMode::eOff);
	CHECK(to_sl_reflex_mode(ReflexMode::On) == sl::ReflexMode::eLowLatency);
	CHECK(to_sl_reflex_mode(ReflexMode::OnBoost) == sl::ReflexMode::eLowLatencyWithBoost);
}

TEST(sl_reflex_sleeps_once_per_frame_and_advances_the_shared_token) {
	reset_fake();
	FrameTokenSource source(&fake_get_new_frame_token);
	SlReflexDevice device(fake_api(source), SleepMode{ ReflexMode::On, 0 });
	// The sleep mints this frame's token (index 0) and is exactly one slReflexSleep call.
	CHECK(device.sleep());
	CHECK_EQ(g_sleeps.size(), size_t(1));
	CHECK_EQ(g_sleeps[0], 0u);
	CHECK_EQ(static_cast<uint32_t>(*source.current()), 0u); // the SR evaluate / FG would use this same token
	// The next sleep advances to the next frame's token (index 1).
	CHECK(device.sleep());
	CHECK_EQ(g_sleeps.size(), size_t(2)); // exactly one sleep per call
	CHECK_EQ(g_sleeps[1], 1u);
	CHECK_EQ(static_cast<uint32_t>(*source.current()), 1u);
	CHECK_EQ(device.state().counters.sleeps, uint64_t(2));
}

TEST(sl_reflex_emits_the_six_markers_in_order_and_drops_input_sample) {
	reset_fake();
	FrameTokenSource source(&fake_get_new_frame_token);
	SlReflexDevice device(fake_api(source), SleepMode{ ReflexMode::On, 0 });
	FrameSequencer &seq = device.sequencer();
	// One full frame through the shared sequencer.
	const uint64_t id = seq.begin_simulation(); // SIMULATION_START, INPUT_SAMPLE
	const uint64_t drew = seq.end_simulation(); // SIMULATION_END
	seq.render_begin(drew); // RENDERSUBMIT_START
	const uint64_t pid = seq.present_begin(); // [RENDERSUBMIT_END] PRESENT_START
	seq.present_end(pid); // PRESENT_END
	CHECK_EQ(id, uint64_t(1));
	// INPUT_SAMPLE has no PCL equivalent and is dropped, so six markers reach Streamline, in order.
	const sl::PCLMarker expected[] = { sl::PCLMarker::eSimulationStart, sl::PCLMarker::eSimulationEnd,
		sl::PCLMarker::eRenderSubmitStart, sl::PCLMarker::eRenderSubmitEnd, sl::PCLMarker::ePresentStart,
		sl::PCLMarker::ePresentEnd };
	CHECK_EQ(g_markers.size(), size_t(6));
	// Every marker carries the one frame token current() returns — the same one the SR evaluate and
	// the FG present use. (No sleep ran, so current() minted the first token, index 0.)
	const uint32_t frame_token = static_cast<uint32_t>(*source.current());
	CHECK_EQ(frame_token, 0u);
	if (g_markers.size() == 6) {
		for (size_t i = 0; i < 6; ++i) {
			CHECK(g_markers[i].first == expected[i]);
			CHECK_EQ(g_markers[i].second, frame_token);
		}
	}
	const DeviceState st = device.state();
	CHECK_EQ(st.counters.markers[static_cast<size_t>(Marker::SimulationStart)], uint64_t(1));
	CHECK_EQ(st.counters.markers[static_cast<size_t>(Marker::PresentEnd)], uint64_t(1));
	CHECK_EQ(st.counters.markers[static_cast<size_t>(Marker::InputSample)], uint64_t(0)); // dropped, not counted
}

TEST(sl_reflex_one_token_per_frame_shared_by_markers_sleep_and_the_rest) {
	reset_fake();
	FrameTokenSource source(&fake_get_new_frame_token);
	SlReflexDevice device(fake_api(source), SleepMode{ ReflexMode::On, 0 });
	FrameSequencer &seq = device.sequencer();
	// Two frames: each sleep advances the shared token (frame start); the frame's markers and what the
	// SR evaluate / FG would use (source.current()) all carry that one token.
	for (uint32_t frame = 0; frame <= 1; ++frame) {
		device.sleep(); // advance -> this frame's token (index == frame)
		seq.begin_simulation(); // SIMULATION_START, INPUT_SAMPLE
		seq.end_simulation(); // SIMULATION_END
		CHECK_EQ(g_sleeps.back(), frame); // the sleep used this frame's token
		CHECK_EQ(static_cast<uint32_t>(*source.current()), frame); // the SR evaluate / FG see the same
	}
	CHECK_EQ(g_sleeps.size(), size_t(2));
	// Markers: SIMULATION_START + SIMULATION_END per frame, token 0 for frame 0, token 1 for frame 1.
	CHECK_EQ(g_markers.size(), size_t(4));
	if (g_markers.size() == 4) {
		CHECK_EQ(g_markers[0].second, 0u);
		CHECK_EQ(g_markers[1].second, 0u);
		CHECK_EQ(g_markers[2].second, 1u);
		CHECK_EQ(g_markers[3].second, 1u);
	}
}

TEST(sl_reflex_maps_statistics_from_streamline) {
	reset_fake();
	sl::ReflexReport &r = g_state_to_return.frameReport[0];
	r.frameID = 7;
	r.inputSampleTime = 900;
	r.simStartTime = 1000;
	r.simEndTime = 1100;
	r.renderSubmitStartTime = 1200;
	r.renderSubmitEndTime = 1300;
	r.presentStartTime = 1400;
	r.presentEndTime = 1500;
	r.driverStartTime = 1600;
	r.driverEndTime = 1700;
	r.osRenderQueueStartTime = 1800;
	r.osRenderQueueEndTime = 1900;
	r.gpuRenderStartTime = 2000;
	r.gpuRenderEndTime = 5000;
	// frameReport[1] stays empty (frameID 0) and must be skipped.
	FrameTokenSource source(&fake_get_new_frame_token);
	SlReflexDevice device(fake_api(source), SleepMode{ ReflexMode::On, 0 });
	const std::vector<FrameReport> reports = device.latency_reports();
	CHECK_EQ(reports.size(), size_t(1));
	if (!reports.empty()) {
		const FrameReport &f = reports[0];
		CHECK_EQ(f.present_id, uint64_t(7));
		CHECK_EQ(f.input_sample_us, uint64_t(900));
		CHECK_EQ(f.sim_start_us, uint64_t(1000));
		CHECK_EQ(f.sim_end_us, uint64_t(1100));
		CHECK_EQ(f.render_submit_start_us, uint64_t(1200));
		CHECK_EQ(f.present_end_us, uint64_t(1500));
		CHECK_EQ(f.gpu_render_end_us, uint64_t(5000));
	}
	// The existing aggregator works unchanged on the mapped reports.
	const LatencyStats stats = compute_latency_stats(reports);
	CHECK_EQ(stats.frames, 1);
	CHECK_EQ(stats.latest_present_id, uint64_t(7));
	CHECK_NEAR(stats.sim_start_to_gpu_end_ms, 4.0, 1e-9); // (5000 - 1000) us

	// A failed query yields no reports.
	reset_fake();
	g_get_state_result = sl::Result::eErrorFeatureMissing;
	FrameTokenSource source2(&fake_get_new_frame_token);
	SlReflexDevice device2(fake_api(source2), SleepMode{ ReflexMode::On, 0 });
	CHECK(device2.latency_reports().empty());
}
