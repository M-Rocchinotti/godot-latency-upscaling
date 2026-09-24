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
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
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

// A fake driver call closed by a gate blocks until the test opens it, so the lock tests are
// ordered by events, not by timing: a correct implementation passes however slow the machine is,
// and the timeouts below only bound how long a failing (e.g. mutated) run takes.
class Gate {
public:
	void close() {
		std::lock_guard<std::mutex> lock(mutex_);
		open_ = false;
		entered_ = false;
	}
	void open() {
		std::lock_guard<std::mutex> lock(mutex_);
		open_ = true;
		changed_.notify_all();
	}
	void pass() { // called by the fake driver function
		std::unique_lock<std::mutex> lock(mutex_);
		entered_ = true;
		changed_.notify_all();
		changed_.wait(lock, [this] { return open_; });
	}
	bool wait_until_entered() {
		std::unique_lock<std::mutex> lock(mutex_);
		return changed_.wait_for(lock, std::chrono::seconds(5), [this] { return entered_; });
	}

private:
	std::mutex mutex_;
	std::condition_variable changed_;
	bool open_ = true;
	bool entered_ = false;
};

Gate g_sleep_gate; // vkLatencySleepNV
Gate g_sleep_mode_gate; // vkSetLatencySleepModeNV (called with the swapchain lock held exclusive)
std::atomic<int> g_sleep_block_ms{ 0 };

VKAPI_ATTR VkResult VKAPI_CALL fake_set_sleep_mode(VkDevice, VkSwapchainKHR, const VkLatencySleepModeInfoNV *) {
	g_sleep_mode_gate.pass();
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL fake_latency_sleep(VkDevice, VkSwapchainKHR, const VkLatencySleepInfoNV *) {
	g_sleep_gate.pass();
	std::this_thread::sleep_for(std::chrono::milliseconds(g_sleep_block_ms.load()));
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

template <typename Fn>
std::future<void> run_async(Fn fn) {
	return std::async(std::launch::async, fn);
}

bool finishes(std::future<void> &f, std::chrono::milliseconds within) {
	return f.wait_for(within) == std::future_status::ready;
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
	g_sleep_gate.close();
	std::future<void> sleeper = run_async([&] { window.device.sleep(); });
	CHECK(g_sleep_gate.wait_until_entered()); // the main thread is inside vkLatencySleepNV
	// The render thread presents meanwhile: with the shared lock it must finish while the sleep is
	// still blocked (with the lock taken exclusive it could only finish after the sleep).
	std::future<void> present = run_async([&] { window.present(); });
	const bool presented_during_sleep = finishes(present, std::chrono::seconds(3));
	g_sleep_gate.open();
	present.wait();
	sleeper.wait();
	CHECK(presented_during_sleep);
	CHECK_EQ(window.device.state().counters.present_lock_waits_over_1ms, uint64_t(0));
}

TEST(ll2_swapchain_destroy_waits_for_an_in_flight_sleep) {
	Window window;
	g_sleep_gate.close();
	std::future<void> sleeper = run_async([&] { window.device.sleep(); });
	CHECK(g_sleep_gate.wait_until_entered());
	// Exclusive: forgetting the swapchain (before the host frees it) must wait for the sleep.
	std::future<void> destroy = run_async([&] { window.device.forget_swapchain(window.swapchain); });
	const bool destroyed_during_sleep = finishes(destroy, std::chrono::milliseconds(300));
	g_sleep_gate.open();
	const bool destroyed_after_sleep = finishes(destroy, std::chrono::seconds(5));
	sleeper.wait();
	CHECK(!destroyed_during_sleep);
	CHECK(destroyed_after_sleep);
}

TEST(ll2_present_lock_wait_counter_counts_a_forced_wait) {
	// The counter the engine test relies on must be able to see a wait: hold the lock exclusive
	// (a sleep-mode change blocked inside the driver) while the render thread presents.
	Window window;
	g_sleep_mode_gate.close();
	std::future<void> mode_change = run_async([&] { window.device.set_sleep_mode(SleepMode{ ReflexMode::OnBoost, 0 }); });
	CHECK(g_sleep_mode_gate.wait_until_entered());
	std::future<void> present = run_async([&] { window.present(); });
	const bool presented_while_held = finishes(present, std::chrono::milliseconds(200));
	g_sleep_mode_gate.open();
	present.wait();
	mode_change.wait();
	const Counters counters = window.device.state().counters;
	CHECK(!presented_while_held);
	CHECK(counters.present_lock_waits_over_1ms >= 1);
	CHECK(counters.present_lock_wait_max_us >= 1000);
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
