#include "godot/upscaler.h"

#include "core/log.h"
#include "dlss/jitter.h"
#include "godot/bootstrap.h"
#include "godot/reference_upscaler_glsl.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/render_scene_buffers_configuration.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cmath>
#include <cstring>

using namespace godot;

namespace lu {
namespace {

const StringName &buffers_context() {
	static const StringName name("render_buffers");
	return name;
}
const StringName &upscaled_name() {
	static const StringName name("color_upscaled");
	return name;
}

// Scene UBO offsets in Godot 4.7.2 (float build, MAX_RENDER_VIEWS 2): RenderSceneDataRD::UBO,
// render_scene_data_rd.h:118-184. taa_frame_count is Godot's frame_number % phase_count.
constexpr uint32_t UBO_TAA_FRAME_COUNT_OFFSET = 2820;
constexpr uint32_t UBO_TAA_JITTER_OFFSET = 2824;

// Godot's geometric conventions, proven by tests/upscaler_tests.gd: a projection jitter of +j
// render pixels (texture orientation, Y down) moves the rendered content by +j, and velocity is
// the current-to-previous UV delta (prev_uv = uv + velocity).
constexpr float JITTER_SIGN_X = 1.0f;
constexpr float JITTER_SIGN_Y = 1.0f;

bool effect_in(const Ref<Compositor> &compositor, const Ref<LatencyUpscalingEffect> &effect) {
	if (compositor.is_null()) {
		return false;
	}
	const TypedArray<CompositorEffect> effects = compositor->get_compositor_effects();
	for (int i = 0; i < effects.size(); ++i) {
		if (Object::cast_to<LatencyUpscalingEffect>(effects[i]) == effect.ptr()) {
			return true;
		}
	}
	return false;
}

// A camera's own compositor replaces the scenario's (WorldEnvironment) one, so when the camera has
// none, the world environment's effects must be carried over into the merged compositor.
Ref<Compositor> world_environment_compositor(Window *root) {
	const TypedArray<Node> found = root->find_children("*", "WorldEnvironment", true, false);
	for (int i = 0; i < found.size(); ++i) {
		WorldEnvironment *env = Object::cast_to<WorldEnvironment>(found[i]);
		if (env != nullptr && env->get_compositor().is_valid()) {
			return env->get_compositor();
		}
	}
	return Ref<Compositor>();
}

} // namespace

// --- LatencyUpscalingEffect ----------------------------------------------------------------------

LatencyUpscalingEffect::LatencyUpscalingEffect() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_needs_motion_vectors(true);
}

void LatencyUpscalingEffect::_render_callback(int32_t p_effect_callback_type, RenderData *p_render_data) {
	if (p_effect_callback_type == EFFECT_CALLBACK_TYPE_POST_TRANSPARENT && p_render_data != nullptr) {
		Upscaler::get().render(p_render_data);
	}
}

// --- Upscaler: main thread -----------------------------------------------------------------------

const char *upscale_backend_name(UpscaleBackend backend) {
	switch (backend) {
		case UpscaleBackend::None:
			return "none";
		case UpscaleBackend::Reference:
			return "reference";
		case UpscaleBackend::Dlss:
			return "dlss";
	}
	return "unknown";
}

Upscaler &Upscaler::get() {
	static Upscaler *instance = new Upscaler(); // never destroyed: render-thread callbacks may outlive SCENE deinit
	return *instance;
}

void Upscaler::configure(UpscaleBackend backend, const std::string &reason, upscale::Quality quality,
		upscale::Preset preset, bool enabled) {
	backend_ = backend;
	backend_reason_ = reason;
	quality_ = quality;
	preset_ = preset;
	enabled_ = enabled;
	if (backend_ != UpscaleBackend::None && effect_.is_null()) {
		effect_.instantiate();
	}
}

void Upscaler::set_enabled(bool enabled) {
	enabled_ = enabled;
}

void Upscaler::set_quality(upscale::Quality quality) {
	quality_ = quality;
}

void Upscaler::set_preset(upscale::Preset preset) {
	preset_ = preset; // consumed by the DLSS backend (NGX render-preset hints)
}

