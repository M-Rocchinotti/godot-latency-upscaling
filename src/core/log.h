// Minimal logging facade.
//
// The hook and Vulkan layers run before and outside Godot's normal flow (inside vkCreateDevice,
// on the render thread, inside LoadLibraryA), and the pure modules are also compiled into the
// C++ unit tests. They therefore log through a replaceable sink instead of calling Godot
// directly. register_types.cpp installs a sink that forwards to Godot's print functions.
#pragma once

#include <atomic>

namespace nvdlss::log {

enum class Level { Info, Warning, Error };

using Sink = void (*)(Level level, const char *message);

// Replaces the sink. nullptr restores the default (OutputDebugStringA).
void set_sink(Sink sink);

// Only Info messages are affected: warnings and errors are always emitted.
void set_verbose(bool verbose);
bool is_verbose();

void info(const char *format, ...);
void warning(const char *format, ...);
void error(const char *format, ...);

} // namespace nvdlss::log

// Emits a warning the first time the enclosing statement runs, then stays silent. Use it
// for conditions that would otherwise repeat every frame.
#define NVDLSS_WARN_ONCE(...)                                            \
	do {                                                                 \
		static std::atomic<bool> nvdlss_warned_once_{ false };           \
		if (!nvdlss_warned_once_.exchange(true)) {                       \
			::nvdlss::log::warning(__VA_ARGS__);                         \
		}                                                                \
	} while (false)
