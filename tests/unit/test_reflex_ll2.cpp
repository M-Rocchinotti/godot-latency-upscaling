// Ll2Device against a fake driver: the paths a Godot 4.7 run cannot reach or cannot time
// deterministically.
//
// - The swapchain retry after a failed latency-mode create. Godot 4.7 never passes oldSwapchain
//   (swap_chain_resize releases the old swapchain first and zero-initialises the create info), so
//   an engine resize cannot show whether the retry drops the retired oldSwapchain
//   (VUID-VkSwapchainCreateInfoKHR-oldSwapchain-01933). A fake create that fails once can.
// - Other windows' swapchains must pass through untouched.
// - The sleep lock: a fake vkLatencySleepNV that blocks shows whether the present hook (render
//   thread) waits for it (it must not), and whether a swapchain destroy does (it must).
#include "reflex/reflex_ll2.h"
#include "test_framework.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace lu;
using namespace lu::reflex;
using Clock = std::chrono::steady_clock;

namespace {

// Dispatchable handles start with a pointer to their dispatch table (see vk::dispatch_key).
void *g_table = nullptr;
struct FakeDispatchable {
	void **table = &g_table;
};
FakeDispatchable g_device_object;
FakeDispatchable g_queue_object;
VkDevice fake_device() { return reinterpret_cast<VkDevice>(&g_device_object); }
VkQueue fake_queue() { return reinterpret_cast<VkQueue>(&g_queue_object); }

template <typename Handle>
Handle fake_handle(uintptr_t value) { return reinterpret_cast<Handle>(value); }

std::atomic<int> g_sleep_block_ms{ 0 };
std::atomic<bool> g_in_sleep{ false };

VKAPI_ATTR VkResult VKAPI_CALL fake_set_sleep_mode(VkDevice, VkSwapchainKHR, const VkLatencySleepModeInfoNV *) {
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL fake_latency_sleep(VkDevice, VkSwapchainKHR, const VkLatencySleepInfoNV *) {
	g_in_sleep = true;
	std::this_thread::sleep_for(std::chrono::milliseconds(g_sleep_block_ms.load()));
	g_in_sleep = false;
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL fake_set_marker(VkDevice, VkSwapchainKHR, const VkSetLatencyMarkerInfoNV *) {}
VKAPI_ATTR void VKAPI_CALL fake_get_timings(VkDevice, VkSwapchainKHR, VkGetLatencyMarkerInfoNV *info) {
	info->timingCount = 0;
}
VKAPI_ATTR VkResult VKAPI_CALL fake_create_semaphore(VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *,
		VkSemaphore *semaphore) {
	*semaphore = fake_handle<VkSemaphore>(0x5E3A);
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL fake_destroy_semaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *) {}
VKAPI_ATTR VkResult VKAPI_CALL fake_wait_semaphores(VkDevice, const VkSemaphoreWaitInfo *, uint64_t) {
	return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_get_device_proc_addr(VkDevice, const char *name) {
	struct Entry {
		const char *name;
		PFN_vkVoidFunction function;
	};
	static const Entry entries[] = {
		{ "vkSetLatencySleepModeNV", reinterpret_cast<PFN_vkVoidFunction>(&fake_set_sleep_mode) },
		{ "vkLatencySleepNV", reinterpret_cast<PFN_vkVoidFunction>(&fake_latency_sleep) },
		{ "vkSetLatencyMarkerNV", reinterpret_cast<PFN_vkVoidFunction>(&fake_set_marker) },
		{ "vkGetLatencyTimingsNV", reinterpret_cast<PFN_vkVoidFunction>(&fake_get_timings) },
		{ "vkCreateSemaphore", reinterpret_cast<PFN_vkVoidFunction>(&fake_create_semaphore) },
		{ "vkDestroySemaphore", reinterpret_cast<PFN_vkVoidFunction>(&fake_destroy_semaphore) },
		{ "vkWaitSemaphores", reinterpret_cast<PFN_vkVoidFunction>(&fake_wait_semaphores) },
	};
	for (const Entry &e : entries) {
		if (std::strcmp(e.name, name) == 0) {
			return e.function;
		}
	}
	return nullptr;
}

// Records every create; fails the first `g_failures` of them (a failed create still retires
// oldSwapchain, per the spec).
struct CreateCall {
	VkSurfaceKHR surface;
	VkSwapchainKHR old_swapchain;
	bool latency_mode;
};
std::vector<CreateCall> g_creates;
int g_failures = 0;

VKAPI_ATTR VkResult VKAPI_CALL fake_create_swapchain(VkDevice, const VkSwapchainCreateInfoKHR *info,
		const VkAllocationCallbacks *, VkSwapchainKHR *swapchain) {
	g_creates.push_back({ info->surface, info->oldSwapchain,
			vk::find_in_chain(info->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV) != nullptr });
	if (g_failures > 0) {
		--g_failures;
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*swapchain = fake_handle<VkSwapchainKHR>(0x5C00 + g_creates.size());
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_present(VkQueue, const VkPresentInfoKHR *) {
	return VK_SUCCESS;
}

vk::DeviceContext fake_context() {
	vk::DeviceContext context;
	context.device = fake_device();
	context.get_device_proc_addr = &fake_get_device_proc_addr;
	context.caps.effective_api_version = VK_API_VERSION_1_2;
	context.enabled_extensions = { VK_NV_LOW_LATENCY_2_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME };
	context.enabled_features.present_id = true;
	context.enabled_features.timeline_semaphore = true;
	return context;
}

VkSwapchainCreateInfoKHR swapchain_info(uintptr_t surface) {
	VkSwapchainCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface = fake_handle<VkSurfaceKHR>(surface);
	return info;
}

// A device with a tracked window swapchain in latency mode, and one frame presented so the next
// sleep() really sleeps.
struct Window {
	Ll2Device device{ fake_context(), SleepMode{} };
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	uint32_t image_index = 0;

	Window() {
		g_creates.clear();
		g_failures = 0;
		const VkSwapchainCreateInfoKHR info = swapchain_info(0xA);
		device.create_swapchain(fake_device(), &info, nullptr, &swapchain, &fake_create_swapchain);
		present();
	}
	void present() {
		VkPresentInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		info.swapchainCount = 1;
		info.pSwapchains = &swapchain;
		info.pImageIndices = &image_index;
		device.present(fake_queue(), &info, &fake_present);
	}
};

// Runs sleep() on another thread with vkLatencySleepNV blocking for `block_ms`; returns once the
// fake is inside vkLatencySleepNV.
std::thread start_blocking_sleep(Ll2Device &device, int block_ms) {
	g_in_sleep = false;
	g_sleep_block_ms = block_ms;
	std::thread sleeper([&device] { device.sleep(); });
	while (!g_in_sleep) {
		std::this_thread::yield();
	}
	return sleeper;
}

double ms_since(Clock::time_point start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

} // namespace

TEST(ll2_fake_device_is_valid) {
	Window window;
	CHECK(window.device.valid());
	CHECK(window.swapchain != VK_NULL_HANDLE);
	CHECK(window.device.state().latency_mode_enabled);
}

TEST(ll2_swapchain_retry_does_not_pass_the_retired_old_swapchain) {
	Ll2Device device(fake_context(), SleepMode{});
	g_creates.clear();
	g_failures = 1; // the latency-mode create fails (and, per the spec, retires oldSwapchain)
	VkSwapchainCreateInfoKHR info = swapchain_info(0xA);
	info.oldSwapchain = fake_handle<VkSwapchainKHR>(0x01D);
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	CHECK(device.create_swapchain(fake_device(), &info, nullptr, &swapchain, &fake_create_swapchain) == VK_SUCCESS);
	CHECK_EQ(g_creates.size(), size_t(2));
	if (g_creates.size() == 2) {
		CHECK(g_creates[0].latency_mode);
		CHECK(g_creates[0].old_swapchain == info.oldSwapchain); // first attempt as the host asked
		CHECK(g_creates[1].old_swapchain == VK_NULL_HANDLE); // VUID-VkSwapchainCreateInfoKHR-oldSwapchain-01933
		CHECK(!g_creates[1].latency_mode);
	}
	const Ll2Device::State state = device.state();
	CHECK(!state.latency_mode_enabled);
	CHECK(state.swapchain_note.find("created without it") != std::string::npos);
}

TEST(ll2_other_windows_swapchains_pass_through) {
	Window window; // tracks surface 0xA
	const VkSwapchainCreateInfoKHR other = swapchain_info(0xB);
	VkSwapchainKHR other_swapchain = VK_NULL_HANDLE;
	CHECK(window.device.create_swapchain(fake_device(), &other, nullptr, &other_swapchain, &fake_create_swapchain) == VK_SUCCESS);
	CHECK(!g_creates.back().latency_mode); // created exactly as the host asked
	const Ll2Device::State state = window.device.state();
	CHECK_EQ(state.counters.other_swapchains, uint64_t(1));
	CHECK_EQ(state.counters.swapchains_with_latency_mode, uint64_t(1));
	// The main window keeps its latency-mode swapchain; destroying the other one changes nothing.
	window.device.forget_swapchain(other_swapchain);
	CHECK(window.device.state().latency_mode_enabled);
	// The main window re-creating its swapchain is still tracked.
	const VkSwapchainCreateInfoKHR again = swapchain_info(0xA);
	VkSwapchainKHR recreated = VK_NULL_HANDLE;
	window.device.create_swapchain(fake_device(), &again, nullptr, &recreated, &fake_create_swapchain);
	CHECK(g_creates.back().latency_mode);
	CHECK_EQ(window.device.state().counters.swapchains_with_latency_mode, uint64_t(2));
}

TEST(ll2_present_hook_does_not_wait_for_the_sleep) {
	Window window;
	std::thread sleeper = start_blocking_sleep(window.device, 150);
	const Clock::time_point start = Clock::now();
	window.present(); // the render thread presents while the main thread is in vkLatencySleepNV
	const double waited = ms_since(start);
	sleeper.join();
	CHECK(waited < 50.0); // a shared lock: no wait (the exclusive lock waited ~150 ms)
	CHECK_EQ(window.device.state().counters.present_lock_waits_over_1ms, uint64_t(0));
	if (waited >= 50.0) {
		std::printf("    present waited %.1f ms behind vkLatencySleepNV\n", waited);
	}
}

TEST(ll2_swapchain_destroy_waits_for_an_in_flight_sleep) {
	Window window;
	std::thread sleeper = start_blocking_sleep(window.device, 150);
	const Clock::time_point start = Clock::now();
	window.device.forget_swapchain(window.swapchain); // exclusive: must not free it under the sleep
	const double waited = ms_since(start);
	const bool sleep_still_running = g_in_sleep.load();
	sleeper.join();
	CHECK(waited > 100.0);
	CHECK(!sleep_still_running);
}

TEST(ll2_sleep_time_includes_the_blocking_vkLatencySleepNV_call) {
	// Under a frame-rate cap the driver blocks inside vkLatencySleepNV, not in the semaphore wait.
	Window window;
	g_sleep_block_ms = 40;
	CHECK(window.device.sleep());
	g_sleep_block_ms = 0;
	const Counters counters = window.device.state().counters;
	CHECK(counters.last_sleep_ms >= 35.0);
	CHECK(counters.latency_sleep_call_max_us >= 35000);
}
