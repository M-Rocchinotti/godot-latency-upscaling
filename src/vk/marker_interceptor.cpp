#include "vk/marker_interceptor.h"

#include <algorithm>
#include <cstring>

namespace lu::vk {
namespace {

uint64_t pipeline_bits(VkPipeline pipeline) {
	return reinterpret_cast<uint64_t>(pipeline);
}

} // namespace

bool MarkerInterceptor::add_marker(VkPipeline pipeline, MarkerRecorder *recorder) {
	if (pipeline == VK_NULL_HANDLE || recorder == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	int free_slot = -1;
	for (uint32_t i = 0; i < MAX_MARKERS; ++i) {
		const uint64_t slot = marker_pipelines_[i].load(std::memory_order_relaxed);
		if (slot == pipeline_bits(pipeline)) {
			return false;
		}
		if (slot == 0 && free_slot < 0) {
			free_slot = static_cast<int>(i);
		}
	}
	if (free_slot < 0) {
		return false;
	}
	// The recorder first: a reader that sees the pipeline also sees its recorder.
	marker_recorders_[free_slot].store(recorder, std::memory_order_release);
	marker_pipelines_[free_slot].store(pipeline_bits(pipeline), std::memory_order_release);
	marker_count_.fetch_add(1, std::memory_order_acq_rel);
	return true;
}

void MarkerInterceptor::remove_marker(VkPipeline pipeline) {
	if (pipeline == VK_NULL_HANDLE) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	for (uint32_t i = 0; i < MAX_MARKERS; ++i) {
		if (marker_pipelines_[i].load(std::memory_order_relaxed) == pipeline_bits(pipeline)) {
			marker_pipelines_[i].store(0, std::memory_order_release);
			marker_count_.fetch_sub(1, std::memory_order_acq_rel);
		}
	}
	// A command buffer that has it bound right now stops being tracked: its dispatch runs as recorded.
	for (auto it = tracked_.begin(); it != tracked_.end();) {
		it = it->second.marker == pipeline ? tracked_.erase(it) : std::next(it);
	}
	tracked_count_.store(tracked_.size(), std::memory_order_release);
}

MarkerRecorder *MarkerInterceptor::find_marker(VkPipeline pipeline) const {
	if (pipeline == VK_NULL_HANDLE || marker_count_.load(std::memory_order_acquire) == 0) {
		return nullptr;
	}
	for (uint32_t i = 0; i < MAX_MARKERS; ++i) {
		if (marker_pipelines_[i].load(std::memory_order_acquire) == pipeline_bits(pipeline)) {
			return marker_recorders_[i].load(std::memory_order_acquire);
		}
	}
	return nullptr;
}

void MarkerInterceptor::bind_pipeline(VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point, VkPipeline pipeline) {
	next_.bind_pipeline(command_buffer, bind_point, pipeline);
	if (bind_point != VK_PIPELINE_BIND_POINT_COMPUTE) {
		return; // compute state is all the recorders change
	}
	MarkerRecorder *recorder = find_marker(pipeline);
	if (recorder != nullptr) {
		std::lock_guard<std::mutex> lock(mutex_);
		Tracked &state = tracked_[command_buffer];
		state = Tracked();
		state.marker = pipeline;
		state.recorder = recorder;
		tracked_count_.store(tracked_.size(), std::memory_order_release);
	} else if (tracked_count_.load(std::memory_order_acquire) != 0) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (tracked_.erase(command_buffer) != 0) {
			tracked_count_.store(tracked_.size(), std::memory_order_release);
		}
	}
}

void MarkerInterceptor::bind_descriptor_sets(VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
		VkPipelineLayout layout, uint32_t first_set, uint32_t set_count, const VkDescriptorSet *sets,
		uint32_t dynamic_offset_count, const uint32_t *dynamic_offsets) {
	next_.bind_descriptor_sets(command_buffer, bind_point, layout, first_set, set_count, sets, dynamic_offset_count,
			dynamic_offsets);
	if (bind_point != VK_PIPELINE_BIND_POINT_COMPUTE || tracked_count_.load(std::memory_order_acquire) == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = tracked_.find(command_buffer);
	if (it == tracked_.end()) {
		return;
	}
	Tracked &state = it->second;
	if (state.set_binds.size() >= MAX_TRACKED_CALLS || (set_count > 0 && sets == nullptr) ||
			(dynamic_offset_count > 0 && dynamic_offsets == nullptr)) {
		state.overflow = true;
		return;
	}
	SetBind bind;
	bind.layout = layout;
	bind.first_set = first_set;
	if (set_count > 0) {
		bind.sets.assign(sets, sets + set_count);
	}
	if (dynamic_offset_count > 0) {
		bind.dynamic_offsets.assign(dynamic_offsets, dynamic_offsets + dynamic_offset_count);
	}
	state.set_binds.push_back(std::move(bind));
}

void MarkerInterceptor::push_constants(VkCommandBuffer command_buffer, VkPipelineLayout layout, VkShaderStageFlags stages,
		uint32_t offset, uint32_t size, const void *values) {
	next_.push_constants(command_buffer, layout, stages, offset, size, values);
	if (tracked_count_.load(std::memory_order_acquire) == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = tracked_.find(command_buffer);
	if (it == tracked_.end()) {
		return;
	}
	Tracked &state = it->second;
	if (state.push_calls.size() >= MAX_TRACKED_CALLS || values == nullptr || offset > MARKER_MAX_PUSH_BYTES ||
			size > MARKER_MAX_PUSH_BYTES - offset) {
		state.overflow = true;
		return;
	}
	PushCall call;
	call.layout = layout;
	call.stages = stages;
	call.offset = offset;
	const uint8_t *bytes = static_cast<const uint8_t *>(values);
	call.bytes.assign(bytes, bytes + size);
	state.push_calls.push_back(std::move(call));
}

void MarkerInterceptor::dispatch(VkCommandBuffer command_buffer, uint32_t x, uint32_t y, uint32_t z) {
	if (tracked_count_.load(std::memory_order_acquire) == 0) {
		next_.dispatch(command_buffer, x, y, z);
		return;
	}
	Tracked state;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto it = tracked_.find(command_buffer);
		if (it != tracked_.end()) {
			state = it->second; // copied: the recorder runs without the lock
		}
	}
	if (state.recorder == nullptr) {
		next_.dispatch(command_buffer, x, y, z);
		return;
	}
	marker_dispatches_.fetch_add(1, std::memory_order_relaxed);
	if (state.overflow) {
		untracked_.fetch_add(1, std::memory_order_relaxed);
		next_.dispatch(command_buffer, x, y, z);
		return;
	}
	MarkerDispatch dispatch;
	dispatch.command_buffer = command_buffer;
	dispatch.marker = state.marker;
	dispatch.group_count_x = x;
	dispatch.group_count_y = y;
	dispatch.group_count_z = z;
	for (const PushCall &call : state.push_calls) {
		if (!call.bytes.empty()) {
			std::memcpy(dispatch.push_constants + call.offset, call.bytes.data(), call.bytes.size());
		}
		dispatch.push_constants_size =
				std::max(dispatch.push_constants_size, call.offset + static_cast<uint32_t>(call.bytes.size()));
	}
	for (const SetBind &bind : state.set_binds) {
		for (size_t i = 0; i < bind.sets.size(); ++i) {
			const size_t index = bind.first_set + i;
			if (index < MARKER_MAX_SETS) {
				dispatch.sets[index] = bind.sets[i];
			}
		}
	}
	if (!state.recorder->record(dispatch)) {
		declined_.fetch_add(1, std::memory_order_relaxed);
		next_.dispatch(command_buffer, x, y, z);
		return;
	}
	substituted_.fetch_add(1, std::memory_order_relaxed);
	restore(command_buffer, state);
}

void MarkerInterceptor::restore(VkCommandBuffer command_buffer, const Tracked &state) const {
	next_.bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.marker);
	for (const SetBind &bind : state.set_binds) {
		next_.bind_descriptor_sets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, bind.layout, bind.first_set,
				static_cast<uint32_t>(bind.sets.size()), bind.sets.empty() ? nullptr : bind.sets.data(),
				static_cast<uint32_t>(bind.dynamic_offsets.size()),
				bind.dynamic_offsets.empty() ? nullptr : bind.dynamic_offsets.data());
	}
	for (const PushCall &call : state.push_calls) {
		next_.push_constants(command_buffer, call.layout, call.stages, call.offset, static_cast<uint32_t>(call.bytes.size()),
				call.bytes.data());
	}
}

void MarkerInterceptor::forget(VkCommandBuffer command_buffer) {
	if (tracked_count_.load(std::memory_order_acquire) == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (tracked_.erase(command_buffer) != 0) {
		tracked_count_.store(tracked_.size(), std::memory_order_release);
	}
}

InterceptorStats MarkerInterceptor::stats() const {
	InterceptorStats s;
	s.marker_dispatches = marker_dispatches_.load(std::memory_order_relaxed);
	s.substituted = substituted_.load(std::memory_order_relaxed);
	s.declined = declined_.load(std::memory_order_relaxed);
	s.untracked = untracked_.load(std::memory_order_relaxed);
	return s;
}

size_t MarkerInterceptor::tracked_command_buffers() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return tracked_.size();
}

} // namespace lu::vk
