// CPU-side reproduction of Godot 4.7's temporal camera jitter (Phase 2 groundwork).
//
// DLSS needs the sub-pixel jitter of every frame, and Godot does not expose it to extensions
// (RenderSceneData has no jitter accessor). The formula is simple enough to reproduce exactly:
//   phase count  servers/rendering/renderer_viewport.cpp:269-277 (4.7.2)
//   Halton value servers/rendering/renderer_scene_cull.cpp:55-64
//   selection    renderer_scene_cull.cpp:2690 (frame_number % phase_count) / viewport size
//   FSR2 input   render_forward_clustered.cpp:2477 (taa_jitter * internal_size * 0.5 = pixels)
// Phase 2 still has to find Godot's frame_number (RendererCompositorRD's counter) once, by a
// one-time readback of the scene UBO; see docs/ARCHITECTURE.md.
//
// Pure C++, unit-tested against values computed with Godot's own code.
#pragma once

#include <cstdint>

namespace lu::dlss {

// Godot's Halton value in [-1, 1] (float arithmetic identical to the engine's).
float halton(int index, int base);

// Godot's jitter_phase_count: 8 * (target / render)^2 for a temporal upscaler, 16 for TAA,
// else 0 (no jitter).
uint32_t jitter_phase_count(uint32_t target_width, uint32_t render_width, bool temporal_upscaler, bool taa);

struct JitterPixels {
	float x = 0.0f;
	float y = 0.0f;
};

// Jitter of Godot frame `frame_number` in render-resolution pixels, in [-0.5, 0.5]: the value
// FSR2 receives and the unit DLSS expects. Zero when phase_count is 0.
JitterPixels jitter_in_pixels(uint64_t frame_number, uint32_t phase_count);

} // namespace lu::dlss
