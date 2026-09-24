// Runs every registered unit test. Output ends with "unit tests: <passed> passed, <failed> failed";
// the exit code is the number of failed tests (capped at 125).
#include "test_framework.h"

#include <cstring>

namespace unit {
namespace {
int g_failures_in_current = 0;
}

std::vector<TestCase> &registry() {
	static std::vector<TestCase> tests;
	return tests;
}

void report_failure(const char *file, int line, const std::string &message) {
	++g_failures_in_current;
	std::printf("    %s:%d: %s\n", file, line, message.c_str());
}

} // namespace unit

int main(int argc, char **argv) {
	const char *filter = argc > 1 ? argv[1] : nullptr;
	int passed = 0;
	int failed = 0;
	for (const unit::TestCase &test : unit::registry()) {
		if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
			continue;
		}
		unit::g_failures_in_current = 0;
		test.run();
		if (unit::g_failures_in_current == 0) {
			++passed;
			std::printf("PASS %s\n", test.name);
		} else {
			++failed;
			std::printf("FAIL %s\n", test.name);
		}
	}
	std::printf("unit tests: %d passed, %d failed\n", passed, failed);
	return failed > 125 ? 125 : failed;
}
