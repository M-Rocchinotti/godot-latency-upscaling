// The in-process Vulkan dispatch layer that sits between Godot and the Vulkan loader.
//
// Godot 4.7 resolves every Vulkan function through the vkGetInstanceProcAddr of the module
// that LoadLibraryA("vulkan-1.dll") returns (volk). The LoadLibrary hook hands Godot this DLL
// instead; its exported vkGetInstanceProcAddr (below) returns the real loader's functions for
// everything except the few calls the layer has to see:
//   vkCreateInstance / vkDestroyInstance     - remember the instance (API version)
//   vkCreateDevice / vkDestroyDevice         - add the features' extensions and feature bits
//   vkGetDeviceProcAddr                      - keep returning the hooks for device functions
//   vkCreateSwapchainKHR / vkDestroySwapchainKHR, vkQueueSubmit, vkQueuePresentKHR
//                                            - forwarded to the QueueHooks owner (Reflex)
// Each hook ends in the loader's own exported trampoline for that function, so validation
// layers, overlays and the driver see the calls exactly as without the hook.
#pragma once

#include "vk/queue_hooks.h"
#include "vk/vk_feature.h"

#include <windows.h>

#include <string>
#include <vector>

namespace nvdlss::vk {

// Registration happens at CORE initialisation, before the host creates a Vulkan instance.
void register_feature(VulkanFeature *feature);
void set_queue_hooks(QueueHooks *hooks);

// Repeats the host's original LoadLibrary* call (same function, same arguments) so the real
// loader is found exactly where the host would have found it.
using LoadRealLoader = HMODULE (*)(const void *request);

// Called by the LoadLibrary hook for a "vulkan-1.dll" request. Loads the real loader and
// returns the module the host should get: this DLL (whose exported vkGetInstanceProcAddr is the
// proxy) or, when interception is impossible, the real loader (pure pass-through). Returns
// nullptr only when the real loader cannot be loaded; the host then sees the original failure
// (GetLastError is left as the real LoadLibrary set it).
HMODULE on_loader_requested(LoadRealLoader load_real, const void *request);

struct LayerSnapshot {
	bool loader_intercepted = false; // the host's vulkan-1.dll request reached the layer
	std::string loader_problem; // non-empty when the proxy could not be used
	int instances_created = 0;
	int devices_created = 0;
	std::vector<std::string> instance_layers; // explicit layers of the last instance (e.g. validation)
	// Last device created through the layer (Godot's main device in practice).
	uint64_t device_handle = 0;
	std::string gpu_name;
	uint32_t vendor_id = 0;
	uint32_t driver_version = 0;
	uint32_t api_version = 0;
	std::vector<std::string> device_extensions;
	std::vector<std::string> added_device_extensions;
	std::string device_note; // e.g. the fallback to an unpatched vkCreateDevice
};
LayerSnapshot snapshot();

} // namespace nvdlss::vk
