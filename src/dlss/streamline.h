// DLSS through NVIDIA Streamline 2.14.1, from the runtime the developer fetched (never shipped).
//
// Streamline runs as an interposer in front of the Vulkan loader, the way NVIDIA's own Godot
// integration uses it: the layer's `next` functions come from sl.interposer.dll instead of
// vulkan-1.dll (vk::set_loader_override), so Streamline's vkCreateInstance / vkCreateDevice add
// what DLSS needs (extensions, Vulkan 1.2 features, queues) and its present path runs every frame.
// Command-list state tracking stays off: the layer's MarkerInterceptor re-binds Godot's state after
// each evaluation.
//
// Lifetime: load() at CORE initialisation (before Godot asks for vulkan-1.dll), only when the files
// are present, sl.interposer.dll carries NVIDIA's signature and a GPU passes the checks; the module
// then stays loaded for the process. shutdown() runs right before the device is destroyed.
//
// Windows only; not unit-tested (it needs NVIDIA's runtime): tests/dlss_tests.gd exercises it on an
// RTX GPU.
#pragma once

#include "dlss/upscaler_provider.h"

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

namespace lu::dlss {

class Streamline : public UpscalerProvider {
public:
	static Streamline &get();

	// CORE initialisation: verifies sl.interposer.dll's NVIDIA signature, loads it and initialises
	// Streamline for Vulkan with DLSS only (no over-the-air updates: only the pinned files run).
	bool load(const std::wstring &runtime_dir, std::string &reason);
	// Undoes load() while no Vulkan call went through Streamline yet (no GPU supports DLSS).
	void unload();
	bool loaded() const;
	HMODULE interposer() const;

	// Whether DLSS runs on this physical device (Streamline's own check: RTX, driver, OS).
	bool adapter_supported(VkPhysicalDevice device, std::string &reason);

	// UpscalerProvider
	bool request_instance_extensions(std::vector<std::string> &extensions, std::string &reason) override;
	bool request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) override;
	bool initialize(const vk::DeviceContext &context, std::string &reason) override;
	void shutdown() override;
	bool optimal_render_size(ImageSize target, upscale::Quality mode, ImageSize &render) const override;
	bool evaluate(VkCommandBuffer command_buffer, const EvaluateInputs &inputs, std::string &error) override;

	// "DLSS 310.9.1 through Streamline 2.14.1" once initialised, else "".
	std::string version() const;
	// The last warnings / errors Streamline reported (for the status).
	std::vector<std::string> log_tail() const;

private:
	struct Api;
	Api &api() const;

	mutable std::mutex mutex_;
	HMODULE module_ = nullptr;
	bool initialized_ = false; // slInit succeeded
	bool device_ready_ = false; // the DLSS functions are resolved for the device
	std::string version_;
	uint64_t frame_ = 0;
};

// Streamline's log sink: warnings and errors go to the addon's log (kept for the status too).
void streamline_log(int type, const char *message);

} // namespace lu::dlss
