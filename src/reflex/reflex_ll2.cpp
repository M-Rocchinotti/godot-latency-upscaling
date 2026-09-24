#include "reflex/reflex_ll2.h"

#include "core/log.h"
#include "core/testing.h"
#include "reflex/ll2_patches.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace lu::reflex {
namespace {

constexpr uint32_t NVIDIA_VENDOR_ID = 0x10DE;
constexpr uint32_t MAX_TIMING_REPORTS = 64; // the driver keeps the last 64 frames
// At device destruction, how long to wait for a sleep signal the driver may still deliver.
constexpr uint64_t PENDING_SIGNAL_WAIT_NS = 100ull * 1000ull * 1000ull;

std::string format(const char *fmt, ...) {
	char buffer[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	return buffer;
}

template <typename Fn>
Fn load(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const char *name) {
	return reinterpret_cast<Fn>(gdpa(device, name));
}

VkLatencySleepModeInfoNV to_vk(const SleepMode &mode) {
	VkLatencySleepModeInfoNV info{};
	info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
	info.lowLatencyMode = mode.mode != ReflexMode::Off ? VK_TRUE : VK_FALSE;
	info.lowLatencyBoost = mode.mode == ReflexMode::OnBoost ? VK_TRUE : VK_FALSE;
	info.minimumIntervalUs = mode.minimum_interval_us;
	return info;
}

FrameReport to_report(const VkLatencyTimingsFrameReportNV &r) {
	FrameReport out;
	out.present_id = r.presentID;
	out.input_sample_us = r.inputSampleTimeUs;
	out.sim_start_us = r.simStartTimeUs;
	out.sim_end_us = r.simEndTimeUs;
	out.render_submit_start_us = r.renderSubmitStartTimeUs;
	out.render_submit_end_us = r.renderSubmitEndTimeUs;
	out.present_start_us = r.presentStartTimeUs;
	out.present_end_us = r.presentEndTimeUs;
	out.driver_start_us = r.driverStartTimeUs;
	out.driver_end_us = r.driverEndTimeUs;
	out.os_render_queue_start_us = r.osRenderQueueStartTimeUs;
	out.os_render_queue_end_us = r.osRenderQueueEndTimeUs;
	out.gpu_render_start_us = r.gpuRenderStartTimeUs;
	out.gpu_render_end_us = r.gpuRenderEndTimeUs;
	return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Ll2Device
// ---------------------------------------------------------------------------------------------

Ll2Device::Ll2Device(const vk::DeviceContext &context, const SleepMode &mode) :
		device_(context.device), key_(vk::dispatch_key(context.device)), mode_(mode) {
	const PFN_vkGetDeviceProcAddr gdpa = context.get_device_proc_addr;
	set_sleep_mode_ = load<PFN_vkSetLatencySleepModeNV>(gdpa, device_, "vkSetLatencySleepModeNV");
	latency_sleep_ = load<PFN_vkLatencySleepNV>(gdpa, device_, "vkLatencySleepNV");
	set_marker_ = load<PFN_vkSetLatencyMarkerNV>(gdpa, device_, "vkSetLatencyMarkerNV");
	get_timings_ = load<PFN_vkGetLatencyTimingsNV>(gdpa, device_, "vkGetLatencyTimingsNV");
	create_semaphore_ = load<PFN_vkCreateSemaphore>(gdpa, device_, "vkCreateSemaphore");
	destroy_semaphore_ = load<PFN_vkDestroySemaphore>(gdpa, device_, "vkDestroySemaphore");
	const bool core_timeline = context.caps.effective_api_version >= VK_API_VERSION_1_2;
	wait_semaphores_ = load<PFN_vkWaitSemaphores>(gdpa, device_, core_timeline ? "vkWaitSemaphores" : "vkWaitSemaphoresKHR");
	if (!set_sleep_mode_ || !latency_sleep_ || !set_marker_ || !get_timings_ || !create_semaphore_ ||
			!destroy_semaphore_ || !wait_semaphores_) {
		init_error_ = "VK_NV_low_latency2 is enabled but its entry points could not be resolved";
		return;
	}
	present_id_enabled_ = context.enabled_features.present_id &&
			context.has_enabled_extension(VK_KHR_PRESENT_ID_EXTENSION_NAME);

	VkSemaphoreTypeCreateInfo type_info{};
	type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	type_info.initialValue = 0;
	VkSemaphoreCreateInfo create_info{};
	create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	create_info.pNext = &type_info;
	const VkResult result = create_semaphore_(device_, &create_info, nullptr, &sleep_semaphore_);
	if (result != VK_SUCCESS) {
		init_error_ = format("creating the Reflex sleep semaphore failed (VkResult %d)", result);
		sleep_semaphore_ = VK_NULL_HANDLE;
		return;
	}
	valid_ = true;
}

Ll2Device::~Ll2Device() {
	shutdown();
}

void Ll2Device::shutdown() {
	VkSemaphore semaphore = VK_NULL_HANDLE;
	{
		std::unique_lock lock(swapchain_mutex_);
		valid_ = false;
		semaphore = sleep_semaphore_;
		sleep_semaphore_ = VK_NULL_HANDLE;
		swapchain_ = VK_NULL_HANDLE;
		latency_mode_enabled_ = false;
	}
	if (semaphore == VK_NULL_HANDLE) {
		return;
	}
	if (signal_pending_) {
		// A sleep timed out and the driver may still signal it; a semaphore must not be destroyed
		// with a pending signal operation. Wait briefly; the device is idle at this point anyway.
		VkSemaphoreWaitInfo wait{};
		wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
		wait.semaphoreCount = 1;
		wait.pSemaphores = &semaphore;
		wait.pValues = &sleep_value_;
		wait_semaphores_(device_, &wait, PENDING_SIGNAL_WAIT_NS);
	}
	destroy_semaphore_(device_, semaphore, nullptr);
}

void Ll2Device::emit_marker(uint64_t frame_id, Marker marker) {
	std::shared_lock lock(swapchain_mutex_);
	if (!valid_ || swapchain_ == VK_NULL_HANDLE || !latency_mode_enabled_) {
		return;
	}
	VkSetLatencyMarkerInfoNV info{};
	info.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
	info.presentID = frame_id;
	info.marker = static_cast<VkLatencyMarkerNV>(marker);
	set_marker_(device_, swapchain_, &info); // LL2 markers may run concurrently with sleep/present
	std::lock_guard<std::mutex> counters(counters_mutex_);
	counters_.markers[static_cast<size_t>(marker)]++;
}

void Ll2Device::stop_sleeping(SleepState state, const std::string &reason) {
	SleepState expected = SleepState::Enabled;
	if (!sleep_state_.compare_exchange_strong(expected, state)) {
		return;
	}
	{
		std::lock_guard<std::mutex> counters(counters_mutex_);
		sleep_disabled_reason_ = reason;
	}
	log::warning("Reflex sleep %s: %s. Latency markers are still sent.",
			state == SleepState::Suspended ? "suspended until the window swapchain is re-created" : "disabled", reason.c_str());
}

bool Ll2Device::sleep() {
	if (sleep_state_.load() != SleepState::Enabled) {
		return false;
	}
	// One sleep per presented frame ("between presents"): without a present since the last
	// sleep (minimised window, a frame that did not draw, a swapchain being re-created) there is
	// nothing to pace, and a driver may hold the signal back until the next present.
	if (!presented_since_sleep_.exchange(false)) {
		std::lock_guard<std::mutex> counters(counters_mutex_);
		counters_.sleeps_skipped++;
		return false;
	}
	uint64_t value = 0;
	VkSemaphore semaphore = VK_NULL_HANDLE;
	VkResult result = VK_SUCCESS;
	{
		// Shared: vkLatencySleepNV may block for a frame on this driver, but under a shared lock it
		// does not stall the render thread's present/marker hooks; only a swapchain destroy (which
		// takes the lock exclusive) waits for it. sleep() is single-threaded (main), so sleep_value_
		// has no other writer.
		std::shared_lock lock(swapchain_mutex_);
		if (!valid_ || swapchain_ == VK_NULL_HANDLE || !latency_mode_enabled_) {
			return false;
		}
		semaphore = sleep_semaphore_;
		value = sleep_value_ + 1;
		VkLatencySleepInfoNV info{};
		info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
		info.signalSemaphore = semaphore;
		info.value = value;
		result = latency_sleep_(device_, swapchain_, &info);
		if (result == VK_SUCCESS) {
			sleep_value_ = value; // only a value the driver will signal advances the counter
		}
	}
	if (result != VK_SUCCESS) {
		stop_sleeping(SleepState::Disabled, format("vkLatencySleepNV returned VkResult %d", result));
		return false;
	}
	// Only our own semaphore is involved, so no lock is held: a swapchain re-creation on another
	// thread must not stall behind a frame of sleep.
	VkSemaphoreWaitInfo wait{};
	wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
	wait.semaphoreCount = 1;
	wait.pSemaphores = &semaphore;
	wait.pValues = &value;
	const auto start = std::chrono::steady_clock::now();
	result = wait_semaphores_(device_, &wait, SLEEP_TIMEOUT_NS);
	const double slept_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	signal_pending_ = result == VK_TIMEOUT;
	{
		std::lock_guard<std::mutex> counters(counters_mutex_);
		counters_.sleeps++;
		counters_.last_sleep_ms = slept_ms;
		counters_.average_sleep_ms = counters_.sleeps == 1 ? slept_ms : counters_.average_sleep_ms * 0.95 + slept_ms * 0.05;
		if (result == VK_TIMEOUT) {
			counters_.sleep_timeouts++;
		}
	}
	if (result == VK_TIMEOUT) {
		if (++consecutive_timeouts_ >= SLEEP_TIMEOUTS_BEFORE_SUSPENDING) {
			stop_sleeping(SleepState::Suspended,
					format("the driver did not signal the sleep semaphore within %llu ms, %d frames in a row",
							static_cast<unsigned long long>(SLEEP_TIMEOUT_NS / 1000000ull), SLEEP_TIMEOUTS_BEFORE_SUSPENDING));
		}
		return false;
	}
	if (result != VK_SUCCESS) {
		stop_sleeping(SleepState::Disabled, format("vkWaitSemaphores returned VkResult %d", result));
		return false;
	}
	consecutive_timeouts_ = 0;
	return true;
}

std::string Ll2Device::apply_sleep_mode_locked() {
	// Called with swapchain_mutex_ held (shared is enough for the LL2 call).
	if (!valid_ || swapchain_ == VK_NULL_HANDLE || !latency_mode_enabled_) {
		return ""; // applied when the window swapchain appears
	}
	const VkLatencySleepModeInfoNV info = to_vk(mode_);
	const VkResult result = set_sleep_mode_(device_, swapchain_, &info);
	std::lock_guard<std::mutex> counters(counters_mutex_);
	if (result != VK_SUCCESS) {
		sleep_mode_error_ = format("vkSetLatencySleepModeNV(%s, %u us) returned VkResult %d", reflex_mode_name(mode_.mode),
				mode_.minimum_interval_us, result);
		return sleep_mode_error_;
	}
	sleep_mode_error_.clear();
	return "";
}

void Ll2Device::set_sleep_mode(const SleepMode &mode) {
	std::string error;
	{
		std::unique_lock lock(swapchain_mutex_); // writes mode_
		mode_ = mode;
		error = apply_sleep_mode_locked();
	}
	if (!error.empty()) {
		log::warning("%s", error.c_str());
	} else {
		log::info("Reflex sleep mode: %s, minimum interval %u us", reflex_mode_name(mode.mode), mode.minimum_interval_us);
	}
}

std::vector<FrameReport> Ll2Device::latency_reports() {
	std::vector<FrameReport> out;
	std::shared_lock lock(swapchain_mutex_);
	if (!valid_ || swapchain_ == VK_NULL_HANDLE || !latency_mode_enabled_) {
		return out;
	}
	VkGetLatencyMarkerInfoNV info{};
	info.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV;
	get_timings_(device_, swapchain_, &info); // pTimings == nullptr: returns the available count
	const uint32_t count = std::min(info.timingCount, MAX_TIMING_REPORTS);
	if (count == 0) {
		return out;
	}
	std::vector<VkLatencyTimingsFrameReportNV> raw(count);
	for (VkLatencyTimingsFrameReportNV &r : raw) {
		r = {};
		r.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
	}
	info.timingCount = count;
	info.pTimings = raw.data();
	get_timings_(device_, swapchain_, &info);
	const uint32_t written = std::min(info.timingCount, count);
	out.reserve(written);
	for (uint32_t i = 0; i < written; ++i) {
		out.push_back(to_report(raw[i]));
	}
	return out;
}

VkResult Ll2Device::create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
		const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain, PFN_vkCreateSwapchainKHR next) {
	if (info == nullptr || !valid_) {
		return next(device, info, allocator, swapchain);
	}
	{
		std::shared_lock lock(swapchain_mutex_);
		// Latency mode is for the window the game runs in: the first surface a swapchain is
		// created for (Godot creates the main window before any other). Other windows' swapchains
		// pass through untouched.
		if (tracked_surface_ != VK_NULL_HANDLE && info->surface != tracked_surface_) {
			return next(device, info, allocator, swapchain);
		}
	}
	const SwapchainLatencyPatch patch(*info);
	VkResult result;
#ifdef DEBUG_ENABLED
	// Fault injection: force the first latency-mode create to fail so the fallback to a swapchain
	// without latency mode is exercised (GODOT_LATENCY_UPSCALING_TEST_FAULT=swapchain_create).
	if (testing::fault("swapchain_create") && !patch.host_provided() && info->oldSwapchain == VK_NULL_HANDLE) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else
#endif
		result = next(device, patch.get(), allocator, swapchain);
	bool latency_mode = patch.latency_mode();
	std::string note;
	if (result != VK_SUCCESS && !patch.host_provided()) {
		// Retry as the host asked. A failed create retires oldSwapchain, which must not be passed
		// again (VUID-VkSwapchainCreateInfoKHR-oldSwapchain-01933); a retired swapchain does not
		// block a new one on the same surface.
		note = format("creating the swapchain with latency mode failed (VkResult %d); created without it", result);
		VkSwapchainCreateInfoKHR retry = *info;
		retry.oldSwapchain = VK_NULL_HANDLE;
		result = next(device, &retry, allocator, swapchain);
		latency_mode = false;
	}
	if (result != VK_SUCCESS) {
		return result;
	}
	std::string mode_error;
	{
		std::unique_lock lock(swapchain_mutex_); // installs the new swapchain handle
		tracked_surface_ = info->surface;
		swapchain_ = *swapchain;
		latency_mode_enabled_ = latency_mode;
		{
			std::lock_guard<std::mutex> counters(counters_mutex_);
			swapchain_note_ = note;
			if (latency_mode) {
				counters_.swapchains_with_latency_mode++;
			}
		}
		mode_error = apply_sleep_mode_locked();
	}
	presented_since_sleep_ = false;
	consecutive_timeouts_ = 0;
	SleepState suspended = SleepState::Suspended;
	sleep_state_.compare_exchange_strong(suspended, SleepState::Enabled); // a new swapchain gets a new chance
	sequencer_.reset_present_history();
	for (const std::string &message : { note, mode_error }) {
		if (!message.empty()) {
			log::warning("Reflex: %s", message.c_str());
		}
	}
	return result;
}

void Ll2Device::forget_swapchain(VkSwapchainKHR swapchain) {
	// Exclusive: waits for any in-flight LL2 call (sleep/marker/present-id) before the caller's
	// vkDestroySwapchainKHR frees the handle, so none of them touches a freed swapchain.
	std::unique_lock lock(swapchain_mutex_);
	if (swapchain != VK_NULL_HANDLE && swapchain == swapchain_) {
		swapchain_ = VK_NULL_HANDLE;
		latency_mode_enabled_ = false;
	}
}

void Ll2Device::forget_surface(VkSurfaceKHR surface) {
	std::unique_lock lock(swapchain_mutex_);
	if (surface != VK_NULL_HANDLE && surface == tracked_surface_) {
		tracked_surface_ = VK_NULL_HANDLE; // the next window swapchain is tracked afresh
	}
}

VkResult Ll2Device::present(VkQueue queue, const VkPresentInfoKHR *info, PFN_vkQueuePresentKHR next) {
	if (info == nullptr) {
		return next(queue, info);
	}
	uint32_t index = UINT32_MAX;
	{
		std::shared_lock lock(swapchain_mutex_);
		if (!valid_ || swapchain_ == VK_NULL_HANDLE || !latency_mode_enabled_) {
			return next(queue, info);
		}
		for (uint32_t i = 0; i < info->swapchainCount; ++i) {
			if (info->pSwapchains[i] == swapchain_) {
				index = i;
				break;
			}
		}
	}
	if (index == UINT32_MAX) {
		return next(queue, info);
	}
	const uint64_t present_id = sequencer_.present_begin(); // [RENDERSUBMIT_END] PRESENT_START
	// Without the presentId feature the present goes out unstamped; markers still carry the ID.
	const PresentIdPatch patch(*info, index, present_id_enabled_ ? present_id : 0);
	if (patch.stamped()) {
		std::lock_guard<std::mutex> counters(counters_mutex_);
		counters_.presents_stamped++;
	}
	const VkResult result = next(queue, patch.get());
	sequencer_.present_end(present_id); // PRESENT_END (no-op without an ID)
	const VkResult window_result = info->pResults != nullptr ? info->pResults[index] : result;
	if (window_result == VK_SUCCESS || window_result == VK_SUBOPTIMAL_KHR) {
		presented_since_sleep_ = true;
	}
	return result;
}

VkResult Ll2Device::submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence, PFN_vkQueueSubmit next) {
	const uint64_t frame_id = sequencer_.submission_id();
	if (frame_id == 0 || count == 0 || submits == nullptr || !valid_) {
		return next(queue, count, submits, fence);
	}
	const SubmitTagPatch patch(submits, count, frame_id);
	if (patch.tagged() > 0) {
		std::lock_guard<std::mutex> counters(counters_mutex_);
		counters_.submits_tagged += patch.tagged();
	}
	return next(queue, count, patch.get(), fence);
}

Ll2Device::State Ll2Device::state() {
	State s;
	{
		std::shared_lock lock(swapchain_mutex_);
		s.has_swapchain = swapchain_ != VK_NULL_HANDLE;
		s.latency_mode_enabled = latency_mode_enabled_;
	}
	s.sleep_enabled = sleep_state_.load() == SleepState::Enabled;
	std::lock_guard<std::mutex> counters(counters_mutex_);
	s.sleep_disabled_reason = sleep_disabled_reason_;
	s.swapchain_note = swapchain_note_;
	s.sleep_mode_error = sleep_mode_error_;
	s.counters = counters_;
	return s;
}

// ---------------------------------------------------------------------------------------------
// ReflexFeature
// ---------------------------------------------------------------------------------------------

ReflexFeature &ReflexFeature::get() {
	// Never destroyed: Vulkan hooks may still run during process teardown, after static
	// destructors of this DLL would otherwise have executed.
	static ReflexFeature *instance = new ReflexFeature();
	return *instance;
}

void ReflexFeature::configure(bool enabled, const std::string &reason, const Settings &settings,
		const std::vector<std::string> &hidden_extensions) {
	std::lock_guard<std::mutex> lock(mutex_);
	enabled_ = enabled;
	disabled_reason_ = enabled ? "" : reason;
	hidden_extensions_ = hidden_extensions;
	mode_.mode = settings.reflex_mode;
	mode_.minimum_interval_us = fps_limit_to_interval_us(settings.reflex_fps_limit);
}

void ReflexFeature::request_instance_extensions(std::vector<std::string> &) {
	// VK_NV_low_latency2 and its dependencies are device extensions; Godot's instance already
	// uses Vulkan 1.2 (or VK_KHR_get_physical_device_properties2) for the feature queries.
}

bool ReflexFeature::request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_) {
		reason = disabled_reason_;
		return false;
	}
	auto version = [&](const char *name) -> uint32_t {
		for (const std::string &hidden : hidden_extensions_) {
			if (hidden == name) {
				return 0;
			}
		}
		return caps.extension_version(name);
	};
	const uint32_t ll2 = version(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
	if (ll2 == 0) {
		if (caps.vendor_id != NVIDIA_VENDOR_ID) {
			reason = format("'%s' (vendor 0x%04X) is not an NVIDIA GPU", caps.name.c_str(), caps.vendor_id);
		} else {
			reason = "the NVIDIA driver does not expose VK_NV_low_latency2 (update the NVIDIA graphics driver)";
		}
		return false;
	}
	if (ll2 < MIN_LL2_SPEC_VERSION) {
		reason = format("the driver's VK_NV_low_latency2 is revision %u; revision %u is needed (update the NVIDIA driver)",
				ll2, MIN_LL2_SPEC_VERSION);
		return false;
	}
	if (version(VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0 || !caps.present_id_feature) {
		reason = "VK_KHR_present_id, which VK_NV_low_latency2 depends on, is not supported";
		return false;
	}
	const bool core_timeline = caps.effective_api_version >= VK_API_VERSION_1_2;
	if (!caps.timeline_semaphore_feature || (!core_timeline && version(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0)) {
		reason = "timeline semaphores, which VK_NV_low_latency2 depends on, are not supported";
		return false;
	}
	request.add_extension(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
	request.add_extension(VK_KHR_PRESENT_ID_EXTENSION_NAME);
	if (!core_timeline) {
		request.add_extension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
	}
	request.features.timeline_semaphore = true;
	request.features.present_id = true;
	return true;
}

void ReflexFeature::on_device_created(const vk::DeviceContext &context, bool accepted, const std::string &reason) {
	std::shared_ptr<Ll2Device> device;
	if (accepted) {
		SleepMode mode;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			mode = mode_;
		}
		device = std::make_shared<Ll2Device>(context, mode);
		if (!device->valid()) {
			log::warning("Reflex unavailable on '%s': %s", context.caps.name.c_str(), device->init_error().c_str());
		}
	}
	std::lock_guard<std::mutex> lock(mutex_);
	const bool first = !any_device_;
	any_device_ = true;
	if (device && device->valid()) {
		devices_[device->key()] = device;
	} else if (first) {
		decline_reason_ = device ? device->init_error() : reason;
	}
}

void ReflexFeature::on_device_destroyed(VkDevice device) {
	std::shared_ptr<Ll2Device> gone;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = devices_.find(vk::dispatch_key(device));
		if (it == devices_.end()) {
			return;
		}
		gone = it->second;
		devices_.erase(it);
		if (active_ == gone) {
			fast_key_.store(nullptr);
			fast_device_.store(nullptr);
			active_.reset();
		}
	}
	gone->shutdown();
}

std::shared_ptr<Ll2Device> ReflexFeature::device_for(vk::DispatchKey key) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = devices_.find(key);
	return it != devices_.end() ? it->second : nullptr;
}

