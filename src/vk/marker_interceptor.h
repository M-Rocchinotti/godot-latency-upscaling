// Command-buffer interception for Phase 2: records an upscaler into Godot's own command buffer
// at the point Godot's render graph put a "marker" compute dispatch.
//
// The addon dispatches a marker compute pipeline through RenderingDevice (a compute list with a
// uniform set that names the upscaler's inputs and output), so Godot's render graph orders the
// dispatch and transitions every image the upscaler touches. When Godot records that compute list
// into a VkCommandBuffer, the layer sees:
//   vkCmdBindPipeline(COMPUTE, marker) -> vkCmdBindDescriptorSets / vkCmdPushConstants -> vkCmdDispatch
// and on the vkCmdDispatch hands the command buffer to the marker's MarkerRecorder instead. When the
// recorder recorded its commands, the marker dispatch is dropped and the state Godot bound for it
// (pipeline, descriptor sets, push constants) is bound again, so whatever Godot records next sees
// exactly the state it set up (Streamline's rule for manual hooking). When the recorder declines,
// the marker dispatch runs as Godot recorded it.
//
// Only compute state bound while a marker pipeline is bound is tracked, per command buffer; with no
// marker registered every call is forwarded after one atomic load. Pure C++ plus Vulkan headers:
// unit-tested with fake command functions (tests/unit/test_marker_interceptor.cpp).
#pragma once

#include "vk/vk_common.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lu::vk {

inline constexpr uint32_t MARKER_MAX_SETS = 8;
inline constexpr uint32_t MARKER_MAX_PUSH_BYTES = 256;

// What Godot recorded for the marker dispatch.
struct MarkerDispatch {
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	VkPipeline marker = VK_NULL_HANDLE;
	// The push-constant bytes pushed while the marker was bound (bytes never pushed are 0) and the
	// end of the highest range pushed.
	uint8_t push_constants[MARKER_MAX_PUSH_BYTES] = {};
	uint32_t push_constants_size = 0;
	// Compute descriptor sets bound while the marker was bound, by set index.
	VkDescriptorSet sets[MARKER_MAX_SETS] = {};
	uint32_t group_count_x = 0;
	uint32_t group_count_y = 0;
	uint32_t group_count_z = 0;
};

class MarkerRecorder {
public:
	virtual ~MarkerRecorder() = default;
	// Records the replacement for the marker's vkCmdDispatch into dispatch.command_buffer, using
	// function pointers that do not go through the layer's hooks. Returns true when it recorded
	// (the marker dispatch is then dropped and Godot's state bound again), false when it recorded
	// nothing (the marker dispatch runs). Called on the thread recording the command buffer,
	// without any lock held. A recorder must stay valid for the rest of the process once it was
	// registered (the addon's recorders are never destroyed).
	virtual bool record(const MarkerDispatch &dispatch) = 0;
};

// The next functions in the chain (the loader's trampolines in the layer).
struct CommandFunctions {
	PFN_vkCmdBindPipeline bind_pipeline = nullptr;
	PFN_vkCmdBindDescriptorSets bind_descriptor_sets = nullptr;
	PFN_vkCmdPushConstants push_constants = nullptr;
	PFN_vkCmdDispatch dispatch = nullptr;

	bool complete() const { return bind_pipeline && bind_descriptor_sets && push_constants && dispatch; }
};

struct InterceptorStats {
	uint64_t marker_dispatches = 0; // vkCmdDispatch with a marker pipeline bound
	uint64_t substituted = 0; // recorded by the recorder, marker dispatch dropped, state re-bound
	uint64_t declined = 0; // the recorder declined: the marker dispatch ran
	uint64_t untracked = 0; // too much state to re-bind: the marker dispatch ran, recorder not asked
};

class MarkerInterceptor {
public:
	static constexpr uint32_t MAX_MARKERS = 4;
	static constexpr size_t MAX_TRACKED_CALLS = 32; // bind / push calls per marker binding

	// Once, before any command hook can run.
	void set_next(const CommandFunctions &next) { next_ = next; }

	// Any thread. false: no free slot, a null argument, or the pipeline is already a marker.
	bool add_marker(VkPipeline pipeline, MarkerRecorder *recorder);
	void remove_marker(VkPipeline pipeline);
	bool has_markers() const { return marker_count_.load(std::memory_order_acquire) != 0; }

	// Hook bodies. Every call is forwarded to `next`, except the marker's vkCmdDispatch when its
	// recorder recorded instead.
	void bind_pipeline(VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point, VkPipeline pipeline);
	void bind_descriptor_sets(VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point, VkPipelineLayout layout,
			uint32_t first_set, uint32_t set_count, const VkDescriptorSet *sets, uint32_t dynamic_offset_count,
			const uint32_t *dynamic_offsets);
	void push_constants(VkCommandBuffer command_buffer, VkPipelineLayout layout, VkShaderStageFlags stages,
			uint32_t offset, uint32_t size, const void *values);
	void dispatch(VkCommandBuffer command_buffer, uint32_t x, uint32_t y, uint32_t z);
	// vkBeginCommandBuffer / vkEndCommandBuffer: the command buffer's tracked state ends (the hook
	// forwards those calls itself).
	void forget(VkCommandBuffer command_buffer);

	InterceptorStats stats() const;
	size_t tracked_command_buffers() const; // command buffers with a marker bound (tests)

private:
	struct SetBind {
		VkPipelineLayout layout;
		uint32_t first_set;
		std::vector<VkDescriptorSet> sets;
		std::vector<uint32_t> dynamic_offsets;
	};
	struct PushCall {
		VkPipelineLayout layout;
		VkShaderStageFlags stages;
		uint32_t offset;
		std::vector<uint8_t> bytes;
	};
	struct Tracked {
		VkPipeline marker = VK_NULL_HANDLE;
		MarkerRecorder *recorder = nullptr;
		std::vector<SetBind> set_binds;
		std::vector<PushCall> push_calls;
		bool overflow = false; // more state than can be re-bound
	};

	MarkerRecorder *find_marker(VkPipeline pipeline) const;
	void restore(VkCommandBuffer command_buffer, const Tracked &state) const;

	CommandFunctions next_;
	// Marker slots: written under mutex_, read lock-free by vkCmdBindPipeline.
	std::atomic<uint64_t> marker_pipelines_[MAX_MARKERS] = {};
	std::atomic<MarkerRecorder *> marker_recorders_[MAX_MARKERS] = {};
	std::atomic<uint32_t> marker_count_{ 0 };

	mutable std::mutex mutex_; // guards tracked_ and the marker slot writes
	std::unordered_map<VkCommandBuffer, Tracked> tracked_;
	std::atomic<size_t> tracked_count_{ 0 }; // tracked_.size(), readable without the lock

	std::atomic<uint64_t> marker_dispatches_{ 0 };
	std::atomic<uint64_t> substituted_{ 0 };
	std::atomic<uint64_t> declined_{ 0 };
	std::atomic<uint64_t> untracked_{ 0 };
};

} // namespace lu::vk
