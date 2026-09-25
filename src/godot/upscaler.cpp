#include "godot/upscaler.h"

#include "core/log.h"
#include "dlss/dlss_feature.h"
#include "dlss/jitter.h"
#include "godot/bootstrap.h"
#include "godot/reference_upscaler_glsl.h"
#include "upscale/image_usage.h"
#include "upscale/reference_params.h"
#include "upscale/upscale_feature.h"
#include "vk/dispatch.h"

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

upscale::CameraPose camera_pose(const Transform3D &transform) {
	upscale::CameraPose pose;
	for (int c = 0; c < 3; ++c) {
		const Vector3 axis = transform.basis.get_column(c);
		pose.basis[c][0] = axis.x;
		pose.basis[c][1] = axis.y;
		pose.basis[c][2] = axis.z;
	}
	pose.origin[0] = transform.origin.x;
	pose.origin[1] = transform.origin.y;
	pose.origin[2] = transform.origin.z;
	return pose;
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

const char *Upscaler::reset_reason_name(ResetReason reason) {
	switch (reason) {
		case ResetReason::Takeover:
			return "takeover";
		case ResetReason::Resize:
			return "resize";
		case ResetReason::Api:
			return "api";
		case ResetReason::Test:
			return "test";
		case ResetReason::CameraSwitch:
			return "camera_switch";
		case ResetReason::Rotation:
			return "rotation";
		case ResetReason::Translation:
			return "translation";
		case ResetReason::InvalidPose:
			return "invalid_pose";
		case ResetReason::Count:
			break;
	}
	return "unknown";
}

Upscaler &Upscaler::get() {
	static Upscaler *instance = new Upscaler(); // never destroyed: render-thread callbacks may outlive SCENE deinit
	return *instance;
}

void Upscaler::configure(UpscaleBackend backend, const std::string &reason, upscale::Quality quality,
		upscale::Preset preset, bool enabled, bool native) {
	backend_ = backend;
	native_requested_ = native || backend == UpscaleBackend::Dlss; // DLSS records at the marker too
	dlss_provider_ = backend == UpscaleBackend::Dlss ? dlss::DlssFeature::get().backend() : nullptr;
	dlss_recorder_.set_provider(dlss_provider_);
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

void Upscaler::set_cut_thresholds(const upscale::CutThresholds &thresholds) {
	std::lock_guard<std::mutex> lock(snapshot_mutex_);
	snapshot_.cut_thresholds = thresholds;
}

void Upscaler::set_test_velocity_fill(int mode) {
	std::lock_guard<std::mutex> lock(snapshot_mutex_);
	snapshot_.velocity_fill = mode == 1 ? VelocityFillMode::AllPixels : mode == 2 ? VelocityFillMode::Off : VelocityFillMode::FarPlane;
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

	// 1. The viewport runs in the FSR2 slot at the mode's scale. With DLSS, the scale that makes
	// Godot's render size (target * scale, truncated: renderer_viewport.cpp) exactly the render size
	// DLSS asks for; otherwise NVIDIA's ratio.
	float scale = upscale::render_scale(mode);
	dlss::ImageSize optimal;
	if (dlss_provider_ != nullptr && target.x > 0 &&
			dlss_provider_->optimal_render_size(dlss::ImageSize{ static_cast<uint32_t>(target.x), static_cast<uint32_t>(target.y) },
					mode, optimal) &&
			optimal.width > 0) {
		// DLAA: exactly 1 (above 1 Godot leaves the FSR2 slot for bilinear downsampling).
		scale = optimal.width >= static_cast<uint32_t>(target.x) ? 1.0f :
																  (static_cast<float>(optimal.width) + 0.5f) / static_cast<float>(target.x);
	}
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
		snapshot_.quality = resolved_quality_;
		snapshot_.preset = preset_;
	}

	// 2. The current camera carries our effect (normally done by pre_draw() already).
	track_camera(root);
}

void Upscaler::pre_draw() {
	if (!applied_ || backend_ == UpscaleBackend::None || !enabled_ || !inactive_reason_.empty()) {
		return;
	}
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (Window *root = tree != nullptr ? tree->get_root() : nullptr) {
		track_camera(root);
	}
}

// The current camera carries our effect, merged with the effects it already had. Called before the
// frame renders (frame_pre_draw), so a camera made current during this frame's processing is upscaled
// from its first frame on, and the switch reaches the render thread in the same frame: a switch is
// a camera cut (the history of another view must not be blended into it).
void Upscaler::track_camera(Window *root) {
	Camera3D *camera = root->get_camera_3d();
	if (camera == nullptr) {
		return;
	}
	if (camera->get_instance_id() != camera_id_) {
		if (camera_id_ != 0) {
			std::lock_guard<std::mutex> lock(snapshot_mutex_);
			snapshot_.camera_serial++;
		}
		Object *previous = ObjectDB::get_instance(camera_id_);
		if (Camera3D *old = Object::cast_to<Camera3D>(previous)) {
			old->set_compositor(camera_original_compositor_);
		}
		camera_id_ = camera->get_instance_id();
		camera_original_compositor_ = camera->get_compositor();
	}
	if (!effect_in(camera->get_compositor(), effect_)) {
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
	d["frames_since_reset"] = static_cast<int64_t>(stats_.frames_since_reset);
	d["render_size"] = Vector2i(stats_.render_width, stats_.render_height);
	d["target_size"] = Vector2i(stats_.target_width, stats_.target_height);
	Dictionary jitter;
	jitter["phase_count"] = stats_.phase_count;
	jitter["calibrated_phase"] = stats_.calibrated_phase;
	jitter["checks"] = static_cast<int64_t>(stats_.jitter_checks);
	jitter["mismatches"] = static_cast<int64_t>(stats_.jitter_mismatches);
	jitter["last_pixels"] = Vector2(stats_.last_jitter_x, stats_.last_jitter_y);
	jitter["phase"] = stats_.phase;
	jitter["ubo_checks"] = static_cast<int64_t>(stats_.ubo_checks);
	jitter["ubo_mismatches"] = static_cast<int64_t>(stats_.ubo_mismatches);
	d["jitter"] = jitter;
	d["velocity_samples"] = stats_.velocity_samples.duplicate();
	d["filled_velocity_samples"] = stats_.filled_velocity_samples.duplicate();
	Dictionary resets;
	for (int i = 0; i < static_cast<int>(ResetReason::Count); ++i) {
		resets[reset_reason_name(static_cast<ResetReason>(i))] = static_cast<int64_t>(stats_.resets[i]);
	}
	d["history_resets"] = resets;
	d["last_reset_reason"] = String::utf8(stats_.last_reset_reason.c_str());
	d["error"] = String::utf8(stats_.error.c_str());
	Dictionary native;
	native["requested"] = native_requested_;
	native["active"] = marker_.load() != 0;
	native["error"] = String::utf8(native_error_.c_str());
	native["command_hooks"] = vk::command_interception_active();
	const vk::InterceptorStats intercepted = vk::interceptor_stats();
	native["marker_dispatches"] = static_cast<int64_t>(intercepted.marker_dispatches);
	native["substituted"] = static_cast<int64_t>(intercepted.substituted);
	native["declined"] = static_cast<int64_t>(intercepted.declined);
	native["untracked"] = static_cast<int64_t>(intercepted.untracked);
	const upscale::NativeRecorderStats recorder = upscale::UpscaleFeature::get().reference_recorder().stats();
	native["recorded"] = static_cast<int64_t>(recorder.recorded);
	native["missing_job"] = static_cast<int64_t>(recorder.missing_job);
	native["not_created"] = static_cast<int64_t>(recorder.not_created);
	d["native"] = native;
	const dlss::DlssRecorderStats evaluated = dlss_recorder_.stats();
	Dictionary dlss_frames;
	dlss_frames["evaluated"] = static_cast<int64_t>(evaluated.evaluated);
	dlss_frames["missing_job"] = static_cast<int64_t>(evaluated.missing_job);
	dlss_frames["failed"] = static_cast<int64_t>(evaluated.failed);
	dlss_frames["last_error"] = String::utf8(evaluated.last_error.c_str());
	if (dlss_provider_ != nullptr) {
		dlss_frames["runtime"] = String::utf8(dlss_provider_->version().c_str());
		dlss::ImageSize optimal;
		if (stats_.target_width > 0 &&
				dlss_provider_->optimal_render_size(dlss::ImageSize{ static_cast<uint32_t>(stats_.target_width),
															static_cast<uint32_t>(stats_.target_height) },
						resolved_quality_, optimal)) {
			dlss_frames["optimal_render_size"] = Vector2i(static_cast<int>(optimal.width), static_cast<int>(optimal.height));
		}
	}
	d["dlss_frames"] = dlss_frames;
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
	if (backend_ == UpscaleBackend::Reference || backend_ == UpscaleBackend::Dlss) {
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
		// The target-size output, as RenderSceneBuffersRD::ensure_upscaled() would create it, plus
		// copy-to (VK_IMAGE_USAGE_TRANSFER_DST_BIT): DLSS clears its output with vkCmdClearColorImage
		// inside the evaluation (VUID-vkCmdClearColorImage-image-00002, seen on the RTX 5080).
		const Ref<RDTextureFormat> color_format = rd->texture_get_format(rb->get_color_texture());
		const uint32_t usage = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_INPUT_ATTACHMENT_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		rb->create_texture(buffers_context(), upscaled_name(), color_format->get_format(), usage,
				RenderingDevice::TEXTURE_SAMPLES_1, rb->get_target_size(), 1, 1, true, false);
		views_[rt].reset = true;
		views_[rt].reset_reason = ResetReason::Takeover;
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
	const PackedByteArray bytecode = spirv->get_stage_bytecode(RenderingDevice::SHADER_STAGE_COMPUTE);
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
	if (backend_ == UpscaleBackend::Dlss) {
		ensure_dlss_marker();
	} else if (native_requested_) {
		ensure_native(bytecode);
	}
	return true;
}

// DLSS: the reference pipeline is the marker; its dispatch is replaced by DLSS's evaluation. When
// DLSS cannot evaluate a frame, the marker runs as recorded: the reference upscaler fills the output.
void Upscaler::ensure_dlss_marker() {
	std::string error;
	if (!vk::command_interception_active()) {
		error = "Godot's vkCmd* functions are not hooked (the command hooks were not enabled before the Vulkan instance)";
	} else if (dlss_provider_ == nullptr) {
		error = "no DLSS runtime backend";
	} else {
		RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
		const uint64_t marker = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMPUTE_PIPELINE, pipeline_, 0);
		if (marker == 0 || !vk::add_marker(reinterpret_cast<VkPipeline>(marker), &dlss_recorder_)) {
			error = "the marker pipeline could not be registered";
		} else {
			marker_ = marker;
		}
	}
	if (!error.empty()) {
		set_error("DLSS cannot record: " + error + "; the reference upscaler runs instead");
		std::lock_guard<std::mutex> lock(stats_mutex_);
		native_error_ = error;
	}
}

namespace {

dlss::EvaluateImage evaluate_image(RenderingDevice *rd, const RID &texture, VkImageLayout layout) {
	dlss::EvaluateImage image;
	image.image = reinterpret_cast<VkImage>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, texture, 0));
	image.view = reinterpret_cast<VkImageView>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, texture, 0));
	image.format = static_cast<VkFormat>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, texture, 0));
	const Ref<RDTextureFormat> format = rd->texture_get_format(texture);
	if (format.is_valid()) {
		image.width = format->get_width();
		image.height = format->get_height();
		image.usage = upscale::vk_image_usage(static_cast<uint32_t>(format->get_usage_bits()));
	}
	image.layout = layout;
	return image;
}