Ll2Device *ReflexFeature::fast_device_for(vk::DispatchKey key) const {
	return key != nullptr && fast_key_.load(std::memory_order_acquire) == key ? fast_device_.load(std::memory_order_acquire) : nullptr;
}

VkResult ReflexFeature::create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
		const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain, PFN_vkCreateSwapchainKHR next) {
	std::shared_ptr<Ll2Device> target = device_for(vk::dispatch_key(device));
	if (!target) {
		return next(device, info, allocator, swapchain);
	}
	const VkResult result = target->create_swapchain(device, info, allocator, swapchain, next);
	// The window device becomes the active one once it holds the tracked window swapchain, whether
	// or not latency mode was enabled: when the fallback path created it without latency mode, this
	// lets the status report that (instead of "waiting for the window swapchain"). The per-frame
	// hooks are no-ops while latency mode is off, so nothing runs on it in that case.
	if (result == VK_SUCCESS && target->state().has_swapchain) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!active_) {
			active_ = target;
			fast_device_.store(target.get(), std::memory_order_release);
			fast_key_.store(target->key(), std::memory_order_release);
		}
	}
	return result;
}

void ReflexFeature::destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator,
		PFN_vkDestroySwapchainKHR next) {
	if (std::shared_ptr<Ll2Device> target = device_for(vk::dispatch_key(device))) {
		target->forget_swapchain(swapchain);
	}
	next(device, swapchain, allocator);
}

