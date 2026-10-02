// The interface a DLSS Super Resolution / DLAA runtime backend implements (Streamline, loaded from
// the NVIDIA runtime the developer fetched: dlss/streamline.h). DlssFeature (dlss_feature.h) drives
// it from the Vulkan layer; the Upscaler (godot/upscaler.h) has it evaluate at its marker dispatch.
//
//   1. Vulkan requirements  -> before vkCreateInstance / vkCreateDevice: the backend's instance and
//                              device extensions, feature bits and extra queues (they go through
//                              vk::DeviceRequest like Reflex's; Streamline adds its own in its
//                              vkCreateDevice, see streamline.h).
//   2. The FSR2 slot        -> godot/upscaler.h: render size, jitter, mip bias, TAA-off from Godot's
//                              FSR2 mode; the buffers re-configured to "no upscaler"; the target-size
//                              "color_upscaled" texture DLSS writes.
//   3. Command recording    -> the reference pipeline's dispatch is the marker; the layer's
//                              MarkerInterceptor hands the command buffer to evaluate() and re-binds
//                              Godot's state afterwards (Streamline's rule for manual state handling).
#pragma once

#include "upscale/quality.h"
#include "vk/vk_feature.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lu::dlss {

struct ImageSize {
	uint32_t width = 0;
	uint32_t height = 0;
};

// One image as Vulkan knows it (Streamline needs the full description in Vulkan).
struct EvaluateImage {
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t width = 0;
	uint32_t height = 0;
	VkImageUsageFlags usage = 0;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // the layout it is in when evaluate() runs
	bool valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE && width > 0 && height > 0; }
};

// Everything one evaluation needs: Godot's images of one view and the frame's camera state.
struct EvaluateInputs {
	uint32_t viewport = 0; // one id per view (render target)
	EvaluateImage color; // render size, linear HDR
	EvaluateImage depth; // render size, reverse Z
	EvaluateImage velocity; // render size, current -> previous, UV units
	EvaluateImage output; // target size ("color_upscaled")
	upscale::Quality mode = upscale::Quality::Quality; // concrete (Auto resolved)
	upscale::Preset preset = upscale::Preset::Default;
	float jitter_x = 0.0f; // render pixels, as Godot hands it to FSR2 (taa_jitter * render size / 2)
	float jitter_y = 0.0f;
	float mvec_scale_x = 1.0f; // velocity is in UV units: already normalised
	float mvec_scale_y = 1.0f;
	bool reset = false; // no history (first frame, resize, cut)
	bool depth_inverted = true; // Godot renders reverse Z
	// Row-major, without jitter.
	float view_to_clip[4][4] = {};
	float clip_to_view[4][4] = {};
	float clip_to_prev_clip[4][4] = {};
	float prev_clip_to_clip[4][4] = {};
	float camera_pos[3] = {};
	float camera_up[3] = {};
	float camera_right[3] = {};
	float camera_forward[3] = {};
	float camera_near = 0.0f;
	float camera_far = 0.0f;
	float camera_fov_y = 0.0f; // radians
	float camera_aspect = 0.0f;
	bool orthographic = false;
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

	// Render size DLSS wants for a target size and a concrete mode; false when it cannot say.
	virtual bool optimal_render_size(ImageSize target, upscale::Quality mode, ImageSize &render) const = 0;

	// Records the upscale into the host's command buffer, called from inside the Vulkan layer's
	// vkCmdDispatch interception of the marker pipeline. false (with `error`) when nothing was
	// recorded. The caller restores Godot's pipeline, descriptor-set and push-constant state.
	virtual bool evaluate(VkCommandBuffer command_buffer, const EvaluateInputs &inputs, std::string &error) = 0;

	// E.g. "DLSS 310.9.1 through Streamline 2.14.1", once initialised.
	virtual std::string version() const { return std::string(); }

	// DLSS Frame Generation (Phase 3, present-only). The upscaler controller sets the request from the
	// addon's settings; the backend drives Frame Generation on its own viewport inside evaluate() (the
	// Super Resolution tags provide depth + motion vectors, the backbuffer is intercepted). A backend
	// that cannot do Frame Generation leaves these as no-ops.
	struct FrameGenerationRuntime {
		bool active = false; // Frame Generation is generating frames now
		uint32_t num_frames_to_generate = 0; // generated frames per real frame (multiplier - 1)
		uint32_t num_frames_presented = 0; // slDLSSGGetState().numFramesActuallyPresented
		uint32_t render_width = 0; // the Super Resolution render size
		uint32_t render_height = 0;
		std::string status; // why not active (when a runtime status bit is set), e.g. "Reflex not active"
	};
	virtual void set_frame_generation_request(bool on, uint32_t num_frames_to_generate) {
		(void)on;
		(void)num_frames_to_generate;
	}
	virtual bool frame_generation_runtime(FrameGenerationRuntime &out) const {
		(void)out;
		return false;
	}
};

} // namespace lu::dlss
