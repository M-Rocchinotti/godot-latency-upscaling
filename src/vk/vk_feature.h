// The extension point of the Vulkan layer: a feature (Reflex now, DLSS in Phase 2) states what
// it needs from the instance and the device, and is told about the device it got.
#pragma once

#include "vk/device_patch.h"
#include "vk/vk_common.h"

#include <map>
#include <string>
#include <vector>

namespace lu::vk {

// What the physical device offers, gathered once per vkCreateDevice.
struct PhysicalDeviceCaps {
	VkPhysicalDevice handle = VK_NULL_HANDLE;
	uint32_t vendor_id = 0;
	uint32_t device_id = 0;
	uint32_t driver_version = 0;
	// min(device apiVersion, the instance's VkApplicationInfo::apiVersion): the version the
	// device is actually used at, which decides what is core.
	uint32_t effective_api_version = VK_API_VERSION_1_0;
	std::string name;
	std::map<std::string, uint32_t> extensions; // name -> spec version
	bool timeline_semaphore_feature = false;
	bool present_id_feature = false;

	bool has_extension(const char *extension) const { return extension_version(extension) != 0; }
	uint32_t extension_version(const char *extension) const; // 0 = not offered
};

// Functions a feature may use on the device it was created with. Resolved through the real
// loader (never through the hook) so a feature's own calls are not intercepted.
struct DeviceContext {
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
	PhysicalDeviceCaps caps;
	std::vector<std::string> enabled_extensions; // final list the device was created with
	DeviceFeatureRequest enabled_features; // bits the patch turned on (or the host already had)
	bool has_enabled_extension(const char *name) const;
};

class VulkanFeature {
public:
	virtual ~VulkanFeature() = default;
	virtual const char *name() const = 0;

	// vkCreateInstance: instance extensions to enable when the loader offers them.
	virtual void request_instance_extensions(std::vector<std::string> &extensions) = 0;

	// vkCreateDevice: returns false with a human-readable reason when the feature cannot run on
	// this device (then it must not have touched `request`).
	virtual bool request_device(const PhysicalDeviceCaps &caps, DeviceRequest &request, std::string &reason) = 0;

	// After a successful vkCreateDevice. `accepted` is false when request_device() declined or
	// when the device had to be created without the patch (see dispatch.cpp's fallback).
	virtual void on_device_created(const DeviceContext &context, bool accepted, const std::string &reason) = 0;
	// Before vkDestroyDevice.
	virtual void on_device_destroyed(VkDevice device) = 0;
};

} // namespace lu::vk