bool Upscaler::is_active() const {
	return applied_ && inactive_reason_.empty();
}

std::string Upscaler::reason() const {
	if (backend_ == UpscaleBackend::None) {
		return backend_reason_;
	}
	return inactive_reason_;
}

void Upscaler::update() {
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	Window *root = tree != nullptr ? tree->get_root() : nullptr;
	if (root == nullptr || Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (backend_ == UpscaleBackend::None || !enabled_) {
		inactive_reason_ = backend_ == UpscaleBackend::None ? backend_reason_ : "disabled (dlss/enabled is off)";
		if (applied_) {
			restore_viewport_and_camera();
		}
		return;
	}
	// Auto picks the mode from the output size (NVIDIA's table): below 1920x1080 it means "off".
	Vector2i target = root->get_size();
	{
		std::lock_guard<std::mutex> lock(stats_mutex_);
		if (stats_.target_width > 0) {
			target = Vector2i(stats_.target_width, stats_.target_height);
		}
	}
	upscale::Quality mode = quality_;
	if (mode == upscale::Quality::Auto &&
			!upscale::resolve_auto(upscale::Size{ static_cast<uint32_t>(target.x), static_cast<uint32_t>(target.y) }, mode)) {
		inactive_reason_ = "Auto: output below 1920x1080, where NVIDIA recommends DLSS off";
		if (applied_) {
			restore_viewport_and_camera();
		}
		return;
	}
	resolved_quality_ = mode;
	inactive_reason_.clear();

	// 1. The viewport runs in the FSR2 slot at the mode's scale.
	const float scale = upscale::render_scale(mode);
	if (!applied_) {
		saved_scaling_mode_ = root->get_scaling_3d_mode();
		saved_scaling_scale_ = root->get_scaling_3d_scale();
		applied_ = true;
	}
	if (root->get_scaling_3d_mode() != Viewport::SCALING_3D_MODE_FSR2) {
		root->set_scaling_3d_mode(Viewport::SCALING_3D_MODE_FSR2);
	}
	if (std::fabs(root->get_scaling_3d_scale() - scale) > 1e-4f) {
		root->set_scaling_3d_scale(scale);
	}
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		snapshot_.render_target = RenderingServer::get_singleton()->viewport_get_render_target(root->get_viewport_rid()).get_id();
		// renderer_viewport.cpp: log2(min(scale, 1)) + the viewport's own bias.
		snapshot_.mipmap_bias = std::log2(std::fmin(scale, 1.0f)) + root->get_texture_mipmap_bias();
		snapshot_.anisotropic_filtering = static_cast<int>(root->get_anisotropic_filtering_level());
	}

	// 2. The current camera carries our effect, merged with the effects it already had.
	Camera3D *camera = root->get_camera_3d();
	if (camera != nullptr && !effect_in(camera->get_compositor(), effect_)) {
		if (camera->get_instance_id() != camera_id_) {
			Object *previous = ObjectDB::get_instance(camera_id_);
			if (Camera3D *old = Object::cast_to<Camera3D>(previous)) {
				old->set_compositor(camera_original_compositor_);
			}
			camera_id_ = camera->get_instance_id();
			camera_original_compositor_ = camera->get_compositor();
		}
		Ref<Compositor> base = camera->get_compositor();
		if (base.is_null()) {
			base = world_environment_compositor(root);
		}
		TypedArray<CompositorEffect> effects;
		if (base.is_valid()) {
			effects = base->get_compositor_effects().duplicate();
		}
		effects.push_back(effect_);
		Ref<Compositor> merged;
		merged.instantiate();
		merged->set_compositor_effects(effects);
		camera->set_compositor(merged); // runtime only: never saved, restored on shutdown
	}
}

void Upscaler::restore_viewport_and_camera() {
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	Window *root = tree != nullptr ? tree->get_root() : nullptr;
	if (root != nullptr && applied_) {
		root->set_scaling_3d_mode(static_cast<Viewport::Scaling3DMode>(saved_scaling_mode_));
		root->set_scaling_3d_scale(saved_scaling_scale_);
	}
	if (Camera3D *camera = Object::cast_to<Camera3D>(ObjectDB::get_instance(camera_id_))) {
		camera->set_compositor(camera_original_compositor_);
	}
	camera_id_ = 0;
	camera_original_compositor_ = Ref<Compositor>();
	applied_ = false;
	std::lock_guard<std::mutex> lock(snapshot_mutex_);
	snapshot_.render_target = 0;
}

