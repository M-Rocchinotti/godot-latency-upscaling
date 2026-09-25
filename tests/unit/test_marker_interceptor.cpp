// MarkerInterceptor against fake command functions: which calls reach the next layer, when the
// recorder is asked, what it is given, and that the host's compute state is bound again after it
// recorded (pipeline, descriptor sets with their dynamic offsets, push constants, in order).
#include "vk/marker_interceptor.h"
#include "test_framework.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace lu::vk;

namespace {

template <typename Handle>
Handle handle(uintptr_t value) {
	return reinterpret_cast<Handle>(value);
}

template <typename Handle>
uintptr_t bits(Handle h) {
	return reinterpret_cast<uintptr_t>(h);
}

// --- The "next layer": logs every call it receives --------------------------------------------

std::mutex g_log_mutex;
std::vector<std::string> g_log;
std::atomic<bool> g_logging{ true };
std::atomic<uint64_t> g_forwarded_dispatches{ 0 };

void log_call(const std::string &line) {
	if (g_logging.load()) {
		std::lock_guard<std::mutex> lock(g_log_mutex);
		g_log.push_back(line);
	}
}

VKAPI_ATTR void VKAPI_CALL fake_bind_pipeline(VkCommandBuffer cb, VkPipelineBindPoint bind_point, VkPipeline pipeline) {
	std::ostringstream s;
	s << "bind_pipeline cb=" << bits(cb) << " bp=" << bind_point << " p=" << bits(pipeline);
	log_call(s.str());
}

VKAPI_ATTR void VKAPI_CALL fake_bind_sets(VkCommandBuffer cb, VkPipelineBindPoint bind_point, VkPipelineLayout layout,
		uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *dynamic) {
	std::ostringstream s;
	s << "bind_sets cb=" << bits(cb) << " bp=" << bind_point << " layout=" << bits(layout) << " first=" << first << " sets=";
	for (uint32_t i = 0; i < count; ++i) {
		s << bits(sets[i]) << (i + 1 < count ? "," : "");
	}
	s << " dyn=";
	for (uint32_t i = 0; i < dynamic_count; ++i) {
		s << dynamic[i] << (i + 1 < dynamic_count ? "," : "");
	}
	log_call(s.str());
}

VKAPI_ATTR void VKAPI_CALL fake_push(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
		uint32_t size, const void *values) {
	std::ostringstream s;
	s << "push cb=" << bits(cb) << " layout=" << bits(layout) << " stages=" << stages << " offset=" << offset
	  << " size=" << size << " first=" << static_cast<int>(static_cast<const uint8_t *>(values)[0]);
	log_call(s.str());
}

VKAPI_ATTR void VKAPI_CALL fake_dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
	g_forwarded_dispatches++;
	std::ostringstream s;
	s << "dispatch cb=" << bits(cb) << " " << x << "," << y << "," << z;
	log_call(s.str());
}

CommandFunctions fakes() {
	CommandFunctions f;
	f.bind_pipeline = &fake_bind_pipeline;
	f.bind_descriptor_sets = &fake_bind_sets;
	f.push_constants = &fake_push;
	f.dispatch = &fake_dispatch;
	return f;
}

void reset_log() {
	std::lock_guard<std::mutex> lock(g_log_mutex);
	g_log.clear();
	g_logging = true;
	g_forwarded_dispatches = 0;
}

std::vector<std::string> take_log() {
	std::lock_guard<std::mutex> lock(g_log_mutex);
	std::vector<std::string> out;
	out.swap(g_log);
	return out;
}

// A recorder that logs where in the stream it was asked, and answers as told.
class FakeRecorder : public MarkerRecorder {
public:
	explicit FakeRecorder(bool answer) : answer_(answer) {}
	bool record(const MarkerDispatch &dispatch) override {
		calls++;
		last = dispatch;
		log_call("recorder");
		return answer_;
	}
	int calls = 0;
	MarkerDispatch last;

private:
	bool answer_;
};

const VkCommandBuffer CB1 = handle<VkCommandBuffer>(0x100);
const VkCommandBuffer CB2 = handle<VkCommandBuffer>(0x200);
const VkPipeline MARKER = handle<VkPipeline>(0xA1);
const VkPipeline OTHER = handle<VkPipeline>(0xB2);
const VkPipelineLayout LAYOUT = handle<VkPipelineLayout>(0x33);
const VkDescriptorSet SET0 = handle<VkDescriptorSet>(0x40);
const VkDescriptorSet SET1 = handle<VkDescriptorSet>(0x41);

