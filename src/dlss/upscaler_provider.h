// The interface a DLSS Super Resolution / DLAA runtime backend implements (Streamline, loaded from
// the NVIDIA runtime the developer fetched). DlssFeature (dlss_feature.h) drives it from the Vulkan
// layer; the Upscaler (godot/upscaler.h) records evaluate() at its marker dispatch.
//
//   1. Vulkan requirements  -> before vkCreateInstance / vkCreateDevice: the backend's instance and
//                              device extensions, feature bits and extra queues (the runtime reports
//                              them; they go through vk::DeviceRequest like Reflex's).
//   2. The FSR2 slot        -> godot/upscaler.h: render size, jitter, mip bias, TAA-off from Godot's
//                              FSR2 mode; the buffers re-configured to "no upscaler"; the target-size
//                              "color_upscaled" texture DLSS writes.
//   3. Command recording    -> the reference pipeline's dispatch is the marker; the layer's
//                              MarkerInterceptor hands the command buffer to evaluate() and re-binds
//                              Godot's state afterwards (Streamline's rule for manual hooking).
#pragma once

#include "dlss/jitter.h"
#include "vk/vk_feature.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lu::dlss {

enum class QualityMode { DLAA, Quality, Balanced, Performance, UltraPerformance };

struct ImageSize {
	uint32_t width = 0;
	uint32_t height = 0;
};

// Native images of one view, all owned by Godot's RenderSceneBuffersRD for the frame.
struct EvaluateInputs {
	VkImage color = VK_NULL_HANDLE; // render size, linear HDR, SHADER_READ_ONLY_OPTIMAL
	VkImageView color_view = VK_NULL_HANDLE;
	VkImage depth = VK_NULL_HANDLE; // render size, reverse Z
	VkImageView depth_view = VK_NULL_HANDLE;
	VkImage velocity = VK_NULL_HANDLE; // render size, current -> previous, clip * 0.5 units
	VkImageView velocity_view = VK_NULL_HANDLE;
	VkImage output = VK_NULL_HANDLE; // target size "color_upscaled", GENERAL (storage)
	VkImageView output_view = VK_NULL_HANDLE;
	VkFormat color_format = VK_FORMAT_UNDEFINED;
	VkFormat output_format = VK_FORMAT_UNDEFINED;
	ImageSize render_size;
	ImageSize target_size;
	JitterPixels jitter; // render-resolution pixels, [-0.5, 0.5]
	float motion_vector_scale_x = 0.0f; // converts Godot's velocity texels to render pixels
	float motion_vector_scale_y = 0.0f;
	bool reset_history = false; // camera cut (Godot has no signal: heuristic)
};

class UpscalerProvider {
public:
	virtual ~UpscalerProvider() = default;

	// Before vkCreateInstance / vkCreateDevice: what the backend needs enabled. Returning
	// false (with a reason) disables DLSS without affecting Reflex; it must then leave the
	// request untouched.
	virtual bool request_instance_extensions(std::vector<std::string> &extensions, std::string &reason) = 0;
	virtual bool request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) = 0;

	// After the host's device exists (the runtime is initialised on it). `context.queues` holds the
	// queues request_device() asked for.
	virtual bool initialize(const vk::DeviceContext &context, std::string &reason) = 0;
	virtual void shutdown() = 0;

	// Render size DLSS wants for a target size and quality mode (drives Viewport.scaling_3d_scale).
	virtual ImageSize optimal_render_size(ImageSize target, QualityMode mode) const = 0;

	// Records the upscale into the host's command buffer, called from inside the Vulkan layer's
	// vkCmdDispatch interception of the marker pipeline. The caller restores Godot's pipeline,
	// descriptor-set and push-constant state afterwards (Streamline's rule for manual hooking).
	virtual bool evaluate(VkCommandBuffer command_buffer, const EvaluateInputs &inputs) = 0;
};

} // namespace lu::dlss
