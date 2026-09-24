// DlssFeature and its checks against fake device capabilities and a fake runtime backend: the
// reasons the API reports for AMD / old-driver / non-RTX GPUs and a missing runtime, and that a
// backend's instance, device and queue needs reach the layer (and nothing does when it declines).
#include "dlss/dlss_feature.h"
#include "dlss/runtime_files.h"
#include "test_framework.h"

#include <windows.h>

#include <string>
#include <vector>

using namespace lu;
using namespace lu::dlss;

namespace {

uint32_t nvidia_raw(uint32_t major, uint32_t minor) {
	return (major << 22) | (minor << 14);
}

vk::PhysicalDeviceCaps rtx_caps() {
	vk::PhysicalDeviceCaps caps;
	caps.name = "NVIDIA GeForce RTX 5080";
	caps.vendor_id = 0x10DE;
	caps.driver_version = nvidia_raw(591, 86);
	caps.extensions["VK_NVX_binary_import"] = 2;
	caps.extensions["VK_NVX_image_view_handle"] = 3;
	caps.extensions["VK_KHR_push_descriptor"] = 2;
	caps.extensions["VK_KHR_cooperative_matrix"] = 2;
	return caps;
}

class FakeBackend : public UpscalerProvider {
public:
	bool accept_instance = true;
	bool accept_device = true;
	bool init_ok = true;
	int initialized = 0;
	int shut_down = 0;
	vk::DeviceContext seen;

	bool request_instance_extensions(std::vector<std::string> &extensions, std::string &reason) override {
		if (!accept_instance) {
			reason = "backend: no instance";
			return false;
		}
		extensions.push_back("VK_KHR_get_physical_device_properties2");
		return true;
	}
	bool request_device(const vk::PhysicalDeviceCaps &, vk::DeviceRequest &request, std::string &reason) override {
		if (!accept_device) {
			reason = "backend: GPU not supported";
			return false;
		}
		request.add_extension("VK_NVX_binary_import");
		request.add_extension("VK_NVX_image_view_handle");
		request.features.buffer_device_address = true;
		request.queues.push_back({ VK_QUEUE_COMPUTE_BIT, 1 });
		return true;
	}
	bool initialize(const vk::DeviceContext &context, std::string &reason) override {
		initialized++;
		seen = context;
		if (!init_ok) {
			reason = "backend: init failed";
		}
		return init_ok;
	}
	void shutdown() override { shut_down++; }
	bool optimal_render_size(ImageSize target, upscale::Quality, ImageSize &render) const override {
		render = target;
		return true;
	}
	bool evaluate(VkCommandBuffer, const EvaluateInputs &, std::string &) override { return false; }
};

vk::DeviceContext context_for(VkDevice device) {
	vk::DeviceContext context;
	context.device = device;
	context.queues.push_back(vk::QueueAssignment{ true, 1, 0, 1 });
	return context;
}

const VkDevice DEVICE = reinterpret_cast<VkDevice>(uintptr_t(0xD5));

} // namespace

TEST(dlss_driver_version_decodes_nvidias_packing) {
	const DriverVersion v = nvidia_driver_version(nvidia_raw(591, 86) | 0x3f); // low bits: build, ignored
	CHECK_EQ(v.major, 591u);
	CHECK_EQ(v.minor, 86u);
	CHECK_EQ(nvidia_driver_version(nvidia_raw(512, 15)).minor, 15u);
}

