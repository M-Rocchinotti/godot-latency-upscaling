// Import Address Table patching for one loaded PE module.
//
// Every call a module makes to an imported function goes through a pointer in its IAT.
// Replacing that pointer redirects the module's calls (and only that module's calls) to a
// replacement. Other modules, e.g. Streamline or NGX loading vulkan-1.dll through their own
// imports, are not affected.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace nvdlss::hook {

struct IatSlot {
	void **slot = nullptr; // address inside the module's IAT
	void *original = nullptr; // value before patching
	std::string dll; // import descriptor name, e.g. "KERNEL32.dll"
};

// Replaces every IAT entry of `module` that imports `function` (by name) with `replacement`.
// Entries of import descriptors without a name table (bound imports) are matched by
// comparing their address with `real_function` instead.
// Returns the patched slots (empty when the module does not import the function). On a PE
// format or memory-protection problem nothing is left half-patched and `error` is set.
std::vector<IatSlot> patch_imports(HMODULE module, const char *function, void *real_function, void *replacement,
		std::string *error);

// Writes the original pointers back.
void restore_imports(const std::vector<IatSlot> &slots);

} // namespace nvdlss::hook
