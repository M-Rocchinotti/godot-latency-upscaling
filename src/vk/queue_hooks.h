// Interception of the swapchain / submit / present calls, for the feature that needs them
// (Reflex in Phase 1). The dispatch layer calls these instead of the loader when a handler is
// installed; the handler must always end in `next`.
#pragma once

#include "vk/vk_common.h"

namespace nvdlss::vk {

class QueueHooks {
public:
	virtual ~QueueHooks() = default;
	virtual VkResult create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
			const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain, PFN_vkCreateSwapchainKHR next) = 0;
	virtual void destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator,
			PFN_vkDestroySwapchainKHR next) = 0;
	virtual VkResult queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence,
			PFN_vkQueueSubmit next) = 0;
	virtual VkResult queue_present(VkQueue queue, const VkPresentInfoKHR *info, PFN_vkQueuePresentKHR next) = 0;
};

} // namespace nvdlss::vk
