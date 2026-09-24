// Vulkan headers (prototypes disabled: every entry point is resolved at run time) and small
// helpers for pNext chains and dispatchable handles.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

namespace lu::vk {

// First structure of type `type` in a pNext chain, or nullptr.
inline const VkBaseInStructure *find_in_chain(const void *next, VkStructureType type) {
	for (auto *s = static_cast<const VkBaseInStructure *>(next); s != nullptr; s = s->pNext) {
		if (s->sType == type) {
			return s;
		}
	}
	return nullptr;
}

// Every dispatchable handle the Vulkan loader hands out starts with a pointer to its dispatch
// table. A device and all of its queues share it, as do an instance and its physical devices
// (the loader's own dispatch mechanism relies on this). It identifies the parent object of a
// handle without a lookup table per handle.
using DispatchKey = const void *;

template <typename Handle>
DispatchKey dispatch_key(Handle handle) {
	return handle != VK_NULL_HANDLE ? *reinterpret_cast<const void *const *>(handle) : nullptr;
}

} // namespace lu::vk
