#include "vk/dispatch.h"

#include "core/log.h"
#include "core/testing.h"
#include "vk/device_patch.h"
#include "vk/marker_interceptor.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace lu::vk {
namespace {

// The loader's exported entry points ("trampolines"). They dispatch through the handle they
// receive, so they are valid for every instance and device the loader created, and a hook that
// ends in them behaves exactly like the call the host would have made without the layer.
struct Next {
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
	PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
	PFN_vkCreateInstance CreateInstance = nullptr;
	PFN_vkDestroyInstance DestroyInstance = nullptr;
	PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr; // for queue requests
	PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr; // loaders >= 1.1
	PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
	PFN_vkCreateDevice CreateDevice = nullptr;
	PFN_vkDestroyDevice DestroyDevice = nullptr;
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
	PFN_vkQueueSubmit QueueSubmit = nullptr;
	PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
	PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
	PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
	PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
	// Command-buffer interception (Phase 2, only handed out when enabled).
	PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
	PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
	PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
	PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
	PFN_vkCmdPushConstants CmdPushConstants = nullptr;
	PFN_vkCmdDispatch CmdDispatch = nullptr;
};

struct InstanceInfo {
	VkInstance instance = VK_NULL_HANDLE;
	uint32_t app_api_version = VK_API_VERSION_1_0;
	bool has_properties2_extension = false;
};

struct Layer {
	std::mutex mutex; // guards instances, snap and the loader set-up
	HMODULE real_loader = nullptr;
	std::atomic<bool> ready{ false }; // `next` is complete; set once, never cleared
	Next next; // written once before `ready`, read-only afterwards
	// Registered at CORE initialisation, before the host makes any Vulkan call, and never
	// changed afterwards: read without the lock.
	std::vector<VulkanFeature *> features;
	std::atomic<QueueHooks *> queue_hooks{ nullptr };
	// The vkCmd* hooks: requested at CORE initialisation (enable_command_interception), usable
	// once the loader provided every command function.
	std::atomic<bool> command_hooks_wanted{ false };
	std::atomic<bool> command_hooks_usable{ false };
	std::atomic<bool> command_hooks_resolved{ false }; // the host resolved at least one of them
	MarkerInterceptor interceptor;
	// Phase 2: a module exporting the Vulkan API in front of the real loader (Streamline's
	// interposer), set at CORE initialisation. Held for the process lifetime.
	HMODULE loader_override = nullptr;
	std::map<DispatchKey, InstanceInfo> instances;
	bool main_device_created = false; // the first (presenting) device; later ones are local devices
	LayerSnapshot snap;
};

Layer &layer() {
	// Never destroyed: the host may call through the proxy during its own teardown.
	static Layer *instance = new Layer();
	return *instance;
}

template <typename Fn>
Fn loader_export(HMODULE loader, const char *name) {
	return reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(loader, name)));
}

