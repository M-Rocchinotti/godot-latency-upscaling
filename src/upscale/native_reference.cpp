#include "upscale/native_reference.h"

#include "upscale/reference_params.h"

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace lu::upscale {
namespace {

constexpr uint32_t BINDING_COUNT = 6; // reference_upscaler_glsl.h, set 0
constexpr uint32_t SAMPLED_BINDINGS = 4; // 0..3 sampled, 4..5 storage

std::string vk_error(const char *what, VkResult result) {
	char buffer[160];
	std::snprintf(buffer, sizeof(buffer), "%s failed (VkResult %d)", what, static_cast<int>(result));
	return buffer;
}

} // namespace

bool NativeReferenceRecorder::create(VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr,
		const std::vector<uint32_t> &spirv, std::string &error) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (pipeline_ != VK_NULL_HANDLE) {
		if (device == device_) {
			return true;
		}
		error = "the native reference pipeline already exists on another device";
		return false;
	}
	if (device == VK_NULL_HANDLE || get_device_proc_addr == nullptr || spirv.empty()) {
		error = "no device or no SPIR-V for the native reference pipeline";
		return false;
	}
	Functions fn;
	auto load = [&](auto &function, const char *name) {
		function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(get_device_proc_addr(device, name));
		return function != nullptr;
	};
	const bool core = load(fn.CreateShaderModule, "vkCreateShaderModule") &&
			load(fn.DestroyShaderModule, "vkDestroyShaderModule") &&
			load(fn.CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout") &&
			load(fn.DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout") &&
			load(fn.CreatePipelineLayout, "vkCreatePipelineLayout") &&
			load(fn.DestroyPipelineLayout, "vkDestroyPipelineLayout") &&
			load(fn.CreateComputePipelines, "vkCreateComputePipelines") &&
			load(fn.DestroyPipeline, "vkDestroyPipeline") && load(fn.CmdBindPipeline, "vkCmdBindPipeline") &&
			load(fn.CmdPushConstants, "vkCmdPushConstants") && load(fn.CmdDispatch, "vkCmdDispatch");
	if (!core) {
		error = "the device does not provide the Vulkan functions of the native reference pipeline";
		return false;
	}
	if (!load(fn.CmdPushDescriptorSetKHR, "vkCmdPushDescriptorSetKHR")) {
		error = "vkCmdPushDescriptorSetKHR is not available (VK_KHR_push_descriptor is not enabled on the device)";
		return false;
	}

	VkShaderModule module = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	auto fail = [&](const std::string &message) {
		if (pipeline_layout != VK_NULL_HANDLE) {
			fn.DestroyPipelineLayout(device, pipeline_layout, nullptr);
		}
		if (set_layout != VK_NULL_HANDLE) {
			fn.DestroyDescriptorSetLayout(device, set_layout, nullptr);
		}
		if (module != VK_NULL_HANDLE) {
			fn.DestroyShaderModule(device, module, nullptr);
		}
		error = message;
		return false;
	};

	VkShaderModuleCreateInfo module_info{};
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = spirv.size() * sizeof(uint32_t);
	module_info.pCode = spirv.data();
	VkResult result = fn.CreateShaderModule(device, &module_info, nullptr, &module);
	if (result != VK_SUCCESS) {
		module = VK_NULL_HANDLE;
		return fail(vk_error("vkCreateShaderModule", result));
	}

	VkDescriptorSetLayoutBinding bindings[BINDING_COUNT]{};
	for (uint32_t i = 0; i < BINDING_COUNT; ++i) {
		bindings[i].binding = i;
		bindings[i].descriptorType =
				i < SAMPLED_BINDINGS ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo set_layout_info{};
	set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	set_layout_info.bindingCount = BINDING_COUNT;
	set_layout_info.pBindings = bindings;
	result = fn.CreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout);
	if (result != VK_SUCCESS) {
		set_layout = VK_NULL_HANDLE;
		return fail(vk_error("vkCreateDescriptorSetLayout", result));
	}

	VkPushConstantRange push_range{};
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.offset = 0;
	push_range.size = sizeof(ReferenceParams);
	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	result = fn.CreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout);
	if (result != VK_SUCCESS) {
		pipeline_layout = VK_NULL_HANDLE;
		return fail(vk_error("vkCreatePipelineLayout", result));
	}

	VkComputePipelineCreateInfo pipeline_info{};
	pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName = "main";
	pipeline_info.layout = pipeline_layout;
	result = fn.CreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
	if (result != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
		return fail(vk_error("vkCreateComputePipelines", result));
	}

	device_ = device;
	fn_ = fn;
	module_ = module;
	set_layout_ = set_layout;
	pipeline_layout_ = pipeline_layout;
	pipeline_ = pipeline;
	for (Slot &slot : slots_) {
		slot = Slot();
	}
	return true;
}

void NativeReferenceRecorder::destroy() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (device_ == VK_NULL_HANDLE) {
		return;
	}
	fn_.DestroyPipeline(device_, pipeline_, nullptr);
	fn_.DestroyPipelineLayout(device_, pipeline_layout_, nullptr);
	fn_.DestroyDescriptorSetLayout(device_, set_layout_, nullptr);
	fn_.DestroyShaderModule(device_, module_, nullptr);
	pipeline_ = VK_NULL_HANDLE;
	pipeline_layout_ = VK_NULL_HANDLE;
	set_layout_ = VK_NULL_HANDLE;
	module_ = VK_NULL_HANDLE;
	device_ = VK_NULL_HANDLE;
	fn_ = Functions();
	for (Slot &slot : slots_) {
		slot = Slot();
	}
}

