#include "godot/velocity_fill.h"

#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstring>

using namespace godot;

namespace lu {
namespace {

const StringName &fill_context() {
	static const StringName name("latency_upscaling");
	return name;
}
const StringName &fill_name() {
	static const StringName name("velocity_filled");
	return name;
}

constexpr const char *VELOCITY_FILL_GLSL = R"GLSL(#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_tex;    // render size, reverse Z (0 = far plane)
layout(set = 0, binding = 1) uniform sampler2D velocity_tex; // Godot's: current -> previous, UV units
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D filled_img;

layout(push_constant, std430) uniform Params {
	mat4 reprojection; // clip -> previous clip, without jitter
	vec2 size;         // render size
	uint mode;         // 0: far-plane pixels, 1: every pixel (debug)
	uint pad;
} params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.size)))) {
		return;
	}
	float depth = texelFetch(depth_tex, pos, 0).r;
	vec2 velocity = texelFetch(velocity_tex, pos, 0).xy;
	if (depth == 0.0 || params.mode == 1u) {
		// Godot's derive_motion_vector() (shaders/effects/motion_vector_inc.glsl).
		vec2 uv = (vec2(pos) + 0.5) / params.size;
		vec4 previous = params.reprojection * vec4(uv * 2.0 - 1.0, depth, 1.0);
		velocity = previous.w > 0.0 ? previous.xy / previous.w * 0.5 + 0.5 - uv : vec2(2.0);
	}
	imageStore(filled_img, pos, vec4(velocity, 0.0, 0.0));
}
)GLSL";

struct FillParams {
	float reprojection[16]; // column-major, as GLSL's mat4
	float size[2];
	uint32_t mode;
	uint32_t pad;
};
static_assert(sizeof(FillParams) == 80, "must match the GLSL push-constant block");

} // namespace

bool VelocityFill::ensure_pipeline(std::string &error) {
	if (pipeline_.is_valid()) {
		return true;
	}
	if (failed_) {
		error = "the velocity fill pipeline could not be created";
		return false;
	}
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, VELOCITY_FILL_GLSL);
	const Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	const String compile_error =
			spirv.is_valid() ? spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE) : String("no SPIR-V");
	if (!compile_error.is_empty()) {
		failed_ = true;
		error = std::string("velocity fill shader: ") + compile_error.utf8().get_data();
		return false;
	}
	shader_ = rd->shader_create_from_spirv(spirv, "LatencyUpscaling velocity fill");
	pipeline_ = rd->compute_pipeline_create(shader_);
	Ref<RDSamplerState> state;
	state.instantiate(); // nearest, clamp: only texelFetch is used, but the uniform needs a sampler
	sampler_ = rd->sampler_create(state);
	if (!pipeline_.is_valid() || !sampler_.is_valid()) {
		failed_ = true;
		error = "the velocity fill pipeline could not be created";
		return false;
	}
	return true;
}

RID VelocityFill::run(RenderSceneBuffersRD *rb, const Projection &reprojection, VelocityFillMode mode, std::string &error) {
	const RID velocity = rb->get_velocity_texture();
	if (mode == VelocityFillMode::Off || !ensure_pipeline(error)) {
		return velocity;
	}
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	const RID depth = rb->get_depth_texture();
	const Vector2i size = rb->get_internal_size();
	// Our own texture in the render buffers. Every re-configuration (resize, quality change, the
	// takeover) frees all named textures (RenderSceneBuffersRD::configure -> cleanup(),
	// render_scene_buffers_rd.cpp:127-137 in 4.7.2), so it is created again at the new size here.
	RID filled = rb->has_texture(fill_context(), fill_name()) ? rb->get_texture(fill_context(), fill_name()) : RID();
	if (!filled.is_valid()) {
		// RGBA16F: a storage format every Vulkan GPU supports without extended-format features.
		filled = rb->create_texture(fill_context(), fill_name(), RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT,
				RenderingDevice::TEXTURE_SAMPLES_1, size, 1, 1, true, false);
	}
	if (!filled.is_valid() || !depth.is_valid() || !velocity.is_valid()) {
		error = "the velocity fill texture could not be created";
		return velocity;
	}
	// Freeing any of its textures (the re-configuration above) frees the uniform set with it
	// (RenderingDevice frees dependent uniform sets), so a valid set still names today's textures.
	if (!uniform_set_.is_valid() || !rd->uniform_set_is_valid(uniform_set_)) {
		if (uniform_set_.is_valid() && rd->uniform_set_is_valid(uniform_set_)) {
			rd->free_rid(uniform_set_);
		}
		TypedArray<RDUniform> uniforms;
		for (int binding = 0; binding < 2; ++binding) {
			Ref<RDUniform> u;
			u.instantiate();
			u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
			u->set_binding(binding);
			u->add_id(sampler_);
			u->add_id(binding == 0 ? depth : velocity);
			uniforms.push_back(u);
		}
		Ref<RDUniform> out;
		out.instantiate();
		out->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
		out->set_binding(2);
		out->add_id(filled);
		uniforms.push_back(out);
		uniform_set_ = rd->uniform_set_create(uniforms, shader_, 0);
	}
	FillParams params = {};
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			params.reprojection[c * 4 + r] = static_cast<float>(reprojection.columns[c][r]);
		}
	}
	params.size[0] = static_cast<float>(size.x);
	params.size[1] = static_cast<float>(size.y);
	params.mode = mode == VelocityFillMode::AllPixels ? 1u : 0u;
	PackedByteArray push;
	push.resize(sizeof(params));
	std::memcpy(push.ptrw(), &params, sizeof(params));
	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uniform_set_, 0);
	rd->compute_list_set_push_constant(list, push, static_cast<uint32_t>(push.size()));
	rd->compute_list_dispatch(list, static_cast<uint32_t>((size.x + 7) / 8), static_cast<uint32_t>((size.y + 7) / 8), 1);
	rd->compute_list_end();
	return filled;
}

void VelocityFill::release() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) {
		return;
	}
	if (uniform_set_.is_valid() && rd->uniform_set_is_valid(uniform_set_)) {
		rd->free_rid(uniform_set_);
	}
	uniform_set_ = RID();
	for (RID *rid : { &pipeline_, &shader_, &sampler_ }) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
			*rid = RID();
		}
	}
}

} // namespace lu
