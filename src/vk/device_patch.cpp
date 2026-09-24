#include "vk/device_patch.h"

#include <cstring>

namespace lu::vk {
namespace {

// The host's chain is const from the host's point of view; the in-place edits below are
// undone before vkCreateDevice returns to it.
template <typename T>
T *mutable_in_chain(const void *next, VkStructureType type) {
	return reinterpret_cast<T *>(const_cast<VkBaseInStructure *>(find_in_chain(next, type)));
}

} // namespace

void DeviceRequest::add_extension(const char *name) {
	for (const std::string &existing : extensions) {
		if (existing == name) {
			return;
		}
	}
	extensions.emplace_back(name);
}

DeviceCreatePatch::DeviceCreatePatch(const VkDeviceCreateInfo &original, const DeviceRequest &request) :
		info_(original) {
	names_.assign(original.ppEnabledExtensionNames, original.ppEnabledExtensionNames + original.enabledExtensionCount);
	for (const std::string &wanted : request.extensions) {
		bool present = false;
		for (const char *name : names_) {
			present = present || std::strcmp(name, wanted.c_str()) == 0;
		}
		if (!present) {
			added_.push_back(wanted);
		}
	}
	// added_ is complete before taking c_str() pointers: no reallocation invalidates them.
	for (const std::string &name : added_) {
		names_.push_back(name.c_str());
	}
	if (!added_.empty()) {
		info_.enabledExtensionCount = static_cast<uint32_t>(names_.size());
		info_.ppEnabledExtensionNames = names_.data();
		changed_ = true;
	}

	const void *chain = original.pNext;
	bool prepend_timeline = false;
	bool prepend_present_id = false;
	if (request.features.timeline_semaphore) {
		// VkPhysicalDeviceVulkan12Features and VkPhysicalDeviceTimelineSemaphoreFeatures must not
		// both be in a chain, so an existing 1.2 structure is the one to edit.
		if (auto *v12 = mutable_in_chain<VkPhysicalDeviceVulkan12Features>(chain,
					VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
			request_bit(&v12->timelineSemaphore, nullptr);
		} else if (auto *ts = mutable_in_chain<VkPhysicalDeviceTimelineSemaphoreFeatures>(chain,
						   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)) {
			request_bit(&ts->timelineSemaphore, nullptr);
		} else {
			request_bit(nullptr, &prepend_timeline);
		}
	}
	if (request.features.present_id) {
		if (auto *pid = mutable_in_chain<VkPhysicalDevicePresentIdFeaturesKHR>(chain,
					VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR)) {
			request_bit(&pid->presentId, nullptr);
		} else {
			request_bit(nullptr, &prepend_present_id);
		}
	}

	const void *head = original.pNext;
	if (prepend_timeline) {
		timeline_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
		timeline_.pNext = const_cast<void *>(head);
		timeline_.timelineSemaphore = VK_TRUE;
		head = &timeline_;
	}
	if (prepend_present_id) {
		present_id_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
		present_id_.pNext = const_cast<void *>(head);
		present_id_.presentId = VK_TRUE;
		head = &present_id_;
	}
	info_.pNext = head;
}

void DeviceCreatePatch::request_bit(VkBool32 *existing_field, bool *prepend_flag) {
	if (existing_field != nullptr) {
		if (*existing_field != VK_TRUE) {
			in_place_.push_back({ existing_field, *existing_field });
			*existing_field = VK_TRUE;
			changed_ = true;
		}
	} else {
		*prepend_flag = true;
		changed_ = true;
	}
}

DeviceCreatePatch::~DeviceCreatePatch() {
	restore();
}

void DeviceCreatePatch::restore() {
	for (const InPlaceEdit &edit : in_place_) {
		*edit.field = edit.old_value;
	}
	in_place_.clear();
}

std::vector<std::string> DeviceCreatePatch::enabled_extensions() const {
	return std::vector<std::string>(names_.begin(), names_.end());
}

} // namespace lu::vk