// The entry points the layer calls, from a module exporting the Vulkan API (the real loader, or
// an interposer in front of it). Returns whether everything the layer needs is there.
bool fill_next(HMODULE module, Next &next, CommandFunctions &commands, bool &commands_complete) {
	next.GetInstanceProcAddr = loader_export<PFN_vkGetInstanceProcAddr>(module, "vkGetInstanceProcAddr");
	next.EnumerateInstanceExtensionProperties = loader_export<PFN_vkEnumerateInstanceExtensionProperties>(module, "vkEnumerateInstanceExtensionProperties");
	next.CreateInstance = loader_export<PFN_vkCreateInstance>(module, "vkCreateInstance");
	next.DestroyInstance = loader_export<PFN_vkDestroyInstance>(module, "vkDestroyInstance");
	next.GetPhysicalDeviceProperties = loader_export<PFN_vkGetPhysicalDeviceProperties>(module, "vkGetPhysicalDeviceProperties");
	next.GetPhysicalDeviceQueueFamilyProperties =
			loader_export<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(module, "vkGetPhysicalDeviceQueueFamilyProperties");
	next.GetPhysicalDeviceFeatures2 = loader_export<PFN_vkGetPhysicalDeviceFeatures2>(module, "vkGetPhysicalDeviceFeatures2");
	next.EnumerateDeviceExtensionProperties = loader_export<PFN_vkEnumerateDeviceExtensionProperties>(module, "vkEnumerateDeviceExtensionProperties");
	next.CreateDevice = loader_export<PFN_vkCreateDevice>(module, "vkCreateDevice");
	next.DestroyDevice = loader_export<PFN_vkDestroyDevice>(module, "vkDestroyDevice");
	next.GetDeviceProcAddr = loader_export<PFN_vkGetDeviceProcAddr>(module, "vkGetDeviceProcAddr");
	next.QueueSubmit = loader_export<PFN_vkQueueSubmit>(module, "vkQueueSubmit");
	next.CreateSwapchainKHR = loader_export<PFN_vkCreateSwapchainKHR>(module, "vkCreateSwapchainKHR");
	next.DestroySwapchainKHR = loader_export<PFN_vkDestroySwapchainKHR>(module, "vkDestroySwapchainKHR");
	next.QueuePresentKHR = loader_export<PFN_vkQueuePresentKHR>(module, "vkQueuePresentKHR");
	next.DestroySurfaceKHR = loader_export<PFN_vkDestroySurfaceKHR>(module, "vkDestroySurfaceKHR");
	next.BeginCommandBuffer = loader_export<PFN_vkBeginCommandBuffer>(module, "vkBeginCommandBuffer");
	next.EndCommandBuffer = loader_export<PFN_vkEndCommandBuffer>(module, "vkEndCommandBuffer");
	next.CmdBindPipeline = loader_export<PFN_vkCmdBindPipeline>(module, "vkCmdBindPipeline");
	next.CmdBindDescriptorSets = loader_export<PFN_vkCmdBindDescriptorSets>(module, "vkCmdBindDescriptorSets");
	next.CmdPushConstants = loader_export<PFN_vkCmdPushConstants>(module, "vkCmdPushConstants");
	next.CmdDispatch = loader_export<PFN_vkCmdDispatch>(module, "vkCmdDispatch");
	commands.bind_pipeline = next.CmdBindPipeline;
	commands.bind_descriptor_sets = next.CmdBindDescriptorSets;
	commands.push_constants = next.CmdPushConstants;
	commands.dispatch = next.CmdDispatch;
	commands_complete = commands.complete() && next.BeginCommandBuffer && next.EndCommandBuffer;
	return next.GetInstanceProcAddr && next.EnumerateInstanceExtensionProperties && next.CreateInstance &&
			next.DestroyInstance && next.GetPhysicalDeviceProperties && next.EnumerateDeviceExtensionProperties &&
			next.CreateDevice && next.DestroyDevice && next.GetDeviceProcAddr && next.QueueSubmit &&
			next.CreateSwapchainKHR && next.DestroySwapchainKHR && next.QueuePresentKHR && next.DestroySurfaceKHR;
}

bool contains(const std::vector<std::string> &list, const char *name) {
	return std::find(list.begin(), list.end(), name) != list.end();
}

std::vector<std::string> names_of(const char *const *names, uint32_t count) {
	return std::vector<std::string>(names, names + count);
}