void record_marker_list(MarkerInterceptor &mi, VkCommandBuffer cb) {
	mi.bind_pipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	const uint32_t dynamic = 64;
	mi.bind_descriptor_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, LAYOUT, 0, 1, &SET0, 1, &dynamic);
	uint8_t bytes[48];
	for (int i = 0; i < 48; ++i) {
		bytes[i] = static_cast<uint8_t>(i + 1);
	}
	mi.push_constants(cb, LAYOUT, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bytes), bytes);
	mi.dispatch(cb, 8, 4, 1);
}

} // namespace

TEST(interceptor_forwards_everything_without_markers) {
	reset_log();
	MarkerInterceptor mi;
	mi.set_next(fakes());
	CHECK(!mi.has_markers());
	record_marker_list(mi, CB1); // MARKER is not registered: an ordinary pipeline
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_GRAPHICS, OTHER);
	mi.forget(CB1);
	const std::vector<std::string> expected = {
		"bind_pipeline cb=256 bp=1 p=161",
		"bind_sets cb=256 bp=1 layout=51 first=0 sets=64 dyn=64",
		"push cb=256 layout=51 stages=32 offset=0 size=48 first=1",
		"dispatch cb=256 8,4,1",
		"bind_pipeline cb=256 bp=0 p=178",
	};
	CHECK(take_log() == expected);
	CHECK_EQ(mi.tracked_command_buffers(), static_cast<size_t>(0));
	const InterceptorStats s = mi.stats();
	CHECK_EQ(s.marker_dispatches, 0u);
	CHECK_EQ(s.substituted, 0u);
}

TEST(interceptor_substitutes_the_marker_dispatch_and_rebinds_the_hosts_state) {
	reset_log();
	MarkerInterceptor mi;
	mi.set_next(fakes());
	FakeRecorder recorder(true);
	CHECK(mi.add_marker(MARKER, &recorder));
	record_marker_list(mi, CB1);
	const std::vector<std::string> expected = {
		// Godot's calls reach the driver as recorded...
		"bind_pipeline cb=256 bp=1 p=161",
		"bind_sets cb=256 bp=1 layout=51 first=0 sets=64 dyn=64",
		"push cb=256 layout=51 stages=32 offset=0 size=48 first=1",
		// ...the marker's dispatch is replaced by the recorder's commands...
		"recorder",
		// ...and the state Godot bound for it is bound again, with the same dynamic offsets and bytes.
		"bind_pipeline cb=256 bp=1 p=161",
		"bind_sets cb=256 bp=1 layout=51 first=0 sets=64 dyn=64",
		"push cb=256 layout=51 stages=32 offset=0 size=48 first=1",
	};
	CHECK(take_log() == expected);
	CHECK_EQ(g_forwarded_dispatches.load(), 0u);
	CHECK_EQ(recorder.calls, 1);
	CHECK(recorder.last.command_buffer == CB1);
	CHECK(recorder.last.marker == MARKER);
	CHECK(recorder.last.sets[0] == SET0);
	CHECK(recorder.last.sets[1] == VK_NULL_HANDLE);
	CHECK_EQ(recorder.last.push_constants_size, 48u);
	CHECK_EQ(static_cast<int>(recorder.last.push_constants[0]), 1);
	CHECK_EQ(static_cast<int>(recorder.last.push_constants[47]), 48);
	CHECK_EQ(recorder.last.group_count_x, 8u);
	CHECK_EQ(recorder.last.group_count_y, 4u);
	CHECK_EQ(recorder.last.group_count_z, 1u);
	const InterceptorStats s = mi.stats();
	CHECK_EQ(s.marker_dispatches, 1u);
	CHECK_EQ(s.substituted, 1u);
	CHECK_EQ(s.declined, 0u);
	// The marker is still bound (re-bound): a second dispatch in the same list is intercepted too.
	mi.dispatch(CB1, 2, 2, 1);
	CHECK_EQ(recorder.calls, 2);
	CHECK_EQ(g_forwarded_dispatches.load(), 0u);
}