TEST(dlss_check_device_explains_every_refusal) {
	CHECK_EQ(check_device(rtx_caps()), std::string());

	vk::PhysicalDeviceCaps amd = rtx_caps();
	amd.vendor_id = 0x1002;
	amd.name = "AMD Radeon RX 7900 XTX";
	CHECK(check_device(amd).find("AMD Radeon RX 7900 XTX is not an NVIDIA GPU") == 0);

	vk::PhysicalDeviceCaps old_driver = rtx_caps();
	old_driver.driver_version = nvidia_raw(512, 14);
	const std::string old_reason = check_device(old_driver);
	CHECK(old_reason.find("512.14 is older than 512.15") != std::string::npos);
	old_driver.driver_version = nvidia_raw(511, 99);
	CHECK(!check_device(old_driver).empty());
	old_driver.driver_version = nvidia_raw(512, 15);
	CHECK(check_device(old_driver).empty()); // the minimum itself passes

	// What the reference GTX 1080 reports: neither NGX extension.
	vk::PhysicalDeviceCaps gtx1080 = rtx_caps();
	gtx1080.name = "NVIDIA GeForce GTX 1080";
	gtx1080.extensions.erase("VK_NVX_binary_import");
	gtx1080.extensions.erase("VK_NVX_image_view_handle");
	const std::string gtx_reason = check_device(gtx1080);
	CHECK(gtx_reason.find("NVIDIA GeForce GTX 1080 does not expose VK_NVX_binary_import, VK_NVX_image_view_handle") == 0);

	// The debug knob that hides an extension.
	CHECK(check_device(rtx_caps(), { "VK_NVX_image_view_handle" }).find("does not expose VK_NVX_image_view_handle") !=
			std::string::npos);
}

TEST(dlss_feature_without_a_backend_requests_nothing_and_says_why) {
	DlssFeature feature;
	feature.configure(nullptr, "the NVIDIA DLSS runtime is not installed");
	std::vector<std::string> instance_extensions;
	feature.request_instance_extensions(instance_extensions);
	CHECK(instance_extensions.empty());
	vk::DeviceRequest request;
	std::string reason;
	CHECK(!feature.request_device(rtx_caps(), request, reason));
	CHECK(request.empty());
	CHECK_EQ(reason, std::string("the NVIDIA DLSS runtime is not installed"));
	CHECK(!feature.status().device_seen);
	feature.on_device_created(context_for(DEVICE), false, reason);
	const DlssDeviceStatus s = feature.status();
	CHECK(s.device_seen && !s.available);
	CHECK_EQ(s.reason, std::string("the NVIDIA DLSS runtime is not installed"));
	CHECK(s.tensor_core_hint);

	// A device check failure wins over the missing runtime: it is the more useful answer.
	vk::PhysicalDeviceCaps gtx1080 = rtx_caps();
	gtx1080.extensions.erase("VK_NVX_binary_import");
	gtx1080.extensions.erase("VK_KHR_cooperative_matrix");
	CHECK(!feature.request_device(gtx1080, request, reason));
	CHECK(reason.find("does not expose VK_NVX_binary_import") != std::string::npos);
	feature.on_device_created(context_for(DEVICE), false, reason);
	CHECK(feature.status().reason.find("VK_NVX_binary_import") != std::string::npos);
	CHECK(!feature.status().tensor_core_hint);
}

TEST(dlss_feature_forwards_a_backends_needs_and_initialises_it) {
	FakeBackend backend;
	DlssFeature feature;
	feature.configure(&backend, "unused");
	std::vector<std::string> instance_extensions = { "VK_KHR_surface" };
	feature.request_instance_extensions(instance_extensions);
	CHECK_EQ(instance_extensions.size(), static_cast<size_t>(2));
	CHECK_EQ(instance_extensions[1], std::string("VK_KHR_get_physical_device_properties2"));

	vk::DeviceRequest request;
	request.add_extension("VK_NV_low_latency2"); // another feature's
	std::string reason;
	CHECK(feature.request_device(rtx_caps(), request, reason));
	CHECK_EQ(request.extensions.size(), static_cast<size_t>(3));
	CHECK(request.features.buffer_device_address);
	CHECK_EQ(request.queues.size(), static_cast<size_t>(1));
	CHECK_EQ(static_cast<int>(request.queues[0].required), static_cast<int>(VK_QUEUE_COMPUTE_BIT));

	feature.on_device_created(context_for(DEVICE), true, "");
	CHECK_EQ(backend.initialized, 1);
	CHECK(backend.seen.device == DEVICE);
	CHECK_EQ(backend.seen.queues.size(), static_cast<size_t>(1)); // the queue it asked for
	CHECK(feature.status().available);

	feature.on_device_destroyed(reinterpret_cast<VkDevice>(uintptr_t(0x99))); // a local device
	CHECK_EQ(backend.shut_down, 0);
	feature.on_device_destroyed(DEVICE);
	CHECK_EQ(backend.shut_down, 1);
	CHECK(!feature.status().available);
}

