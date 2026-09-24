// A minimal test harness: no third-party dependency for a handful of pure-logic tests.
#pragma once

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace unit {

struct TestCase {
	const char *name;
	void (*run)();
};

std::vector<TestCase> &registry();
void report_failure(const char *file, int line, const std::string &message);

struct Registrar {
	Registrar(const char *name, void (*run)()) { registry().push_back({ name, run }); }
};

template <typename A, typename B>
std::string describe_pair(const A &a, const B &b) {
	std::ostringstream s;
	s << a << " vs " << b;
	return s.str();
}

} // namespace unit

#define TEST(name)                                           \
	static void name();                                      \
	static const unit::Registrar name##_registrar(#name, &name); \
	static void name()

#define CHECK(condition)                                                  \
	do {                                                                  \
		if (!(condition)) {                                               \
			unit::report_failure(__FILE__, __LINE__, "CHECK(" #condition ")"); \
		}                                                                 \
	} while (false)

#define CHECK_EQ(actual, expected)                                                                                  \
	do {                                                                                                            \
		const auto &nvdlss_a_ = (actual);                                                                           \
		const auto &nvdlss_e_ = (expected);                                                                         \
		if (!(nvdlss_a_ == nvdlss_e_)) {                                                                            \
			unit::report_failure(__FILE__, __LINE__, "CHECK_EQ(" #actual ", " #expected "): " + unit::describe_pair(nvdlss_a_, nvdlss_e_)); \
		}                                                                                                           \
	} while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                                     \
	do {                                                                                                            \
		const double nvdlss_a_ = static_cast<double>(actual);                                                       \
		const double nvdlss_e_ = static_cast<double>(expected);                                                     \
		if (!(std::fabs(nvdlss_a_ - nvdlss_e_) <= (tolerance))) {                                                   \
			unit::report_failure(__FILE__, __LINE__, "CHECK_NEAR(" #actual ", " #expected "): " + unit::describe_pair(nvdlss_a_, nvdlss_e_)); \
		}                                                                                                           \
	} while (false)
