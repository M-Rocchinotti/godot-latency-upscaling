// NVIDIA Reflex through the Khronos extension VK_NV_low_latency2 (no NVIDIA DLLs involved).
//
// ReflexFeature is the process-wide object: it requests the extension from the Vulkan layer,
// watches swapchains, submissions and presents, and is what the Godot singleton talks to.
// Each device that accepted the extension gets an Ll2Device holding its function pointers,
// its sleep semaphore, the tracked window swapchain and the marker sequencer.
//
// Frame-loop mapping (why each call sits where it does) is documented in docs/ARCHITECTURE.md
// and in godot/nvidia_dlss.cpp.
#pragma once

#include "core/settings.h"
#include "reflex/frame_sequencer.h"
#include "reflex/latency_stats.h"
#include "vk/queue_hooks.h"
#include "vk/vk_feature.h"

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nvdlss::reflex {

// How long a Reflex sleep may take before it is treated as "the driver never signalled".
// Frame-rate limits are clamped to >= 10 fps (100 ms), so a legitimate sleep is shorter.
inline constexpr uint64_t SLEEP_TIMEOUT_NS = 250ull * 1000ull * 1000ull;
inline constexpr int SLEEP_TIMEOUTS_BEFORE_GIVING_UP = 3;

struct SleepMode {
	ReflexMode mode = ReflexMode::On;
	uint32_t minimum_interval_us = 0;
};

struct Counters {
	std::array<uint64_t, MARKER_COUNT> markers{};
	uint64_t sleeps = 0;
	uint64_t sleep_timeouts = 0;
	uint64_t presents_stamped = 0;
	uint64_t submits_tagged = 0;
	uint64_t swapchains_with_latency_mode = 0;
	double last_sleep_ms = 0.0;
	double average_sleep_ms = 0.0; // exponential moving average
};

class Ll2Device final : public MarkerSink {
public:
	explicit Ll2Device(const vk::DeviceContext &context);
	~Ll2Device() override;
	Ll2Device(const Ll2Device &) = delete;
	Ll2Device &operator=(const Ll2Device &) = delete;

	bool valid() const { return valid_; }
	const std::string &init_error() const { return init_error_; }
	VkDevice device() const { return device_; }
	vk::DispatchKey key() const { return key_; }
	FrameSequencer &sequencer() { return sequencer_; }

	// Main thread.
	bool sleep(); // vkLatencySleepNV + wait; false when skipped or disabled
	void apply_sleep_mode(const SleepMode &mode);
	std::vector<FrameReport> latency_reports();

	// Vulkan hooks.
	VkResult create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
			const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain, PFN_vkCreateSwapchainKHR next,
			const SleepMode &mode);
	void forget_swapchain(VkSwapchainKHR swapchain);
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
	void disable_sleep(const std::string &reason);

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

	VkSemaphore sleep_semaphore_ = VK_NULL_HANDLE;
	uint64_t sleep_value_ = 0;
	int consecutive_timeouts_ = 0;

	// Guards the swapchain handle against a destroy on another thread while an LL2 call uses
	// it, and the counters/state strings. Never held across the semaphore wait.
	std::mutex mutex_;
	VkSurfaceKHR tracked_surface_ = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
	bool latency_mode_enabled_ = false;
	std::atomic<bool> sleep_enabled_{ true };
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
	VkResult queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence, PFN_vkQueueSubmit next) override;
	VkResult queue_present(VkQueue queue, const VkPresentInfoKHR *info, PFN_vkQueuePresentKHR next) override;

	// --- Godot side (see godot/nvidia_dlss.cpp for the frame-loop placement) ---
	void set_sleep_mode(const SleepMode &mode);
	SleepMode sleep_mode() const;
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

	mutable std::mutex mutex_;
	bool enabled_ = false;
	std::string disabled_reason_ = "the Vulkan hook was not installed";
	std::vector<std::string> hidden_extensions_; // test knob (debug builds)
	SleepMode mode_;
	bool any_device_ = false;
	std::string decline_reason_; // why the first device could not use Reflex
	std::map<vk::DispatchKey, std::shared_ptr<Ll2Device>> devices_;
	std::shared_ptr<Ll2Device> active_;
	// Lock-free lookup for the per-submit/present hooks: the device that owns the window swapchain.
	std::atomic<Ll2Device *> fast_active_{ nullptr };
};

} // namespace nvdlss::reflex
