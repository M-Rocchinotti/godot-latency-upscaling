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
//   vkBeginCommandBuffer / vkEndCommandBuffer, vkCmdBindPipeline, vkCmdBindDescriptorSets,
//   vkCmdPushConstants, vkCmdDispatch
//                                            - only when command interception is enabled: the
//                                              upscaler's marker dispatch (marker_interceptor.h)
// Each hook ends in the loader's own exported trampoline for that function, so validation
// layers, overlays and the driver see the calls exactly as without the hook.
#pragma once

#include "vk/marker_interceptor.h"
#include "vk/queue_hooks.h"
#include "vk/vk_feature.h"

#include <windows.h>

#include <string>
#include <vector>

namespace lu::vk {

// Registration happens at CORE initialisation, before the host creates a Vulkan instance.
void register_feature(VulkanFeature *feature);
void set_queue_hooks(QueueHooks *hooks);

// Phase 2: send the host's Vulkan calls through `module` (a DLL exporting the Vulkan API in front of
// the real loader: Streamline's sl.interposer.dll) instead of the real loader. Must be set at CORE
// initialisation, before the host asks for vulkan-1.dll; the module must stay loaded. When it
// lacks an entry point the layer needs, the real loader is used (snapshot().loader_override_problem).
void set_loader_override(HMODULE module, const char *name);

// Command-buffer interception (Phase 2). Must be enabled at CORE initialisation: the host resolves
// its vkCmd* functions right after vkCreateInstance, and only then can they be hooked.
void enable_command_interception();
// Enabled, the loader provides the functions, and the host resolved them through the layer.
bool command_interception_active();
bool add_marker(VkPipeline pipeline, MarkerRecorder *recorder); // see MarkerInterceptor::add_marker
void remove_marker(VkPipeline pipeline);
InterceptorStats interceptor_stats();

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
	int devices_created = 0; // the presenting device (0 or 1)
	int local_devices_created = 0; // secondary devices (create_local_rendering_device), passed through
	std::vector<std::string> instance_layers; // explicit layers of the last instance (e.g. validation)
	// The presenting device (the first created through the layer); local devices do not change it.
	uint64_t device_handle = 0;
	std::string gpu_name;
	uint32_t vendor_id = 0;
	uint32_t driver_version = 0;
	uint32_t api_version = 0;
	std::vector<std::string> device_extensions;
	std::vector<std::string> added_device_extensions;
	std::string device_note; // e.g. the fallback to an unpatched vkCreateDevice
	bool command_hooks_resolved = false; // the host resolved the vkCmd* hooks (command interception)
	std::string loader_override; // e.g. "sl.interposer.dll" (set_loader_override)
	bool loader_override_active = false; // the host's Vulkan calls go through it
	std::string loader_override_problem;
};
LayerSnapshot snapshot();

} // namespace lu::vk