TEST(dlss_feature_leaves_the_device_alone_when_the_backend_declines) {
	FakeBackend backend;
	DlssFeature feature;
	feature.configure(&backend, "unused");
	backend.accept_device = false;
	vk::DeviceRequest request;
	std::string reason;
	CHECK(!feature.request_device(rtx_caps(), request, reason));
	CHECK(request.empty());
	CHECK_EQ(reason, std::string("backend: GPU not supported"));

	// The backend declined the instance: the device is not asked for either.
	backend.accept_device = true;
	backend.accept_instance = false;
	std::vector<std::string> instance_extensions;
	feature.request_instance_extensions(instance_extensions);
	CHECK(instance_extensions.empty());
	CHECK(!feature.request_device(rtx_caps(), request, reason));
	CHECK(request.empty());
	CHECK_EQ(reason, std::string("backend: no instance"));

	// Accepted, but the layer had to create the device without the additions.
	backend.accept_instance = true;
	feature.request_instance_extensions(instance_extensions);
	CHECK(feature.request_device(rtx_caps(), request, reason));
	feature.on_device_created(context_for(DEVICE), false, "vkCreateDevice with the addon's extensions failed");
	CHECK_EQ(backend.initialized, 0);
	CHECK(feature.status().reason.find("failed") != std::string::npos);

	// Initialisation fails: unavailable with the backend's reason.
	backend.init_ok = false;
	feature.on_device_created(context_for(DEVICE), true, "");
	CHECK(!feature.status().available);
	CHECK_EQ(feature.status().reason, std::string("backend: init failed"));
	feature.on_device_destroyed(DEVICE);
	CHECK_EQ(backend.shut_down, 0); // never initialised: nothing to shut down
}

TEST(dlss_runtime_files_are_checked_by_existence_only) {
	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	const std::wstring dir = std::wstring(temp) + L"lu_runtime_files_test";
	CreateDirectoryW(dir.c_str(), nullptr);
	for (const std::wstring &name : super_resolution_files()) {
		DeleteFileW((dir + L"\\" + name).c_str());
	}
	RuntimeFiles none = check_runtime_files(dir);
	CHECK(!none.present());
	CHECK_EQ(none.missing.size(), super_resolution_files().size());
	CHECK_EQ(none.missing[0], std::string("sl.interposer.dll"));

	// Text files with the right names count (nothing is loaded); a folder with a DLL's name does not.
	for (const std::wstring &name : super_resolution_files()) {
		if (name == L"nvngx_dlss.dll") {
			CreateDirectoryW((dir + L"\\" + name).c_str(), nullptr);
			continue;
		}
		HANDLE f = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		CHECK(f != INVALID_HANDLE_VALUE);
		CloseHandle(f);
	}
	RuntimeFiles partial = check_runtime_files(dir);
	CHECK_EQ(partial.missing.size(), static_cast<size_t>(1));
	CHECK_EQ(partial.missing[0], std::string("nvngx_dlss.dll"));
	RemoveDirectoryW((dir + L"\\nvngx_dlss.dll").c_str());
	HANDLE f = CreateFileW((dir + L"\\nvngx_dlss.dll").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	CloseHandle(f);
	CHECK(check_runtime_files(dir + L"\\").present()); // a trailing separator is fine

	for (const std::wstring &name : super_resolution_files()) {
		DeleteFileW((dir + L"\\" + name).c_str());
	}
	RemoveDirectoryW(dir.c_str());
	// The default folder sits next to the module that contains this code.
	const std::wstring default_dir = default_runtime_dir();
	CHECK(default_dir.size() > 7 && default_dir.substr(default_dir.size() - 7) == L"\\nvidia");
}