// Row-major copy of a Godot (column-major) projection.
void row_major(const Projection &m, float out[4][4]) {
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 4; ++c) {
			out[r][c] = static_cast<float>(m.columns[c][r]);
		}
	}
}

// The projection without the TAA jitter Godot folded into it (clip xy offset by jitter * w).
Projection without_jitter(const Projection &jittered, float ndc_x, float ndc_y) {
	Projection p = jittered;
	for (int c = 0; c < 4; ++c) {
		p.columns[c][0] -= ndc_x * p.columns[c][3];
		p.columns[c][1] -= ndc_y * p.columns[c][3];
	}
	return p;
}

} // namespace

// This frame's camera without the jitter, and clip -> previous clip. After a history reset the
// previous camera is not used (DLSS ignores the history, the fill writes no motion).
Upscaler::CameraFrame Upscaler::camera_frame(RenderSceneBuffersRD *rb, RenderSceneData *scene, const ViewState &view,
		float jitter_x, float jitter_y) {
	CameraFrame frame;
	const Vector2i internal = rb->get_internal_size();
	const float ndc_x = internal.x > 0 ? 2.0f * jitter_x / static_cast<float>(internal.x) : 0.0f;
	const float ndc_y = internal.y > 0 ? 2.0f * jitter_y / static_cast<float>(internal.y) : 0.0f;
	frame.projection = without_jitter(scene->get_cam_projection(), ndc_x, ndc_y);
	frame.transform = scene->get_cam_transform();
	const bool use_previous = view.have_previous_camera && !view.reset;
	const Projection previous = use_previous ? view.previous_projection : frame.projection;
	const Transform3D previous_transform = use_previous ? view.previous_transform : frame.transform;
	// clip -> previous clip = previous view-to-clip * previous world-to-view * view-to-world * clip-to-view.
	frame.clip_to_prev = previous * Projection(previous_transform.affine_inverse() * frame.transform) * frame.projection.inverse();
	return frame;
}

