#include "dlss/gpu_precheck.h"

#include <windows.h>

#include <cstdio>

namespace lu::dlss {

std::vector<AdapterVerdict> check_adapters(
		const std::function<bool(const vk::PhysicalDeviceCaps &caps, std::string &reason)> &accept, std::string &error) {
	std::vector<AdapterVerdict> verdicts;
	// Our own reference to the real loader (the addon's LoadLibrary hook only patches godot.exe's
	// imports, so this is the system's vulkan-1.dll).
	HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
	if (loader == nullptr) {
		error = "the Vulkan loader (vulkan-1.dll) is not installed";
		return verdicts;
	}
	auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void *>(GetProcAddress(loader, "vkGetInstanceProcAddr")));
	auto create_instance = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
	if (create_instance == nullptr) {
		error = "the Vulkan loader has no vkCreateInstance";
		FreeLibrary(loader);
		return verdicts;
	}
	VkApplicationInfo app{};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "godot-latency-upscaling GPU check";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &app;
	VkInstance instance = VK_NULL_HANDLE;
	const VkResult r = create_instance(&info, nullptr, &instance);
	if (r != VK_SUCCESS) {
		char buffer[96];
		std::snprintf(buffer, sizeof(buffer), "a Vulkan instance could not be created (VkResult %d)", r);
		error = buffer;
		FreeLibrary(loader);
		return verdicts;
	}
	auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
	auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
	auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
	auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
	if (enumerate && properties && extensions) {
		uint32_t count = 0;
		enumerate(instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		enumerate(instance, &count, devices.data());
		for (VkPhysicalDevice device : devices) {
			VkPhysicalDeviceProperties props{};
			properties(device, &props);
			vk::PhysicalDeviceCaps caps;
			caps.instance = instance;
			caps.handle = device;
			caps.vendor_id = props.vendorID;
			caps.device_id = props.deviceID;
			caps.driver_version = props.driverVersion;
			caps.effective_api_version = props.apiVersion;
			caps.name = props.deviceName;
			uint32_t n = 0;
			extensions(device, nullptr, &n, nullptr);
			std::vector<VkExtensionProperties> list(n);
			extensions(device, nullptr, &n, list.data());
			for (const VkExtensionProperties &e : list) {
				caps.extensions[e.extensionName] = e.specVersion;
			}
			AdapterVerdict verdict;
			verdict.name = caps.name;
			verdict.accepted = accept(caps, verdict.reason);
			verdicts.push_back(verdict);
		}
	} else {
		error = "the Vulkan loader lacks the functions to list GPUs";
	}
	if (destroy) {
		destroy(instance, nullptr);
	}
	FreeLibrary(loader);
	return verdicts;
}

} // namespace lu::dlss