// --- Hooks -------------------------------------------------------------------------------

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hook_GetDeviceProcAddr(VkDevice device, const char *name);

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
		VkInstance *instance) {
	Layer &L = layer();
	if (info == nullptr || instance == nullptr) {
		return L.next.CreateInstance(info, allocator, instance);
	}
	std::vector<std::string> wanted;
	for (VulkanFeature *feature : L.features) {
		feature->request_instance_extensions(wanted);
	}
	std::vector<std::string> enabled = names_of(info->ppEnabledExtensionNames, info->enabledExtensionCount);
	std::vector<std::string> added;
	if (!wanted.empty()) {
		uint32_t count = 0;
		L.next.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> available(count);
		L.next.EnumerateInstanceExtensionProperties(nullptr, &count, available.data());
		for (const std::string &name : wanted) {
			const bool offered = std::any_of(available.begin(), available.end(),
					[&](const VkExtensionProperties &p) { return name == p.extensionName; });
			if (offered && !contains(enabled, name.c_str()) && !contains(added, name.c_str())) {
				added.push_back(name);
			}
		}
	}
	VkResult result;
	if (!added.empty()) {
		std::vector<const char *> names(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
		for (const std::string &name : added) {
			names.push_back(name.c_str());
		}
		VkInstanceCreateInfo patched = *info;
		patched.enabledExtensionCount = static_cast<uint32_t>(names.size());
		patched.ppEnabledExtensionNames = names.data();
		result = L.next.CreateInstance(&patched, allocator, instance);
		if (result != VK_SUCCESS) {
			log::warning("vkCreateInstance with the addon's instance extensions failed (VkResult %d); retrying without them", result);
			added.clear();
			result = L.next.CreateInstance(info, allocator, instance);
		}
	} else {
		result = L.next.CreateInstance(info, allocator, instance);
	}
	if (result != VK_SUCCESS) {
		return result;
	}
	enabled.insert(enabled.end(), added.begin(), added.end());
	InstanceInfo inst;
	inst.instance = *instance;
	if (info->pApplicationInfo != nullptr && info->pApplicationInfo->apiVersion != 0) {
		inst.app_api_version = info->pApplicationInfo->apiVersion;
	}
	inst.has_properties2_extension = contains(enabled, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	std::lock_guard<std::mutex> lock(L.mutex);
	L.instances[dispatch_key(*instance)] = inst;
	L.snap.instances_created++;
	L.snap.instance_layers = names_of(info->ppEnabledLayerNames, info->enabledLayerCount);
	return result;
}

VKAPI_ATTR void VKAPI_CALL hook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
	Layer &L = layer();
	{
		std::lock_guard<std::mutex> lock(L.mutex);
		L.instances.erase(dispatch_key(instance));
	}
	L.next.DestroyInstance(instance, allocator);
}

PhysicalDeviceCaps query_caps(const InstanceInfo &inst, VkPhysicalDevice physical_device) {
	const Next &next = layer().next;
	PhysicalDeviceCaps caps;
	caps.instance = inst.instance;
	caps.handle = physical_device;
	VkPhysicalDeviceProperties props{};
	next.GetPhysicalDeviceProperties(physical_device, &props);
	caps.vendor_id = props.vendorID;
	caps.device_id = props.deviceID;
	caps.driver_version = props.driverVersion;
	caps.effective_api_version = std::min(props.apiVersion, inst.app_api_version);
	caps.name = props.deviceName;

	uint32_t count = 0;
	next.EnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> extensions(count);
	next.EnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data());
	for (uint32_t i = 0; i < count; ++i) {
		caps.extensions[extensions[i].extensionName] = extensions[i].specVersion;
	}
	if (next.GetPhysicalDeviceQueueFamilyProperties != nullptr) {
		uint32_t family_count = 0;
		next.GetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
		caps.queue_families.resize(family_count);
		next.GetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, caps.queue_families.data());
		caps.queue_families.resize(family_count);
	}

	// Feature queries need Vulkan 1.1 on the instance or VK_KHR_get_physical_device_properties2.
	PFN_vkGetPhysicalDeviceFeatures2 features2 = nullptr;
	if (inst.app_api_version >= VK_API_VERSION_1_1) {
		features2 = next.GetPhysicalDeviceFeatures2;
	} else if (inst.has_properties2_extension) {
		features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
				next.GetInstanceProcAddr(inst.instance, "vkGetPhysicalDeviceFeatures2KHR"));
	}
	if (features2 == nullptr) {
		return caps;
	}
	// Only chain the structures of extensions the device offers.
	VkPhysicalDeviceFeatures2 query{};
	query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
	timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
	VkPhysicalDevicePresentIdFeaturesKHR present_id{};
	present_id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
	void *chain = nullptr;
	if (caps.effective_api_version >= VK_API_VERSION_1_2 || caps.has_extension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
		timeline.pNext = chain;
		chain = &timeline;
	}
	if (caps.has_extension(VK_KHR_PRESENT_ID_EXTENSION_NAME)) {
		present_id.pNext = chain;
		chain = &present_id;
	}
	query.pNext = chain;
	features2(physical_device, &query);
	caps.timeline_semaphore_feature = timeline.timelineSemaphore == VK_TRUE;
	caps.present_id_feature = present_id.presentId == VK_TRUE;
	return caps;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkDevice *device) {
	Layer &L = layer();
	InstanceInfo inst;
	bool known_instance = false;
	{
		std::lock_guard<std::mutex> lock(L.mutex);
		auto it = L.instances.find(dispatch_key(physical_device));
		if (it != L.instances.end()) {
			inst = it->second;
			known_instance = true;
		}
	}
	if (info == nullptr || device == nullptr) {
		return L.next.CreateDevice(physical_device, info, allocator, device); // invalid call: let the loader report it
	}
	if (!known_instance) {
		LU_WARN_ONCE("vkCreateDevice for an instance the layer did not see being created; passing it through");
		return L.next.CreateDevice(physical_device, info, allocator, device);
	}

	// Only the presenting device gets the Reflex extensions and a sleep semaphore. Godot creates
	// it once during DisplayServer setup, before any script runs; later vkCreateDevice calls are
	// secondary devices (RenderingDevice.create_local_rendering_device(), which never presents), so
	// they pass through unpatched and do not change the reported device.
	bool is_main;
	{
		std::lock_guard<std::mutex> lock(L.mutex);
		is_main = !L.main_device_created;
	}
	if (!is_main) {
		const VkResult r = L.next.CreateDevice(physical_device, info, allocator, device);
		if (r == VK_SUCCESS) {
			std::lock_guard<std::mutex> lock(L.mutex);
			L.snap.local_devices_created++;
		}
		return r;
	}

	const PhysicalDeviceCaps caps = query_caps(inst, physical_device);
	DeviceRequest request;
	struct Decision {
		VulkanFeature *feature;
		bool accepted;
		std::string reason;
		size_t first_queue; // this feature's queue requests within request.queues
		size_t queue_count;
	};
	std::vector<Decision> decisions;
	for (VulkanFeature *feature : L.features) {
		DeviceRequest own;
		Decision d{ feature, false, {}, request.queues.size(), 0 };
		d.accepted = feature->request_device(caps, own, d.reason);
		if (d.accepted) {
			for (const std::string &name : own.extensions) {
				request.add_extension(name.c_str());
			}
			request.features.merge(own.features);
			request.queues.insert(request.queues.end(), own.queues.begin(), own.queues.end());
			d.queue_count = own.queues.size();
		}
		decisions.push_back(d);
	}
#ifdef DEBUG_ENABLED
	// Fault injection: a bogus extension makes the patched vkCreateDevice fail, so the fallback to
	// the host's own create info is exercised (GODOT_LATENCY_UPSCALING_TEST_FAULT=device_extension).
	if (testing::fault("device_extension")) {
		request.add_extension("VK_LU_TEST_nonexistent_extension");
	}
#endif

	std::vector<std::string> enabled = names_of(info->ppEnabledExtensionNames, info->enabledExtensionCount);
	std::vector<std::string> added;
	std::string note;
	bool patch_applied = false;
	bool patch_failed = false; // the device had to be created without the features' additions
	std::vector<QueueAssignment> queues;
	VkResult result;
	if (!request.empty()) {
		DeviceCreatePatch patch(*info, request, caps.queue_families);
		result = L.next.CreateDevice(physical_device, patch.get(), allocator, device);
		patch.restore();
		if (result == VK_SUCCESS) {
			patch_applied = true;
			enabled = patch.enabled_extensions();
			added = patch.added_extensions();
			queues = patch.queue_assignments();
		} else {
			patch_failed = true;
			char buffer[200];
			std::snprintf(buffer, sizeof(buffer),
					"vkCreateDevice with the addon's extensions failed (VkResult %d); the device was created without them", result);
			note = buffer;
			log::warning("%s", note.c_str());
			result = L.next.CreateDevice(physical_device, info, allocator, device);
		}
	} else {
		result = L.next.CreateDevice(physical_device, info, allocator, device);
	}
	if (result != VK_SUCCESS) {
		return result;
	}

	{
		std::lock_guard<std::mutex> lock(L.mutex);
		L.main_device_created = true;
		LayerSnapshot &s = L.snap;
		s.devices_created++;
		s.device_handle = reinterpret_cast<uint64_t>(*device);
		s.gpu_name = caps.name;
		s.vendor_id = caps.vendor_id;
		s.driver_version = caps.driver_version;
		s.api_version = caps.effective_api_version;
		s.device_extensions = enabled;
		s.added_device_extensions = added;
		s.device_note = note;
	}
	log::info("device '%s' created through the layer, %d extension(s) added", caps.name.c_str(), static_cast<int>(added.size()));

	DeviceContext context;
	context.instance = inst.instance;
	context.physical_device = physical_device;
	context.device = *device;
	context.get_instance_proc_addr = L.next.GetInstanceProcAddr;
	context.get_device_proc_addr = L.next.GetDeviceProcAddr;
	context.caps = caps;
	context.enabled_extensions = enabled;
	if (patch_applied) {
		context.enabled_features = request.features;
	}
	for (const Decision &d : decisions) {
		const bool usable = d.accepted && !patch_failed;
		context.queues.clear();
		for (size_t i = 0; i < d.queue_count && d.first_queue + i < queues.size(); ++i) {
			context.queues.push_back(queues[d.first_queue + i]);
		}
		context.queues.resize(d.queue_count); // unassigned entries when the patch was not applied
		d.feature->on_device_created(context, usable, d.accepted ? note : d.reason);
	}
	return result;
}

