#include "reflex/frame_sequencer.h"
#include "test_framework.h"
#include "vk/vk_common.h"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

using namespace nvdlss::reflex;

namespace {

struct Recorder : MarkerSink {
	std::vector<std::pair<uint64_t, Marker>> events;
	void emit_marker(uint64_t frame_id, Marker marker) override { events.emplace_back(frame_id, marker); }
	std::vector<Marker> markers_of(uint64_t id) const {
		std::vector<Marker> out;
		for (const auto &e : events) {
			if (e.first == id) {
				out.push_back(e.second);
			}
		}
		return out;
	}
};

// Canonical order of a frame's markers (INPUT_SAMPLE and TRIGGER_FLASH belong to the
// simulation, between its start and end).
int rank(Marker m) {
	switch (m) {
		case Marker::SimulationStart:
			return 0;
		case Marker::InputSample:
			return 1;
		case Marker::TriggerFlash:
			return 2;
		case Marker::SimulationEnd:
			return 3;
		case Marker::RenderSubmitStart:
			return 4;
		case Marker::RenderSubmitEnd:
			return 5;
		case Marker::PresentStart:
			return 6;
		case Marker::PresentEnd:
			return 7;
	}
	return 99;
}

// One Godot main-loop iteration in the default (single-threaded) model.
uint64_t godot_frame(FrameSequencer &seq, bool draws, bool presents) {
	seq.close_open_simulation();
	const uint64_t id = seq.begin_simulation();
	if (draws) {
		const uint64_t drawn = seq.end_simulation();
		seq.render_begin(drawn);
		if (presents) {
			const uint64_t presented = seq.present_begin();
			seq.present_end(presented);
		}
		seq.render_end(drawn);
	}
	return id;
}

} // namespace

TEST(sequencer_normal_frame_emits_every_marker_once_in_order) {
	Recorder rec;
	FrameSequencer seq(rec);
	const uint64_t id = godot_frame(seq, true, true);
	CHECK_EQ(id, 1ull);
	const std::vector<Marker> expected = { Marker::SimulationStart, Marker::InputSample, Marker::SimulationEnd,
		Marker::RenderSubmitStart, Marker::RenderSubmitEnd, Marker::PresentStart, Marker::PresentEnd };
	CHECK(rec.markers_of(1) == expected);
	CHECK_EQ(seq.last_presented_id(), 1ull);
}

TEST(sequencer_ids_increase_per_frame) {
	Recorder rec;
	FrameSequencer seq(rec);
	CHECK_EQ(godot_frame(seq, true, true), 1ull);
	CHECK_EQ(godot_frame(seq, true, true), 2ull);
	CHECK_EQ(godot_frame(seq, true, true), 3ull);
}

TEST(sequencer_closes_a_simulation_that_never_drew) {
	Recorder rec;
	FrameSequencer seq(rec);
	godot_frame(seq, false, false); // frame 1 skips drawing (e.g. low-processor mode)
	godot_frame(seq, true, true);
	const std::vector<Marker> one = { Marker::SimulationStart, Marker::InputSample, Marker::SimulationEnd };
	CHECK(rec.markers_of(1) == one);
	// SIMULATION_END of frame 1 is sent before SIMULATION_START of frame 2.
	size_t end1 = 0, start2 = 0;
	for (size_t i = 0; i < rec.events.size(); ++i) {
		if (rec.events[i] == std::make_pair(uint64_t(1), Marker::SimulationEnd)) {
			end1 = i;
		}
		if (rec.events[i] == std::make_pair(uint64_t(2), Marker::SimulationStart)) {
			start2 = i;
		}
	}
	CHECK(end1 < start2);
}

TEST(sequencer_begin_without_close_still_ends_the_previous_simulation) {
	Recorder rec;
	FrameSequencer seq(rec);
	seq.begin_simulation();
	seq.begin_simulation();
	CHECK(rec.markers_of(1).back() == Marker::SimulationEnd);
	CHECK_EQ(seq.current_simulation_id(), 2ull);
}

TEST(sequencer_render_end_covers_draws_without_present) {
	Recorder rec;
	FrameSequencer seq(rec);
	godot_frame(seq, true, false);
	const std::vector<Marker> expected = { Marker::SimulationStart, Marker::InputSample, Marker::SimulationEnd,
		Marker::RenderSubmitStart, Marker::RenderSubmitEnd };
	CHECK(rec.markers_of(1) == expected);
	CHECK_EQ(seq.last_presented_id(), 0ull);
}

TEST(sequencer_present_without_a_rendered_frame_gets_no_id) {
	Recorder rec;
	FrameSequencer seq(rec);
	CHECK_EQ(seq.present_begin(), 0ull);
	seq.present_end(0);
	CHECK(rec.events.empty());
	CHECK_EQ(seq.submission_id(), 0ull);
}

TEST(sequencer_second_present_of_the_same_frame_gets_no_id) {
	Recorder rec;
	FrameSequencer seq(rec);
	seq.begin_simulation();
	seq.render_begin(seq.end_simulation());
	CHECK_EQ(seq.present_begin(), 1ull);
	seq.present_end(1);
	const size_t count = rec.events.size();
	CHECK_EQ(seq.present_begin(), 0ull); // present IDs must increase per swapchain
	CHECK_EQ(rec.events.size(), count);
}