void Upscaler::shutdown() {
	if (applied_) {
		restore_viewport_and_camera();
	}
	effect_.unref(); // the camera no longer references it either
}

Dictionary Upscaler::status() const {
	Dictionary d;
	d["backend"] = upscale_backend_name(backend_);
	d["enabled"] = enabled_;
	d["active"] = is_active();
	d["reason"] = String::utf8(reason().c_str());
	d["quality"] = upscale::quality_name(quality_);
	d["resolved_quality"] = is_active() ? upscale::quality_name(resolved_quality_) : "";
	d["preset"] = upscale::preset_name(preset_);
	std::lock_guard<std::mutex> lock(stats_mutex_);
	d["frames_upscaled"] = static_cast<int64_t>(stats_.frames_upscaled);
	d["fsr2_frames"] = static_cast<int64_t>(stats_.fsr2_frames);
	d["takeovers"] = static_cast<int64_t>(stats_.takeovers);
	d["render_size"] = Vector2i(stats_.render_width, stats_.render_height);
	d["target_size"] = Vector2i(stats_.target_width, stats_.target_height);
	Dictionary jitter;
	jitter["phase_count"] = stats_.phase_count;
	jitter["calibrated_phase"] = stats_.calibrated_phase;
	jitter["checks"] = static_cast<int64_t>(stats_.jitter_checks);
	jitter["mismatches"] = static_cast<int64_t>(stats_.jitter_mismatches);
	jitter["last_pixels"] = Vector2(stats_.last_jitter_x, stats_.last_jitter_y);
	jitter["ubo_checks"] = static_cast<int64_t>(stats_.ubo_checks);
	jitter["ubo_mismatches"] = static_cast<int64_t>(stats_.ubo_mismatches);
	d["jitter"] = jitter;
	d["velocity_samples"] = stats_.velocity_samples.duplicate();
	d["error"] = String::utf8(stats_.error.c_str());
	return d;
}

// --- Upscaler: render thread ---------------------------------------------------------------------

void Upscaler::set_error(const std::string &error) {
	std::lock_guard<std::mutex> lock(stats_mutex_);
	if (stats_.error.empty()) {
		stats_.error = error;
		log::warning("upscaler: %s", error.c_str());
	}
}

void Upscaler::render(RenderData *data) {
	Ref<RenderSceneBuffersRD> rb = data->get_render_scene_buffers();
	if (rb.is_null()) {
		return;
	}
	uint64_t managed;
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		managed = snapshot_.render_target;
	}
	const uint64_t rt = rb->get_render_target().get_id();
	if (managed == 0 || rt != managed || rb->get_view_count() != 1) {
		return; // not the main viewport (or multiview, which is not supported)
	}
	const RenderingServer::ViewportScaling3DMode mode = rb->get_scaling_3d_mode();
	if (mode == RenderingServer::VIEWPORT_SCALING_3D_MODE_FSR2) {
		// Godot (re)configured the buffers: FSR2 runs this frame; take them over behind it.
		pending_[rt] = rb;
		takeover_pending_ = true;
		std::lock_guard<std::mutex> lock(stats_mutex_);
		stats_.fsr2_frames++;
		return;
	}
	if (mode != RenderingServer::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL || !rb->has_texture(buffers_context(), upscaled_name())) {
		return;
	}
	if (backend_ == UpscaleBackend::Reference) {
		dispatch_reference(rb.ptr(), data->get_render_scene_data(), views_[rt]);
	}
}