VKAPI_ATTR void VKAPI_CALL hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
	Layer &L = layer();
	if (device != VK_NULL_HANDLE) {
		for (VulkanFeature *feature : L.features) {
			feature->on_device_destroyed(device);
		}
	}
	L.next.DestroyDevice(device, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
		const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain) {
	Layer &L = layer();
	QueueHooks *hooks = L.queue_hooks.load(std::memory_order_acquire);
	return hooks != nullptr ? hooks->create_swapchain(device, info, allocator, swapchain, L.next.CreateSwapchainKHR)
							: L.next.CreateSwapchainKHR(device, info, allocator, swapchain);
}

VKAPI_ATTR void VKAPI_CALL hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator) {
	Layer &L = layer();
	QueueHooks *hooks = L.queue_hooks.load(std::memory_order_acquire);
	if (hooks != nullptr) {
		hooks->destroy_swapchain(device, swapchain, allocator, L.next.DestroySwapchainKHR);
	} else {
		L.next.DestroySwapchainKHR(device, swapchain, allocator);
	}
}

VKAPI_ATTR void VKAPI_CALL hook_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator) {
	Layer &L = layer();
	QueueHooks *hooks = L.queue_hooks.load(std::memory_order_acquire);
	if (hooks != nullptr) {
		hooks->destroy_surface(instance, surface, allocator, L.next.DestroySurfaceKHR);
	} else {
		L.next.DestroySurfaceKHR(instance, surface, allocator);
	}
}

