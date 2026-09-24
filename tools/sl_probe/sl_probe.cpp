// sl_probe: what NVIDIA's Streamline DLSS plugin says about this machine, without a window.
//
// Loads the NVIDIA runtime the developer fetched (after checking sl.interposer.dll's NVIDIA
// signature), initialises Streamline for Vulkan in manual-hooking mode with only the DLSS feature
// and no over-the-air updates, and prints one JSON object:
//   - the Vulkan instance / device extensions, 1.2 / 1.3 features and extra queues DLSS requires
//     (slGetFeatureRequirements), and the driver it needs;
//   - per GPU: whether DLSS is supported (slIsFeatureSupported) and why not;
//   - on the first supported GPU, with a device created exactly as required (no surface, no
//     swapchain): the DLSS and NGX versions and the optimal render size of every mode for 1080p,
//     1440p and 4K (slDLSSGetOptimalSettings), to compare with src/upscale/quality.cpp.
// It is the addon's own requirement path, tried outside Godot (tools/rtx_remote.py runs it on the
// RTX test machine over SSH).
//
//     build: python tools/build.py sl_probe      -> build/tools/sl_probe.exe
//     run:   build/tools/sl_probe.exe [runtime folder]   (default: addons/latency_upscaling/bin/nvidia)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <windows.h>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers.h>
#include <sl_helpers_vk.h>
#include <sl_security.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

std::string json_escape(const char *s) {
	std::string out;
	for (; s != nullptr && *s; ++s) {
		if (*s == '"' || *s == '\\') {
			out += '\\';
			out += *s;
		} else if (static_cast<unsigned char>(*s) < 0x20) {
			out += ' ';
		} else {
			out += *s;
		}
	}
	return out;
}

std::string utf8(const std::wstring &w) {
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
	return s;
}

std::string string_list(uint32_t count, const char **names) {
	std::string out = "[";
	for (uint32_t i = 0; i < count; ++i) {
		out += (i ? ", \"" : "\"") + json_escape(names[i]) + "\"";
	}
	return out + "]";
}

std::string version_string(const sl::Version &v) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%u.%u.%u", v.major, v.minor, v.build);
	return buffer;
}

std::vector<std::string> g_log;

// Progress on stderr (the JSON goes to stdout), so a run that stalls shows where.
void stage(const char *what) {
	std::fprintf(stderr, "sl_probe: %s\n", what);
	std::fflush(stderr);
}

bool g_verbose = false; // --verbose: every Streamline log line on stderr as it arrives

void log_callback(sl::LogType type, const char *message) {
	std::string line = message != nullptr ? message : "";
	while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
		line.pop_back();
	}
	if (g_verbose) {
		std::fprintf(stderr, "sl: %s\n", line.c_str());
		std::fflush(stderr);
	}
	if (type != sl::LogType::eInfo && g_log.size() < 40) {
		g_log.push_back(line);
	}
}

void print_log() {
	std::printf(", \"log\": [");
	for (size_t i = 0; i < g_log.size(); ++i) {
		std::printf("%s\"%s\"", i ? ", " : "", json_escape(g_log[i].c_str()).c_str());
	}
	std::printf("]");
}

std::wstring default_runtime_dir() {
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring dir(exe);
	dir = dir.substr(0, dir.find_last_of(L"\\/")); // build\tools
	return dir + L"\\..\\..\\addons\\latency_upscaling\\bin\\nvidia";
}

