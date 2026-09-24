// Phase 2 design: the interface a DLSS Super Resolution / DLAA backend implements.
//
// Nothing in Phase 1 implements or calls this. It fixes the contract between the three parts
// the research (RESEARCH.md §2.2-2.4) identified, so that Phase 2 can be built and reviewed
// piece by piece:
//
//   1. Vulkan requirements  -> collected before vkCreateInstance / vkCreateDevice by the hook
//                              (vk::DeviceRequest; NGX reports its extensions at run time).
//   2. The FSR2 slot        -> the viewport runs in Scaling3DMode FSR2 (Godot computes render
//                              size, jitter, mip bias, TAA-off), then its RenderSceneBuffersRD is
//                              re-configured with SCALING_3D_MODE_METALFX_TEMPORAL, which maps to
//                              "no upscaler" on Vulkan, and a target-size "color_upscaled"
//                              texture is created. Tonemapping then reads DLSS's output.
//   3. Command recording    -> a POST_TRANSPARENT CompositorEffect dispatches a marker compute
//                              pipeline; the Vulkan layer recognises that vkCmdDispatch and
//                              records evaluate() on the same VkCommandBuffer instead.
//
// See docs/ARCHITECTURE.md ("Phase 2") for the sequence and the open risks.
#pragma once

#include "dlss/jitter.h"
#include "vk/device_patch.h"

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
	// false (with a reason) disables the upscaler without affecting Reflex.
	virtual bool request_instance_extensions(std::vector<std::string> &extensions, std::string &reason) = 0;
	virtual bool request_device(VkPhysicalDevice physical_device, vk::DeviceRequest &request, std::string &reason) = 0;

	// After the host's device exists (NGX / Streamline initialisation needs it).
	virtual bool initialize(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
			PFN_vkGetInstanceProcAddr get_instance_proc_addr, PFN_vkGetDeviceProcAddr get_device_proc_addr,
			std::string &reason) = 0;
	virtual void shutdown() = 0;

	// Render size DLSS wants for a target size and quality mode (drives Viewport.scaling_3d_scale).
	virtual ImageSize optimal_render_size(ImageSize target, QualityMode mode) const = 0;

	// Records the upscale into the host's command buffer, called from inside the Vulkan layer's
	// vkCmdDispatch interception of the marker pipeline. The caller restores Godot's pipeline,
	// descriptor-set and push-constant state afterwards (Streamline's rule for manual hooking).
	virtual bool evaluate(VkCommandBuffer command_buffer, const EvaluateInputs &inputs) = 0;
};

} // namespace lu::dlss