void Upscaler::run_takeovers() {
	takeover_pending_ = false;
	Snapshot snap;
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		snap = snapshot_;
	}
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	for (auto &[rt, rb] : pending_) {
		if (rb.is_null() || rb->get_scaling_3d_mode() != RenderingServer::VIEWPORT_SCALING_3D_MODE_FSR2) {
			continue;
		}
		// Identical configuration, but "no upscaler" (METALFX_TEMPORAL maps to SCALE_NONE on
		// Vulkan, render_forward_clustered.cpp:1789-1800). Not exposed and so lost: use_debanding.
		Ref<RenderSceneBuffersConfiguration> config;
		config.instantiate();
		config->set_render_target(rb->get_render_target());
		config->set_internal_size(rb->get_internal_size());
		config->set_target_size(rb->get_target_size());
		config->set_view_count(rb->get_view_count());
		config->set_scaling_3d_mode(RenderingServer::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL);
		config->set_msaa_3d(rb->get_msaa_3d());
		config->set_screen_space_aa(rb->get_screen_space_aa());
		config->set_fsr_sharpness(rb->get_fsr_sharpness());
		config->set_texture_mipmap_bias(snap.mipmap_bias);
		config->set_anisotropic_filtering_level(static_cast<RenderingServer::ViewportAnisotropicFiltering>(snap.anisotropic_filtering));
		rb->configure(config);
		// The target-size output, as RenderSceneBuffersRD::ensure_upscaled() would create it.
		const Ref<RDTextureFormat> color_format = rd->texture_get_format(rb->get_color_texture());
		const uint32_t usage = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_INPUT_ATTACHMENT_BIT;
		rb->create_texture(buffers_context(), upscaled_name(), color_format->get_format(), usage,
				RenderingDevice::TEXTURE_SAMPLES_1, rb->get_target_size(), 1, 1, true, false);
		views_[rt].reset = true;
		std::lock_guard<std::mutex> lock(stats_mutex_);
		stats_.takeovers++;
	}
	pending_.clear();
}

bool Upscaler::ensure_pipeline() {
	if (pipeline_.is_valid() || pipeline_failed_) {
		return pipeline_.is_valid();
	}
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, REFERENCE_UPSCALER_GLSL);
	const Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	const String error = spirv.is_valid() ? spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE) : String("no SPIR-V");
	if (!error.is_empty()) {
		pipeline_failed_ = true;
		set_error(std::string("reference upscaler shader: ") + error.utf8().get_data());
		return false;
	}
	shader_ = rd->shader_create_from_spirv(spirv, "LatencyUpscaling reference upscaler");
	pipeline_ = rd->compute_pipeline_create(shader_);
	Ref<RDSamplerState> sampler;
	sampler.instantiate();
	sampler->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_ = rd->sampler_create(sampler);
	if (!pipeline_.is_valid() || !sampler_.is_valid()) {
		pipeline_failed_ = true;
		set_error("reference upscaler pipeline could not be created");
		return false;
	}
	return true;
}

void Upscaler::track_jitter(RenderSceneData *scene, ViewState &view, int render_width, int target_width,
		int render_height, float &jitter_x, float &jitter_y) {
	const Projection projection = scene->get_cam_projection();
	float columns[4][4];
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			columns[c][r] = static_cast<float>(projection.columns[c][r]);
		}
	}
	const dlss::JitterPixels observed =
			dlss::ndc_to_pixels(dlss::jitter_ndc_from_projection(columns), static_cast<uint32_t>(render_width),
					static_cast<uint32_t>(render_height));
	jitter_x = observed.x;
	jitter_y = observed.y;
	// CPU reproduction of Godot's Halton sequence, calibrated once against the projection.
	const uint32_t phase_count = dlss::jitter_phase_count(static_cast<uint32_t>(target_width),
			static_cast<uint32_t>(render_width), true, false);
	std::lock_guard<std::mutex> lock(stats_mutex_);
	stats_.phase_count = phase_count;
	stats_.last_jitter_x = observed.x;
	stats_.last_jitter_y = observed.y;
	if (phase_count == 0) {
		return;
	}
	if (!view.calibrated) {
		const int phase = dlss::find_phase(observed, phase_count);
		if (phase >= 0) {
			view.calibrated = true;
			view.phase_offset = static_cast<int64_t>(phase) - static_cast<int64_t>(view.frames % phase_count);
			stats_.calibrated_phase = phase;
		}
		return;
	}
	const uint64_t godot_phase =
			static_cast<uint64_t>((static_cast<int64_t>(view.frames % phase_count) + view.phase_offset + phase_count) % phase_count);
	const dlss::JitterPixels predicted = dlss::jitter_in_pixels(godot_phase, phase_count);
	stats_.jitter_checks++;
	if (std::fabs(predicted.x - observed.x) > 1e-3f || std::fabs(predicted.y - observed.y) > 1e-3f) {
		stats_.jitter_mismatches++;
		view.calibrated = false; // a skipped frame shifts the phase: calibrate again
	}
}

