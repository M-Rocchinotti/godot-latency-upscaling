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

SlReflexApi fake_api() {
	SlReflexApi api;
	api.get_new_frame_token = &fake_get_new_frame_token;
	api.set_options = &fake_set_options;
	api.sleep = &fake_sleep;
	api.get_state = &fake_get_state;
	api.set_marker = &fake_set_marker;
	return api;
}

} // namespace

TEST(sl_reflex_mode_maps_to_streamline_and_carries_the_fps_cap) {
	reset_fake();
	SlReflexDevice device(fake_api(), SleepMode{ ReflexMode::On, 0 }); // ctor applies the options once
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

TEST(sl_reflex_sleeps_once_per_frame_with_the_upcoming_frames_token) {
	reset_fake();
	SlReflexDevice device(fake_api(), SleepMode{ ReflexMode::On, 0 });
	FrameSequencer &seq = device.sequencer();
	// Before the first begin_simulation() the upcoming frame is 1: the sleep uses token 1.
	CHECK(device.sleep());
	CHECK_EQ(g_sleeps.size(), size_t(1));
	CHECK_EQ(g_sleeps[0], 1u);
	// begin_simulation() opens frame 1; the next sleep is for frame 2.
	CHECK_EQ(seq.begin_simulation(), uint64_t(1));
	CHECK(device.sleep());
	CHECK_EQ(g_sleeps.size(), size_t(2)); // exactly one sleep per call
	CHECK_EQ(g_sleeps[1], 2u);
	CHECK_EQ(device.state().counters.sleeps, uint64_t(2));
}

TEST(sl_reflex_emits_the_six_markers_in_order_and_drops_input_sample) {
	reset_fake();
	SlReflexDevice device(fake_api(), SleepMode{ ReflexMode::On, 0 });
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
	if (g_markers.size() == 6) {
		for (size_t i = 0; i < 6; ++i) {
			CHECK(g_markers[i].first == expected[i]);
			CHECK_EQ(g_markers[i].second, 1u); // every marker of frame 1 carries frame 1's token
		}
	}
	const DeviceState st = device.state();
	CHECK_EQ(st.counters.markers[static_cast<size_t>(Marker::SimulationStart)], uint64_t(1));
	CHECK_EQ(st.counters.markers[static_cast<size_t>(Marker::PresentEnd)], uint64_t(1));
	CHECK_EQ(st.counters.markers[static_cast<size_t>(Marker::InputSample)], uint64_t(0)); // dropped, not counted
}

TEST(sl_reflex_threads_a_distinct_token_through_each_frame) {
	reset_fake();
	SlReflexDevice device(fake_api(), SleepMode{ ReflexMode::On, 0 });
	FrameSequencer &seq = device.sequencer();
	for (uint32_t expected_frame = 1; expected_frame <= 2; ++expected_frame) {
		device.sleep(); // token for the upcoming frame
		const uint64_t id = seq.begin_simulation();
		seq.end_simulation();
		CHECK_EQ(id, uint64_t(expected_frame));
	}
	CHECK_EQ(g_sleeps.size(), size_t(2));
	CHECK_EQ(g_sleeps[0], 1u);
	CHECK_EQ(g_sleeps[1], 2u);
	// Markers: SIMULATION_START + SIMULATION_END per frame, token 1 for frame 1, token 2 for frame 2.
	CHECK_EQ(g_markers.size(), size_t(4));
	if (g_markers.size() == 4) {
		CHECK_EQ(g_markers[0].second, 1u);
		CHECK_EQ(g_markers[1].second, 1u);
		CHECK_EQ(g_markers[2].second, 2u);
		CHECK_EQ(g_markers[3].second, 2u);
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
	SlReflexDevice device(fake_api(), SleepMode{ ReflexMode::On, 0 });
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
	SlReflexDevice device2(fake_api(), SleepMode{ ReflexMode::On, 0 });
	CHECK(device2.latency_reports().empty());
}