TEST(sequencer_new_swapchain_accepts_ids_again) {
	Recorder rec;
	FrameSequencer seq(rec);
	seq.begin_simulation();
	seq.render_begin(seq.end_simulation());
	CHECK_EQ(seq.present_begin(), 1ull);
	seq.reset_present_history();
	CHECK_EQ(seq.present_begin(), 1ull);
}

TEST(sequencer_flash_is_sent_inside_the_simulation) {
	Recorder rec;
	FrameSequencer seq(rec);
	seq.begin_simulation();
	seq.request_flash();
	seq.end_simulation();
	const std::vector<Marker> expected = { Marker::SimulationStart, Marker::InputSample, Marker::TriggerFlash, Marker::SimulationEnd };
	CHECK(rec.markers_of(1) == expected);
	// A flash request without an open simulation is dropped.
	seq.request_flash();
	seq.begin_simulation();
	seq.end_simulation();
	CHECK(rec.markers_of(2).size() == 3u);
}

TEST(sequencer_submissions_carry_the_rendered_frame_id) {
	Recorder rec;
	FrameSequencer seq(rec);
	CHECK_EQ(seq.submission_id(), 0ull);
	seq.begin_simulation();
	const uint64_t drawn = seq.end_simulation();
	seq.begin_simulation(); // the main thread already simulates the next frame (render thread model)
	seq.render_begin(drawn);
	CHECK_EQ(seq.submission_id(), 1ull);
	CHECK_EQ(seq.present_begin(), 1ull);
}

TEST(sequencer_render_begin_closes_an_unfinished_render) {
	Recorder rec;
	FrameSequencer seq(rec);
	seq.render_begin(1);
	seq.render_begin(2);
	const std::vector<Marker> one = { Marker::RenderSubmitStart, Marker::RenderSubmitEnd };
	CHECK(rec.markers_of(1) == one);
	seq.render_end(1); // stale: frame 2 is being rendered
	CHECK(rec.markers_of(1) == one);
	seq.render_begin(0); // no frame: ignored
	CHECK_EQ(seq.submission_id(), 2ull);
}

TEST(sequencer_random_event_streams_stay_well_formed) {
	// Property check: for any mix of draws, skipped draws, missing presents and flashes, every
	// frame's markers appear at most once, in canonical order, and present IDs increase.
	uint32_t state = 12345;
	auto next = [&]() {
		state = state * 1664525u + 1013904223u;
		return state >> 16;
	};
	for (int run = 0; run < 200; ++run) {
		Recorder rec;
		FrameSequencer seq(rec);
		uint64_t last_present = 0;
		bool monotonic = true;
		for (int frame = 0; frame < 40; ++frame) {
			seq.close_open_simulation();
			seq.begin_simulation();
			if (next() % 4 == 0) {
				seq.request_flash();
			}
			if (next() % 5 != 0) {
				const uint64_t drawn = seq.end_simulation();
				seq.render_begin(drawn);
				if (next() % 6 != 0) {
					const uint64_t id = seq.present_begin();
					if (id != 0) {
						monotonic = monotonic && id > last_present;
						last_present = id;
					}
					seq.present_end(id);
				}
				seq.render_end(drawn);
			}
		}
		CHECK(monotonic);
		std::map<uint64_t, std::vector<Marker>> per_frame;
		for (const auto &e : rec.events) {
			per_frame[e.first].push_back(e.second);
		}
		for (const auto &[id, markers] : per_frame) {
			for (size_t i = 1; i < markers.size(); ++i) {
				if (!(rank(markers[i - 1]) < rank(markers[i]))) {
					unit::report_failure(__FILE__, __LINE__, "frame " + std::to_string(id) + ": " + marker_name(markers[i - 1]) +
									" before " + marker_name(markers[i]));
				}
			}
			CHECK(!markers.empty() && markers.front() == Marker::SimulationStart);
		}
	}
}

TEST(marker_values_match_vk_latency_marker_nv) {
	CHECK_EQ(static_cast<uint32_t>(Marker::SimulationStart), static_cast<uint32_t>(VK_LATENCY_MARKER_SIMULATION_START_NV));
	CHECK_EQ(static_cast<uint32_t>(Marker::SimulationEnd), static_cast<uint32_t>(VK_LATENCY_MARKER_SIMULATION_END_NV));
	CHECK_EQ(static_cast<uint32_t>(Marker::RenderSubmitStart), static_cast<uint32_t>(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV));
	CHECK_EQ(static_cast<uint32_t>(Marker::RenderSubmitEnd), static_cast<uint32_t>(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV));
	CHECK_EQ(static_cast<uint32_t>(Marker::PresentStart), static_cast<uint32_t>(VK_LATENCY_MARKER_PRESENT_START_NV));
	CHECK_EQ(static_cast<uint32_t>(Marker::PresentEnd), static_cast<uint32_t>(VK_LATENCY_MARKER_PRESENT_END_NV));
	CHECK_EQ(static_cast<uint32_t>(Marker::InputSample), static_cast<uint32_t>(VK_LATENCY_MARKER_INPUT_SAMPLE_NV));
	CHECK_EQ(static_cast<uint32_t>(Marker::TriggerFlash), static_cast<uint32_t>(VK_LATENCY_MARKER_TRIGGER_FLASH_NV));
	CHECK_EQ(MARKER_COUNT, static_cast<size_t>(8));
}