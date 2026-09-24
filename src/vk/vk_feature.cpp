#include "vk/vk_feature.h"

#include <algorithm>

namespace nvdlss::vk {

bool PhysicalDeviceCaps::has_extension(const char *extension) const {
	return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool DeviceContext::has_enabled_extension(const char *extension) const {
	return std::find(enabled_extensions.begin(), enabled_extensions.end(), extension) != enabled_extensions.end();
}

} // namespace nvdlss::vk
