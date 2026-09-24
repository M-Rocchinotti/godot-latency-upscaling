// DLSS's VulkanFeature: decides from the presenting device whether DLSS can run, forwards the
// runtime backend's instance / device / queue requirements to the layer, and initialises the
// backend on the device it got. Every "no" comes with the reason the API reports
// (LatencyUpscaling.get_dlss_status(): "unavailable: <reason>").
//
// The checks that need no NVIDIA runtime (check_device): an NVIDIA GPU, a driver of at least
// 512.15 (DLSS Programming Guide 2.1; Streamline's minimum too), and the NGX device extensions
// VK_NVX_binary_import / VK_NVX_image_view_handle, which NVIDIA's driver exposes where NGX can run.
// Whether the GPU really runs DLSS (RTX, tensor cores) is the runtime's own capability query.
//
// Pure C++ plus Vulkan headers: unit-tested with fake capabilities and a fake backend.
#pragma once

#include "dlss/upscaler_provider.h"
#include "vk/vk_feature.h"

#include <mutex>
#include <string>
#include <vector>

namespace lu::dlss {

inline constexpr uint32_t MIN_DRIVER_MAJOR = 512;
inline constexpr uint32_t MIN_DRIVER_MINOR = 15;

struct DriverVersion {
	uint32_t major = 0;
	uint32_t minor = 0;
};
// NVIDIA packs VkPhysicalDeviceProperties::driverVersion as 10.8.8.6 bits.
DriverVersion nvidia_driver_version(uint32_t raw);

// The NGX device extensions the driver must offer (NGX reports its full list at run time).
const std::vector<const char *> &ngx_device_extensions();

// "" when the device passes the checks that need no NVIDIA runtime, otherwise why not.
// `hidden_extensions` (debug test knob) count as not offered.
std::string check_device(const vk::PhysicalDeviceCaps &caps, const std::vector<std::string> &hidden_extensions = {});

struct DlssDeviceStatus {
	bool device_seen = false; // the presenting device was created through the layer
	bool available = false; // the backend is initialised on it
	std::string reason; // why not (when !available)
	bool tensor_core_hint = false; // the driver exposes cooperative matrices (Turing and newer RTX)
};

class DlssFeature : public vk::VulkanFeature {
public:
	static DlssFeature &get();

	// CORE initialisation. `backend`: the DLSS runtime backend for this run, or nullptr with
	// `no_backend_reason` (the runtime is not installed, or this build has none).
	void configure(UpscalerProvider *backend, const std::string &no_backend_reason,
			const std::vector<std::string> &hidden_extensions = {});

	const char *name() const override { return "dlss"; }
	void request_instance_extensions(std::vector<std::string> &extensions) override;
	bool request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) override;
	void on_device_created(const vk::DeviceContext &context, bool accepted, const std::string &reason) override;
	void on_device_destroyed(VkDevice device) override;

	DlssDeviceStatus status() const;
	UpscalerProvider *backend() const;

private:
	mutable std::mutex mutex_;
	UpscalerProvider *backend_ = nullptr;
	std::string no_backend_reason_ = "no DLSS runtime backend";
	std::vector<std::string> hidden_extensions_;
	std::string instance_reason_; // the backend declined the instance
	std::string device_reason_; // why request_device() declined
	VkDevice device_ = VK_NULL_HANDLE;
	DlssDeviceStatus status_;
};

} // namespace lu::dlss