int fail(const std::string &message) {
	std::printf("{\"error\": \"%s\", \"log\": [", json_escape(message.c_str()).c_str());
	for (size_t i = 0; i < g_log.size(); ++i) {
		std::printf("%s\"%s\"", i ? ", " : "", json_escape(g_log[i].c_str()).c_str());
	}
	std::printf("]}\n");
	return 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
	std::wstring dir;
	for (int i = 1; i < argc; ++i) {
		if (std::wcscmp(argv[i], L"--verbose") == 0) {
			g_verbose = true;
		} else {
			dir = argv[i];
		}
	}
	if (dir.empty()) {
		dir = default_runtime_dir();
	}
	wchar_t full[MAX_PATH];
	if (GetFullPathNameW(dir.c_str(), MAX_PATH, full, nullptr) != 0) {
		dir = full;
	}
	const std::wstring interposer = dir + L"\\sl.interposer.dll";
	if (GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) {
		return fail("no sl.interposer.dll in " + utf8(dir));
	}
	// Streamline's own recommendation: load the interposer only when it carries NVIDIA's signature.
	stage("verifying the signature of sl.interposer.dll");
	if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
		return fail("sl.interposer.dll is not signed by NVIDIA");
	}
	stage("loading sl.interposer.dll");
	HMODULE sl_module = LoadLibraryW(interposer.c_str());
	if (sl_module == nullptr) {
		return fail("LoadLibrary(sl.interposer.dll) failed");
	}
#define SL_PROC(name) auto name = reinterpret_cast<PFun_##name *>(reinterpret_cast<void *>(GetProcAddress(sl_module, #name)))
	SL_PROC(slInit);
	SL_PROC(slShutdown);
	SL_PROC(slIsFeatureSupported);
	SL_PROC(slGetFeatureRequirements);
	SL_PROC(slGetFeatureVersion);
	SL_PROC(slGetFeatureFunction);
	SL_PROC(slSetVulkanInfo);
#undef SL_PROC
	if (!slInit || !slShutdown || !slIsFeatureSupported || !slGetFeatureRequirements || !slGetFeatureVersion ||
			!slGetFeatureFunction || !slSetVulkanInfo) {
		return fail("sl.interposer.dll lacks an expected export");
	}

	const sl::Feature features[] = { sl::kFeatureDLSS };
	const wchar_t *paths[] = { dir.c_str() };
	sl::Preferences pref{};
	pref.showConsole = false;
	pref.logLevel = g_verbose ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
	pref.pathsToPlugins = paths;
	pref.numPathsToPlugins = 1;
	pref.pathToLogsAndData = nullptr; // no log files
	pref.logMessageCallback = &log_callback;
	// Manual hooking (the addon routes the calls itself), no command-list state tracking (the addon
	// re-binds Godot's state), and no over-the-air updates: only the pinned, verified files run.
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = 1;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "Godot 4.7.2";
	pref.projectId = "5f8e0c9a-7b1d-4c62-9a43-2d6e8b1f0c57"; // godot-latency-upscaling
	pref.renderAPI = sl::RenderAPI::eVulkan;
	stage("slInit");
	sl::Result r = slInit(pref, sl::kSDKVersion);
	if (r != sl::Result::eOk) {
		return fail(std::string("slInit failed: ") + sl::getResultAsStr(r));
	}

	sl::FeatureRequirements req{};
	stage("slGetFeatureRequirements");
	r = slGetFeatureRequirements(sl::kFeatureDLSS, req);
	std::printf("{\"streamline\": \"%u.%u.%u\", \"runtime_dir\": \"%s\", \"requirements_result\": \"%s\"", SL_VERSION_MAJOR,
			SL_VERSION_MINOR, SL_VERSION_PATCH, json_escape(utf8(dir).c_str()).c_str(), sl::getResultAsStr(r));
	if (r == sl::Result::eOk) {
		std::printf(", \"requirements\": {\"flags\": %u, \"vulkan_supported\": %s, \"instance_extensions\": %s, \"device_extensions\": %s, "
					"\"features12\": %s, \"features13\": %s, \"compute_queues\": %u, \"graphics_queues\": %u, \"optical_flow_queues\": %u, "
					"\"driver_required\": \"%s\", \"driver_detected\": \"%s\", \"os_required\": \"%s\", \"os_detected\": \"%s\"}",
				static_cast<uint32_t>(req.flags),
				(static_cast<uint32_t>(req.flags) & static_cast<uint32_t>(sl::FeatureRequirementFlags::eVulkanSupported)) ? "true" : "false",
				string_list(req.vkNumInstanceExtensions, req.vkInstanceExtensions).c_str(),
				string_list(req.vkNumDeviceExtensions, req.vkDeviceExtensions).c_str(),
				string_list(req.vkNumFeatures12, req.vkFeatures12).c_str(), string_list(req.vkNumFeatures13, req.vkFeatures13).c_str(),
				req.vkNumComputeQueuesRequired, req.vkNumGraphicsQueuesRequired, req.vkNumOpticalFlowQueuesRequired,
				version_string(req.driverVersionRequired).c_str(), version_string(req.driverVersionDetected).c_str(),
				version_string(req.osVersionRequired).c_str(), version_string(req.osVersionDetected).c_str());
	}

	HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
	auto gipa = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void *>(GetProcAddress(loader, "vkGetInstanceProcAddr"))) : nullptr;
	if (gipa == nullptr) {
		std::printf(", \"error\": \"no Vulkan loader\"}\n");
		slShutdown();
		return 1;
	}
	auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
	VkApplicationInfo app{};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "sl_probe";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = req.vkNumInstanceExtensions;
	ici.ppEnabledExtensionNames = req.vkInstanceExtensions;
	VkInstance instance = VK_NULL_HANDLE;
	stage("vkCreateInstance");
	VkResult vr = create_instance(&ici, nullptr, &instance);
	if (vr != VK_SUCCESS) {
		std::printf(", \"error\": \"vkCreateInstance with the required extensions failed (%d)\"}\n", vr);
		slShutdown();
		return 1;
	}
