// Recognises a request for the Vulkan loader in LoadLibrary* arguments.
#pragma once

namespace nvdlss::hook {

// True for "vulkan-1.dll" / "vulkan-1" in any letter case, with or without a directory
// ("C:\\Windows\\System32\\vulkan-1.dll"). False for anything else, including nullptr and
// look-alikes such as "vulkan-1.dll.bak" or "myvulkan-1.dll".
bool is_vulkan_loader_name(const char *name);
bool is_vulkan_loader_name(const wchar_t *name);

} // namespace nvdlss::hook