void Upscaler::set_dlss_job(uint32_t job, RenderSceneBuffersRD *rb, const RID &velocity, const CameraFrame &camera,
		ViewState &view, const Snapshot &snap, float jitter_x, float jitter_y) {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (view.viewport_id == 0) {
		view.viewport_id = next_viewport_id_++;
	}
	dlss::EvaluateInputs in;
	in.viewport = view.viewport_id;
	// The layouts Godot's render graph gives the marker's uniform set (sampled / storage).
	in.color = evaluate_image(rd, rb->get_color_texture(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	in.depth = evaluate_image(rd, rb->get_depth_texture(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	in.velocity = evaluate_image(rd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); // the filled copy
	in.output = evaluate_image(rd, rb->get_texture(buffers_context(), upscaled_name()), VK_IMAGE_LAYOUT_GENERAL);
	in.mode = snap.quality;
	in.preset = snap.preset;
	// As Godot hands its jitter to FSR2 (render pixels); the tests can change the convention.
	in.jitter_x = jitter_x * JITTER_SIGN_X * snap.jitter_sign_x;
	in.jitter_y = jitter_y * JITTER_SIGN_Y * snap.jitter_sign_y;
	in.mvec_scale_x = snap.motion_sign; // velocity is already in UV units
	in.mvec_scale_y = snap.motion_sign;
	in.reset = view.reset;
	in.depth_inverted = true;

	const Projection &projection = camera.projection;
	const Transform3D &transform = camera.transform;
	row_major(projection, in.view_to_clip);
	row_major(projection.inverse(), in.clip_to_view);
	row_major(camera.clip_to_prev, in.clip_to_prev_clip);
	row_major(camera.clip_to_prev.inverse(), in.prev_clip_to_clip);
	const Basis &basis = transform.basis;
	const Vector3 axes[4] = { transform.origin, basis.get_column(1), basis.get_column(0), -basis.get_column(2) };
	float *targets[4] = { in.camera_pos, in.camera_up, in.camera_right, in.camera_forward };
	for (int i = 0; i < 4; ++i) {
		targets[i][0] = static_cast<float>(axes[i].x);
		targets[i][1] = static_cast<float>(axes[i].y);
		targets[i][2] = static_cast<float>(axes[i].z);
	}
	in.camera_near = static_cast<float>(projection.get_z_near());
	in.camera_far = static_cast<float>(projection.get_z_far());
	in.camera_aspect = static_cast<float>(projection.get_aspect());
	in.camera_fov_y = static_cast<float>(Math::deg_to_rad(Projection::get_fovy(projection.get_fov(), static_cast<real_t>(1.0 / projection.get_aspect()))));
	in.orthographic = projection.is_orthogonal();
	dlss_recorder_.set_job(job, in);
}

// Registers the reference pipeline as the marker and creates its native twin from the same SPIR-V.
// On any failure the marker is not registered, so Godot's own dispatch of it keeps upscaling.
void Upscaler::ensure_native(const PackedByteArray &bytecode) {
	std::string error;
	upscale::UpscaleFeature &feature = upscale::UpscaleFeature::get();
	if (!vk::command_interception_active()) {
		error = "Godot's vkCmd* functions are not hooked (the command hooks were not enabled before the Vulkan instance)";
	} else if (bytecode.size() == 0 || bytecode.size() % 4 != 0) {
		error = "no SPIR-V for the native reference pipeline";
	} else if (feature.native_ready(error)) {
		std::vector<uint32_t> words(static_cast<size_t>(bytecode.size()) / 4);
		std::memcpy(words.data(), bytecode.ptr(), words.size() * sizeof(uint32_t));
		if (feature.ensure_reference(words, error)) {
			RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
			const uint64_t marker = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMPUTE_PIPELINE, pipeline_, 0);
			if (marker == 0 || !vk::add_marker(reinterpret_cast<VkPipeline>(marker), &feature.reference_recorder())) {
				error = "the reference pipeline could not be registered as the marker";
			} else {
				marker_ = marker;
			}
		}
	}
	if (!error.empty()) {
		log::warning("upscaler: native recording unavailable, Godot records the reference dispatch: %s", error.c_str());
		std::lock_guard<std::mutex> lock(stats_mutex_);
		native_error_ = error;
	}
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
	const RID output = rb->get_texture(buffers_context(), upscaled_name());
	const Vector2i internal = rb->get_internal_size();
	const Vector2i target = rb->get_target_size();
	if (!color.is_valid() || !depth.is_valid() || !rb->get_velocity_texture().is_valid() || !output.is_valid()) {
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
		if (!view.reset) {
			view.reset = true;
			view.reset_reason = ResetReason::Resize;
		}
		view.inputs_key[0] = 0; // rebuild the uniform sets
	}

	float jitter_x = 0.0f, jitter_y = 0.0f;
	track_jitter(scene, view, internal.x, target.x, internal.y, jitter_x, jitter_y);
	Snapshot snap;
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		snap = snapshot_;
	}
	// Godot's jitter phase of this frame (UINT32_MAX until calibrated).
	const uint32_t phase_count = dlss::jitter_phase_count(static_cast<uint32_t>(target.x), static_cast<uint32_t>(internal.x), true, false);
	const uint32_t phase = view.calibrated && phase_count > 0 ?
			static_cast<uint32_t>((static_cast<int64_t>(view.frames % phase_count) + view.phase_offset + phase_count) % phase_count) :
			UINT32_MAX;
	// History resets: a takeover or a resize (set above), a requested one (reset_dlss_history(); the
	// debug tests can hold it back until a given jitter phase, to compare runs from the same phase),
	// and camera cuts: another camera, or a turn or jump past the thresholds (upscale/camera_cut.h).
	if (reset_requested_.load()) {
		const int wanted_phase = reset_phase_.load();
		if (wanted_phase < 0 || static_cast<uint32_t>(wanted_phase) == phase) {
			reset_requested_ = false;
			reset_phase_ = -1;
			if (!view.reset) {
				view.reset = true;
				view.reset_reason = static_cast<ResetReason>(requested_reason_.load());
			}
		}
	}
	const Transform3D transform = scene->get_cam_transform();
	if (!view.reset && view.have_previous_camera) {
		const upscale::CutReason cut = upscale::detect_cut(camera_pose(view.previous_transform), camera_pose(transform),
				snap.camera_serial != view.camera_serial, snap.cut_thresholds);
		if (cut != upscale::CutReason::None) {
			view.reset = true;
			switch (cut) {
				case upscale::CutReason::CameraSwitch:
					view.reset_reason = ResetReason::CameraSwitch;
					break;
				case upscale::CutReason::Rotation:
					view.reset_reason = ResetReason::Rotation;
					break;
				case upscale::CutReason::Translation:
					view.reset_reason = ResetReason::Translation;
					break;
				default:
					view.reset_reason = ResetReason::InvalidPose;
					break;
			}
		}
	}
	view.camera_serial = snap.camera_serial;
	const CameraFrame camera = camera_frame(rb, scene, view, jitter_x, jitter_y);
	// Motion vectors for the sky and background (godot/velocity_fill.h); what both backends read.
	std::string fill_error;
	const RID velocity = velocity_fill_.run(rb, camera.clip_to_prev, snap.velocity_fill, fill_error);
	if (!fill_error.empty()) {
		set_error(fill_error + "; the upscaler reads Godot's velocity unchanged (the sky has no motion)");
	}
	const uint64_t key[4] = { static_cast<uint64_t>(color.get_id()), static_cast<uint64_t>(depth.get_id()),
		static_cast<uint64_t>(velocity.get_id()), static_cast<uint64_t>(output.get_id()) };
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

	// Godot's conventions (JITTER_SIGN_*, velocity as-is); the tests can deliberately change them
	// (set_test_conventions, debug builds) to show that wrong conventions measurably look worse.
	const float jitter_sign_x = JITTER_SIGN_X * snap.jitter_sign_x;
	const float jitter_sign_y = JITTER_SIGN_Y * snap.jitter_sign_y;
	const float motion_sign = snap.motion_sign;
	const float max_history = snap.max_history;
	upscale::ReferenceParams params;
	params.render_size[0] = static_cast<float>(internal.x);
	params.render_size[1] = static_cast<float>(internal.y);
	params.target_size[0] = static_cast<float>(target.x);
	params.target_size[1] = static_cast<float>(target.y);
	params.jitter[0] = jitter_x * jitter_sign_x;
	params.jitter[1] = jitter_y * jitter_sign_y;
	params.motion_sign[0] = motion_sign;
	params.motion_sign[1] = motion_sign;
	params.max_history = max_history;
	params.reset = view.reset ? 1.0f : 0.0f;
	if (marker_.load() != 0) {
		// Native recording: the job id travels with the marker's push constants and names the images
		// (Godot's own views of them) for the recorder.
		if (++job_counter_ == 0) {
			job_counter_ = 1; // 0 means "no job"
		}
		params.job = job_counter_;
		if (backend_ == UpscaleBackend::Dlss) {
			set_dlss_job(params.job, rb, velocity, camera, view, snap, jitter_x, jitter_y);
		}
	}
	if (marker_.load() != 0 && backend_ != UpscaleBackend::Dlss) {
		auto view_of = [&](const RID &texture) {
			return reinterpret_cast<VkImageView>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, texture, 0));
		};
		upscale::ReferenceJob job;
		job.color = view_of(color);
		job.depth = view_of(depth);
		job.velocity = view_of(velocity);
		job.history_read = view_of(view.history[view.read]);
		job.output = view_of(output);
		job.history_write = view_of(view.history[1 - view.read]);
		job.sampler = reinterpret_cast<VkSampler>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_SAMPLER, sampler_, 0));
		upscale::UpscaleFeature::get().reference_recorder().set_job(params.job, job);
	}
	PackedByteArray push;
	push.resize(sizeof(params));
	std::memcpy(push.ptrw(), &params, sizeof(params));

	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, view.uniform_set[view.read], 0);
	rd->compute_list_set_push_constant(list, push, static_cast<uint32_t>(push.size()));
	rd->compute_list_dispatch(list, static_cast<uint32_t>((target.x + 7) / 8), static_cast<uint32_t>((target.y + 7) / 8), 1);
	rd->compute_list_end();

