#include "test_framework.h"
#include "vk/device_patch.h"

#include <cstring>
#include <map>

using namespace lu::vk;

namespace {

std::map<VkStructureType, int> count_chain(const void *next) {
	std::map<VkStructureType, int> counts;
	for (auto *s = static_cast<const VkBaseInStructure *>(next); s != nullptr; s = s->pNext) {
		counts[s->sType]++;
	}
	return counts;
}

DeviceRequest reflex_request() {
	DeviceRequest r;
	r.add_extension(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
	r.add_extension(VK_KHR_PRESENT_ID_EXTENSION_NAME);
	r.features.timeline_semaphore = true;
	r.features.present_id = true;
	return r;
}

// What Godot 4.7 chains on a Vulkan 1.2+ device (rendering_device_driver_vulkan.cpp:1339-1488):
// promoted per-feature structures plus VkPhysicalDeviceVulkan11Features, never 1.2's.
struct GodotLikeChain {
	VkPhysicalDeviceShaderFloat16Int8Features float16{};
	VkPhysicalDeviceBufferDeviceAddressFeatures bda{};
	VkPhysicalDeviceVulkan11Features v11{};
	VkPhysicalDeviceFeatures core{};
	const char *extensions[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME };
	VkDeviceCreateInfo info{};

	GodotLikeChain() {
		float16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
		bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
		bda.pNext = &float16;
		bda.bufferDeviceAddress = VK_TRUE;
		v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
		v11.pNext = &bda;
		info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		info.pNext = &v11;
		info.enabledExtensionCount = 2;
		info.ppEnabledExtensionNames = extensions;
		info.pEnabledFeatures = &core;
	}
	GodotLikeChain(const GodotLikeChain &) = delete;
};

} // namespace

TEST(device_patch_adds_extensions_and_prepends_feature_structs) {
	GodotLikeChain godot;
	DeviceCreatePatch patch(godot.info, reflex_request());
	const VkDeviceCreateInfo *out = patch.get();

	CHECK(patch.changed());
	CHECK_EQ(out->enabledExtensionCount, 4u);
	CHECK(std::strcmp(out->ppEnabledExtensionNames[0], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0);
	CHECK(std::strcmp(out->ppEnabledExtensionNames[1], VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0);
	CHECK(std::strcmp(out->ppEnabledExtensionNames[2], VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0);
	CHECK(std::strcmp(out->ppEnabledExtensionNames[3], VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0);
	CHECK_EQ(patch.added_extensions().size(), static_cast<size_t>(2));
	CHECK_EQ(patch.enabled_extensions().size(), static_cast<size_t>(4));

	const auto counts = count_chain(out->pNext);
	CHECK_EQ(counts.at(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES), 1);
	CHECK_EQ(counts.at(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR), 1);
	CHECK_EQ(counts.at(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES), 1);
	CHECK(counts.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) == 0);
	for (const auto &entry : counts) {
		CHECK_EQ(entry.second, 1); // no structure type twice
	}
	auto *timeline = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures *>(
			find_in_chain(out->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES));
	auto *present_id = reinterpret_cast<const VkPhysicalDevicePresentIdFeaturesKHR *>(
			find_in_chain(out->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR));
	CHECK(timeline != nullptr && timeline->timelineSemaphore == VK_TRUE);
	CHECK(present_id != nullptr && present_id->presentId == VK_TRUE);
	// The host's own chain is still reachable, unmodified, behind the new structures.
	CHECK(find_in_chain(out->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) ==
			reinterpret_cast<const VkBaseInStructure *>(&godot.bda));
	CHECK(out->pEnabledFeatures == &godot.core);
	CHECK_EQ(out->queueCreateInfoCount, godot.info.queueCreateInfoCount);

	// The caller's create info is untouched.
	CHECK(godot.info.pNext == &godot.v11);
	CHECK_EQ(godot.info.enabledExtensionCount, 2u);
	CHECK(godot.info.ppEnabledExtensionNames == godot.extensions);
	CHECK(godot.v11.pNext == &godot.bda);
}

TEST(device_patch_does_not_duplicate_extensions_the_host_enabled) {
	GodotLikeChain godot;
	godot.extensions[1] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
	DeviceCreatePatch patch(godot.info, reflex_request());
	CHECK_EQ(patch.get()->enabledExtensionCount, 3u);
	CHECK_EQ(patch.added_extensions().size(), static_cast<size_t>(1));
	CHECK_EQ(patch.added_extensions()[0], std::string(VK_NV_LOW_LATENCY_2_EXTENSION_NAME));
}

TEST(device_patch_sets_bits_in_an_existing_vulkan12_struct_and_restores_them) {
	GodotLikeChain godot;
	// A host that chains VkPhysicalDeviceVulkan12Features instead of the promoted structures.
	VkPhysicalDeviceVulkan12Features v12{};
	v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	v12.pNext = &godot.v11;
	v12.timelineSemaphore = VK_FALSE;
	godot.info.pNext = &v12;
	{
		DeviceCreatePatch patch(godot.info, reflex_request());
		const auto counts = count_chain(patch.get()->pNext);
		CHECK(counts.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) == 0); // would be invalid next to 1.2
		CHECK_EQ(counts.at(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR), 1);
		CHECK(v12.timelineSemaphore == VK_TRUE); // set in place for the call
		patch.restore();
		CHECK(v12.timelineSemaphore == VK_FALSE);
		patch.restore(); // idempotent
		CHECK(v12.timelineSemaphore == VK_FALSE);
	}
	{
		DeviceCreatePatch patch(godot.info, reflex_request());
		CHECK(v12.timelineSemaphore == VK_TRUE);
	} // destructor restores
	CHECK(v12.timelineSemaphore == VK_FALSE);
}

TEST(device_patch_edits_existing_feature_structs_instead_of_adding_duplicates) {
	GodotLikeChain godot;
	VkPhysicalDevicePresentIdFeaturesKHR present_id{};
	present_id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
	timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
	timeline.pNext = &present_id;
	present_id.pNext = &godot.v11;
	godot.info.pNext = &timeline;
	DeviceCreatePatch patch(godot.info, reflex_request());
	const auto counts = count_chain(patch.get()->pNext);
	CHECK_EQ(counts.at(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES), 1);
	CHECK_EQ(counts.at(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR), 1);
	CHECK(patch.get()->pNext == &timeline); // nothing prepended
	CHECK(timeline.timelineSemaphore == VK_TRUE && present_id.presentId == VK_TRUE);
	patch.restore();
	CHECK(timeline.timelineSemaphore == VK_FALSE && present_id.presentId == VK_FALSE);
}

TEST(device_patch_leaves_already_enabled_bits_alone) {
	GodotLikeChain godot;
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
	timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
	timeline.timelineSemaphore = VK_TRUE;
	timeline.pNext = &godot.v11;
	godot.info.pNext = &timeline;
	DeviceRequest request;
	request.features.timeline_semaphore = true;
	DeviceCreatePatch patch(godot.info, request);
	CHECK(!patch.changed());
	CHECK(patch.get()->pNext == &timeline);
}

TEST(device_patch_with_an_empty_request_is_a_pass_through) {
	GodotLikeChain godot;
	DeviceCreatePatch patch(godot.info, DeviceRequest{});
	CHECK(!patch.changed());
	CHECK(patch.get()->pNext == godot.info.pNext);
	CHECK(patch.get()->ppEnabledExtensionNames == godot.info.ppEnabledExtensionNames);
	CHECK_EQ(patch.get()->enabledExtensionCount, godot.info.enabledExtensionCount);
	CHECK(DeviceRequest{}.empty());
}

TEST(device_request_ignores_duplicate_extensions) {
	DeviceRequest r;
	r.add_extension("VK_A");
	r.add_extension("VK_B");
	r.add_extension("VK_A");
	CHECK_EQ(r.extensions.size(), static_cast<size_t>(2));
	CHECK(!r.empty());
}

// --- buffer device address (NGX lists it among its runtime requirements) --------------------------

TEST(device_patch_sets_buffer_device_address_where_the_host_chains_it) {
	GodotLikeChain godot;
	godot.bda.bufferDeviceAddress = VK_FALSE;
	DeviceRequest request;
	request.features.buffer_device_address = true;
	{
		DeviceCreatePatch patch(godot.info, request);
		CHECK(patch.changed());
		CHECK(godot.bda.bufferDeviceAddress == VK_TRUE); // Godot's own structure, set in place
		CHECK_EQ(count_chain(patch.get()->pNext).at(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES), 1);
	}
	CHECK(godot.bda.bufferDeviceAddress == VK_FALSE); // restored

	// Without one in the host's chain, the patch prepends its own.
	godot.v11.pNext = &godot.float16;
	DeviceCreatePatch patch(godot.info, request);
	auto *bda = reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures *>(
			find_in_chain(patch.get()->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES));
	CHECK(bda != nullptr && bda->bufferDeviceAddress == VK_TRUE && bda != &godot.bda);
}

// --- queues (Streamline's Frame Generation asks for an optical-flow queue, for instance) ---------

namespace {

// An NVIDIA-like layout: graphics+compute+transfer (16), compute+transfer (8), transfer (2),
// video decode (4), optical flow (1).
std::vector<VkQueueFamilyProperties> nvidia_like_families() {
	std::vector<VkQueueFamilyProperties> f(5);
	f[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
	f[0].queueCount = 16;
	f[1].queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
	f[1].queueCount = 8;
	f[2].queueFlags = VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
	f[2].queueCount = 2;
	f[3].queueFlags = VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT;
	f[3].queueCount = 4;
	f[4].queueFlags = VK_QUEUE_OPTICAL_FLOW_BIT_NV;
	f[4].queueCount = 1;
	return f;
}

// What Godot creates: one graphics queue (family 0).
struct GodotQueues {
	float priorities[1] = { 1.0f };
	VkDeviceQueueCreateInfo main{};
	GodotLikeChain chain;
	GodotQueues() {
		main.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		main.queueFamilyIndex = 0;
		main.queueCount = 1;
		main.pQueuePriorities = priorities;
		chain.info.queueCreateInfoCount = 1;
		chain.info.pQueueCreateInfos = &main;
	}
};

const VkDeviceQueueCreateInfo *entry_for(const VkDeviceCreateInfo *info, uint32_t family) {
	for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
		if (info->pQueueCreateInfos[i].queueFamilyIndex == family) {
			return &info->pQueueCreateInfos[i];
		}
	}
	return nullptr;
}

} // namespace

TEST(device_patch_places_queues_in_the_most_specific_family) {
	GodotQueues godot;
	DeviceRequest request;
	request.queues.push_back({ VK_QUEUE_OPTICAL_FLOW_BIT_NV, 1 });
	request.queues.push_back({ VK_QUEUE_COMPUTE_BIT, 1 });
	request.queues.push_back({ VK_QUEUE_GRAPHICS_BIT, 1 });
	DeviceCreatePatch patch(godot.chain.info, request, nvidia_like_families());
	CHECK(patch.changed());
	const std::vector<QueueAssignment> &q = patch.queue_assignments();
	CHECK_EQ(q.size(), static_cast<size_t>(3));
	// Optical flow: its own family. Compute: the async-compute family, not the graphics one.
	CHECK(q[0].assigned && q[0].family == 4 && q[0].first_index == 0 && q[0].count == 1);
	CHECK(q[1].assigned && q[1].family == 1 && q[1].first_index == 0);
	// Graphics shares Godot's family: after Godot's queue 0, so Godot's queue is unchanged.
	CHECK(q[2].assigned && q[2].family == 0 && q[2].first_index == 1);

	const VkDeviceCreateInfo *out = patch.get();
	CHECK_EQ(out->queueCreateInfoCount, 3u);
	const VkDeviceQueueCreateInfo *graphics = entry_for(out, 0);
	CHECK(graphics != nullptr && graphics->queueCount == 2 && graphics->pQueuePriorities != godot.priorities);
	CHECK(graphics != nullptr && graphics->pQueuePriorities[0] == 1.0f && graphics->pQueuePriorities[1] == 1.0f);
	const VkDeviceQueueCreateInfo *flow = entry_for(out, 4);
	CHECK(flow != nullptr && flow->queueCount == 1 && flow->sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
	// Godot's own array is untouched.
	CHECK_EQ(godot.main.queueCount, 1u);
	CHECK(godot.main.pQueuePriorities == godot.priorities);
	CHECK(godot.chain.info.pQueueCreateInfos == &godot.main);
}

TEST(device_patch_keeps_the_hosts_priorities_and_counts_its_queues) {
	GodotQueues godot;
	float host_priorities[2] = { 1.0f, 0.25f };
	godot.main.queueFamilyIndex = 1; // a host that already uses two compute queues
	godot.main.queueCount = 2;
	godot.main.pQueuePriorities = host_priorities;
	DeviceRequest request;
	request.queues.push_back({ VK_QUEUE_COMPUTE_BIT, 2 });
	DeviceCreatePatch patch(godot.chain.info, request, nvidia_like_families());
	const QueueAssignment &a = patch.queue_assignments()[0];
	CHECK(a.assigned && a.family == 1 && a.first_index == 2 && a.count == 2);
	const VkDeviceQueueCreateInfo *compute = entry_for(patch.get(), 1);
	CHECK(compute != nullptr && compute->queueCount == 4);
	CHECK(compute != nullptr && compute->pQueuePriorities[0] == 1.0f && compute->pQueuePriorities[1] == 0.25f);
}

TEST(device_patch_leaves_queue_requests_it_cannot_serve_unassigned) {
	GodotQueues godot;
	DeviceRequest request;
	request.queues.push_back({ VK_QUEUE_OPTICAL_FLOW_BIT_NV, 2 }); // only one exists
	request.queues.push_back({ VK_QUEUE_OPTICAL_FLOW_BIT_NV, 1 });
	request.queues.push_back({ VK_QUEUE_OPTICAL_FLOW_BIT_NV, 1 }); // the one is taken now
	std::vector<VkQueueFamilyProperties> families = nvidia_like_families();
	DeviceCreatePatch patch(godot.chain.info, request, families);
	const std::vector<QueueAssignment> &q = patch.queue_assignments();
	CHECK(!q[0].assigned);
	CHECK(q[1].assigned && q[1].family == 4);
	CHECK(!q[2].assigned);
	CHECK_EQ(entry_for(patch.get(), 4)->queueCount, 1u);

	// A family the host created with flags (e.g. protected) is never extended.
	godot.main.flags = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
	DeviceRequest graphics;
	graphics.queues.push_back({ VK_QUEUE_GRAPHICS_BIT, 1 });
	DeviceCreatePatch flagged(godot.chain.info, graphics, families);
	CHECK(!flagged.queue_assignments()[0].assigned);
	CHECK(!flagged.changed());

	// Without the queue families nothing can be placed.
	DeviceCreatePatch blind(godot.chain.info, graphics);
	CHECK(!blind.queue_assignments()[0].assigned);
}
