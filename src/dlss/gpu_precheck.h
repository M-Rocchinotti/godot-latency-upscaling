// Which GPUs could run DLSS, decided before Godot creates its Vulkan instance.
//
// Whether the host's Vulkan calls go through Streamline has to be decided when Godot asks for
// vulkan-1.dll, long before its device exists. So at CORE initialisation a temporary instance
// (through the real loader, no surface) lists the GPUs; only when one passes the checks does the
// addon load Streamline at all. The instance is destroyed again before this returns.
//
// Windows only (it loads vulkan-1.dll); the decision logic it feeds is dlss::check_device.
#pragma once

#include "vk/vk_feature.h"

#include <functional>
#include <string>
#include <vector>

namespace lu::dlss {

struct AdapterVerdict {
	std::string name;
	bool accepted = false;
	std::string reason; // why not (when !accepted)
};

// Calls `accept` for every physical device, with the temporary instance alive; returns the
// verdicts (empty with `error` when Vulkan is unavailable).
std::vector<AdapterVerdict> check_adapters(
		const std::function<bool(const vk::PhysicalDeviceCaps &caps, std::string &reason)> &accept, std::string &error);

} // namespace lu::dlss
