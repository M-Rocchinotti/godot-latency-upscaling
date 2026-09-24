#include "upscale/upscale_feature.h"

#include <algorithm>

namespace lu::upscale {

UpscaleFeature &UpscaleFeature::get() {
	static UpscaleFeature *instance = new UpscaleFeature(); // never destroyed: the layer may call it during teardown
	return *instance;
}

void UpscaleFeature::configure(bool native_recording, const std::vector<std::string> &hidden_extensions) {
	std::lock_guard<std::mutex> lock(mutex_);
	native_wanted_ = native_recording;
	hidden_extensions_ = hidden_extensions;
}

bool UpscaleFeature::native_recording_wanted() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return native_wanted_;
}

void UpscaleFeature::request_instance_extensions(std::vector<std::string> &) {
	// VK_KHR_push_descriptor is a device extension; nothing is needed on the instance.
}

bool UpscaleFeature::request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!native_wanted_) {
		reason = "native recording is not used in this run";
		return false;
	}
	const bool hidden = std::find(hidden_extensions_.begin(), hidden_extensions_.end(), VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) !=
			hidden_extensions_.end();
	if (hidden || !caps.has_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
		reason = std::string("the GPU does not offer ") + VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
		return false;
	}
	request.add_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	return true;
}

void UpscaleFeature::on_device_created(const vk::DeviceContext &context, bool accepted, const std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	device_seen_ = true;
	accepted_ = accepted && context.has_enabled_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	reason_ = accepted_ ? std::string() : (reason.empty() ? std::string("the device was created without ") + VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME : reason);
	device_ = context.device;
	get_device_proc_addr_ = context.get_device_proc_addr;
}

void UpscaleFeature::on_device_destroyed(VkDevice device) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (device == VK_NULL_HANDLE || device != device_) {
		return; // a local device, or not ours
	}
	reference_.destroy(); // Godot has waited for the device to go idle before destroying it
	device_ = VK_NULL_HANDLE;
	get_device_proc_addr_ = nullptr;
	accepted_ = false;
	reason_ = "the device was destroyed";
}

bool UpscaleFeature::native_ready(std::string &reason) const {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!native_wanted_) {
		reason = "native recording is not used in this run";
		return false;
	}
	if (!device_seen_) {
		reason = "the Vulkan device was not created through the addon's layer";
		return false;
	}
	if (!accepted_) {
		reason = reason_;
		return false;
	}
	return true;
}

bool UpscaleFeature::ensure_reference(const std::vector<uint32_t> &spirv, std::string &error) {
	VkDevice device;
	PFN_vkGetDeviceProcAddr get_device_proc_addr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!accepted_ || device_ == VK_NULL_HANDLE) {
			error = reason_.empty() ? "no device for native recording" : reason_;
			return false;
		}
		device = device_;
		get_device_proc_addr = get_device_proc_addr_;
	}
	return reference_.create(device, get_device_proc_addr, spirv, error);
}

} // namespace lu::upscale
