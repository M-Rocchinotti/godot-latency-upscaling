// Adds device extensions, feature bits and queues to the host's VkDeviceCreateInfo for the one
// vkCreateDevice call the hook forwards.
//
// The host owns its create info and pNext chain. The patch never rewrites the caller's
// arrays: it builds its own extension array and prepends its own feature structures. Only
// when the host already chains a structure that holds a requested feature bit (for example
// VkPhysicalDeviceVulkan12Features, which must not coexist with the promoted per-feature
// structures) is that bit set in place, and the old value is written back by restore() or
// the destructor, right after the call.
//
// Queues: a feature may ask for queues of its own (Streamline's DLSS Frame Generation wants an
// optical-flow queue, for instance). Each request is served from the most specific family that has
// every requested capability and room left; the host's own queues keep their family and index
// (its VkDeviceQueueCreateInfo for that family is copied with a larger queueCount), so the host's
// vkGetDeviceQueue calls return what they always did.
//
// Pure C++ plus Vulkan headers: unit-tested without a GPU.
#pragma once

#include "vk/vk_common.h"

#include <string>
#include <vector>

namespace lu::vk {

struct DeviceFeatureRequest {
	bool timeline_semaphore = false;
	bool present_id = false;
	bool buffer_device_address = false;

	bool any() const { return timeline_semaphore || present_id || buffer_device_address; }
	void merge(const DeviceFeatureRequest &other);
};

// Extra queues a feature needs on the device.
struct QueueRequest {
	VkQueueFlags required = 0; // every bit must be supported by the family (e.g. VK_QUEUE_OPTICAL_FLOW_BIT_NV)
	uint32_t count = 1;
};

// Where a QueueRequest was placed (vkGetDeviceQueue(device, family, first_index + i)).
struct QueueAssignment {
	bool assigned = false;
	uint32_t family = 0;
	uint32_t first_index = 0;
	uint32_t count = 0;
};

// What the enabled features need from the device.
struct DeviceRequest {
	std::vector<std::string> extensions;
	DeviceFeatureRequest features;
	std::vector<QueueRequest> queues;

	void add_extension(const char *name); // ignores duplicates
	bool empty() const { return extensions.empty() && !features.any() && queues.empty(); }
};

class DeviceCreatePatch {
public:
	// `families`: the physical device's queue families (vkGetPhysicalDeviceQueueFamilyProperties),
	// needed only for queue requests.
	DeviceCreatePatch(const VkDeviceCreateInfo &original, const DeviceRequest &request,
			const std::vector<VkQueueFamilyProperties> &families = {});
	~DeviceCreatePatch();
	DeviceCreatePatch(const DeviceCreatePatch &) = delete;
	DeviceCreatePatch &operator=(const DeviceCreatePatch &) = delete;

	const VkDeviceCreateInfo *get() const { return &info_; }
	bool changed() const { return changed_; }

	// Extensions the patch appended (the host did not already enable them).
	const std::vector<std::string> &added_extensions() const { return added_; }
	// The complete list the device is created with.
	std::vector<std::string> enabled_extensions() const;
	// One per request.queues entry, in order; unassigned when no family had room.
	const std::vector<QueueAssignment> &queue_assignments() const { return queues_; }

	// Undoes the in-place feature-bit edits. Idempotent.
	void restore();

private:
	void request_bit(VkBool32 *existing_field, bool *prepend_flag);
	void add_queues(const VkDeviceCreateInfo &original, const std::vector<QueueRequest> &requests,
			const std::vector<VkQueueFamilyProperties> &families);

	VkDeviceCreateInfo info_{};
	std::vector<std::string> added_;
	std::vector<const char *> names_;
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline_{};
	VkPhysicalDevicePresentIdFeaturesKHR present_id_{};
	VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address_{};
	std::vector<VkDeviceQueueCreateInfo> queue_infos_;
	std::vector<std::vector<float>> priorities_; // per queue_infos_ entry the patch rewrote or added
	std::vector<QueueAssignment> queues_;

	struct InPlaceEdit {
		VkBool32 *field;
		VkBool32 old_value;
	};
	std::vector<InPlaceEdit> in_place_;
	bool changed_ = false;
};

} // namespace lu::vk