bool NativeReferenceRecorder::created() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return pipeline_ != VK_NULL_HANDLE;
}

void NativeReferenceRecorder::set_job(uint32_t job, const ReferenceJob &images) {
	if (job == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	Slot &slot = slots_[job % JOB_SLOTS];
	slot.job = job;
	slot.images = images;
}

bool NativeReferenceRecorder::record(const vk::MarkerDispatch &dispatch) {
	const NativeMode mode = mode_.load();
	std::lock_guard<std::mutex> lock(mutex_);
	if (pipeline_ == VK_NULL_HANDLE) {
		stats_.not_created++;
		return false;
	}
	if (mode == NativeMode::Decline) {
		stats_.declined++;
		return false;
	}
	ReferenceParams params;
	if (dispatch.push_constants_size < sizeof(params)) {
		stats_.missing_job++;
		return false;
	}
	std::memcpy(&params, dispatch.push_constants, sizeof(params));
	const Slot &slot = slots_[params.job % JOB_SLOTS];
	const ReferenceJob &images = slot.images;
	if (params.job == 0 || slot.job != params.job || images.color == VK_NULL_HANDLE || images.depth == VK_NULL_HANDLE ||
			images.velocity == VK_NULL_HANDLE || images.history_read == VK_NULL_HANDLE || images.output == VK_NULL_HANDLE ||
			images.history_write == VK_NULL_HANDLE || images.sampler == VK_NULL_HANDLE) {
		stats_.missing_job++;
		return false;
	}
	if (mode == NativeMode::TestNegatedJitterX) {
		params.jitter[0] = -params.jitter[0];
	}

	const VkImageView views[BINDING_COUNT] = { images.color, images.depth, images.velocity, images.history_read,
		images.output, images.history_write };
	VkDescriptorImageInfo infos[BINDING_COUNT]{};
	VkWriteDescriptorSet writes[BINDING_COUNT]{};
	for (uint32_t i = 0; i < BINDING_COUNT; ++i) {
		const bool sampled = i < SAMPLED_BINDINGS;
		infos[i].sampler = sampled ? images.sampler : VK_NULL_HANDLE;
		infos[i].imageView = views[i];
		// The layouts Godot's render graph transitions the marker's uniform set to.
		infos[i].imageLayout = sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].pImageInfo = &infos[i];
	}
	const VkCommandBuffer cb = dispatch.command_buffer;
	fn_.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
	fn_.CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, BINDING_COUNT, writes);
	fn_.CmdPushConstants(cb, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
	fn_.CmdDispatch(cb, dispatch.group_count_x, dispatch.group_count_y, dispatch.group_count_z);
	stats_.recorded++;
	return true;
}

NativeRecorderStats NativeReferenceRecorder::stats() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return stats_;
}

} // namespace lu::upscale