VKAPI_ATTR VkResult VKAPI_CALL hook_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
	Layer &L = layer();
	QueueHooks *hooks = L.queue_hooks.load(std::memory_order_acquire);
	return hooks != nullptr ? hooks->queue_submit(queue, count, submits, fence, L.next.QueueSubmit)
							: L.next.QueueSubmit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
	Layer &L = layer();
	QueueHooks *hooks = L.queue_hooks.load(std::memory_order_acquire);
	return hooks != nullptr ? hooks->queue_present(queue, info, L.next.QueuePresentKHR) : L.next.QueuePresentKHR(queue, info);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_BeginCommandBuffer(VkCommandBuffer command_buffer, const VkCommandBufferBeginInfo *info) {
	Layer &L = layer();
	L.interceptor.forget(command_buffer);
	return L.next.BeginCommandBuffer(command_buffer, info);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_EndCommandBuffer(VkCommandBuffer command_buffer) {
	Layer &L = layer();
	L.interceptor.forget(command_buffer);
	return L.next.EndCommandBuffer(command_buffer);
}

VKAPI_ATTR void VKAPI_CALL hook_CmdBindPipeline(VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point, VkPipeline pipeline) {
	layer().interceptor.bind_pipeline(command_buffer, bind_point, pipeline);
}

VKAPI_ATTR void VKAPI_CALL hook_CmdBindDescriptorSets(VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
		VkPipelineLayout layout, uint32_t first_set, uint32_t set_count, const VkDescriptorSet *sets,
		uint32_t dynamic_offset_count, const uint32_t *dynamic_offsets) {
	layer().interceptor.bind_descriptor_sets(command_buffer, bind_point, layout, first_set, set_count, sets,
			dynamic_offset_count, dynamic_offsets);
}

VKAPI_ATTR void VKAPI_CALL hook_CmdPushConstants(VkCommandBuffer command_buffer, VkPipelineLayout layout,
		VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values) {
	layer().interceptor.push_constants(command_buffer, layout, stages, offset, size, values);
}

VKAPI_ATTR void VKAPI_CALL hook_CmdDispatch(VkCommandBuffer command_buffer, uint32_t x, uint32_t y, uint32_t z) {
	layer().interceptor.dispatch(command_buffer, x, y, z);
}

// Which names the layer answers itself. `device_level` hooks are also returned by
// vkGetDeviceProcAddr; `queue` hooks only matter when a QueueHooks owner is installed; `command`
// hooks only when command interception was enabled (and the loader provides those functions).
struct HookEntry {
	const char *name;
	PFN_vkVoidFunction hook;
	bool device_level;
	bool queue;
	bool command;
};

PFN_vkVoidFunction find_hook(const char *name, bool device_scope);

} // namespace

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lu_proxy_vkGetInstanceProcAddr(VkInstance instance, const char *name) {
	Layer &L = layer();
	if (name == nullptr || !L.ready.load(std::memory_order_acquire)) {
		return nullptr; // only reachable through the module handed out once the layer is ready
	}
	const PFN_vkVoidFunction real = L.next.GetInstanceProcAddr(instance, name);
	if (real == nullptr) {
		return nullptr; // keep the loader's "not available" answers
	}
	const PFN_vkVoidFunction hook = find_hook(name, false);
	return hook != nullptr ? hook : real;
}

