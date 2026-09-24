#include "dlss/dlss_feature.h"

#include <algorithm>
#include <cstdio>

namespace lu::dlss {
namespace {

bool hidden(const std::vector<std::string> &hidden_extensions, const char *name) {
	return std::find(hidden_extensions.begin(), hidden_extensions.end(), name) != hidden_extensions.end();
}

bool offers(const vk::PhysicalDeviceCaps &caps, const std::vector<std::string> &hidden_extensions, const char *name) {
	return caps.has_extension(name) && !hidden(hidden_extensions, name);
}

} // namespace

DriverVersion nvidia_driver_version(uint32_t raw) {
	return DriverVersion{ (raw >> 22) & 0x3ffu, (raw >> 14) & 0xffu };
}

const std::vector<const char *> &ngx_device_extensions() {
	static const std::vector<const char *> names = { "VK_NVX_binary_import", "VK_NVX_image_view_handle" };
	return names;
}

std::string check_device(const vk::PhysicalDeviceCaps &caps, const std::vector<std::string> &hidden_extensions) {
	char buffer[256];
	if (caps.vendor_id != 0x10DE) {
		std::snprintf(buffer, sizeof(buffer), "%s is not an NVIDIA GPU (DLSS needs an NVIDIA RTX GPU)", caps.name.c_str());
		return buffer;
	}
	const DriverVersion driver = nvidia_driver_version(caps.driver_version);
	if (driver.major < MIN_DRIVER_MAJOR || (driver.major == MIN_DRIVER_MAJOR && driver.minor < MIN_DRIVER_MINOR)) {
		std::snprintf(buffer, sizeof(buffer), "the NVIDIA driver %u.%02u is older than %u.%02u, the minimum for DLSS: update the driver",
				driver.major, driver.minor, MIN_DRIVER_MAJOR, MIN_DRIVER_MINOR);
		return buffer;
	}
	std::string missing;
	for (const char *name : ngx_device_extensions()) {
		if (!offers(caps, hidden_extensions, name)) {
			missing += missing.empty() ? name : std::string(", ") + name;
		}
	}
	if (!missing.empty()) {
		return caps.name + " does not expose " + missing + " (DLSS needs an NVIDIA RTX GPU and a current driver)";
	}
	return std::string();
}

DlssFeature &DlssFeature::get() {
	static DlssFeature *instance = new DlssFeature(); // never destroyed: the layer may call it during teardown
	return *instance;
}

void DlssFeature::configure(UpscalerProvider *backend, const std::string &no_backend_reason,
		const std::vector<std::string> &hidden_extensions) {
	std::lock_guard<std::mutex> lock(mutex_);
	backend_ = backend;
	no_backend_reason_ = no_backend_reason.empty() ? std::string("no DLSS runtime backend") : no_backend_reason;
	hidden_extensions_ = hidden_extensions;
}

void DlssFeature::request_instance_extensions(std::vector<std::string> &extensions) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (backend_ == nullptr) {
		return;
	}
	std::vector<std::string> own;
	std::string reason;
	if (backend_->request_instance_extensions(own, reason)) {
		extensions.insert(extensions.end(), own.begin(), own.end());
		instance_reason_.clear();
	} else {
		instance_reason_ = reason.empty() ? std::string("the DLSS runtime declined the Vulkan instance") : reason;
	}
}

bool DlssFeature::request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	status_.tensor_core_hint = caps.has_extension("VK_NV_cooperative_matrix") || caps.has_extension("VK_KHR_cooperative_matrix");
	device_reason_ = check_device(caps, hidden_extensions_);
	if (device_reason_.empty() && backend_ == nullptr) {
		device_reason_ = no_backend_reason_;
	}
	if (device_reason_.empty() && !instance_reason_.empty()) {
		device_reason_ = instance_reason_;
	}
	if (device_reason_.empty()) {
		vk::DeviceRequest own;
		std::string backend_reason;
		if (backend_->request_device(caps, own, backend_reason)) {
			for (const std::string &name : own.extensions) {
				request.add_extension(name.c_str());
			}
			request.features.merge(own.features);
			request.queues.insert(request.queues.end(), own.queues.begin(), own.queues.end());
			return true;
		}
		device_reason_ = backend_reason.empty() ? std::string("the DLSS runtime does not support this GPU") : backend_reason;
	}
	reason = device_reason_;
	return false;
}

void DlssFeature::on_device_created(const vk::DeviceContext &context, bool accepted, const std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	status_.device_seen = true;
	status_.available = false;
	device_ = context.device;
	if (!accepted) {
		status_.reason = !device_reason_.empty() ? device_reason_ : reason;
		return;
	}
	std::string init_reason;
	if (backend_ != nullptr && backend_->initialize(context, init_reason)) {
		status_.available = true;
		status_.reason.clear();
	} else {
		status_.reason = init_reason.empty() ? std::string("the DLSS runtime could not be initialised") : init_reason;
	}
}

void DlssFeature::on_device_destroyed(VkDevice device) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (device == VK_NULL_HANDLE || device != device_) {
		return;
	}
	if (status_.available && backend_ != nullptr) {
		backend_->shutdown();
	}
	status_.available = false;
	status_.reason = "the Vulkan device was destroyed";
	device_ = VK_NULL_HANDLE;
}

DlssDeviceStatus DlssFeature::status() const {
	std::lock_guard<std::mutex> lock(mutex_);
	DlssDeviceStatus s = status_;
	if (!s.device_seen) {
		s.reason = "no Vulkan device was created through the addon's layer";
	}
	return s;
}

UpscalerProvider *DlssFeature::backend() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return backend_;
}

} // namespace lu::dlss
