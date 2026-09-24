// The upscaler's VulkanFeature: what recording an upscaler into Godot's command buffer needs from
// the presenting device, and the device-lifetime objects of the native recorders.
//
// Registered at CORE initialisation only when this run may upscale natively (see bootstrap.cpp);
// then vkCreateDevice gets VK_KHR_push_descriptor (the native recorders bind their images as push
// descriptors, so they never allocate from or disturb Godot's descriptor pools), and the native
// objects are destroyed right before the device, when Godot has already waited for it to go idle.
//
// Pure C++ plus Vulkan headers: unit-tested (tests/unit/test_upscale_feature.cpp).
#pragma once

#include "upscale/native_reference.h"
#include "vk/vk_feature.h"

#include <mutex>
#include <string>
#include <vector>

namespace lu::upscale {

class UpscaleFeature : public vk::VulkanFeature {
public:
	static UpscaleFeature &get();

	// CORE initialisation, before the host creates its device. `hidden_extensions` (debug test
	// knob) are treated as not offered by the GPU.
	void configure(bool native_recording, const std::vector<std::string> &hidden_extensions = {});
	bool native_recording_wanted() const;

	const char *name() const override { return "upscaler"; }
	void request_instance_extensions(std::vector<std::string> &extensions) override;
	bool request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) override;
	void on_device_created(const vk::DeviceContext &context, bool accepted, const std::string &reason) override;
	void on_device_destroyed(VkDevice device) override;

	// True when the presenting device was created with what native recording needs; otherwise
	// `reason` says why not.
	bool native_ready(std::string &reason) const;
	// Render thread: creates the native reference pipeline on the presenting device (idempotent).
	bool ensure_reference(const std::vector<uint32_t> &spirv, std::string &error);
	NativeReferenceRecorder &reference_recorder() { return reference_; }

private:
	mutable std::mutex mutex_;
	bool native_wanted_ = false;
	std::vector<std::string> hidden_extensions_;
	bool device_seen_ = false; // on_device_created ran for the presenting device
	bool accepted_ = false;
	std::string reason_; // why the device cannot be used (when !accepted_)
	VkDevice device_ = VK_NULL_HANDLE;
	PFN_vkGetDeviceProcAddr get_device_proc_addr_ = nullptr;
	NativeReferenceRecorder reference_; // never destroyed (a registered MarkerRecorder must outlive its use)
};

} // namespace lu::upscale
