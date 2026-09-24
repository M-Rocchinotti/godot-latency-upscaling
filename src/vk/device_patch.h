// Adds device extensions and feature bits to the host's VkDeviceCreateInfo for the one
// vkCreateDevice call the hook forwards.
//
// The host owns its create info and pNext chain. The patch never rewrites the caller's
// arrays: it builds its own extension array and prepends its own feature structures. Only
// when the host already chains a structure that holds a requested feature bit (for example
// VkPhysicalDeviceVulkan12Features, which must not coexist with the promoted per-feature
// structures) is that bit set in place, and the old value is written back by restore() or
// the destructor, right after the call.
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
};

// What the enabled features need from the device. Extension points for later phases: NGX
// adds its device extensions here, Frame Generation would add its optical-flow queue.
struct DeviceRequest {
	std::vector<std::string> extensions;
	DeviceFeatureRequest features;

	void add_extension(const char *name); // ignores duplicates
	bool empty() const { return extensions.empty() && !features.timeline_semaphore && !features.present_id; }
};

class DeviceCreatePatch {
public:
	DeviceCreatePatch(const VkDeviceCreateInfo &original, const DeviceRequest &request);
	~DeviceCreatePatch();
	DeviceCreatePatch(const DeviceCreatePatch &) = delete;
	DeviceCreatePatch &operator=(const DeviceCreatePatch &) = delete;

	const VkDeviceCreateInfo *get() const { return &info_; }
	bool changed() const { return changed_; }

	// Extensions the patch appended (the host did not already enable them).
	const std::vector<std::string> &added_extensions() const { return added_; }
	// The complete list the device is created with.
	std::vector<std::string> enabled_extensions() const;

	// Undoes the in-place feature-bit edits. Idempotent.
	void restore();

private:
	void request_bit(VkBool32 *existing_field, bool *prepend_flag);

	VkDeviceCreateInfo info_{};
	std::vector<std::string> added_;
	std::vector<const char *> names_;
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline_{};
	VkPhysicalDevicePresentIdFeaturesKHR present_id_{};

	struct InPlaceEdit {
		VkBool32 *field;
		VkBool32 old_value;
	};
	std::vector<InPlaceEdit> in_place_;
	bool changed_ = false;
};

} // namespace lu::vk
