#include "reflex/ll2_patches.h"

namespace lu::reflex {

SwapchainLatencyPatch::SwapchainLatencyPatch(const VkSwapchainCreateInfoKHR &info) :
		info_(info) {
	if (const auto *existing = reinterpret_cast<const VkSwapchainLatencyCreateInfoNV *>(
				vk::find_in_chain(info.pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV))) {
		host_provided_ = true;
		latency_mode_ = existing->latencyModeEnable == VK_TRUE;
		return;
	}
	latency_.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
	latency_.pNext = info.pNext;
	latency_.latencyModeEnable = VK_TRUE;
	info_.pNext = &latency_;
	latency_mode_ = true;
}

PresentIdPatch::PresentIdPatch(const VkPresentInfoKHR &info, uint32_t index, uint64_t present_id) :
		info_(info) {
	const bool host_has_ids = vk::find_in_chain(info.pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR) != nullptr ||
			vk::find_in_chain(info.pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) != nullptr;
	if (host_has_ids || index >= info.swapchainCount || present_id == 0) {
		return;
	}
	ids_.assign(info.swapchainCount, 0);
	ids_[index] = present_id;
	id_info_.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
	id_info_.pNext = info.pNext;
	id_info_.swapchainCount = info.swapchainCount;
	id_info_.pPresentIds = ids_.data();
	info_.pNext = &id_info_;
	stamped_ = true;
}

SubmitTagPatch::SubmitTagPatch(const VkSubmitInfo *submits, uint32_t count, uint64_t present_id) :
		copies_(submits, submits + count), tags_(count) {
	for (uint32_t i = 0; i < count; ++i) {
		if (vk::find_in_chain(submits[i].pNext, VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV) != nullptr) {
			continue; // the host tags its own submissions
		}
		tags_[i].sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV;
		tags_[i].pNext = submits[i].pNext;
		tags_[i].presentID = present_id;
		copies_[i].pNext = &tags_[i];
		++tagged_;
	}
}

} // namespace lu::reflex
