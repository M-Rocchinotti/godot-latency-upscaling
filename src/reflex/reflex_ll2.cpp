#include "reflex/reflex_ll2.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace nvdlss::reflex {
namespace {

constexpr uint32_t NVIDIA_VENDOR_ID = 0x10DE;
constexpr uint32_t MAX_TIMING_REPORTS = 64; // the driver keeps the last 64 frames

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

Ll2Device::Ll2Device(const vk::DeviceContext &context) :
		device_(context.device), key_(vk::dispatch_key(context.device)) {
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
	std::lock_guard<std::mutex> lock(mutex_);
	if (sleep_semaphore_ != VK_NULL_HANDLE && destroy_semaphore_ != nullptr) {
		destroy_semaphore_(device_, sleep_semaphore_, nullptr);
	}
	sleep_semaphore_ = VK_NULL_HANDLE;
	swapchain_ = VK_NULL_HANDLE;
	latency_mode_enabled_ = false;
	valid_ = false;
}

void Ll2Device::emit_marker(uint64_t frame_id, Marker marker) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!valid_ || swapchain_ == VK_NULL_HANDLE || !latency_mode_enabled_) {
		return;
	}
	VkSetLatencyMarkerInfoNV info{};
	info.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
	info.presentID = frame_id;
	info.marker = static_cast<VkLatencyMarkerNV>(marker);
	set_marker_(device_, swapchain_, &info);
	counters_.markers[static_cast<size_t>(marker)]++;
}

void Ll2Device::disable_sleep(const std::string &reason) {
	if (sleep_enabled_.exchange(false)) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			sleep_disabled_reason_ = reason;
		}
		log::warning("Reflex sleep disabled: %s. Latency markers are still sent.", reason.c_str());
	}
}

bool Ll2Device::sleep() {
	if (!sleep_enabled_.load()) {
		return false;
	}
	uint64_t value = 0;
	std::string failure;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!valid_ || swapchain_ == VK_NULL_HANDLE || !latency_mode_enabled_) {
			return false;
		}
		value = ++sleep_value_;
		VkLatencySleepInfoNV info{};
		info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
		info.signalSemaphore = sleep_semaphore_;
		info.value = value;
		const VkResult result = latency_sleep_(device_, swapchain_, &info);
		if (result != VK_SUCCESS) {
			--sleep_value_; // nothing will signal this value
			failure = format("vkLatencySleepNV returned VkResult %d", result);
		}
	}
	if (!failure.empty()) {
		disable_sleep(failure);
		return false;
	}
	// The wait only touches our own semaphore, so the swapchain lock is not held here: a resize
	// on another thread must not stall behind a full frame of sleep.
	VkSemaphoreWaitInfo wait{};
	wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
	wait.semaphoreCount = 1;
	wait.pSemaphores = &sleep_semaphore_;
	wait.pValues = &value;
	const auto start = std::chrono::steady_clock::now();
	const VkResult result = wait_semaphores_(device_, &wait, SLEEP_TIMEOUT_NS);
	const double slept_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		counters_.sleeps++;
		counters_.last_sleep_ms = slept_ms;
		counters_.average_sleep_ms = counters_.sleeps == 1 ? slept_ms : counters_.average_sleep_ms * 0.95 + slept_ms * 0.05;
		if (result == VK_TIMEOUT) {
			counters_.sleep_timeouts++;
		}
	}
	if (result == VK_TIMEOUT) {
		if (++consecutive_timeouts_ >= SLEEP_TIMEOUTS_BEFORE_GIVING_UP) {
			disable_sleep(format("the driver did not signal the sleep semaphore within %llu ms, %d frames in a row",
					static_cast<unsigned long long>(SLEEP_TIMEOUT_NS / 1000000ull), SLEEP_TIMEOUTS_BEFORE_GIVING_UP));
		}
		return false;
	}
	if (result != VK_SUCCESS) {
		disable_sleep(format("vkWaitSemaphores returned VkResult %d", result));
		return false;
	}
	consecutive_timeouts_ = 0;
	return true;
}