void Upscaler::dispatch_reference(RenderSceneBuffersRD *rb, RenderSceneData *scene, ViewState &view) {
	if (scene == nullptr || !ensure_pipeline()) {
		return;
	}
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	const RID color = rb->get_color_texture();
	const RID depth = rb->get_depth_texture();
	const RID velocity = rb->get_velocity_texture();
	const RID output = rb->get_texture(buffers_context(), upscaled_name());
	const Vector2i internal = rb->get_internal_size();
	const Vector2i target = rb->get_target_size();
	if (!color.is_valid() || !depth.is_valid() || !velocity.is_valid() || !output.is_valid()) {
		set_error("render buffers lack colour, depth, velocity or color_upscaled");
		return;
	}

	// History ping-pong at target size.
	if (view.history_size != target || !view.history[0].is_valid()) {
		for (RID &h : view.history) {
			if (h.is_valid()) {
				rd->free_rid(h);
			}
			Ref<RDTextureFormat> format;
			format.instantiate();
			format->set_format(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT);
			format->set_width(target.x);
			format->set_height(target.y);
			format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT);
			h = rd->texture_create(format, Ref<RDTextureView>(memnew(RDTextureView)));
		}
		view.history_size = target;
		view.reset = true;
		view.inputs_key[0] = 0; // rebuild the uniform sets
	}
	const uint64_t key[4] = { color.get_id(), depth.get_id(), velocity.get_id(), output.get_id() };
	if (std::memcmp(key, view.inputs_key, sizeof(key)) != 0) {
		for (int i = 0; i < 2; ++i) {
			if (view.uniform_set[i].is_valid() && rd->uniform_set_is_valid(view.uniform_set[i])) {
				rd->free_rid(view.uniform_set[i]);
			}
			auto sampled = [&](int binding, const RID &texture) {
				Ref<RDUniform> u;
				u.instantiate();
				u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
				u->set_binding(binding);
				u->add_id(sampler_);
				u->add_id(texture);
				return u;
			};
			auto storage = [&](int binding, const RID &texture) {
				Ref<RDUniform> u;
				u.instantiate();
				u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
				u->set_binding(binding);
				u->add_id(texture);
				return u;
			};
			TypedArray<RDUniform> uniforms;
			uniforms.push_back(sampled(0, color));
			uniforms.push_back(sampled(1, depth));
			uniforms.push_back(sampled(2, velocity));
			uniforms.push_back(sampled(3, view.history[i])); // read
			uniforms.push_back(storage(4, output));
			uniforms.push_back(storage(5, view.history[1 - i])); // write
			view.uniform_set[i] = rd->uniform_set_create(uniforms, shader_, 0);
		}
		std::memcpy(view.inputs_key, key, sizeof(key));
	}

	float jitter_x = 0.0f, jitter_y = 0.0f;
	track_jitter(scene, view, internal.x, target.x, internal.y, jitter_x, jitter_y);
	Snapshot snap;
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		snap = snapshot_;
	}
	// Godot's conventions (JITTER_SIGN_*, velocity as-is); the tests can deliberately change them
	// (set_test_conventions, debug builds) to show that wrong conventions measurably look worse.
	const float jitter_sign_x = JITTER_SIGN_X * snap.jitter_sign_x;
	const float jitter_sign_y = JITTER_SIGN_Y * snap.jitter_sign_y;
	const float motion_sign = snap.motion_sign;
	const float max_history = snap.max_history;
	if (reset_requested_.exchange(false)) {
		view.reset = true;
	}
	const float params[12] = { static_cast<float>(internal.x), static_cast<float>(internal.y), static_cast<float>(target.x),
		static_cast<float>(target.y), jitter_x * jitter_sign_x, jitter_y * jitter_sign_y, motion_sign, motion_sign, max_history,
		view.reset ? 1.0f : 0.0f, 0.0f, 0.0f };
	PackedByteArray push;
	push.resize(sizeof(params));
	std::memcpy(push.ptrw(), params, sizeof(params));

	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, view.uniform_set[view.read], 0);
	rd->compute_list_set_push_constant(list, push, static_cast<uint32_t>(push.size()));
	rd->compute_list_dispatch(list, static_cast<uint32_t>((target.x + 7) / 8), static_cast<uint32_t>((target.y + 7) / 8), 1);
	rd->compute_list_end();

