// The three structures VK_NV_low_latency2 adds to the host's calls, each built on a copy of the
// host's create/present/submit info (the host's own structures are never modified).
//
// Pure C++ plus Vulkan headers: unit-tested without a GPU. On the reference driver the effect of
// a missing structure is not observable from the application (the sleep and the reports work
// either way), so these tests are what guards them.
#pragma once

#include "vk/vk_common.h"

#include <cstdint>
#include <vector>

namespace lu::reflex {

// vkCreateSwapchainKHR: VkSwapchainLatencyCreateInfoNV{latencyModeEnable = VK_TRUE} in front of
// the host's chain, unless the host chained one itself (its choice is kept).
class SwapchainLatencyPatch {
public:
	explicit SwapchainLatencyPatch(const VkSwapchainCreateInfoKHR &info);
	SwapchainLatencyPatch(const SwapchainLatencyPatch &) = delete;
	SwapchainLatencyPatch &operator=(const SwapchainLatencyPatch &) = delete;

	const VkSwapchainCreateInfoKHR *get() const { return &info_; }
	bool host_provided() const { return host_provided_; }
	// The latencyModeEnable value the swapchain is created with.
	bool latency_mode() const { return latency_mode_; }

private:
	VkSwapchainCreateInfoKHR info_;
	VkSwapchainLatencyCreateInfoNV latency_{};
	bool host_provided_ = false;
	bool latency_mode_ = false;
};

// vkQueuePresentKHR: VkPresentIdKHR with `present_id` for the swapchain at `index` and 0 ("no
// ID") for the other swapchains of the same call. Nothing is added when the host chained
// present IDs itself or `index` is out of range.
class PresentIdPatch {
public:
	PresentIdPatch(const VkPresentInfoKHR &info, uint32_t index, uint64_t present_id);
	PresentIdPatch(const PresentIdPatch &) = delete;
	PresentIdPatch &operator=(const PresentIdPatch &) = delete;

	const VkPresentInfoKHR *get() const { return &info_; }
	bool stamped() const { return stamped_; }

private:
	VkPresentInfoKHR info_;
	VkPresentIdKHR id_info_{};
	std::vector<uint64_t> ids_;
	bool stamped_ = false;
};

// vkQueueSubmit: VkLatencySubmissionPresentIdNV{presentID} in front of every submission the host
// did not tag itself, so the driver attributes the GPU work to the frame.
class SubmitTagPatch {
public:
	SubmitTagPatch(const VkSubmitInfo *submits, uint32_t count, uint64_t present_id);
	SubmitTagPatch(const SubmitTagPatch &) = delete;
	SubmitTagPatch &operator=(const SubmitTagPatch &) = delete;

	const VkSubmitInfo *get() const { return copies_.data(); }
	uint32_t tagged() const { return tagged_; }

private:
	std::vector<VkSubmitInfo> copies_;
	std::vector<VkLatencySubmissionPresentIdNV> tags_;
	uint32_t tagged_ = 0;
};

} // namespace lu::reflex
