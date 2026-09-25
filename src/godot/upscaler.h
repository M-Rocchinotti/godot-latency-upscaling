// Phase 2: the upscaler controller. It takes over Godot's temporal-upscaler slot for the main
// viewport and runs an upscaler backend there (the reference compute upscaler now, DLSS later).
//
// The FSR2 slot (research 2.2, docs/ARCHITECTURE.md section 7):
//   1. Main thread, end of each iteration (update()): the root viewport runs in
//      SCALING_3D_MODE_FSR2 at the quality mode's scale, so Godot computes the render size, the
//      Halton jitter (8 * ratio^2 phases), the mip bias and TAA-off; the camera gets our
//      CompositorEffect (POST_TRANSPARENT, motion vectors), merged with its existing effects.
//   2. Render thread, the frame after Godot (re)configures the render buffers (they are in FSR2
//      mode then, and FSR2 runs once): render() notes them; run_takeovers(), queued behind that
//      frame's draw, re-configures them identically but with SCALING_3D_MODE_METALFX_TEMPORAL,
//      which is "no upscaler" on Vulkan, and creates the target-size "color_upscaled" texture.
//      Tonemapping reads that texture whenever it exists in this mode.
//   3. Render thread, every frame after that: render() dispatches the backend, which reads
//      colour, depth and velocity (render size) and writes color_upscaled (target size).
//
// Jitter comes from RenderSceneData.get_cam_projection() (the jittered projection) every frame;
// the Halton phase is also tracked on the CPU (calibrated once) and must agree.
//
// Native recording (docs/ARCHITECTURE.md section 7, step 4): the reference pipeline doubles as the
// marker. Its VkPipeline is registered with the layer's MarkerInterceptor, and when Godot records
// the dispatch into its command buffer, upscale::NativeReferenceRecorder records the same shader
// natively instead (the images named by the push constants' job id). DLSS takes that slot later.
#pragma once

#include "dlss/dlss_recorder.h"
#include "godot/velocity_fill.h"
#include "upscale/camera_cut.h"
#include "upscale/quality.h"

#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lu {

// Injected into the main viewport's camera. Stateless: forwards to the Upscaler.
class LatencyUpscalingEffect : public godot::CompositorEffect {
	GDCLASS(LatencyUpscalingEffect, godot::CompositorEffect)

public:
	LatencyUpscalingEffect();
	void _render_callback(int32_t p_effect_callback_type, godot::RenderData *p_render_data) override;
	// Targets of the debug read-backs' async callbacks (a Callable needs an Object).
	void on_ubo_readback(const godot::PackedByteArray &data, float x, float y, int64_t phase, int64_t count);
	void on_velocity_readback(const godot::PackedByteArray &data);

protected:
	static void _bind_methods() {}
};

enum class UpscaleBackend {
	None, // upscaling off (reason says why)
	Reference, // the reference compute upscaler (debug test backend), recorded by Godot or natively
	Dlss, // NVIDIA DLSS (Phase 2; needs the NVIDIA runtime)
};

const char *upscale_backend_name(UpscaleBackend backend);

class Upscaler {
public:
	static Upscaler &get();

	// SCENE init (main thread). `backend` None comes with the reason.
	// `native`: record the backend natively into Godot's command buffer (marker interception).
	void configure(UpscaleBackend backend, const std::string &reason, upscale::Quality quality, upscale::Preset preset,
			bool enabled, bool native);

	// Main thread.
	void pre_draw(); // frame_pre_draw: the current camera, before this frame renders
	void update(); // end of every main-loop iteration
	void shutdown(); // restores the viewport and camera, frees GPU resources
	void set_enabled(bool enabled);
	bool is_enabled() const { return enabled_; }
	void set_quality(upscale::Quality quality);
	upscale::Quality quality() const { return quality_; }
	void set_preset(upscale::Preset preset);
	upscale::Preset preset() const { return preset_; }
	void set_cut_thresholds(const upscale::CutThresholds &thresholds);
	// Throws the history away on the next upscaled frame (the public reset_dlss_history()).
	void reset_history() { request_reset(ResetReason::Api, -1); }
	UpscaleBackend backend() const { return backend_; }
	bool is_active() const; // upscaling the main viewport right now
	std::string reason() const; // why it is not active ("" when active)
	godot::Dictionary status() const;

