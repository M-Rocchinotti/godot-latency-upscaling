// The reference upscaler's push constants (the Params block of reference_upscaler_glsl.h), shared by
// the RenderingDevice dispatch (godot/upscaler.cpp) and the native recorder (native_reference.h).
#pragma once

#include <cstddef>
#include <cstdint>

namespace lu::upscale {

struct ReferenceParams {
	float render_size[2] = {};
	float target_size[2] = {};
	float jitter[2] = {}; // render pixels, texture orientation (Y down)
	float motion_sign[2] = { 1.0f, 1.0f };
	float max_history = 16.0f;
	float reset = 0.0f;
	// The native recorder's job: which images this dispatch reads and writes (0: none). The shader
	// ignores it; it only travels with the marker dispatch's push constants.
	uint32_t job = 0;
	uint32_t pad = 0;
};
static_assert(sizeof(ReferenceParams) == 48, "must match the GLSL push-constant block");
inline constexpr uint32_t REFERENCE_PARAMS_JITTER_X_OFFSET = static_cast<uint32_t>(offsetof(ReferenceParams, jitter));
inline constexpr uint32_t REFERENCE_PARAMS_JOB_OFFSET = static_cast<uint32_t>(offsetof(ReferenceParams, job));

} // namespace lu::upscale
