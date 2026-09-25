// Motion vectors for pixels no geometry wrote: the sky and the background.
//
// In the FSR2 slot Godot clears the velocity buffer to (-1, -1) and FSR2 reconstructs the motion of
// every pixel left at that value from its depth and the camera matrices
// (render_forward_clustered.cpp:2229-2239 and :2502, shaders/effects/motion_vector_inc.glsl,
// ffx_fsr2_callbacks_glsl.h LoadInputMotionVector). After the takeover the buffers run without an
// upscaler, so the opaque pass clears velocity to 0 instead: the sky would claim to stand still on
// screen while the camera turns, and a temporal upscaler smears geometry edges across it.
//
// This pass copies Godot's velocity into a texture of our own and, for every far-plane pixel
// (reverse-Z depth exactly 0: nothing was drawn there), writes the camera motion with Godot's own
// formula:  prev = reprojection * (uv * 2 - 1, depth, 1);  velocity = prev.xy / prev.w * 0.5 + 0.5 - uv
// (current -> previous, UV units, like Godot's vectors). `reprojection` is clip -> previous clip
// without jitter; with our Vulkan-style clip space the depth enters as is (Godot's FSR2 variant uses
// a projection without the Z remap and depth * 2 - 1, the same point). A point that was behind the
// previous camera (w <= 0) had no previous screen position: its vector leads off-screen.
//
// Render thread only.
#pragma once

#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <string>

namespace lu {

enum class VelocityFillMode : uint32_t {
	FarPlane = 0, // fill far-plane pixels (the default)
	AllPixels = 1, // debug: derive every pixel from depth (tests compare that with Godot's vectors)
	Off = 2, // debug: no pass, the raw velocity (the behaviour before the fill)
};

class VelocityFill {
public:
	// Records the pass and returns the texture the upscaler must read: the filled copy, or Godot's
	// velocity itself for VelocityFillMode::Off or when the pass is unavailable (then `error` says why).
	godot::RID run(godot::RenderSceneBuffersRD *rb, const godot::Projection &reprojection, VelocityFillMode mode,
			std::string &error);
	void release(); // frees the pipeline (the texture belongs to the render buffers)

private:
	bool ensure_pipeline(std::string &error);

	godot::RID shader_, pipeline_, sampler_, uniform_set_;
	bool failed_ = false;
	uint64_t set_key_[3] = {}; // depth, velocity, filled RIDs the uniform set was built for
};

} // namespace lu