#ifdef DEBUG_ENABLED
	const uint32_t phase_count = dlss::jitter_phase_count(static_cast<uint32_t>(target.x), static_cast<uint32_t>(internal.x), true, false);
	const uint32_t phase = view.calibrated && phase_count > 0 ?
			static_cast<uint32_t>((static_cast<int64_t>(view.frames % phase_count) + view.phase_offset + phase_count) % phase_count) :
			UINT32_MAX;
	debug_readbacks(scene, rb, jitter_x, jitter_y, phase, phase_count);
#endif

	view.read = 1 - view.read;
	view.reset = false;
	view.frames++;
	std::lock_guard<std::mutex> lock(stats_mutex_);
	stats_.frames_upscaled++;
	stats_.render_width = internal.x;
	stats_.render_height = internal.y;
	stats_.target_width = target.x;
	stats_.target_height = target.y;
}

// Debug builds: for N frames (GODOT_LATENCY_UPSCALING_TEST_READBACK_FRAMES=N or
// request_readbacks()), read back the scene UBO (Godot's own taa_jitter and taa_frame_count) and
// the velocity at the centre of the screen.
void Upscaler::debug_readbacks(RenderSceneData *scene, RenderSceneBuffersRD *rb, float jitter_x, float jitter_y,
		uint32_t phase, uint32_t phase_count) {
	int frames = readback_frames_.load();
	if (frames < 0) {
		frames = boot_state().test_readback_frames;
		readback_frames_ = frames;
	}
	if (frames <= 0) {
		return;
	}
	readback_frames_ = frames - 1;
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	const RID ubo = scene->get_uniform_buffer();
	if (ubo.is_valid() && phase != UINT32_MAX) {
		rd->buffer_get_data_async(ubo,
				callable_mp(effect_.ptr(), &LatencyUpscalingEffect::on_ubo_readback)
						.bind(jitter_x, jitter_y, static_cast<int64_t>(phase), static_cast<int64_t>(phase_count)),
				UBO_TAA_FRAME_COUNT_OFFSET, 12);
	}
	const RID velocity = rb->get_velocity_texture();
	if (!ensure_velocity_probe() || !velocity.is_valid()) {
		return;
	}
	if (probe_velocity_ != velocity.get_id() || !rd->uniform_set_is_valid(probe_set_)) {
		if (probe_set_.is_valid() && rd->uniform_set_is_valid(probe_set_)) {
			rd->free_rid(probe_set_);
		}
		TypedArray<RDUniform> uniforms;
		Ref<RDUniform> tex;
		tex.instantiate();
		tex->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		tex->set_binding(0);
		tex->add_id(sampler_);
		tex->add_id(velocity);
		uniforms.push_back(tex);
		Ref<RDUniform> buf;
		buf.instantiate();
		buf->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		buf->set_binding(1);
		buf->add_id(probe_buffer_);
		uniforms.push_back(buf);
		probe_set_ = rd->uniform_set_create(uniforms, probe_shader_, 0);
		probe_velocity_ = velocity.get_id();
	}
	const Vector2i internal = rb->get_internal_size();
	const float params[4] = { static_cast<float>(internal.x), static_cast<float>(internal.y), 0.0f, 0.0f };
	PackedByteArray push;
	push.resize(sizeof(params));
	std::memcpy(push.ptrw(), params, sizeof(params));
	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, probe_pipeline_);
	rd->compute_list_bind_uniform_set(list, probe_set_, 0);
	rd->compute_list_set_push_constant(list, push, static_cast<uint32_t>(push.size()));
	rd->compute_list_dispatch(list, 1, 1, 1);
	rd->compute_list_end();
	rd->buffer_get_data_async(probe_buffer_, callable_mp(effect_.ptr(), &LatencyUpscalingEffect::on_velocity_readback), 0, 16);
}

