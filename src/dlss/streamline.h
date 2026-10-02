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

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lu::reflex {
class FrameTokenSource; // reflex/sl_reflex_device.h: the one Streamline frame token per frame
}

namespace lu::dlss {

// The Streamline features slInit loads, as sl::Feature values (sl::Feature is uint32_t): DLSS
// Super Resolution always; with `fg_permitted` (Phase 3) also DLSS Frame Generation and the Reflex
// and PC-latency plugins it requires. Pure, so the gating is unit-tested. When fg is off the list
// is exactly {kFeatureDLSS}, so slInit is byte-identical to before Phase 3.
std::vector<uint32_t> streamline_features_to_load(bool fg_permitted);

class Streamline : public UpscalerProvider {
public:
	static Streamline &get();

	// CORE initialisation: verifies sl.interposer.dll's NVIDIA signature, loads it and initialises
	// Streamline for Vulkan (no over-the-air updates: only the pinned files run). `project_id`: the
	// project's GUID-like ID for NGX (dlss/project_id.h). `fg_permitted` (Phase 3, guarded off by
	// default) also loads the Frame Generation plugins so their functions can be resolved once the
	// device exists; nothing drives rendering with them yet and the Reflex backend is not switched.
	bool load(const std::wstring &runtime_dir, const std::string &project_id, bool fg_permitted, std::string &reason);
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
	void set_frame_generation_request(bool on, uint32_t num_frames_to_generate) override;
	bool frame_generation_runtime(FrameGenerationRuntime &out) const override;

	// "DLSS 310.9.1 through Streamline 2.14.1" once initialised, else "".
	std::string version() const override;
	// The last warnings / errors Streamline reported (for the status).
	std::vector<std::string> log_tail() const;

	// Phase 3: whether the Frame Generation plugins were loaded and their functions resolved. It is
	// never used to drive rendering in this milestone (the Reflex backend is not switched); a later
	// milestone gates the actual Frame Generation on it.
	bool frame_generation_ready() const;

private:
	struct Api;
	Api &api() const;
	// Phase 3: resolve the Frame Generation plugins' functions into the Api once the device exists
	// (slGetFeatureFunction requires it). Graceful: on any failure it logs, leaves Frame Generation
	// not ready, and returns without touching DLSS or the non-FG path. Caller holds mutex_.
	void resolve_frame_generation_functions();
	// Phase 3, present-only: drive Frame Generation on this viewport (slDLSSGSetOptions) and read back
	// its state (slDLSSGGetState), from inside evaluate() on the render thread. Caller holds mutex_.
	void drive_frame_generation(uint32_t viewport_id, uint32_t render_width, uint32_t render_height);

	mutable std::mutex mutex_;
	HMODULE module_ = nullptr;
	bool initialized_ = false; // slInit succeeded
	bool device_ready_ = false; // the DLSS functions are resolved for the device
	bool fg_permitted_ = false; // Phase 3: Frame Generation plugins were requested at slInit
	bool fg_runtime_ready_ = false; // Phase 3: their functions were resolved (not yet driving anything)
	bool fg_request_on_ = false; // Phase 3: the addon asked for Frame Generation (present-only)
	uint32_t fg_request_frames_ = 1; // numFramesToGenerate the addon asked for
	FrameGenerationRuntime fg_runtime_; // the last slDLSSGGetState snapshot (for the status)
	// Phase 3: the one frame token per frame, shared by sl.reflex, the SR evaluate and FG. Created
	// when the Frame Generation plugins are loaded; the SR evaluate uses its current() token then.
	std::unique_ptr<reflex::FrameTokenSource> fg_token_source_;
	std::string version_;
	uint64_t frame_ = 0; // the per-evaluate frame index for DLSS Super Resolution without Frame Generation
};

// What Streamline needs from the device merely by being in the call chain, whether or not DLSS
// runs on it: the private-data feature, which its common plugin uses (vkCreatePrivateDataSlot)
// without asking for it. Registered with the layer whenever Vulkan goes through Streamline.
class StreamlineChainFeature : public vk::VulkanFeature {
public:
	static StreamlineChainFeature &get();
	const char *name() const override { return "streamline"; }
	void request_instance_extensions(std::vector<std::string> &) override {}
	bool request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) override;
	void on_device_created(const vk::DeviceContext &, bool, const std::string &) override {}
	void on_device_destroyed(VkDevice) override {}
};

// Streamline's log sink: warnings and errors go to the addon's log (kept for the status too).
void streamline_log(int type, const char *message);

} // namespace lu::dlss