	// Render thread.
	void render(godot::RenderData *data);
	void run_takeovers();
	bool takeover_pending() const { return takeover_pending_.load(); }
	void release_gpu_resources(); // at shutdown, queued onto the render thread
	void on_ubo_readback(const godot::PackedByteArray &data, float expected_x, float expected_y, uint32_t expected_phase,
			uint32_t phase_count);
	void on_velocity_readback(const godot::PackedByteArray &data);

	// Debug builds (tests/upscaler_tests.gd): conventions to compare against Godot's (+1 = Godot's),
	// a history reset, and read-backs of the scene UBO and the velocity texture for `frames`.
	void set_test_conventions(float jitter_x, float jitter_y, float motion);
	// phase >= 0: hold the reset back until the frame with that jitter phase.
	void request_history_reset(int phase = -1) { request_reset(ResetReason::Test, phase); }
	void request_readbacks(int frames) { readback_frames_ = frames; }
	// Debug builds: 0 = fill far-plane motion vectors (the default), 1 = derive every pixel's vector
	// from depth (tests compare that with Godot's), 2 = off (the raw velocity, as before the fill).
	void set_test_velocity_fill(int mode);
	// Debug builds: 0 = record natively, 1 = let the marker dispatch run (the RenderingDevice path),
	// 2 = record natively with the jitter's x negated (proves the image is the native recording).
	void set_test_native_mode(int mode);

private:
	// Why the history was thrown away (counted in status()["history_resets"]).
	enum class ResetReason { Takeover, Resize, Api, Test, CameraSwitch, Rotation, Translation, InvalidPose, Count };
	static const char *reset_reason_name(ResetReason reason);
	void request_reset(ResetReason reason, int phase) {
		requested_reason_ = static_cast<int>(reason);
		reset_phase_ = phase;
		reset_requested_ = true;
	}

	struct Snapshot { // written by the main thread, read by the render thread
		uint64_t render_target = 0; // RID id of the managed viewport's render target
		float mipmap_bias = 0.0f;
		int anisotropic_filtering = 0;
		float max_history = 16.0f; // accumulated-weight cap (effective history length)
		float jitter_sign_x = 1.0f; // test conventions (debug); 1 = Godot's
		float jitter_sign_y = 1.0f;
		float motion_sign = 1.0f;
		upscale::Quality quality = upscale::Quality::Quality; // resolved (never Auto)
		upscale::Preset preset = upscale::Preset::Default;
		upscale::CutThresholds cut_thresholds;
		uint64_t camera_serial = 0; // +1 whenever another camera becomes current
		VelocityFillMode velocity_fill = VelocityFillMode::FarPlane;
	};
	struct ViewState { // render thread only
		godot::RID history[2];
		godot::Vector2i history_size;
		int read = 0;
		godot::RID uniform_set[2];
		uint64_t inputs_key[4] = {}; // color, depth, velocity, output RIDs the sets were built for
		bool reset = true;
		ResetReason reset_reason = ResetReason::Takeover;
		uint64_t frames = 0;
		uint64_t frames_since_reset = 0; // including the reset frame
		bool calibrated = false;
		int64_t phase_offset = 0; // Godot phase = (frames + phase_offset) % phase_count
		uint32_t viewport_id = 0; // DLSS viewport handle
		bool have_previous_camera = false; // last frame's camera: reprojection matrices, cut detection
		godot::Projection previous_projection; // without jitter
		godot::Transform3D previous_transform;
		uint64_t camera_serial = 0; // Snapshot::camera_serial of the last frame
	};
	// This frame's camera, as the DLSS evaluation and the velocity fill need it.
	struct CameraFrame {
		godot::Projection projection; // view -> clip, without the jitter
		godot::Transform3D transform; // camera -> world
		godot::Projection clip_to_prev; // clip -> previous frame's clip (without jitter)
	};
	struct Stats { // render thread writes, status() reads (under stats_mutex_)
		uint64_t frames_upscaled = 0;
		uint64_t fsr2_frames = 0; // frames Godot ran FSR2 before a takeover
		uint64_t takeovers = 0;
		uint64_t frames_since_reset = 0;
		int phase = -1; // Godot's jitter phase of the last upscaled frame (-1: not calibrated)
		int render_width = 0, render_height = 0, target_width = 0, target_height = 0;
		uint32_t phase_count = 0;
		int calibrated_phase = -1;
		uint64_t jitter_checks = 0;
		uint64_t jitter_mismatches = 0; // CPU Halton prediction vs the projection
		float last_jitter_x = 0.0f, last_jitter_y = 0.0f;
		uint64_t ubo_checks = 0; // debug: scene UBO read-backs compared
		uint64_t ubo_mismatches = 0;
		godot::Array velocity_samples; // debug: centre-pixel velocity read-backs (Godot's)
		godot::Array filled_velocity_samples; // debug: the same pixels after the velocity fill
		uint64_t resets[static_cast<int>(ResetReason::Count)] = {};
		std::string last_reset_reason;
		std::string error;
	};

