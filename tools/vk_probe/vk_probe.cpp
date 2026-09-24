// vk_probe: what a machine's Vulkan driver offers for Reflex and DLSS, without a window.
//
// Creates a Vulkan instance and, per GPU, a device with no surface or swapchain (so it runs from
// an SSH session and never shows anything), and prints one JSON object: GPU, driver, API
// version, the extensions Reflex / NGX / Streamline need, tensor-core indicators, the queue
// families (incl. optical flow for Frame Generation), and whether a device with the DLSS
// extension set can actually be created. Loads no NVIDIA SDK binary.
//
//     build: python tools/build.py vk_probe      -> build/tools/vk_probe.exe
//     run:   build/tools/vk_probe.exe            (exit 0 when at least one GPU was probed)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Extensions that matter here, in the order they are reported.
const char *const INTERESTING[] = {
	// Reflex (Phase 1)
	"VK_NV_low_latency2", "VK_KHR_present_id",
	// NGX / Streamline DLSS (RESEARCH.md 1.3)
	"VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_push_descriptor", "VK_EXT_buffer_device_address",
	"VK_KHR_buffer_device_address",
	// Frame Generation (Phase 3)
	"VK_NV_optical_flow",
	// tensor cores (Turing and newer expose cooperative matrices; Pascal does not)
	"VK_NV_cooperative_matrix", "VK_KHR_cooperative_matrix",
};
// The subset a DLSS device would enable (the base NGX list; NGX adds a runtime list on top).
const char *const DLSS_DEVICE_SET[] = { "VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_push_descriptor" };

std::string json_escape(const char *s) {
	std::string out;
	for (; *s; ++s) {
		if (*s == '"' || *s == '\\') {
			out += '\\';
		}
		out += *s;
	}
	return out;
}

std::string driver_version(uint32_t vendor, uint32_t v) {
	char buf[64];
	if (vendor == 0x10DE) {
		std::snprintf(buf, sizeof(buf), "%u.%u", (v >> 22) & 0x3ff, (v >> 14) & 0xff);
	} else {
		std::snprintf(buf, sizeof(buf), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
	}
	return buf;
}

} // namespace

int main() {
	HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
	if (loader == nullptr) {
		std::printf("{\"error\": \"vulkan-1.dll not found\"}\n");
		return 1;
	}
	auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void *>(GetProcAddress(loader, "vkGetInstanceProcAddr")));
	auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));

	VkApplicationInfo app{};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "vk_probe";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	VkInstance instance = VK_NULL_HANDLE;
	VkResult r = create_instance(&ici, nullptr, &instance);
	if (r != VK_SUCCESS) {
		std::printf("{\"error\": \"vkCreateInstance failed (%d)\"}\n", r);
		return 1;
	}
#define LOAD(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
	LOAD(vkEnumeratePhysicalDevices);
	LOAD(vkGetPhysicalDeviceProperties);
	LOAD(vkEnumerateDeviceExtensionProperties);
	LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
	LOAD(vkCreateDevice);
	LOAD(vkDestroyDevice);
	LOAD(vkDestroyInstance);
#undef LOAD

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> gpus(count);
	vkEnumeratePhysicalDevices(instance, &count, gpus.data());

	std::printf("{\"gpus\": [");
	for (uint32_t g = 0; g < count; ++g) {
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(gpus[g], &props);
		uint32_t n = 0;
		vkEnumerateDeviceExtensionProperties(gpus[g], nullptr, &n, nullptr);
		std::vector<VkExtensionProperties> exts(n);
		vkEnumerateDeviceExtensionProperties(gpus[g], nullptr, &n, exts.data());
		auto has = [&](const char *name) {
			for (const VkExtensionProperties &e : exts) {
				if (std::strcmp(e.extensionName, name) == 0) {
					return true;
				}
			}
			return false;
		};
		std::printf("%s{\"name\": \"%s\", \"vendor_id\": %u, \"device_id\": %u, \"driver\": \"%s\", \"api\": \"%u.%u.%u\", \"extensions\": {",
				g ? ", " : "", json_escape(props.deviceName).c_str(), props.vendorID, props.deviceID,
				driver_version(props.vendorID, props.driverVersion).c_str(), VK_API_VERSION_MAJOR(props.apiVersion),
				VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
		bool first = true;
		for (const char *name : INTERESTING) {
			std::printf("%s\"%s\": %s", first ? "" : ", ", name, has(name) ? "true" : "false");
			first = false;
		}
		std::printf("}, \"queue_families\": [");
		uint32_t qn = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(gpus[g], &qn, nullptr);
		std::vector<VkQueueFamilyProperties> qf(qn);
		vkGetPhysicalDeviceQueueFamilyProperties(gpus[g], &qn, qf.data());
		for (uint32_t q = 0; q < qn; ++q) {
			std::printf("%s{\"flags\": %u, \"count\": %u, \"optical_flow\": %s}", q ? ", " : "", qf[q].queueFlags, qf[q].queueCount,
					(qf[q].queueFlags & 0x00000100u) ? "true" : "false"); // VK_QUEUE_OPTICAL_FLOW_BIT_NV
		}
		// Can a device with the DLSS base extension set be created? (Only extensions it offers.)
		std::vector<const char *> enable;
		for (const char *name : DLSS_DEVICE_SET) {
			if (has(name)) {
				enable.push_back(name);
			}
		}
		const float priority = 1.0f;
		VkDeviceQueueCreateInfo qci{};
		qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qci.queueFamilyIndex = 0;
		qci.queueCount = 1;
		qci.pQueuePriorities = &priority;
		VkDeviceCreateInfo dci{};
		dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
		dci.ppEnabledExtensionNames = enable.data();
		VkDevice device = VK_NULL_HANDLE;
		r = vkCreateDevice(gpus[g], &dci, nullptr, &device);
		if (r == VK_SUCCESS) {
			vkDestroyDevice(device, nullptr);
		}
		std::printf("], \"dlss_device_set_enabled\": %u, \"dlss_device_set_offered\": %u, \"device_create\": %d}",
				static_cast<uint32_t>(enable.size()), static_cast<uint32_t>(sizeof(DLSS_DEVICE_SET) / sizeof(DLSS_DEVICE_SET[0])), r);
	}
	std::printf("]}\n");
	vkDestroyInstance(instance, nullptr);
	return count > 0 ? 0 : 1;
}
