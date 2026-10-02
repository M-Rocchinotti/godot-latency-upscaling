#include "dlss/fg_feature.h"

#include <cctype>
#include <cstdio>
#include <string>

namespace lu::dlss {
namespace {

std::string to_lower(const std::string &s) {
	std::string out = s;
	for (char &c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

// The model number right after a tag ("rtx" / "gtx") in a lowercased GPU name, e.g. "rtx 4080" ->
// 4080. 0 when the tag is absent or not followed by digits.
int model_number_after(const std::string &lower, const char *tag) {
	const size_t tag_pos = lower.find(tag);
	if (tag_pos == std::string::npos) {
		return 0;
	}
	size_t pos = tag_pos + std::string(tag).size();
	while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '-')) {
		++pos;
	}
	int value = 0;
	bool any = false;
	while (pos < lower.size() && lower[pos] >= '0' && lower[pos] <= '9') {
		value = value * 10 + (lower[pos] - '0');
		any = true;
		if (value > 100000) {
			break; // a runaway number is not a model we know
		}
		++pos;
	}
	return any ? value : 0;
}

} // namespace

NvArchitecture classify_nvidia_architecture(uint32_t vendor_id, const std::string &name, uint32_t /*device_id*/) {
	if (vendor_id != 0x10DE) {
		return NvArchitecture::Unknown; // not NVIDIA; the vendor check reports that separately
	}
	const std::string lower = to_lower(name);
	// Explicit generation keywords first: they disambiguate workstation cards whose number does not
	// follow the GeForce scheme (e.g. "RTX 6000 Ada", "RTX 5880 Ada").
	if (lower.find("blackwell") != std::string::npos) {
		return NvArchitecture::Blackwell;
	}
	if (lower.find("ada") != std::string::npos) {
		return NvArchitecture::Ada;
	}
	// GeForce RTX consumer numbering.
	const int rtx = model_number_after(lower, "rtx");
	if (rtx > 0) {
		if (rtx >= 5000 && rtx < 6000) {
			return NvArchitecture::Blackwell; // RTX 50xx
		}
		if (rtx >= 4000 && rtx < 5000) {
			return NvArchitecture::Ada; // RTX 40xx
		}
		if (rtx >= 2000 && rtx < 4000) {
			return NvArchitecture::PreAda; // RTX 20xx (Turing) / 30xx (Ampere): no Frame Generation
		}
		return NvArchitecture::Unknown; // an RTX number we do not recognise: be conservative
	}
	// Every GTX card (Turing 16xx, Pascal 10xx, Maxwell 9xx and older) predates Ada.
	if (model_number_after(lower, "gtx") > 0) {
		return NvArchitecture::PreAda;
	}
	return NvArchitecture::Unknown; // an NVIDIA name we cannot place: conservative
}

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

int fg_multiplier_from_frames_to_generate(uint32_t num_frames_to_generate_max) {
	// One real frame plus the generated ones: numFramesToGenerateMax 5 -> 6x. 0 means no Frame
	// Generation (multiplier 1).
	return static_cast<int>(num_frames_to_generate_max) + 1;
}

std::string check_frame_generation(const FgCaps &caps) {
	// Not probed (e.g. the editor, where Frame Generation is moot): the honest, player-actionable
	// default, as in milestone H1.
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
		// Frame Generation needs native Vulkan optical flow (driver 527.64+); older drivers offer it
		// only through Direct3D interop, which this Vulkan addon cannot use.
		std::snprintf(buffer, sizeof(buffer),
				"the NVIDIA driver %u.%02u is below %u.%02u, which Frame Generation needs for native Vulkan optical "
				"flow (older drivers support it only through Direct3D interop): update the driver",
				caps.driver_major, caps.driver_minor, FG_MIN_DRIVER_MAJOR, FG_MIN_DRIVER_MINOR);
		return buffer;
	}
	// The OS / HAGS requirement only refuses a capable GPU once it has actually been determined; the
	// real gate belongs to the milestone that loads Frame Generation (os_known is false until then).
	if (caps.os_known && !caps.os_supported) {
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

void FgFeature::set_runtime_max_multiplier(int multiplier) {
	std::lock_guard<std::mutex> lock(mutex_);
	runtime_max_multiplier_ = multiplier > 0 ? multiplier : 0;
}

FgStatus FgFeature::status() const {
	std::lock_guard<std::mutex> lock(mutex_);
	FgStatus s;
	const std::string reason = check_frame_generation(caps_);
	s.supported = reason.empty();
	s.active = false; // no Frame Generation runtime is loaded before milestone H5 / the RTX milestones
	s.reason = s.supported ? std::string("Frame Generation is not yet available in this version") : reason;
	if (!s.supported) {
		s.max_multiplier = 1; // no Frame Generation (unchanged on the dev box: unavailable -> 1)
	} else if (runtime_max_multiplier_ > 0) {
		s.max_multiplier = runtime_max_multiplier_; // the runtime's real ceiling (slDLSSGGetState)
	} else {
		s.max_multiplier = max_multiplier_for(caps_.architecture); // the architecture guess until then
	}
	return s;
}

} // namespace lu::dlss
