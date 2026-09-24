#include "vk/vk_feature.h"

#include <algorithm>

namespace lu::vk {

uint32_t PhysicalDeviceCaps::extension_version(const char *extension) const {
	const auto it = extensions.find(extension);
	return it != extensions.end() ? it->second : 0;
}

bool DeviceContext::has_enabled_extension(const char *extension) const {
	return std::find(enabled_extensions.begin(), enabled_extensions.end(), extension) != enabled_extensions.end();
}

} // namespace lu::vk
