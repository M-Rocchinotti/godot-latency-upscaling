// Debug-only fault injection so the fallback/retry paths can be exercised at run time.
//
// The set is parsed once at CORE init from GODOT_LATENCY_UPSCALING_TEST_FAULT (a comma list) and
// read at the fault sites, all guarded by DEBUG_ENABLED, so release builds contain none of it.
// Known faults:
//   device_extension  vkCreateDevice is asked for a bogus device extension, so the patched
//                     create fails and the dispatch layer must fall back to the host's own info.
//   swapchain_create  the first latency-mode vkCreateSwapchainKHR is forced to fail, so Reflex
//                     must fall back to a swapchain without latency mode.
#pragma once

#include <string>
#include <vector>

namespace lu::testing {

// Set once at CORE init (debug builds only). Safe to read afterwards from any thread.
void set_faults(const std::vector<std::string> &faults);

// True when `name` was requested. Always false until set_faults() ran.
bool fault(const char *name);

} // namespace lu::testing