void Ll2Device::apply_sleep_mode(const SleepMode &mode) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!valid_ || swapchain_ == VK_NULL_HANDLE || !latency_mode_enabled_) {
		return; // applied when the window swapchain appears
	}
	const VkLatencySleepModeInfoNV info = to_vk(mode);
	const VkResult result = set_sleep_mode_(device_, swapchain_, &info);
	if (result != VK_SUCCESS) {
		sleep_mode_error_ = format("vkSetLatencySleepModeNV(%s, %u us) returned VkResult %d", reflex_mode_name(mode.mode),
				mode.minimum_interval_us, result);
		log::warning("%s", sleep_mode_error_.c_str());
	} else {
		sleep_mode_error_.clear();
		log::info("Reflex sleep mode: %s, minimum interval %u us", reflex_mode_name(mode.mode), mode.minimum_interval_us);
	}
}

std::vector<FrameReport> Ll2Device::latency_reports() {
	std::vector<FrameReport> out;
	std::lock_guard<std::mutex> lock(mutex_);
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
		const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain, PFN_vkCreateSwapchainKHR next,
		const SleepMode &mode) {
	if (info == nullptr || !valid_) {
		return next(device, info, allocator, swapchain);
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		// Latency mode is for the window the game runs in: the first surface a swapchain is
		// created for (Godot creates the main window before any other). Other windows' swapchains
		// pass through untouched.
		if (tracked_surface_ != VK_NULL_HANDLE && info->surface != tracked_surface_) {
			return next(device, info, allocator, swapchain);
		}
	}
	VkResult result;
	bool latency_mode = false;
	std::string note;
	if (const auto *existing = reinterpret_cast<const VkSwapchainLatencyCreateInfoNV *>(
				vk::find_in_chain(info->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV))) {
		// The host asked for latency mode itself: respect its choice.
		result = next(device, info, allocator, swapchain);
		latency_mode = existing->latencyModeEnable == VK_TRUE;
	} else {
		VkSwapchainLatencyCreateInfoNV latency{};
		latency.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
		latency.pNext = info->pNext;
		latency.latencyModeEnable = VK_TRUE;
		VkSwapchainCreateInfoKHR patched = *info;
		patched.pNext = &latency;
		result = next(device, &patched, allocator, swapchain);
		latency_mode = result == VK_SUCCESS;
		// A failed create retires oldSwapchain, which may then not be passed again; retrying
		// without latency mode is only valid when there was none.
		if (result != VK_SUCCESS && info->oldSwapchain == VK_NULL_HANDLE) {
			note = format("creating the swapchain with latency mode failed (VkResult %d); created without it", result);
			log::warning("Reflex: %s", note.c_str());
			result = next(device, info, allocator, swapchain);
		}
	}
	if (result != VK_SUCCESS) {
		return result;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		tracked_surface_ = info->surface;
		swapchain_ = *swapchain;
		latency_mode_enabled_ = latency_mode;
		swapchain_note_ = note;
		if (latency_mode) {
			counters_.swapchains_with_latency_mode++;
		}
	}
	sequencer_.reset_present_history();
	apply_sleep_mode(mode);
	return result;
}

void Ll2Device::forget_swapchain(VkSwapchainKHR swapchain) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (swapchain != VK_NULL_HANDLE && swapchain == swapchain_) {
		swapchain_ = VK_NULL_HANDLE;
		latency_mode_enabled_ = false;
	}
}

VkResult Ll2Device::present(VkQueue queue, const VkPresentInfoKHR *info, PFN_vkQueuePresentKHR next) {
	if (info == nullptr) {
		return next(queue, info);
	}
	uint32_t index = UINT32_MAX;
	{
		std::lock_guard<std::mutex> lock(mutex_);
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
	if (present_id == 0) {
		return next(queue, info);
	}
	VkPresentInfoKHR patched = *info;
	VkPresentIdKHR id_info{};
	thread_local std::vector<uint64_t> ids;
	const bool host_has_ids = vk::find_in_chain(info->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR) != nullptr ||
			vk::find_in_chain(info->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) != nullptr;
	if (present_id_enabled_ && !host_has_ids) {
		ids.assign(info->swapchainCount, 0); // 0 = "no ID" for the other windows' swapchains
		ids[index] = present_id;
		id_info.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
		id_info.pNext = info->pNext;
		id_info.swapchainCount = info->swapchainCount;
		id_info.pPresentIds = ids.data();
		patched.pNext = &id_info;
		std::lock_guard<std::mutex> lock(mutex_);
		counters_.presents_stamped++;
	}
	const VkResult result = next(queue, &patched);
	sequencer_.present_end(present_id); // PRESENT_END
	return result;
}

VkResult Ll2Device::submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence, PFN_vkQueueSubmit next) {
	const uint64_t frame_id = sequencer_.submission_id();
	if (frame_id == 0 || count == 0 || submits == nullptr || !valid_) {
		return next(queue, count, submits, fence);
	}
	thread_local std::vector<VkSubmitInfo> copies;
	thread_local std::vector<VkLatencySubmissionPresentIdNV> tags;
	copies.assign(submits, submits + count);
	tags.assign(count, VkLatencySubmissionPresentIdNV{});
	uint32_t tagged = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (vk::find_in_chain(submits[i].pNext, VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV) != nullptr) {
			continue; // the host tags its own submissions
		}
		tags[i].sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV;
		tags[i].pNext = submits[i].pNext;
		tags[i].presentID = frame_id;
		copies[i].pNext = &tags[i];
		++tagged;
	}
	if (tagged > 0) {
		std::lock_guard<std::mutex> lock(mutex_);
		counters_.submits_tagged += tagged;
	}
	return next(queue, count, copies.data(), fence);
}