void ReflexFeature::destroy_surface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator,
		PFN_vkDestroySurfaceKHR next) {
	std::vector<std::shared_ptr<Ll2Device>> devices;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &entry : devices_) {
			devices.push_back(entry.second);
		}
	}
	for (const std::shared_ptr<Ll2Device> &device : devices) {
		device->forget_surface(surface);
	}
	next(instance, surface, allocator);
}

VkResult ReflexFeature::queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence,
		PFN_vkQueueSubmit next) {
	if (Ll2Device *device = fast_device_for(vk::dispatch_key(queue))) {
		return device->submit(queue, count, submits, fence, next);
	}
	return next(queue, count, submits, fence);
}

VkResult ReflexFeature::queue_present(VkQueue queue, const VkPresentInfoKHR *info, PFN_vkQueuePresentKHR next) {
	if (Ll2Device *device = fast_device_for(vk::dispatch_key(queue))) {
		return device->present(queue, info, next);
	}
	return next(queue, info);
}

void ReflexFeature::set_sleep_mode(const SleepMode &mode) {
	std::vector<std::shared_ptr<Ll2Device>> devices;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		mode_ = mode;
		for (const auto &entry : devices_) {
			devices.push_back(entry.second);
		}
	}
	// Stored per device even without a swapchain, so a swapchain being re-created on the render
	// thread right now picks up this mode instead of the previous one.
	for (const std::shared_ptr<Ll2Device> &device : devices) {
		device->set_sleep_mode(mode);
	}
}