#ifdef DEBUG_ENABLED
	debug_readbacks(scene, rb, velocity, jitter_x, jitter_y, phase, phase_count);
#endif

	view.previous_projection = camera.projection;
	view.previous_transform = camera.transform;
	view.have_previous_camera = true;
	const bool was_reset = view.reset;
	const ResetReason reset_reason = view.reset_reason;
	view.frames_since_reset = view.reset ? 1 : view.frames_since_reset + 1;
	view.read = 1 - view.read;
	view.reset = false;
	view.frames++;
	std::lock_guard<std::mutex> lock(stats_mutex_);
	if (was_reset) {
		stats_.resets[static_cast<int>(reset_reason)]++;
		stats_.last_reset_reason = reset_reason_name(reset_reason);
	}
	stats_.frames_upscaled++;
	stats_.frames_since_reset = view.frames_since_reset;
	stats_.phase = phase == UINT32_MAX ? -1 : static_cast<int>(phase);
	stats_.render_width = internal.x;
	stats_.render_height = internal.y;
	stats_.target_width = target.x;
	stats_.target_height = target.y;
}

// Debug builds: for N frames (GODOT_LATENCY_UPSCALING_TEST_READBACK_FRAMES=N or
// request_readbacks()), read back the scene UBO (Godot's own taa_jitter and taa_frame_count) and
// the velocity at the centre of the screen.
void Upscaler::debug_readbacks(RenderSceneData *scene, RenderSceneBuffersRD *rb, const RID &filled_velocity,
		float jitter_x, float jitter_y, uint32_t phase, uint32_t phase_count) {
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
	if (!ensure_velocity_probe() || !velocity.is_valid() || !filled_velocity.is_valid()) {
		return;
	}
	const uint64_t key[2] = { static_cast<uint64_t>(velocity.get_id()), static_cast<uint64_t>(filled_velocity.get_id()) };
	if (std::memcmp(key, probe_key_, sizeof(key)) != 0 || !rd->uniform_set_is_valid(probe_set_)) {
		if (probe_set_.is_valid() && rd->uniform_set_is_valid(probe_set_)) {
			rd->free_rid(probe_set_);
		}
		TypedArray<RDUniform> uniforms;
		for (int binding = 0; binding < 2; ++binding) {
			Ref<RDUniform> tex;
			tex.instantiate();
			tex->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
			tex->set_binding(binding);
			tex->add_id(sampler_);
			tex->add_id(binding == 0 ? velocity : filled_velocity);
			uniforms.push_back(tex);
		}
		Ref<RDUniform> buf;
		buf.instantiate();
		buf->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		buf->set_binding(2);
		buf->add_id(probe_buffer_);
		uniforms.push_back(buf);
		probe_set_ = rd->uniform_set_create(uniforms, probe_shader_, 0);
		std::memcpy(probe_key_, key, sizeof(key));
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
	if (data.size() < 16) {
		return;
	}
	float v[4];
	std::memcpy(v, data.ptr(), sizeof(v)); // Godot's velocity, then the filled copy
	std::lock_guard<std::mutex> lock(stats_mutex_);
	stats_.velocity_samples.push_back(Vector2(v[0], v[1]));
	stats_.filled_velocity_samples.push_back(Vector2(v[2], v[3]));
	if (stats_.velocity_samples.size() > 256) {
		stats_.velocity_samples.pop_front();
		stats_.filled_velocity_samples.pop_front();
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

void Upscaler::set_test_native_mode(int mode) {
	upscale::NativeMode native = upscale::NativeMode::Record;
	if (mode == 1) {
		native = upscale::NativeMode::Decline;
	} else if (mode == 2) {
		native = upscale::NativeMode::TestNegatedJitterX;
	}
	upscale::UpscaleFeature::get().reference_recorder().set_mode(native);
}

void Upscaler::release_gpu_resources() {
	// The marker first: the pipeline is about to be freed. The native twin lives until the device
	// is destroyed (UpscaleFeature::on_device_destroyed), when it can no longer be in flight.
	if (const uint64_t marker = marker_.exchange(0)) {
		vk::remove_marker(reinterpret_cast<VkPipeline>(marker));
	}
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
	velocity_fill_.release();
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