	bool ensure_pipeline(); // render thread
	void ensure_native(const godot::PackedByteArray &spirv); // render thread, after the pipeline exists
	void ensure_dlss_marker(); // render thread, after the pipeline exists
	void set_dlss_job(uint32_t job, godot::RenderSceneBuffersRD *rb, const godot::RID &velocity, const CameraFrame &camera,
			ViewState &view, const Snapshot &snap, float jitter_x, float jitter_y);
	CameraFrame camera_frame(godot::RenderSceneBuffersRD *rb, godot::RenderSceneData *scene, const ViewState &view,
			float jitter_x, float jitter_y);
	void track_camera(godot::Window *root); // main thread: our effect on the current camera
	bool ensure_velocity_probe(); // render thread, debug
	void dispatch_reference(godot::RenderSceneBuffersRD *rb, godot::RenderSceneData *scene, ViewState &view);
	void track_jitter(godot::RenderSceneData *scene, ViewState &view, int render_width, int target_width,
			int render_height, float &jitter_x, float &jitter_y);
	void debug_readbacks(godot::RenderSceneData *scene, godot::RenderSceneBuffersRD *rb, const godot::RID &filled_velocity,
			float jitter_x, float jitter_y, uint32_t phase, uint32_t phase_count);
	void restore_viewport_and_camera();
	void set_error(const std::string &error);

	// Main thread.
	UpscaleBackend backend_ = UpscaleBackend::None;
	std::string backend_reason_;
	bool enabled_ = true;
	upscale::Quality quality_ = upscale::Quality::Auto;
	upscale::Preset preset_ = upscale::Preset::Default;
	upscale::Quality resolved_quality_ = upscale::Quality::Quality;
	std::string inactive_reason_;
	bool applied_ = false; // the root viewport and a camera are modified
	int saved_scaling_mode_ = 0;
	float saved_scaling_scale_ = 1.0f;
	uint64_t camera_id_ = 0; // camera we injected into
	godot::Ref<godot::Compositor> camera_original_compositor_;
	godot::Ref<LatencyUpscalingEffect> effect_;

	mutable std::mutex snapshot_mutex_;
	Snapshot snapshot_;

	// Render thread.
	godot::RID shader_, pipeline_, sampler_;
	bool pipeline_failed_ = false;
	bool native_requested_ = false; // set by configure() before any frame
	std::atomic<uint64_t> marker_{ 0 }; // VkPipeline registered as the marker (0: none)
	std::string native_error_; // guarded by stats_mutex_
	uint32_t job_counter_ = 0;
	dlss::UpscalerProvider *dlss_provider_ = nullptr; // set by configure() for the DLSS backend
	dlss::DlssRecorder dlss_recorder_; // registered with the layer: must live as long as the process
	uint32_t next_viewport_id_ = 1;
	std::map<uint64_t, ViewState> views_; // by render-target RID id
	std::map<uint64_t, godot::Ref<godot::RenderSceneBuffersRD>> pending_; // to take over
	std::atomic<bool> takeover_pending_{ false };
	std::atomic<bool> reset_requested_{ false };
	std::atomic<int> reset_phase_{ -1 };
	std::atomic<int> requested_reason_{ 0 };
	VelocityFill velocity_fill_;
	std::atomic<int> readback_frames_{ -1 }; // -1: not yet read from the environment
	godot::RID probe_shader_, probe_pipeline_, probe_buffer_, probe_set_;
	uint64_t probe_key_[2] = {}; // velocity and filled-velocity RIDs the probe set was built for

	mutable std::mutex stats_mutex_;
	Stats stats_;
};

} // namespace lu