std::shared_ptr<Ll2Device> ReflexFeature::active_device() {
	std::lock_guard<std::mutex> lock(mutex_);
	return active_;
}

ReflexFeature::Status ReflexFeature::status() {
	std::shared_ptr<Ll2Device> device;
	Status s;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		s.requested = enabled_;
		if (!enabled_) {
			s.reason = disabled_reason_;
			return s;
		}
		if (!any_device_) {
			s.reason = "no Vulkan device has been created through the hook";
			return s;
		}
		if (devices_.empty()) {
			s.reason = decline_reason_.empty() ? "no device could enable VK_NV_low_latency2" : decline_reason_;
			return s;
		}
		s.supported = true;
		device = active_;
	}
	if (!device) {
		s.reason = "waiting for the window swapchain";
		return s;
	}
	const Ll2Device::State st = device->state();
	if (!st.has_swapchain) {
		s.reason = "the window swapchain is being re-created";
		return s;
	}
	if (!st.latency_mode_enabled) {
		s.reason = st.swapchain_note.empty() ? "the window swapchain was created without latency mode" : st.swapchain_note;
		return s;
	}
	s.active = true;
	if (!st.sleep_enabled) {
		s.reason = "markers only, sleep stopped: " + st.sleep_disabled_reason;
	}
	return s;
}

} // namespace lu::reflex
