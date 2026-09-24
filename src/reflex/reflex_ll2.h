// NVIDIA Reflex through the Khronos extension VK_NV_low_latency2 (no NVIDIA DLLs involved).
//
// ReflexFeature is the process-wide object: it requests the extension from the Vulkan layer,
// watches swapchains, submissions and presents, and is what the Godot singleton talks to.
// Each device that accepted the extension gets an Ll2Device holding its function pointers,
// its sleep semaphore, the tracked window swapchain and the marker sequencer.
//
// Frame-loop mapping (why each call sits where it does) is documented in docs/ARCHITECTURE.md
// and in godot/latency_upscaling.cpp.
#pragma once

#include "core/settings.h"
#include "reflex/frame_sequencer.h"
#include "reflex/latency_stats.h"
#include "vk/queue_hooks.h"
#include "vk/vk_feature.h"

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace lu::reflex {

// How long one Reflex sleep may take before it counts as "the driver did not signal".
// Frame-rate limits are clamped to >= 10 fps (100 ms), so a legitimate sleep is shorter.
inline constexpr uint64_t SLEEP_TIMEOUT_NS = 250ull * 1000ull * 1000ull;
// After this many timeouts in a row the sleep is suspended until the window swapchain is
// re-created (a stalled driver must not cost a quarter second per frame).
inline constexpr int SLEEP_TIMEOUTS_BEFORE_SUSPENDING = 3;
// vkGetLatencyTimingsNV changed its signature in revision 2; revision 1 would be called wrongly.
inline constexpr uint32_t MIN_LL2_SPEC_VERSION = 2;

struct SleepMode {
	ReflexMode mode = ReflexMode::On;
	uint32_t minimum_interval_us = 0;
};

struct Counters {
	std::array<uint64_t, MARKER_COUNT> markers{};
	uint64_t sleeps = 0;
	uint64_t sleeps_skipped = 0; // no present since the previous sleep (minimised window, no draw)
	uint64_t sleep_timeouts = 0;
	uint64_t presents_stamped = 0;
	uint64_t submits_tagged = 0;
	uint64_t swapchains_with_latency_mode = 0;
	uint64_t other_swapchains = 0; // swapchains of other windows, passed through untouched
	// The present hook's wait for the swapchain lock. With the shared lock it never waits behind
	// the main thread's vkLatencySleepNV; only swapchain creation/destruction (exclusive) can block it.
	uint64_t present_lock_waits_over_1ms = 0;
	uint64_t present_lock_wait_max_us = 0;
	uint64_t latency_sleep_call_max_us = 0; // how long vkLatencySleepNV itself blocked, at most
	double last_sleep_ms = 0.0;
	double average_sleep_ms = 0.0; // exponential moving average
};

class Ll2Device final : public MarkerSink {
public:
	Ll2Device(const vk::DeviceContext &context, const SleepMode &mode);
	~Ll2Device() override;
	Ll2Device(const Ll2Device &) = delete;
	Ll2Device &operator=(const Ll2Device &) = delete;

	bool valid() const { return valid_; }
	const std::string &init_error() const { return init_error_; }
	vk::DispatchKey key() const { return key_; }
	FrameSequencer &sequencer() { return sequencer_; }

	// Main thread.
	bool sleep(); // vkLatencySleepNV + wait; false when skipped, suspended or disabled
	// Stores the mode and applies it to the window swapchain (now, or when it is created).
	void set_sleep_mode(const SleepMode &mode);
	std::vector<FrameReport> latency_reports();

	// Vulkan hooks.
	VkResult create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
			const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain, PFN_vkCreateSwapchainKHR next);
	void forget_swapchain(VkSwapchainKHR swapchain);
	void forget_surface(VkSurfaceKHR surface);
	VkResult present(VkQueue queue, const VkPresentInfoKHR *info, PFN_vkQueuePresentKHR next);
	VkResult submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence, PFN_vkQueueSubmit next);

	// Called before vkDestroyDevice; afterwards every call is a no-op.
	void shutdown();

	struct State {
		bool has_swapchain = false;
		bool latency_mode_enabled = false;
		bool sleep_enabled = true;
		std::string sleep_disabled_reason;
		std::string swapchain_note;
		std::string sleep_mode_error;
		Counters counters;
	};
	State state();

	void emit_marker(uint64_t frame_id, Marker marker) override;

