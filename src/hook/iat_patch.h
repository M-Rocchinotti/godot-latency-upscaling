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

namespace lu::hook {

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

// The chain target for a set of freshly patched slots. A single shared hook has one `next`
// pointer, so it cannot chain each slot to its own original; when every slot held the same
// non-null original (the value present before this patch, e.g. an earlier hook), that value is
// returned. If the slots held different originals — an unusual partial pre-hook — no single value
// is correct, so `real` (the terminal loader export) is returned and, when given, *mismatch is set.
// Slots that held null are ignored; if none held a value, `real` is returned.
void *select_chain_target(const std::vector<IatSlot> &slots, void *real, bool *mismatch = nullptr);

} // namespace lu::hook
