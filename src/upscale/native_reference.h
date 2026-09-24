// The reference upscaler recorded natively into Godot's command buffer: the MarkerRecorder that
// proves the Phase 2 recording point on any GPU before DLSS takes it over.
//
// The marker is the reference upscaler's own RenderingDevice pipeline. When Godot records its
// dispatch, record() records the same SPIR-V through the addon's own VkPipeline instead, with the
// images bound as push descriptors (VK_KHR_push_descriptor) in exactly the layouts Godot's render
// graph put them in for the marker's uniform set: sampled images in SHADER_READ_ONLY_OPTIMAL,
// storage images in GENERAL (rendering_device_graph.cpp:136-140, rendering_device_driver_vulkan.cpp
// uniform_set_create). The push constants are the marker's own; their `job` field names the images
// (set_job() is called when the marker dispatch is recorded into Godot's render graph).
//
// Pure C++ plus Vulkan headers; the device functions come from a vkGetDeviceProcAddr, so the unit
// tests (tests/unit/test_native_reference.cpp) run it against fakes.
#pragma once

#include "vk/marker_interceptor.h"
#include "vk/vk_common.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace lu::upscale {

// One dispatch's images, as Godot's image views (RenderingDevice::get_driver_resource).
struct ReferenceJob {
	VkImageView color = VK_NULL_HANDLE; // sampled: render size
	VkImageView depth = VK_NULL_HANDLE;
	VkImageView velocity = VK_NULL_HANDLE;
	VkImageView history_read = VK_NULL_HANDLE; // sampled: target size
	VkImageView output = VK_NULL_HANDLE; // storage: target size ("color_upscaled")
	VkImageView history_write = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE; // the marker's sampler, for identical filtering
};

enum class NativeMode {
	Record, // record natively (the marker dispatch is dropped)
	Decline, // record nothing: the marker dispatch runs (the RenderingDevice path)
	TestNegatedJitterX, // debug tests only: record with the jitter's x negated, so the image shows it is ours
};

struct NativeRecorderStats {
	uint64_t recorded = 0;
	uint64_t declined = 0; // NativeMode::Decline
	uint64_t missing_job = 0; // the dispatch's job id is unknown or already overwritten
	uint64_t not_created = 0; // record() before create() succeeded
};

class NativeReferenceRecorder : public vk::MarkerRecorder {
public:
	static constexpr uint32_t JOB_SLOTS = 16; // jobs kept (several frames' worth)

	// Creates the native pipeline from the reference shader's SPIR-V on `device`. Idempotent;
	// false with `error` when a function or an object is missing.
	bool create(VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr, const std::vector<uint32_t> &spirv,
			std::string &error);
	// Destroys the Vulkan objects; the device must be idle (called at vkDestroyDevice).
	void destroy();
	bool created() const;

	void set_job(uint32_t job, const ReferenceJob &images); // job != 0
	void set_mode(NativeMode mode) { mode_.store(mode); }
	NativeMode mode() const { return mode_.load(); }

	bool record(const vk::MarkerDispatch &dispatch) override;
	NativeRecorderStats stats() const;

private:
	struct Functions {
		PFN_vkCreateShaderModule CreateShaderModule = nullptr;
		PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
		PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
		PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
		PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
		PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
		PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
		PFN_vkDestroyPipeline DestroyPipeline = nullptr;
		PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
		PFN_vkCmdPushDescriptorSetKHR CmdPushDescriptorSetKHR = nullptr;
		PFN_vkCmdPushConstants CmdPushConstants = nullptr;
		PFN_vkCmdDispatch CmdDispatch = nullptr;
	};
	struct Slot {
		uint32_t job = 0;
		ReferenceJob images;
	};

	mutable std::mutex mutex_; // guards everything below except mode_
	VkDevice device_ = VK_NULL_HANDLE;
	Functions fn_;
	VkShaderModule module_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	Slot slots_[JOB_SLOTS];
	NativeRecorderStats stats_;
	std::atomic<NativeMode> mode_{ NativeMode::Record };
};

} // namespace lu::upscale