private:
	enum class SleepState { Enabled, Suspended, Disabled };
	void stop_sleeping(SleepState state, const std::string &reason);
	// Counts a present hook's wait for the swapchain lock (see Counters). Takes counters_mutex_.
	void record_present_lock_wait(std::chrono::steady_clock::duration waited);
	// Applies mode_ to swapchain_; called with mutex_ held. Returns the message to log (the log
	// sink may call back into the addon, so nothing is logged under the lock).
	std::string apply_sleep_mode_locked();

	VkDevice device_ = VK_NULL_HANDLE;
	vk::DispatchKey key_ = nullptr;
	std::atomic<bool> valid_{ false };
	std::string init_error_;
	bool present_id_enabled_ = false;

	PFN_vkSetLatencySleepModeNV set_sleep_mode_ = nullptr;
	PFN_vkLatencySleepNV latency_sleep_ = nullptr;
	PFN_vkSetLatencyMarkerNV set_marker_ = nullptr;
	PFN_vkGetLatencyTimingsNV get_timings_ = nullptr;
	PFN_vkCreateSemaphore create_semaphore_ = nullptr;
	PFN_vkDestroySemaphore destroy_semaphore_ = nullptr;
	PFN_vkWaitSemaphores wait_semaphores_ = nullptr;

	// Sleep state; the semaphore value is only touched by the thread that sleeps (main thread).
	VkSemaphore sleep_semaphore_ = VK_NULL_HANDLE;
	uint64_t sleep_value_ = 0;
	std::atomic<bool> signal_pending_{ false }; // the last sleep timed out: the driver may still signal it
	int consecutive_timeouts_ = 0;
	std::atomic<bool> presented_since_sleep_{ false };
	std::atomic<SleepState> sleep_state_{ SleepState::Enabled };

	// Shared/exclusive lock over the swapchain handle and mode. VK_NV_low_latency2's per-frame
	// calls (sleep, markers, timings, present-id) do not mark the swapchain externally
	// synchronised, so they may run concurrently: they hold this shared, while create/destroy of
	// the swapchain and mode changes hold it exclusive. A shared holder therefore never blocks
	// another (the main thread's sleep no longer stalls the render thread's present hook), and
	// a destroy still waits until no LL2 call is in flight. Never held across the semaphore wait
	// or a log call. Lock order when both are taken: swapchain_mutex_ then counters_mutex_.
	std::shared_mutex swapchain_mutex_;
	VkSurfaceKHR tracked_surface_ = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
	bool latency_mode_enabled_ = false;
	SleepMode mode_;
	// Counters and the diagnostic strings; short critical sections only.
	std::mutex counters_mutex_;
	std::string sleep_disabled_reason_;
	std::string swapchain_note_;
	std::string sleep_mode_error_;
	Counters counters_;

	FrameSequencer sequencer_{ *this };
};

class ReflexFeature final : public vk::VulkanFeature, public vk::QueueHooks {
public:
	static ReflexFeature &get();

	// CORE initialisation, before any Vulkan call. `enabled` = false keeps the extension out of
	// the device (the editor by default); `reason` says why.
	void configure(bool enabled, const std::string &reason, const Settings &settings, const std::vector<std::string> &hidden_extensions);

	// --- VulkanFeature ---
	const char *name() const override { return "Reflex (VK_NV_low_latency2)"; }
	void request_instance_extensions(std::vector<std::string> &extensions) override;
	bool request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) override;
	void on_device_created(const vk::DeviceContext &context, bool accepted, const std::string &reason) override;
	void on_device_destroyed(VkDevice device) override;

	// --- QueueHooks ---
	VkResult create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator,
			VkSwapchainKHR *swapchain, PFN_vkCreateSwapchainKHR next) override;
	void destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator,
			PFN_vkDestroySwapchainKHR next) override;
	void destroy_surface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator,
			PFN_vkDestroySurfaceKHR next) override;
	VkResult queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence, PFN_vkQueueSubmit next) override;
	VkResult queue_present(VkQueue queue, const VkPresentInfoKHR *info, PFN_vkQueuePresentKHR next) override;

	// --- Godot side (see godot/latency_upscaling.cpp for the frame-loop placement) ---
	void set_sleep_mode(const SleepMode &mode);
	// The device driving the window, once its swapchain exists.
	std::shared_ptr<Ll2Device> active_device();

	struct Status {
		bool requested = false; // the feature asked for the extension at all
		bool supported = false; // a device accepted it
		bool active = false; // a window swapchain has latency mode enabled
		std::string reason; // "" when active; otherwise why not
	};
	Status status();

private:
	ReflexFeature() = default;
	std::shared_ptr<Ll2Device> device_for(vk::DispatchKey key);
	Ll2Device *fast_device_for(vk::DispatchKey key) const;

	mutable std::mutex mutex_;
	bool enabled_ = false;
	std::string disabled_reason_ = "the Vulkan hook was not installed";
	std::vector<std::string> hidden_extensions_; // test knob (debug builds)
	SleepMode mode_;
	bool any_device_ = false;
	std::string decline_reason_; // why the first device could not use Reflex
	std::map<vk::DispatchKey, std::shared_ptr<Ll2Device>> devices_;
	std::shared_ptr<Ll2Device> active_;
	// Lock-free lookup for the per-submit/present hooks. The key is compared before the device
	// pointer is touched, so a queue of another device never dereferences it; the active device
	// itself cannot be destroyed while its own queues are in use (Vulkan external sync).
	std::atomic<vk::DispatchKey> fast_key_{ nullptr };
	std::atomic<Ll2Device *> fast_device_{ nullptr };
};

} // namespace lu::reflex