TEST(interceptor_runs_the_marker_dispatch_when_the_recorder_declines) {
	reset_log();
	MarkerInterceptor mi;
	mi.set_next(fakes());
	FakeRecorder recorder(false);
	CHECK(mi.add_marker(MARKER, &recorder));
	record_marker_list(mi, CB1);
	const std::vector<std::string> expected = {
		"bind_pipeline cb=256 bp=1 p=161",
		"bind_sets cb=256 bp=1 layout=51 first=0 sets=64 dyn=64",
		"push cb=256 layout=51 stages=32 offset=0 size=48 first=1",
		"recorder",
		// Godot's state is bound again before the marker dispatch runs.
		"bind_pipeline cb=256 bp=1 p=161",
		"bind_sets cb=256 bp=1 layout=51 first=0 sets=64 dyn=64",
		"push cb=256 layout=51 stages=32 offset=0 size=48 first=1",
		"dispatch cb=256 8,4,1",
	};
	CHECK(take_log() == expected);
	const InterceptorStats s = mi.stats();
	CHECK_EQ(s.marker_dispatches, 1u);
	CHECK_EQ(s.substituted, 0u);
	CHECK_EQ(s.declined, 1u);
}

// A recorder that binds its own pipeline (as Streamline does inside an evaluation) and then gives up.
class PartialRecorder : public MarkerRecorder {
public:
	bool record(const MarkerDispatch &dispatch) override {
		log_call("recorder");
		fake_bind_pipeline(dispatch.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, OTHER);
		fake_push(dispatch.command_buffer, LAYOUT, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, "cccc");
		return false;
	}
};

TEST(interceptor_rebinds_the_hosts_state_when_a_recorder_declines_after_recording) {
	reset_log();
	MarkerInterceptor mi;
	mi.set_next(fakes());
	PartialRecorder recorder;
	CHECK(mi.add_marker(MARKER, &recorder));
	record_marker_list(mi, CB1);
	const std::vector<std::string> expected = {
		"bind_pipeline cb=256 bp=1 p=161",
		"bind_sets cb=256 bp=1 layout=51 first=0 sets=64 dyn=64",
		"push cb=256 layout=51 stages=32 offset=0 size=48 first=1",
		"recorder",
		"bind_pipeline cb=256 bp=1 p=178", // the recorder's own state...
		"push cb=256 layout=51 stages=32 offset=0 size=4 first=99",
		"bind_pipeline cb=256 bp=1 p=161", // ...is replaced by Godot's before the marker runs
		"bind_sets cb=256 bp=1 layout=51 first=0 sets=64 dyn=64",
		"push cb=256 layout=51 stages=32 offset=0 size=48 first=1",
		"dispatch cb=256 8,4,1",
	};
	CHECK(take_log() == expected);
	const InterceptorStats s = mi.stats();
	CHECK_EQ(s.declined, 1u);
	CHECK_EQ(s.substituted, 0u);
}

TEST(interceptor_tracks_only_while_the_marker_is_the_bound_compute_pipeline) {
	reset_log();
	MarkerInterceptor mi;
	mi.set_next(fakes());
	FakeRecorder recorder(true);
	CHECK(mi.add_marker(MARKER, &recorder));

	// Another compute pipeline replaces the marker: its dispatch is Godot's own.
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	CHECK_EQ(mi.tracked_command_buffers(), static_cast<size_t>(1));
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, OTHER);
	CHECK_EQ(mi.tracked_command_buffers(), static_cast<size_t>(0));
	mi.dispatch(CB1, 1, 1, 1);
	CHECK_EQ(recorder.calls, 0);
	CHECK_EQ(g_forwarded_dispatches.load(), 1u);

	// A graphics pipeline does not change the compute binding.
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_GRAPHICS, OTHER);
	mi.dispatch(CB1, 1, 1, 1);
	CHECK_EQ(recorder.calls, 1);

	// A new recording (vkBeginCommandBuffer) or its end forgets the command buffer.
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	mi.forget(CB1);
	CHECK_EQ(mi.tracked_command_buffers(), static_cast<size_t>(0));
	mi.dispatch(CB1, 1, 1, 1);
	CHECK_EQ(recorder.calls, 1);

	// Graphics descriptor sets bound while the marker is bound are not part of its state.
	take_log();
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	mi.bind_descriptor_sets(CB1, VK_PIPELINE_BIND_POINT_GRAPHICS, LAYOUT, 0, 1, &SET1, 0, nullptr);
	mi.dispatch(CB1, 1, 1, 1);
	CHECK(recorder.last.sets[0] == VK_NULL_HANDLE);
	const std::vector<std::string> log = take_log();
	CHECK_EQ(log.size(), static_cast<size_t>(4)); // bind, graphics sets, recorder, re-bind of the pipeline only
	CHECK_EQ(log.back(), std::string("bind_pipeline cb=256 bp=1 p=161"));
}