Ll2Device::State Ll2Device::state() {
	std::lock_guard<std::mutex> lock(mutex_);
	State s;
	s.has_swapchain = swapchain_ != VK_NULL_HANDLE;
	s.latency_mode_enabled = latency_mode_enabled_;
	s.sleep_enabled = sleep_enabled_.load();
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
	auto available = [&](const char *name) {
		for (const std::string &hidden : hidden_extensions_) {
			if (hidden == name) {
				return false;
			}
		}
		return caps.has_extension(name);
	};
	if (!available(VK_NV_LOW_LATENCY_2_EXTENSION_NAME)) {
		if (caps.vendor_id != NVIDIA_VENDOR_ID) {
			reason = format("'%s' (vendor 0x%04X) is not an NVIDIA GPU", caps.name.c_str(), caps.vendor_id);
		} else {
			reason = "the NVIDIA driver does not expose VK_NV_low_latency2 (driver 551.70 or newer is needed)";
		}
		return false;
	}
	if (!available(VK_KHR_PRESENT_ID_EXTENSION_NAME) || !caps.present_id_feature) {
		reason = "VK_KHR_present_id, which VK_NV_low_latency2 depends on, is not supported";
		return false;
	}
	const bool core_timeline = caps.effective_api_version >= VK_API_VERSION_1_2;
	if (!caps.timeline_semaphore_feature || (!core_timeline && !available(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))) {
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
		device = std::make_shared<Ll2Device>(context);
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
			fast_active_.store(nullptr);
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

VkResult ReflexFeature::create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
		const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain, PFN_vkCreateSwapchainKHR next) {
	std::shared_ptr<Ll2Device> target = device_for(vk::dispatch_key(device));
	if (!target) {
		return next(device, info, allocator, swapchain);
	}
	const VkResult result = target->create_swapchain(device, info, allocator, swapchain, next, sleep_mode());
	if (result == VK_SUCCESS && target->state().latency_mode_enabled) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!active_) {
			active_ = target;
			fast_active_.store(target.get());
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

VkResult ReflexFeature::queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence,
		PFN_vkQueueSubmit next) {
	Ll2Device *device = fast_active_.load(std::memory_order_acquire);
	if (device != nullptr && device->key() == vk::dispatch_key(queue)) {
		return device->submit(queue, count, submits, fence, next);
	}
	return next(queue, count, submits, fence);
}

VkResult ReflexFeature::queue_present(VkQueue queue, const VkPresentInfoKHR *info, PFN_vkQueuePresentKHR next) {
	Ll2Device *device = fast_active_.load(std::memory_order_acquire);
	if (device != nullptr && device->key() == vk::dispatch_key(queue)) {
		return device->present(queue, info, next);
	}
	return next(queue, info);
}

void ReflexFeature::set_sleep_mode(const SleepMode &mode) {
	std::shared_ptr<Ll2Device> device;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		mode_ = mode;
		device = active_;
	}
	if (device) {
		device->apply_sleep_mode(mode);
	}
}

SleepMode ReflexFeature::sleep_mode() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return mode_;
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
		s.reason = "the window swapchain is being recreated";
		return s;
	}
	if (!st.latency_mode_enabled) {
		s.reason = st.swapchain_note.empty() ? "the window swapchain was created without latency mode" : st.swapchain_note;
		return s;
	}
	s.active = true;
	if (!st.sleep_enabled) {
		s.reason = "markers only, sleep disabled: " + st.sleep_disabled_reason;
	}
	return s;
}

} // namespace nvdlss::reflex
