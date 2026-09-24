#include "hook/loader_name.h"

#include <cwctype>

namespace lu::hook {
namespace {

template <typename Char>
bool matches(const Char *name) {
	if (name == nullptr) {
		return false;
	}
	const Char *base = name;
	for (const Char *p = name; *p != 0; ++p) {
		if (*p == '\\' || *p == '/') {
			base = p + 1;
		}
	}
	auto equals_ignore_case = [](const Char *text, const char *expected) {
		for (; *expected != 0; ++text, ++expected) {
			if (*text == 0 || std::towlower(static_cast<wint_t>(*text)) != static_cast<wint_t>(*expected)) {
				return false;
			}
		}
		return *text == 0;
	};
	return equals_ignore_case(base, "vulkan-1.dll") || equals_ignore_case(base, "vulkan-1");
}

} // namespace

bool is_vulkan_loader_name(const char *name) {
	return matches(name);
}

bool is_vulkan_loader_name(const wchar_t *name) {
	return matches(name);
}

} // namespace lu::hook