bool Upscaler::ensure_velocity_probe() {
	if (probe_pipeline_.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, VELOCITY_PROBE_GLSL);
	const Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null() || !spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE).is_empty()) {
		set_error("velocity probe shader failed to compile");
		return false;
	}
	probe_shader_ = rd->shader_create_from_spirv(spirv, "LatencyUpscaling velocity probe");
	probe_pipeline_ = rd->compute_pipeline_create(probe_shader_);
	PackedByteArray zero;
	zero.resize(16);
	zero.fill(0);
	probe_buffer_ = rd->storage_buffer_create(16, zero);
	return probe_pipeline_.is_valid() && probe_buffer_.is_valid();
}

void Upscaler::on_ubo_readback(const PackedByteArray &data, float expected_x, float expected_y, uint32_t expected_phase,
		uint32_t phase_count) {
	if (data.size() < 12) {
		return;
	}
	float values[3];
	std::memcpy(values, data.ptr(), sizeof(values)); // taa_frame_count, taa_jitter.x, taa_jitter.y (NDC)
	std::lock_guard<std::mutex> lock(stats_mutex_);
	stats_.ubo_checks++;
	const float ubo_x = values[1] * static_cast<float>(stats_.render_width) * 0.5f;
	const float ubo_y = values[2] * static_cast<float>(stats_.render_height) * 0.5f;
	const bool jitter_ok = std::fabs(ubo_x - expected_x) < 1e-3f && std::fabs(ubo_y - expected_y) < 1e-3f;
	const bool phase_ok = static_cast<uint32_t>(std::lround(values[0])) == expected_phase && phase_count > 0;
	if (!jitter_ok || !phase_ok) {
		stats_.ubo_mismatches++;
	}
}

void Upscaler::on_velocity_readback(const PackedByteArray &data) {
	if (data.size() < 8) {
		return;
	}
	float v[2];
	std::memcpy(v, data.ptr(), sizeof(v));
	std::lock_guard<std::mutex> lock(stats_mutex_);
	stats_.velocity_samples.push_back(Vector2(v[0], v[1]));
	if (stats_.velocity_samples.size() > 256) {
		stats_.velocity_samples.pop_front();
	}
}

void Upscaler::set_test_conventions(float jitter_x, float jitter_y, float motion) {
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		snapshot_.jitter_sign_x = jitter_x;
		snapshot_.jitter_sign_y = jitter_y;
		snapshot_.motion_sign = motion;
	}
	reset_requested_ = true;
}

void Upscaler::release_gpu_resources() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) {
		return;
	}
	for (auto &[rt, view] : views_) {
		for (RID &set : view.uniform_set) {
			if (set.is_valid() && rd->uniform_set_is_valid(set)) {
				rd->free_rid(set);
			}
		}
		for (RID &h : view.history) {
			if (h.is_valid()) {
				rd->free_rid(h);
			}
		}
	}
	views_.clear();
	pending_.clear();
	if (probe_set_.is_valid() && rd->uniform_set_is_valid(probe_set_)) {
		rd->free_rid(probe_set_);
	}
	probe_set_ = RID();
	for (RID *rid : { &probe_pipeline_, &probe_shader_, &probe_buffer_, &pipeline_, &shader_, &sampler_ }) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
			*rid = RID();
		}
	}
}

void LatencyUpscalingEffect::on_ubo_readback(const PackedByteArray &data, float x, float y, int64_t phase, int64_t count) {
	Upscaler::get().on_ubo_readback(data, x, y, static_cast<uint32_t>(phase), static_cast<uint32_t>(count));
}

void LatencyUpscalingEffect::on_velocity_readback(const PackedByteArray &data) {
	Upscaler::get().on_velocity_readback(data);
}

} // namespace lu
