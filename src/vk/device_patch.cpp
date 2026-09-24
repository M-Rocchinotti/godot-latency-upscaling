#include "vk/device_patch.h"

#include <bitset>
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

void DeviceFeatureRequest::merge(const DeviceFeatureRequest &other) {
	timeline_semaphore |= other.timeline_semaphore;
	present_id |= other.present_id;
	buffer_device_address |= other.buffer_device_address;
}

void DeviceRequest::add_extension(const char *name) {
	for (const std::string &existing : extensions) {
		if (existing == name) {
			return;
		}
	}
	extensions.emplace_back(name);
}

DeviceCreatePatch::DeviceCreatePatch(const VkDeviceCreateInfo &original, const DeviceRequest &request,
		const std::vector<VkQueueFamilyProperties> &families) :
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
	bool prepend_buffer_device_address = false;
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
	if (request.features.buffer_device_address) {
		// Like the timeline semaphore: promoted to Vulkan 1.2, so VkPhysicalDeviceVulkan12Features
		// holds it when the host chains that structure.
		if (auto *v12 = mutable_in_chain<VkPhysicalDeviceVulkan12Features>(chain,
					VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
			request_bit(&v12->bufferDeviceAddress, nullptr);
		} else if (auto *bda = mutable_in_chain<VkPhysicalDeviceBufferDeviceAddressFeatures>(chain,
						   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)) {
			request_bit(&bda->bufferDeviceAddress, nullptr);
		} else {
			request_bit(nullptr, &prepend_buffer_device_address);
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
	if (prepend_buffer_device_address) {
		buffer_device_address_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
		buffer_device_address_.pNext = const_cast<void *>(head);
		buffer_device_address_.bufferDeviceAddress = VK_TRUE;
		head = &buffer_device_address_;
	}
	info_.pNext = head;

	if (!request.queues.empty()) {
		add_queues(original, request.queues, families);
	}
}

void DeviceCreatePatch::add_queues(const VkDeviceCreateInfo &original, const std::vector<QueueRequest> &requests,
		const std::vector<VkQueueFamilyProperties> &families) {
	queue_infos_.assign(original.pQueueCreateInfos, original.pQueueCreateInfos + original.queueCreateInfoCount);
	priorities_.assign(queue_infos_.size(), {});
	// Queues in use per family: the host's (only entries without flags can be extended; a family
	// the host created with flags, e.g. protected queues, is left alone) plus what was added.
	std::vector<uint32_t> used(families.size(), 0);
	std::vector<bool> blocked(families.size(), false);
	for (const VkDeviceQueueCreateInfo &q : queue_infos_) {
		if (q.queueFamilyIndex < families.size()) {
			used[q.queueFamilyIndex] += q.queueCount;
			blocked[q.queueFamilyIndex] = blocked[q.queueFamilyIndex] || q.flags != 0;
		}
	}
	for (const QueueRequest &request : requests) {
		QueueAssignment assignment;
		// The most specific family: every requested bit, the fewest other capability bits.
		int best = -1;
		size_t best_extra = 0;
		for (uint32_t f = 0; f < families.size() && request.count > 0; ++f) {
			const VkQueueFlags flags = families[f].queueFlags;
			if ((flags & request.required) != request.required || blocked[f] || used[f] + request.count > families[f].queueCount) {
				continue;
			}
			const size_t extra = std::bitset<32>(flags & ~request.required).count();
			if (best < 0 || extra < best_extra) {
				best = static_cast<int>(f);
				best_extra = extra;
			}
		}
		if (best >= 0) {
			const uint32_t family = static_cast<uint32_t>(best);
			size_t entry = queue_infos_.size();
			for (size_t i = 0; i < queue_infos_.size(); ++i) {
				if (queue_infos_[i].queueFamilyIndex == family) {
					entry = i;
				}
			}
			if (entry == queue_infos_.size()) {
				VkDeviceQueueCreateInfo added{};
				added.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				added.queueFamilyIndex = family;
				queue_infos_.push_back(added);
				priorities_.emplace_back();
			}
			VkDeviceQueueCreateInfo &info = queue_infos_[entry];
			std::vector<float> &priorities = priorities_[entry];
			if (priorities.empty() && info.queueCount > 0) {
				priorities.assign(info.pQueuePriorities, info.pQueuePriorities + info.queueCount); // the host's own
			}
			assignment.assigned = true;
			assignment.family = family;
			assignment.first_index = info.queueCount;
			assignment.count = request.count;
			priorities.insert(priorities.end(), request.count, 1.0f);
			info.queueCount += request.count;
			used[family] += request.count;
			changed_ = true;
		}
		queues_.push_back(assignment);
	}
	// Point every rewritten or added entry at the patch's own priorities (all vectors are final now).
	for (size_t i = 0; i < queue_infos_.size(); ++i) {
		if (!priorities_[i].empty()) {
			queue_infos_[i].pQueuePriorities = priorities_[i].data();
		}
	}
	info_.queueCreateInfoCount = static_cast<uint32_t>(queue_infos_.size());
	info_.pQueueCreateInfos = queue_infos_.data();
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
