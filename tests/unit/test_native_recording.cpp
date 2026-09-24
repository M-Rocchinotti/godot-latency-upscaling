// The native reference recorder and the upscaler's VulkanFeature against a fake device: the
// pipeline it builds (push-descriptor layout, push-constant range), the commands it records for a
// marker dispatch (Godot's image layouts, the marker's push constants), when it declines, and what
// the feature asks of vkCreateDevice.
#include "upscale/native_reference.h"
#include "upscale/reference_params.h"
#include "upscale/upscale_feature.h"
#include "test_framework.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace lu;
using namespace lu::upscale;

namespace {

template <typename Handle>
Handle handle(uintptr_t value) {
	return reinterpret_cast<Handle>(value);
}

// --- Fake device ----------------------------------------------------------------------------

struct FakeDevice {
	bool offer_push_descriptor = true;
	VkResult pipeline_result = VK_SUCCESS;
	uintptr_t next_handle = 0x5000;
	// create info seen
	size_t code_size = 0;
	VkDescriptorSetLayoutCreateFlags set_layout_flags = 0;
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	VkPushConstantRange push_range{};
	uint32_t set_layout_count = 0;
	VkShaderModule pipeline_module = VK_NULL_HANDLE;
	std::string entry_point;
	std::map<std::string, int> created, destroyed;
	// recorded commands
	std::vector<std::string> commands;
	VkPipeline bound = VK_NULL_HANDLE;
	std::vector<VkDescriptorImageInfo> pushed_images;
	std::vector<VkDescriptorType> pushed_types;
	std::vector<uint32_t> pushed_bindings;
	std::vector<uint8_t> pushed_constants;
	uint32_t groups[3] = {};
};
FakeDevice g_dev;
const VkDevice DEVICE = handle<VkDevice>(0xD0);

VKAPI_ATTR VkResult VKAPI_CALL f_create_module(VkDevice, const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *,
		VkShaderModule *out) {
	g_dev.code_size = info->codeSize;
	g_dev.created["module"]++;
	*out = handle<VkShaderModule>(g_dev.next_handle++);
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL f_destroy_module(VkDevice, VkShaderModule m, const VkAllocationCallbacks *) {
	if (m != VK_NULL_HANDLE) {
		g_dev.destroyed["module"]++;
	}
}
VKAPI_ATTR VkResult VKAPI_CALL f_create_set_layout(VkDevice, const VkDescriptorSetLayoutCreateInfo *info,
		const VkAllocationCallbacks *, VkDescriptorSetLayout *out) {
	g_dev.set_layout_flags = info->flags;
	g_dev.bindings.assign(info->pBindings, info->pBindings + info->bindingCount);
	g_dev.created["set_layout"]++;
	*out = handle<VkDescriptorSetLayout>(g_dev.next_handle++);
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL f_destroy_set_layout(VkDevice, VkDescriptorSetLayout l, const VkAllocationCallbacks *) {
	if (l != VK_NULL_HANDLE) {
		g_dev.destroyed["set_layout"]++;
	}
}
VKAPI_ATTR VkResult VKAPI_CALL f_create_pipeline_layout(VkDevice, const VkPipelineLayoutCreateInfo *info,
		const VkAllocationCallbacks *, VkPipelineLayout *out) {
	g_dev.set_layout_count = info->setLayoutCount;
	g_dev.push_range = info->pushConstantRangeCount == 1 ? info->pPushConstantRanges[0] : VkPushConstantRange{};
	g_dev.created["pipeline_layout"]++;
	*out = handle<VkPipelineLayout>(g_dev.next_handle++);
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL f_destroy_pipeline_layout(VkDevice, VkPipelineLayout l, const VkAllocationCallbacks *) {
	if (l != VK_NULL_HANDLE) {
		g_dev.destroyed["pipeline_layout"]++;
	}
}
VKAPI_ATTR VkResult VKAPI_CALL f_create_compute_pipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *info,
		const VkAllocationCallbacks *, VkPipeline *out) {
	g_dev.pipeline_module = info->stage.module;
	g_dev.entry_point = info->stage.pName;
	if (g_dev.pipeline_result != VK_SUCCESS) {
		*out = VK_NULL_HANDLE;
		return g_dev.pipeline_result;
	}
	g_dev.created["pipeline"]++;
	*out = handle<VkPipeline>(g_dev.next_handle++);
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL f_destroy_pipeline(VkDevice, VkPipeline p, const VkAllocationCallbacks *) {
	if (p != VK_NULL_HANDLE) {
		g_dev.destroyed["pipeline"]++;
	}
}
VKAPI_ATTR void VKAPI_CALL f_bind_pipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline p) {
	g_dev.commands.push_back("bind");
	g_dev.bound = p;
}
VKAPI_ATTR void VKAPI_CALL f_push_descriptor(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t set,
		uint32_t count, const VkWriteDescriptorSet *writes) {
	g_dev.commands.push_back("push_descriptor set=" + std::to_string(set));
	g_dev.pushed_images.clear();
	g_dev.pushed_types.clear();
	g_dev.pushed_bindings.clear();
	for (uint32_t i = 0; i < count; ++i) {
		g_dev.pushed_images.push_back(writes[i].pImageInfo[0]);
		g_dev.pushed_types.push_back(writes[i].descriptorType);
		g_dev.pushed_bindings.push_back(writes[i].dstBinding);
	}
}
VKAPI_ATTR void VKAPI_CALL f_push_constants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t offset, uint32_t size,
		const void *values) {
	g_dev.commands.push_back("push_constants offset=" + std::to_string(offset));
	const uint8_t *bytes = static_cast<const uint8_t *>(values);
	g_dev.pushed_constants.assign(bytes, bytes + size);
}
VKAPI_ATTR void VKAPI_CALL f_dispatch(VkCommandBuffer, uint32_t x, uint32_t y, uint32_t z) {
	g_dev.commands.push_back("dispatch");
	g_dev.groups[0] = x;
	g_dev.groups[1] = y;
	g_dev.groups[2] = z;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_gdpa(VkDevice device, const char *name) {
	if (device != DEVICE) {
		return nullptr;
	}
	static const std::map<std::string, PFN_vkVoidFunction> table = {
		{ "vkCreateShaderModule", reinterpret_cast<PFN_vkVoidFunction>(&f_create_module) },
		{ "vkDestroyShaderModule", reinterpret_cast<PFN_vkVoidFunction>(&f_destroy_module) },
		{ "vkCreateDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(&f_create_set_layout) },
		{ "vkDestroyDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(&f_destroy_set_layout) },
		{ "vkCreatePipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(&f_create_pipeline_layout) },
		{ "vkDestroyPipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(&f_destroy_pipeline_layout) },
		{ "vkCreateComputePipelines", reinterpret_cast<PFN_vkVoidFunction>(&f_create_compute_pipelines) },
		{ "vkDestroyPipeline", reinterpret_cast<PFN_vkVoidFunction>(&f_destroy_pipeline) },
		{ "vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(&f_bind_pipeline) },
		{ "vkCmdPushDescriptorSetKHR", reinterpret_cast<PFN_vkVoidFunction>(&f_push_descriptor) },
		{ "vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(&f_push_constants) },
		{ "vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(&f_dispatch) },
	};
	if (!g_dev.offer_push_descriptor && std::strcmp(name, "vkCmdPushDescriptorSetKHR") == 0) {
		return nullptr;
	}
	const auto it = table.find(name);
	return it != table.end() ? it->second : nullptr;
}

const std::vector<uint32_t> SPIRV = { 0x07230203, 0x00010000, 0, 1, 0 }; // content is the fake's business

ReferenceJob sample_job() {
	ReferenceJob job;
	job.color = handle<VkImageView>(0x11);
	job.depth = handle<VkImageView>(0x12);
	job.velocity = handle<VkImageView>(0x13);
	job.history_read = handle<VkImageView>(0x14);
	job.output = handle<VkImageView>(0x15);
	job.history_write = handle<VkImageView>(0x16);
	job.sampler = handle<VkSampler>(0x17);
	return job;
}

vk::MarkerDispatch dispatch_for(uint32_t job, float jitter_x = 0.25f, float jitter_y = -0.125f) {
	ReferenceParams params;
	params.render_size[0] = 960;
	params.render_size[1] = 540;
	params.target_size[0] = 1920;
	params.target_size[1] = 1080;
	params.jitter[0] = jitter_x;
	params.jitter[1] = jitter_y;
	params.job = job;
	vk::MarkerDispatch d;
	d.command_buffer = handle<VkCommandBuffer>(0xC0);
	d.marker = handle<VkPipeline>(0xAA);
	std::memcpy(d.push_constants, &params, sizeof(params));
	d.push_constants_size = sizeof(params);
	d.group_count_x = 240;
	d.group_count_y = 135;
	d.group_count_z = 1;
	return d;
}

ReferenceParams pushed_params() {
	ReferenceParams p;
	if (g_dev.pushed_constants.size() == sizeof(p)) {
		std::memcpy(&p, g_dev.pushed_constants.data(), sizeof(p));
	}
	return p;
}

} // namespace

TEST(native_reference_needs_push_descriptors) {
	g_dev = FakeDevice();
	g_dev.offer_push_descriptor = false;
	NativeReferenceRecorder recorder;
	std::string error;
	CHECK(!recorder.create(DEVICE, &fake_gdpa, SPIRV, error));
	CHECK(error.find("VK_KHR_push_descriptor") != std::string::npos);
	CHECK(g_dev.created.empty());
	CHECK(!recorder.created());
	CHECK(!recorder.record(dispatch_for(1)));
	CHECK_EQ(recorder.stats().not_created, 1u);
}

TEST(native_reference_builds_a_push_descriptor_pipeline_from_the_spirv) {
	g_dev = FakeDevice();
	NativeReferenceRecorder recorder;
	std::string error;
	CHECK(recorder.create(DEVICE, &fake_gdpa, SPIRV, error));
	CHECK(error.empty());
	CHECK(recorder.created());
	CHECK_EQ(g_dev.code_size, SPIRV.size() * 4);
	CHECK(g_dev.set_layout_flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	CHECK_EQ(g_dev.bindings.size(), static_cast<size_t>(6));
	for (size_t i = 0; i < g_dev.bindings.size(); ++i) {
		const VkDescriptorSetLayoutBinding &b = g_dev.bindings[i];
		CHECK_EQ(b.binding, static_cast<uint32_t>(i));
		CHECK_EQ(b.descriptorCount, 1u);
		CHECK_EQ(static_cast<int>(b.stageFlags), static_cast<int>(VK_SHADER_STAGE_COMPUTE_BIT));
		// reference_upscaler_glsl.h: 0-3 sampler2D, 4-5 image2D
		CHECK_EQ(static_cast<int>(b.descriptorType),
				static_cast<int>(i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));
	}
	CHECK_EQ(g_dev.set_layout_count, 1u);
	CHECK_EQ(g_dev.push_range.offset, 0u);
	CHECK_EQ(g_dev.push_range.size, static_cast<uint32_t>(sizeof(ReferenceParams)));
	CHECK_EQ(static_cast<int>(g_dev.push_range.stageFlags), static_cast<int>(VK_SHADER_STAGE_COMPUTE_BIT));
	CHECK_EQ(g_dev.entry_point, std::string("main"));
	CHECK(g_dev.pipeline_module != VK_NULL_HANDLE);

	// Idempotent on the same device, refused on another.
	CHECK(recorder.create(DEVICE, &fake_gdpa, SPIRV, error));
	CHECK_EQ(g_dev.created["pipeline"], 1);
	CHECK(!recorder.create(handle<VkDevice>(0xD1), &fake_gdpa, SPIRV, error));

	recorder.destroy();
	CHECK(!recorder.created());
	for (const char *kind : { "module", "set_layout", "pipeline_layout", "pipeline" }) {
		CHECK_EQ(g_dev.created[kind], 1);
		CHECK_EQ(g_dev.destroyed[kind], 1);
	}
	recorder.destroy(); // idempotent
	CHECK_EQ(g_dev.destroyed["pipeline"], 1);
}

TEST(native_reference_cleans_up_a_failed_pipeline) {
	g_dev = FakeDevice();
	g_dev.pipeline_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	NativeReferenceRecorder recorder;
	std::string error;
	CHECK(!recorder.create(DEVICE, &fake_gdpa, SPIRV, error));
	CHECK(error.find("vkCreateComputePipelines") != std::string::npos);
	CHECK(!recorder.created());
	for (const char *kind : { "module", "set_layout", "pipeline_layout" }) {
		CHECK_EQ(g_dev.created[kind], 1);
		CHECK_EQ(g_dev.destroyed[kind], 1);
	}
}

TEST(native_reference_records_the_job_in_godots_layouts) {
	g_dev = FakeDevice();
	NativeReferenceRecorder recorder;
	std::string error;
	CHECK(recorder.create(DEVICE, &fake_gdpa, SPIRV, error));
	const ReferenceJob job = sample_job();
	recorder.set_job(7, job);
	const vk::MarkerDispatch d = dispatch_for(7);
	CHECK(recorder.record(d));
	const std::vector<std::string> expected = { "bind", "push_descriptor set=0", "push_constants offset=0", "dispatch" };
	CHECK(g_dev.commands == expected);
	CHECK(g_dev.bound != VK_NULL_HANDLE && g_dev.bound != d.marker); // its own pipeline, not the marker
	const VkImageView views[6] = { job.color, job.depth, job.velocity, job.history_read, job.output, job.history_write };
	CHECK_EQ(g_dev.pushed_images.size(), static_cast<size_t>(6));
	for (size_t i = 0; i < g_dev.pushed_images.size() && i < 6; ++i) {
		const VkDescriptorImageInfo &info = g_dev.pushed_images[i];
		const bool sampled = i < 4;
		CHECK_EQ(g_dev.pushed_bindings[i], static_cast<uint32_t>(i));
		CHECK(info.imageView == views[i]);
		CHECK(info.sampler == (sampled ? job.sampler : VK_NULL_HANDLE));
		// The layouts Godot's render graph gives the marker's uniform set.
		CHECK_EQ(static_cast<int>(info.imageLayout),
				static_cast<int>(sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL));
		CHECK_EQ(static_cast<int>(g_dev.pushed_types[i]),
				static_cast<int>(sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));
	}
	// The marker's push constants, byte for byte, and its group counts.
	CHECK_EQ(g_dev.pushed_constants.size(), sizeof(ReferenceParams));
	CHECK(std::memcmp(g_dev.pushed_constants.data(), d.push_constants, sizeof(ReferenceParams)) == 0);
	CHECK_EQ(g_dev.groups[0], 240u);
	CHECK_EQ(g_dev.groups[1], 135u);
	CHECK_EQ(g_dev.groups[2], 1u);
	CHECK_EQ(recorder.stats().recorded, 1u);
}

TEST(native_reference_declines_what_it_cannot_record) {
	g_dev = FakeDevice();
	NativeReferenceRecorder recorder;
	std::string error;
	CHECK(recorder.create(DEVICE, &fake_gdpa, SPIRV, error));
	recorder.set_job(7, sample_job());
	CHECK(!recorder.record(dispatch_for(0))); // no job
	CHECK(!recorder.record(dispatch_for(9))); // never registered
	recorder.set_job(7 + NativeReferenceRecorder::JOB_SLOTS, sample_job()); // overwrites job 7's slot
	CHECK(!recorder.record(dispatch_for(7)));
	CHECK(recorder.record(dispatch_for(7 + NativeReferenceRecorder::JOB_SLOTS)));
	vk::MarkerDispatch short_push = dispatch_for(7 + NativeReferenceRecorder::JOB_SLOTS);
	short_push.push_constants_size = 40; // the job id is not in it
	CHECK(!recorder.record(short_push));
	ReferenceJob incomplete = sample_job();
	incomplete.velocity = VK_NULL_HANDLE;
	recorder.set_job(3, incomplete);
	CHECK(!recorder.record(dispatch_for(3)));
	recorder.set_job(0, sample_job()); // ignored: 0 means "no job"
	CHECK(!recorder.record(dispatch_for(0)));
	CHECK_EQ(recorder.stats().missing_job, 6u);
	CHECK_EQ(recorder.stats().recorded, 1u);

	g_dev.commands.clear();
	recorder.set_mode(NativeMode::Decline);
	CHECK(!recorder.record(dispatch_for(7 + NativeReferenceRecorder::JOB_SLOTS)));
	CHECK_EQ(recorder.stats().declined, 1u);
	CHECK(g_dev.commands.empty());
}

TEST(native_reference_test_mode_negates_only_the_jitter_x) {
	g_dev = FakeDevice();
	NativeReferenceRecorder recorder;
	std::string error;
	CHECK(recorder.create(DEVICE, &fake_gdpa, SPIRV, error));
	recorder.set_job(5, sample_job());
	recorder.set_mode(NativeMode::TestNegatedJitterX);
	CHECK(recorder.record(dispatch_for(5, 0.25f, -0.125f)));
	const ReferenceParams p = pushed_params();
	CHECK_NEAR(p.jitter[0], -0.25f, 0.0);
	CHECK_NEAR(p.jitter[1], -0.125f, 0.0);
	CHECK_EQ(p.job, 5u);
	recorder.set_mode(NativeMode::Record);
	CHECK(recorder.record(dispatch_for(5, 0.25f, -0.125f)));
	CHECK_NEAR(pushed_params().jitter[0], 0.25f, 0.0);
}

// --- UpscaleFeature ---------------------------------------------------------------------------

namespace {

vk::PhysicalDeviceCaps caps_with(bool push_descriptor) {
	vk::PhysicalDeviceCaps caps;
	caps.name = "fake GPU";
	caps.vendor_id = 0x10DE;
	if (push_descriptor) {
		caps.extensions[VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME] = 2;
	}
	return caps;
}

vk::DeviceContext context_for(bool push_descriptor_enabled) {
	vk::DeviceContext context;
	context.device = DEVICE;
	context.get_device_proc_addr = &fake_gdpa;
	if (push_descriptor_enabled) {
		context.enabled_extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	}
	return context;
}

} // namespace

TEST(upscale_feature_asks_for_push_descriptors_only_when_native_recording_is_used) {
	UpscaleFeature feature;
	vk::DeviceRequest request;
	std::string reason;
	CHECK(!feature.request_device(caps_with(true), request, reason)); // not configured for native recording
	CHECK(request.empty());
	CHECK(!reason.empty());

	feature.configure(true);
	reason.clear();
	CHECK(!feature.request_device(caps_with(false), request, reason));
	CHECK(reason.find(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) != std::string::npos);
	CHECK(request.empty());

	CHECK(feature.request_device(caps_with(true), request, reason));
	CHECK_EQ(request.extensions.size(), static_cast<size_t>(1));
	CHECK_EQ(request.extensions[0], std::string(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME));

	// The debug knob that hides an extension (the fallback tests) makes it count as not offered.
	UpscaleFeature hiding;
	hiding.configure(true, { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME });
	vk::DeviceRequest untouched;
	CHECK(!hiding.request_device(caps_with(true), untouched, reason));
	CHECK(reason.find(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) != std::string::npos);
	CHECK(untouched.empty());
}

TEST(upscale_feature_reports_why_native_recording_is_unavailable) {
	UpscaleFeature feature;
	std::string reason;
	CHECK(!feature.native_ready(reason));
	CHECK(reason.find("not used") != std::string::npos);

	feature.configure(true);
	CHECK(!feature.native_ready(reason));
	CHECK(reason.find("not created through") != std::string::npos);

	feature.on_device_created(context_for(false), false, "the GPU does not offer VK_KHR_push_descriptor");
	CHECK(!feature.native_ready(reason));
	CHECK_EQ(reason, std::string("the GPU does not offer VK_KHR_push_descriptor"));
	std::string error;
	CHECK(!feature.ensure_reference(SPIRV, error));

	// Accepted, but the device ended up without the extension (the layer's unpatched fallback).
	feature.on_device_created(context_for(false), true, "vkCreateDevice with the addon's extensions failed");
	CHECK(!feature.native_ready(reason));
	CHECK(reason.find("failed") != std::string::npos);

	feature.on_device_created(context_for(true), true, "");
	CHECK(feature.native_ready(reason));
}

TEST(upscale_feature_destroys_the_native_pipeline_with_its_device) {
	g_dev = FakeDevice();
	UpscaleFeature feature;
	feature.configure(true);
	feature.on_device_created(context_for(true), true, "");
	std::string error;
	CHECK(feature.ensure_reference(SPIRV, error));
	CHECK(feature.reference_recorder().created());
	feature.on_device_destroyed(handle<VkDevice>(0xD9)); // another (local) device
	CHECK(feature.reference_recorder().created());
	feature.on_device_destroyed(DEVICE);
	CHECK(!feature.reference_recorder().created());
	CHECK_EQ(g_dev.destroyed["pipeline"], 1);
	std::string reason;
	CHECK(!feature.native_ready(reason));
}
