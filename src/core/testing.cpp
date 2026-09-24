#include "core/testing.h"

#include <algorithm>

namespace lu::testing {
namespace {
// Written once (bootstrap, before any Vulkan call) and only read afterwards, so no lock is needed.
std::vector<std::string> g_faults;
} // namespace

void set_faults(const std::vector<std::string> &faults) {
	g_faults = faults;
}

bool fault(const char *name) {
	return std::find(g_faults.begin(), g_faults.end(), name) != g_faults.end();
}

} // namespace lu::testing