#define LOAD(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
	LOAD(vkEnumeratePhysicalDevices);
	LOAD(vkGetPhysicalDeviceProperties);
	LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
	LOAD(vkCreateDevice);
	LOAD(vkDestroyDevice);
	LOAD(vkDestroyInstance);
	LOAD(vkDeviceWaitIdle);
#undef LOAD
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> gpus(count);
	vkEnumeratePhysicalDevices(instance, &count, gpus.data());
	std::printf(", \"gpus\": [");
	int supported_index = -1;
	for (uint32_t g = 0; g < count; ++g) {
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(gpus[g], &props);
		sl::AdapterInfo adapter{};
		adapter.vkPhysicalDevice = gpus[g];
		stage("slIsFeatureSupported");
		const sl::Result supported = slIsFeatureSupported(sl::kFeatureDLSS, adapter);
		std::printf("%s{\"name\": \"%s\", \"dlss_supported\": \"%s\"}", g ? ", " : "", json_escape(props.deviceName).c_str(),
				sl::getResultAsStr(supported));
		if (supported == sl::Result::eOk && supported_index < 0) {
			supported_index = static_cast<int>(g);
		}
	}
	std::printf("]");

	if (supported_index >= 0) {
		VkPhysicalDevice gpu = gpus[supported_index];
		uint32_t qn = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
		std::vector<VkQueueFamilyProperties> families(qn);
		vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, families.data());
		uint32_t graphics_family = 0;
		for (uint32_t f = 0; f < qn; ++f) {
			if (families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				graphics_family = f;
				break;
			}
		}
		// One graphics queue (the engine's) plus whatever DLSS asks for, in the same family.
		const uint32_t extra = req.vkNumGraphicsQueuesRequired + req.vkNumComputeQueuesRequired;
		std::vector<float> priorities(1 + extra, 1.0f);
		VkDeviceQueueCreateInfo qci{};
		qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qci.queueFamilyIndex = graphics_family;
		qci.queueCount = static_cast<uint32_t>(priorities.size());
		qci.pQueuePriorities = priorities.data();
		VkPhysicalDeviceVulkan12Features f12 = sl::getVkPhysicalDeviceVulkan12Features(req.vkNumFeatures12, req.vkFeatures12);
		VkPhysicalDeviceVulkan13Features f13 = sl::getVkPhysicalDeviceVulkan13Features(req.vkNumFeatures13, req.vkFeatures13);
		f12.pNext = &f13;
		VkDeviceCreateInfo dci{};
		dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		dci.pNext = &f12;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = req.vkNumDeviceExtensions;
		dci.ppEnabledExtensionNames = req.vkDeviceExtensions;
		VkDevice device = VK_NULL_HANDLE;
		stage("vkCreateDevice");
		vr = vkCreateDevice(gpu, &dci, nullptr, &device);
		std::printf(", \"device_create\": %d", vr);
		if (vr == VK_SUCCESS) {
			sl::VulkanInfo info{};
			info.device = device;
			info.instance = instance;
			info.physicalDevice = gpu;
			info.graphicsQueueFamily = graphics_family;
			info.graphicsQueueIndex = 1; // DLSS's own queues follow the engine's queue 0
			info.computeQueueFamily = graphics_family;
			info.computeQueueIndex = 1 + req.vkNumGraphicsQueuesRequired;
			stage("slSetVulkanInfo");
			r = slSetVulkanInfo(info);
			std::printf(", \"set_vulkan_info\": \"%s\"", sl::getResultAsStr(r));
			sl::FeatureVersion version{};
			if (slGetFeatureVersion(sl::kFeatureDLSS, version) == sl::Result::eOk) {
				std::printf(", \"dlss_plugin\": \"%s\", \"ngx\": \"%s\"", version_string(version.versionSL).c_str(),
						version_string(version.versionNGX).c_str());
			}
			void *function = nullptr;
			stage("slDLSSGetOptimalSettings");
			if (slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", function) == sl::Result::eOk && function) {
				auto get_optimal = reinterpret_cast<PFun_slDLSSGetOptimalSettings *>(function);
				const sl::DLSSMode modes[] = { sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced,
					sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance };
				const uint32_t targets[][2] = { { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
				std::printf(", \"optimal_settings\": [");
				bool first = true;
				for (const auto &target : targets) {
					for (sl::DLSSMode mode : modes) {
						sl::DLSSOptions options{};
						options.mode = mode;
						options.outputWidth = target[0];
						options.outputHeight = target[1];
						sl::DLSSOptimalSettings settings{};
						r = get_optimal(options, settings);
						std::printf("%s{\"target\": [%u, %u], \"mode\": \"%s\", \"result\": \"%s\", \"render\": [%u, %u], \"min\": [%u, %u], "
									"\"max\": [%u, %u], \"sharpness\": %.3f}",
								first ? "" : ", ", target[0], target[1], sl::getDLSSModeAsStr(mode), sl::getResultAsStr(r),
								settings.optimalRenderWidth, settings.optimalRenderHeight, settings.renderWidthMin, settings.renderHeightMin,
								settings.renderWidthMax, settings.renderHeightMax, settings.optimalSharpness);
						first = false;
					}
				}
				std::printf("]");
			} else {
				std::printf(", \"optimal_settings\": \"slDLSSGetOptimalSettings not available\"");
			}
		}
		// The result is out before the teardown, whatever the teardown does.
		print_log();
		std::printf("}\n");
		std::fflush(stdout);
		stage("slShutdown");
		const DWORD shutdown_start = GetTickCount();
		slShutdown(); // before the device goes away (Streamline's rule)
		std::fprintf(stderr, "sl_probe: slShutdown took %lu ms\n", GetTickCount() - shutdown_start);
		if (device != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(device);
			vkDestroyDevice(device, nullptr);
		}
	} else {
		print_log();
		std::printf("}\n");
		std::fflush(stdout);
		slShutdown();
	}
	stage("done");
	vkDestroyInstance(instance, nullptr);
	FreeLibrary(sl_module);
	return 0;
}