namespace {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hook_GetDeviceProcAddr(VkDevice device, const char *name) {
	Layer &L = layer();
	if (name == nullptr || !L.ready.load(std::memory_order_acquire)) {
		return nullptr;
	}
	const PFN_vkVoidFunction real = L.next.GetDeviceProcAddr(device, name);
	if (real == nullptr) {
		return nullptr;
	}
	const PFN_vkVoidFunction hook = find_hook(name, true);
	return hook != nullptr ? hook : real;
}

PFN_vkVoidFunction find_hook(const char *name, bool device_scope) {
	static const HookEntry table[] = {
		{ "vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&lu_proxy_vkGetInstanceProcAddr), false, false, false },
		{ "vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(&hook_CreateInstance), false, false, false },
		{ "vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(&hook_DestroyInstance), false, false, false },
		{ "vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(&hook_CreateDevice), false, false, false },
		{ "vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&hook_GetDeviceProcAddr), true, false, false },
		{ "vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(&hook_DestroyDevice), true, false, false },
		{ "vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&hook_CreateSwapchainKHR), true, true, false },
		{ "vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&hook_DestroySwapchainKHR), true, true, false },
		{ "vkDestroySurfaceKHR", reinterpret_cast<PFN_vkVoidFunction>(&hook_DestroySurfaceKHR), false, true, false },
		{ "vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(&hook_QueueSubmit), true, true, false },
		{ "vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(&hook_QueuePresentKHR), true, true, false },
		{ "vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(&hook_BeginCommandBuffer), true, false, true },
		{ "vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(&hook_EndCommandBuffer), true, false, true },
		{ "vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(&hook_CmdBindPipeline), true, false, true },
		{ "vkCmdBindDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(&hook_CmdBindDescriptorSets), true, false, true },
		{ "vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(&hook_CmdPushConstants), true, false, true },
		{ "vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(&hook_CmdDispatch), true, false, true },
	};
	Layer &L = layer();
	const bool have_queue_hooks = L.queue_hooks.load(std::memory_order_acquire) != nullptr;
	const bool have_command_hooks =
			L.command_hooks_wanted.load(std::memory_order_acquire) && L.command_hooks_usable.load(std::memory_order_acquire);
	for (const HookEntry &entry : table) {
		if ((device_scope && !entry.device_level) || (entry.queue && !have_queue_hooks) ||
				(entry.command && !have_command_hooks)) {
			continue;
		}
		if (std::strcmp(entry.name, name) == 0) {
			if (entry.command) {
				L.command_hooks_resolved.store(true, std::memory_order_release);
			}
			return entry.hook;
		}
	}
	return nullptr;
}

} // namespace

// The host resolves vkGetInstanceProcAddr from the module LoadLibraryA("vulkan-1.dll") returned,
// i.e. from this DLL: export the proxy under that name.
#pragma comment(linker, "/EXPORT:vkGetInstanceProcAddr=lu_proxy_vkGetInstanceProcAddr")

void register_feature(VulkanFeature *feature) {
	Layer &L = layer();
	std::lock_guard<std::mutex> lock(L.mutex);
	if (std::find(L.features.begin(), L.features.end(), feature) == L.features.end()) {
		L.features.push_back(feature);
	}
}

void set_queue_hooks(QueueHooks *hooks) {
	layer().queue_hooks.store(hooks, std::memory_order_release);
}

void set_loader_override(HMODULE module, const char *name) {
	Layer &L = layer();
	std::lock_guard<std::mutex> lock(L.mutex);
	if (!L.ready.load()) {
		L.loader_override = module;
		L.snap.loader_override = name != nullptr ? name : "";
	}
}

void enable_command_interception() {
	layer().command_hooks_wanted.store(true, std::memory_order_release);
}

bool command_interception_active() {
	const Layer &L = layer();
	return L.command_hooks_usable.load(std::memory_order_acquire) && L.command_hooks_resolved.load(std::memory_order_acquire);
}

bool add_marker(VkPipeline pipeline, MarkerRecorder *recorder) {
	return layer().interceptor.add_marker(pipeline, recorder);
}

void remove_marker(VkPipeline pipeline) {
	layer().interceptor.remove_marker(pipeline);
}

InterceptorStats interceptor_stats() {
	return layer().interceptor.stats();
}

HMODULE on_loader_requested(LoadRealLoader load_real, const void *request) {
	Layer &L = layer();
	HMODULE real = load_real(request);
	if (real == nullptr) {
		return nullptr;
	}
	std::string problem;
	std::unique_lock<std::mutex> lock(L.mutex);
	L.snap.loader_intercepted = true;
	if (L.ready) {
		FreeLibrary(real); // the layer already holds a reference for the process lifetime
	} else {
		if (L.snap.loader_problem.empty()) {
			// The exports come from the override module (Streamline's interposer) when one is set and
			// complete, otherwise from the real loader.
			Next next;
			CommandFunctions commands;
			bool commands_complete = false;
			bool complete = false;
			bool from_override = false;
			if (L.loader_override != nullptr) {
				complete = from_override = fill_next(L.loader_override, next, commands, commands_complete);
				if (!complete) {
					L.snap.loader_override_problem = L.snap.loader_override + " does not export the Vulkan entry points the layer needs";
					problem = L.snap.loader_override_problem + "; Vulkan goes to the real loader";
				}
			}
			if (!complete) {
				next = Next();
				complete = fill_next(real, next, commands, commands_complete);
			}
#ifdef DEBUG_ENABLED
			// Fault injection: pretend the loader is missing entry points so the pure pass-through
			// safety net is exercised (GODOT_LATENCY_UPSCALING_TEST_FAULT=loader_incomplete).
			if (testing::fault("loader_incomplete")) {
				complete = false;
			}
#endif
			if (complete) {
				L.real_loader = real; // this reference keeps the loader loaded for the process lifetime
				L.next = next;
				L.snap.loader_override_active = from_override;
				if (commands_complete) {
					L.interceptor.set_next(commands);
					L.command_hooks_usable.store(true, std::memory_order_release);
				}
				L.ready.store(true, std::memory_order_release);
			} else {
				L.snap.loader_problem = "the Vulkan loader does not export the entry points the layer needs";
				problem = L.snap.loader_problem;
			}
		}
		if (!L.ready.load()) {
			lock.unlock();
			if (!problem.empty()) {
				log::warning("%s; Vulkan calls pass through untouched", problem.c_str());
			}
			return real; // pure pass-through: the host owns this reference to the real loader
		}
	}
	lock.unlock();
	if (!problem.empty()) {
		log::warning("%s", problem.c_str());
	}
	// The host receives this DLL. The reference taken here balances the FreeLibrary volk issues
	// in volkFinalize; the DLL is pinned anyway (hook/loader_hook.cpp).
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&on_loader_requested), &self)) {
		log::warning("could not reference the addon module; this Vulkan loader request passes through");
		return load_real(request);
	}
	return self;
}

LayerSnapshot snapshot() {
	Layer &L = layer();
	std::lock_guard<std::mutex> lock(L.mutex);
	LayerSnapshot s = L.snap;
	s.command_hooks_resolved = L.command_hooks_resolved.load(std::memory_order_acquire);
	return s;
}

} // namespace lu::vk
