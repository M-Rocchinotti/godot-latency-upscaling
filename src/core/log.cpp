#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace nvdlss::log {
namespace {

void default_sink(Level level, const char *message) {
	const char *prefix = level == Level::Error ? "ERROR: " : (level == Level::Warning ? "WARNING: " : "");
	char line[1100];
	std::snprintf(line, sizeof(line), "%s%s\n", prefix, message);
	OutputDebugStringA(line);
}

std::atomic<Sink> g_sink{ &default_sink };
std::atomic<bool> g_verbose{ false };

void emit(Level level, const char *format, va_list args) {
	char body[1024];
	std::vsnprintf(body, sizeof(body), format, args);
	char message[1060];
	std::snprintf(message, sizeof(message), "[NVIDIA DLSS] %s", body);
	g_sink.load()(level, message);
}

} // namespace

void set_sink(Sink sink) {
	g_sink.store(sink != nullptr ? sink : &default_sink);
}

void set_verbose(bool verbose) {
	g_verbose.store(verbose);
}

bool is_verbose() {
	return g_verbose.load();
}

void info(const char *format, ...) {
	if (!g_verbose.load()) {
		return;
	}
	va_list args;
	va_start(args, format);
	emit(Level::Info, format, args);
	va_end(args);
}

void warning(const char *format, ...) {
	va_list args;
	va_start(args, format);
	emit(Level::Warning, format, args);
	va_end(args);
}

void error(const char *format, ...) {
	va_list args;
	va_start(args, format);
	emit(Level::Error, format, args);
	va_end(args);
}

} // namespace nvdlss::log