TEST(interceptor_keeps_command_buffers_apart) {
	reset_log();
	MarkerInterceptor mi;
	mi.set_next(fakes());
	FakeRecorder recorder(true);
	CHECK(mi.add_marker(MARKER, &recorder));
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	mi.bind_pipeline(CB2, VK_PIPELINE_BIND_POINT_COMPUTE, OTHER);
	const uint8_t other_bytes[4] = { 9, 9, 9, 9 };
	mi.push_constants(CB2, LAYOUT, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, other_bytes);
	mi.bind_descriptor_sets(CB2, VK_PIPELINE_BIND_POINT_COMPUTE, LAYOUT, 0, 1, &SET1, 0, nullptr);
	mi.dispatch(CB2, 3, 3, 1);
	CHECK_EQ(recorder.calls, 0);
	mi.dispatch(CB1, 5, 5, 1);
	CHECK_EQ(recorder.calls, 1);
	CHECK(recorder.last.command_buffer == CB1);
	CHECK_EQ(recorder.last.push_constants_size, 0u);
	CHECK(recorder.last.sets[0] == VK_NULL_HANDLE);
}

TEST(interceptor_merges_partial_push_constant_updates) {
	reset_log();
	MarkerInterceptor mi;
	mi.set_next(fakes());
	FakeRecorder recorder(true);
	CHECK(mi.add_marker(MARKER, &recorder));
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	uint8_t a[16], b[16];
	std::memset(a, 0xAA, sizeof(a));
	std::memset(b, 0xBB, sizeof(b));
	mi.push_constants(CB1, LAYOUT, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, a);
	mi.push_constants(CB1, LAYOUT, VK_SHADER_STAGE_COMPUTE_BIT, 32, 16, b);
	mi.bind_descriptor_sets(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, LAYOUT, 1, 1, &SET1, 0, nullptr);
	take_log();
	mi.dispatch(CB1, 1, 1, 1);
	CHECK_EQ(recorder.last.push_constants_size, 48u);
	CHECK_EQ(static_cast<int>(recorder.last.push_constants[15]), 0xAA);
	CHECK_EQ(static_cast<int>(recorder.last.push_constants[16]), 0);
	CHECK_EQ(static_cast<int>(recorder.last.push_constants[32]), 0xBB);
	CHECK(recorder.last.sets[1] == SET1);
	const std::vector<std::string> expected = {
		"recorder",
		"bind_pipeline cb=256 bp=1 p=161",
		"bind_sets cb=256 bp=1 layout=51 first=1 sets=65 dyn=",
		"push cb=256 layout=51 stages=32 offset=0 size=16 first=170",
		"push cb=256 layout=51 stages=32 offset=32 size=16 first=187",
	};
	CHECK(take_log() == expected);
}

TEST(interceptor_marker_slots_are_bounded_and_removable) {
	reset_log();
	MarkerInterceptor mi;
	mi.set_next(fakes());
	FakeRecorder recorder(true);
	CHECK(!mi.add_marker(VK_NULL_HANDLE, &recorder));
	CHECK(!mi.add_marker(MARKER, nullptr));
	for (uintptr_t i = 0; i < MarkerInterceptor::MAX_MARKERS; ++i) {
		CHECK(mi.add_marker(handle<VkPipeline>(0x1000 + i), &recorder));
	}
	CHECK(!mi.add_marker(MARKER, &recorder)); // full
	CHECK(!mi.add_marker(handle<VkPipeline>(0x1000), &recorder)); // already a marker
	mi.remove_marker(handle<VkPipeline>(0x1001));
	CHECK(mi.add_marker(MARKER, &recorder));
	CHECK(mi.has_markers());

	// Removed while bound: the dispatch runs as recorded.
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	mi.remove_marker(MARKER);
	CHECK_EQ(mi.tracked_command_buffers(), static_cast<size_t>(0));
	mi.dispatch(CB1, 1, 1, 1);
	CHECK_EQ(recorder.calls, 0);
	CHECK_EQ(g_forwarded_dispatches.load(), 1u);
	for (uintptr_t i = 0; i < MarkerInterceptor::MAX_MARKERS; ++i) {
		mi.remove_marker(handle<VkPipeline>(0x1000 + i));
	}
	CHECK(!mi.has_markers());
}

