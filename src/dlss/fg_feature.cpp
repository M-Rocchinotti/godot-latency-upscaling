#include "dlss/fg_feature.h"

#include <cstdio>

namespace lu::dlss {

int max_multiplier_for(NvArchitecture architecture) {
	switch (architecture) {
		case NvArchitecture::Ada:
			return 2; // Frame Generation (one generated frame per rendered frame)
		case NvArchitecture::Blackwell:
			return 4; // Multi Frame Generation (up to three generated frames)
		case NvArchitecture::Unknown:
		case NvArchitecture::PreAda:
			break;
	}
	return 1; // no Frame Generation
}

std::string check_frame_generation(const FgCaps &caps) {
	// Until a later milestone really queries the capability (Streamline + a device observation), the
	// honest answer is "not confirmed as an RTX 40 series+": this is the guarded-off milestone H1.
	if (!caps.probed) {
		return "needs a GeForce RTX 40 series GPU";
	}
	if (!caps.vulkan_device) {
		return "no Vulkan device: Frame Generation needs the Vulkan renderer with a window";
	}
	char buffer[256];
	const char *gpu = caps.gpu_name.empty() ? "this GPU" : caps.gpu_name.c_str();
	if (caps.vendor_id != 0x10DE) {
		std::snprintf(buffer, sizeof(buffer), "%s is not an NVIDIA GPU (Frame Generation needs a GeForce RTX 40 series GPU)", gpu);
		return buffer;
	}
	if (caps.architecture == NvArchitecture::Unknown || caps.architecture == NvArchitecture::PreAda) {
		std::snprintf(buffer, sizeof(buffer), "%s does not support Frame Generation (needs a GeForce RTX 40 series GPU or newer)", gpu);
		return buffer;
	}
	if (caps.driver_major < FG_MIN_DRIVER_MAJOR ||
			(caps.driver_major == FG_MIN_DRIVER_MAJOR && caps.driver_minor < FG_MIN_DRIVER_MINOR)) {
		std::snprintf(buffer, sizeof(buffer),
				"the NVIDIA driver %u.%02u is older than %u.%02u, the minimum for Frame Generation: update the driver",
				caps.driver_major, caps.driver_minor, FG_MIN_DRIVER_MAJOR, FG_MIN_DRIVER_MINOR);
		return buffer;
	}
	if (!caps.os_supported) {
		return "needs Windows 10 20H1 or newer with hardware-accelerated GPU scheduling on";
	}
	return std::string();
}

FgFeature &FgFeature::get() {
	static FgFeature *instance = new FgFeature(); // never destroyed (parallels DlssFeature::get())
	return *instance;
}

void FgFeature::configure(const FgCaps &caps) {
	std::lock_guard<std::mutex> lock(mutex_);
	caps_ = caps;
}

FgStatus FgFeature::status() const {
	std::lock_guard<std::mutex> lock(mutex_);
	FgStatus s;
	const std::string reason = check_frame_generation(caps_);
	s.supported = reason.empty();
	s.active = false; // milestone H1 loads no Frame Generation runtime
	s.reason = s.supported ? std::string("Frame Generation is not yet available in this version") : reason;
	s.max_multiplier = s.supported ? max_multiplier_for(caps_.architecture) : 1;
	return s;
}

} // namespace lu::dlss
