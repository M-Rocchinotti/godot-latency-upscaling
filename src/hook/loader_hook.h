// Routes the host executable's request for the Vulkan loader to the dispatch layer.
//
// Godot 4.7 loads Vulkan through volk: LoadLibraryA("vulkan-1.dll") followed by
// GetProcAddress(module, "vkGetInstanceProcAddr") (thirdparty/volk/volk.c:87,91), called from
// godot.exe itself. Patching godot.exe's import of LoadLibraryA (and the W / Ex variants, for
// other builds) lets the addon return its own module there, without files next to the
// executable, environment variables or registry changes. Only calls made from godot.exe are
// affected; every other DLL keeps loading the real vulkan-1.dll.
#pragma once

#include <string>
#include <vector>

namespace lu::hook {

struct InstallResult {
	bool installed = false;
	std::string reason; // why not, when installed == false
	std::vector<std::string> patched; // e.g. "KERNEL32.dll!LoadLibraryA"
};

// Keeps this DLL loaded until the process exits, whatever FreeLibrary calls follow. Once Godot
// holds pointers into it (the Vulkan dispatch, the patched imports, registered callbacks) it
// must never be unloaded. Idempotent; false if Windows refuses.
bool pin_addon_module();

// Patches the imports of the process executable (after pinning the DLL). Idempotent.
InstallResult install();

} // namespace lu::hook