TEST(interceptor_runs_the_marker_dispatch_when_the_state_cannot_be_rebound) {
	reset_log();
	g_logging = false;
	MarkerInterceptor mi;
	mi.set_next(fakes());
	FakeRecorder recorder(true);
	CHECK(mi.add_marker(MARKER, &recorder));
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	for (size_t i = 0; i <= MarkerInterceptor::MAX_TRACKED_CALLS; ++i) {
		mi.bind_descriptor_sets(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, LAYOUT, 0, 1, &SET0, 0, nullptr);
	}
	mi.dispatch(CB1, 1, 1, 1);
	CHECK_EQ(recorder.calls, 0);
	CHECK_EQ(mi.stats().untracked, 1u);
	CHECK_EQ(g_forwarded_dispatches.load(), 1u);

	// Push constants beyond what can be kept.
	mi.bind_pipeline(CB1, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
	uint8_t bytes[16] = {};
	mi.push_constants(CB1, LAYOUT, VK_SHADER_STAGE_COMPUTE_BIT, MARKER_MAX_PUSH_BYTES - 8, 16, bytes);
	mi.dispatch(CB1, 1, 1, 1);
	CHECK_EQ(recorder.calls, 0);
	CHECK_EQ(mi.stats().untracked, 2u);
	g_logging = true;
}

TEST(interceptor_is_consistent_across_threads) {
	reset_log();
	g_logging = false;
	MarkerInterceptor mi;
	mi.set_next(fakes());
	// Checks that each dispatch carries its own command buffer's push constants.
	class CheckingRecorder : public MarkerRecorder {
	public:
		bool record(const MarkerDispatch &dispatch) override {
			const uint8_t expected = static_cast<uint8_t>(bits(dispatch.command_buffer) >> 8);
			if (dispatch.push_constants[0] != expected || dispatch.push_constants_size != 4) {
				mismatches++;
			}
			calls++;
			return true;
		}
		std::atomic<int> calls{ 0 };
		std::atomic<int> mismatches{ 0 };
	} recorder;
	CHECK(mi.add_marker(MARKER, &recorder));
	constexpr int THREADS = 4;
	constexpr int ITERATIONS = 2000;
	std::vector<std::thread> threads;
	for (int t = 0; t < THREADS; ++t) {
		threads.emplace_back([&, t] {
			const VkCommandBuffer cb = handle<VkCommandBuffer>(static_cast<uintptr_t>(t + 1) << 8);
			const uint8_t bytes[4] = { static_cast<uint8_t>(t + 1), 0, 0, 0 };
			for (int i = 0; i < ITERATIONS; ++i) {
				mi.bind_pipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, MARKER);
				mi.push_constants(cb, LAYOUT, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, bytes);
				mi.dispatch(cb, 1, 1, 1);
				mi.bind_pipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, OTHER); // an ordinary dispatch in between
				mi.dispatch(cb, 1, 1, 1);
				mi.forget(cb);
			}
		});
	}
	for (std::thread &thread : threads) {
		thread.join();
	}
	CHECK_EQ(recorder.calls.load(), THREADS * ITERATIONS);
	CHECK_EQ(recorder.mismatches.load(), 0);
	CHECK_EQ(g_forwarded_dispatches.load(), static_cast<uint64_t>(THREADS * ITERATIONS));
	CHECK_EQ(mi.stats().substituted, static_cast<uint64_t>(THREADS * ITERATIONS));
	CHECK_EQ(mi.tracked_command_buffers(), static_cast<size_t>(0));
	g_logging = true;
}
