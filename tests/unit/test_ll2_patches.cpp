#include "reflex/ll2_patches.h"
#include "test_framework.h"

using namespace nvdlss::reflex;
using nvdlss::vk::find_in_chain;

namespace {

VkSwapchainCreateInfoKHR swapchain_info(const void *next) {
	VkSwapchainCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.pNext = next;
	info.minImageCount = 3;
	return info;
}

} // namespace

TEST(swapchain_patch_enables_latency_mode_in_front_of_the_host_chain) {
	VkImageFormatListCreateInfo host_struct{};
	host_struct.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
	const VkSwapchainCreateInfoKHR host = swapchain_info(&host_struct);
	SwapchainLatencyPatch patch(host);
	const auto *latency = reinterpret_cast<const VkSwapchainLatencyCreateInfoNV *>(
			find_in_chain(patch.get()->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV));
	CHECK(latency != nullptr);
	CHECK(latency != nullptr && latency->latencyModeEnable == VK_TRUE);
	CHECK(latency != nullptr && latency->pNext == &host_struct); // host chain kept behind it
	CHECK(patch.latency_mode());
	CHECK(!patch.host_provided());
	CHECK_EQ(patch.get()->minImageCount, 3u);
	CHECK(host.pNext == &host_struct); // the host's structure is untouched
}

TEST(swapchain_patch_keeps_the_hosts_own_latency_choice) {
	VkSwapchainLatencyCreateInfoNV host_latency{};
	host_latency.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
	host_latency.latencyModeEnable = VK_FALSE;
	const VkSwapchainCreateInfoKHR host = swapchain_info(&host_latency);
	SwapchainLatencyPatch patch(host);
	CHECK(patch.host_provided());
	CHECK(!patch.latency_mode());
	CHECK(patch.get()->pNext == &host_latency); // nothing added
}

TEST(present_patch_stamps_only_the_tracked_swapchain) {
	VkSwapchainKHR swapchains[3] = {};
	uint32_t images[3] = {};
	VkPresentInfoKHR host{};
	host.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	host.swapchainCount = 3;
	host.pSwapchains = swapchains;
	host.pImageIndices = images;
	PresentIdPatch patch(host, 1, 42);
	CHECK(patch.stamped());
	const auto *ids = reinterpret_cast<const VkPresentIdKHR *>(find_in_chain(patch.get()->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR));
	CHECK(ids != nullptr);
	if (ids != nullptr) {
		CHECK_EQ(ids->swapchainCount, 3u); // VUID-VkPresentIdKHR-swapchainCount-04998
		CHECK_EQ(ids->pPresentIds[0], 0ull); // 0 = no ID for the other windows
		CHECK_EQ(ids->pPresentIds[1], 42ull);
		CHECK_EQ(ids->pPresentIds[2], 0ull);
	}
	CHECK(host.pNext == nullptr);
	CHECK(patch.get()->pSwapchains == swapchains);
}

TEST(present_patch_leaves_hosts_ids_and_bad_input_alone) {
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	uint64_t host_id = 7;
	VkPresentIdKHR host_ids{};
	host_ids.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
	host_ids.swapchainCount = 1;
	host_ids.pPresentIds = &host_id;
	VkPresentInfoKHR host{};
	host.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	host.pNext = &host_ids;
	host.swapchainCount = 1;
	host.pSwapchains = &swapchain;
	PresentIdPatch with_host_ids(host, 0, 42);
	CHECK(!with_host_ids.stamped());
	CHECK(with_host_ids.get()->pNext == &host_ids);

	host.pNext = nullptr;
	PresentIdPatch out_of_range(host, 1, 42);
	CHECK(!out_of_range.stamped());
	PresentIdPatch no_id(host, 0, 0);
	CHECK(!no_id.stamped());
	CHECK(no_id.get()->pNext == nullptr);
}

TEST(submit_patch_tags_every_untagged_submission) {
	VkLatencySubmissionPresentIdNV host_tag{};
	host_tag.sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV;
	host_tag.presentID = 5;
	VkTimelineSemaphoreSubmitInfo timeline{};
	timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	VkSubmitInfo submits[3] = {};
	for (VkSubmitInfo &s : submits) {
		s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	}
	submits[1].pNext = &host_tag;
	submits[2].pNext = &timeline;
	SubmitTagPatch patch(submits, 3, 9);
	CHECK_EQ(patch.tagged(), 2u);
	for (uint32_t i : { 0u, 2u }) {
		const auto *tag = reinterpret_cast<const VkLatencySubmissionPresentIdNV *>(
				find_in_chain(patch.get()[i].pNext, VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV));
		CHECK(tag != nullptr && tag->presentID == 9);
	}
	CHECK(patch.get()[1].pNext == &host_tag); // the host's own tag wins
	CHECK(find_in_chain(patch.get()[2].pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) ==
			reinterpret_cast<const VkBaseInStructure *>(&timeline));
	CHECK(submits[0].pNext == nullptr && submits[2].pNext == &timeline); // host structures untouched
}

TEST(submit_patch_handles_an_empty_submit) {
	SubmitTagPatch patch(nullptr, 0, 9);
	CHECK_EQ(patch.tagged(), 0u);
}
