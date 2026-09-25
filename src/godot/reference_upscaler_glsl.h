// The reference upscaler: a small, jitter-aware temporal upscaler in a compute shader.
//
// It stands in for DLSS so the whole Phase 2 plumbing (the FSR2 slot, jitter, motion vectors,
// the recording point) can be proven on any GPU. It is also the "marker" pipeline of the native
// path (docs/ARCHITECTURE.md): its uniform set names exactly DLSS's inputs -- colour, depth and
// velocity sampled, the target-size output as a storage image -- so Godot's render graph orders
// and transitions them the way DLSS needs.
//
// The push constants are upscale::ReferenceParams (src/upscale/reference_params.h).
//
// Per output pixel: take the nearest render sample at the jittered position, weight it by how close
// the scene point it saw is to the pixel centre, and accumulate it with the reprojected history
// (clamped to the current 3x3 neighbourhood; the alpha channel carries the accumulated weight).
// On a still image this converges to a supersampled result from the Halton jitter sequence; with a
// wrong jitter or motion-vector convention it measurably does not (tests/upscaler_tests.gd).
#pragma once

namespace lu {

inline constexpr const char *REFERENCE_UPSCALER_GLSL = R"GLSL(#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D color_tex;    // render size, linear HDR
layout(set = 0, binding = 1) uniform sampler2D depth_tex;    // render size, reverse Z
layout(set = 0, binding = 2) uniform sampler2D velocity_tex; // render size, current -> previous, UV units
layout(set = 0, binding = 3) uniform sampler2D history_tex;  // target size: rgb = colour, a = accumulated weight
layout(set = 0, binding = 4, rgba16f) uniform restrict writeonly image2D output_img;  // "color_upscaled"
layout(set = 0, binding = 5, rgba16f) uniform restrict writeonly image2D history_img; // next history

layout(push_constant, std430) uniform Params {
	vec2 render_size;
	vec2 target_size;
	vec2 jitter;          // render pixels, texture orientation (Y down)
	vec2 motion_sign;     // +1/+1 for Godot's convention; flipped only by the convention tests
	float max_history;    // cap of the accumulated weight (the effective history length)
	float reset;          // 1: ignore the history (first frame, resize)
	uint job;             // the native recorder's job id (0: none); not used by the shader
	uint pad;
} params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.target_size)))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / params.target_size;
	vec2 p = uv * params.render_size; // this output pixel's centre, in render pixels
	// The render was taken with the camera offset by `jitter`: the scene point at p sits at p + jitter
	// in the rendered image. Take the nearest render sample there (a point sample, not a blur) and
	// weight it by how close the scene point it saw is to p. Over the Halton sequence the samples
	// cover the output pixel and the accumulation converges to a supersampled image.
	ivec2 max_texel = ivec2(params.render_size) - 1;
	ivec2 texel = clamp(ivec2(floor(p + params.jitter)), ivec2(0), max_texel);
	vec3 current = texelFetch(color_tex, texel, 0).rgb;
	vec2 seen = vec2(texel) + 0.5 - params.jitter; // the scene point that sample saw
	vec2 d = seen - p;
	float weight = exp(-dot(d, d) / (2.0 * 0.35 * 0.35));
	vec3 lo = current;
	vec3 hi = current;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec3 c = texelFetch(color_tex, clamp(texel + ivec2(x, y), ivec2(0), max_texel), 0).rgb;
			lo = min(lo, c);
			hi = max(hi, c);
		}
	}
	vec2 velocity = texture(velocity_tex, uv).xy * params.motion_sign;
	vec2 prev_uv = uv + velocity;
	vec4 history = texture(history_tex, prev_uv);
	bool off_screen = any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)));
	float history_weight = (params.reset > 0.5 || off_screen) ? 0.0 : min(history.a, params.max_history);
	vec3 history_color = clamp(history.rgb, lo, hi);
	float total = history_weight + weight;
	vec3 result = total > 1e-6 ? (history_color * history_weight + current * weight) / total : current;
	// Depth is not needed by this reference, but the binding must stay live: the marker's uniform
	// set declares DLSS's inputs. Real depth is never negative, so this never changes the result.
	if (texture(depth_tex, uv).r < 0.0) {
		result = vec3(0.0);
	}
	imageStore(output_img, pos, vec4(result, 1.0));
	imageStore(history_img, pos, vec4(result, total));
}
)GLSL";

// Debug builds only: averages Godot's velocity texture and the filled copy (godot/velocity_fill.h) over
// a 5x5 block at the centre into a buffer, so the motion-vector tests can read them (the textures
// themselves are not copyable). result = (Godot's velocity, filled velocity).
inline constexpr const char *VELOCITY_PROBE_GLSL = R"GLSL(#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D velocity_tex;
layout(set = 0, binding = 1) uniform sampler2D filled_tex;
layout(set = 0, binding = 2, std430) restrict writeonly buffer Result {
	vec4 value;
} result;

layout(push_constant, std430) uniform Params {
	vec2 size;
	vec2 pad;
} params;

void main() {
	vec2 sum = vec2(0.0);
	vec2 filled = vec2(0.0);
	ivec2 centre = ivec2(params.size) / 2;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			sum += texelFetch(velocity_tex, centre + ivec2(x, y), 0).xy;
			filled += texelFetch(filled_tex, centre + ivec2(x, y), 0).xy;
		}
	}
	result.value = vec4(sum / 25.0, filled / 25.0);
}
)GLSL";

} // namespace lu
