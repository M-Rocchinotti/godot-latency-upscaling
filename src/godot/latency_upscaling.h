// The LatencyUpscaling engine singleton: the GDScript API and the wiring of Reflex into Godot's frame
// loop. Registered automatically at SCENE initialisation; projects need no setup.
#pragma once

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>

namespace lu {

class LatencyUpscaling : public godot::Object {
	GDCLASS(LatencyUpscaling, godot::Object)

public:
	enum ReflexMode {
		REFLEX_MODE_OFF = 0,
		REFLEX_MODE_ON = 1,
		REFLEX_MODE_ON_BOOST = 2,
	};
	// Values match upscale::Quality / upscale::Preset and the dlss/* project settings.
	enum DlssQuality {
		DLSS_QUALITY_AUTO = 0,
		DLSS_QUALITY_DLAA = 1,
		DLSS_QUALITY_QUALITY = 2,
		DLSS_QUALITY_BALANCED = 3,
		DLSS_QUALITY_PERFORMANCE = 4,
		DLSS_QUALITY_ULTRA_PERFORMANCE = 5,
	};
	enum DlssPreset {
		DLSS_PRESET_DEFAULT = 0,
		DLSS_PRESET_J = 1,
		DLSS_PRESET_K = 2,
		DLSS_PRESET_L = 3,
		DLSS_PRESET_M = 4,
	};

	static LatencyUpscaling *get_singleton() { return singleton_; }

	LatencyUpscaling();
	~LatencyUpscaling() override;

	// --- GDScript API ---
	godot::Dictionary get_status() const;
	bool is_reflex_supported() const;
	bool is_reflex_active() const;
	godot::String get_reflex_status() const;
	void set_reflex_mode(ReflexMode mode);
	ReflexMode get_reflex_mode() const;
	void set_reflex_fps_limit(double fps);
	double get_reflex_fps_limit() const;
	godot::Dictionary get_latency_stats() const;
	godot::Array get_latency_reports() const;
	void trigger_flash();
	void set_overlay_visible(bool visible);
	bool is_overlay_visible() const;
	godot::String get_version() const;
	// DLSS Super Resolution / DLAA (Phase 2).
	bool is_dlss_supported() const; // the DLSS backend can run in this process
	bool is_dlss_active() const; // the main viewport is upscaled by DLSS now
	godot::String get_dlss_status() const; // "active (...)", "off: ..." or "unavailable: ..."
	void set_dlss_enabled(bool enabled);
	bool is_dlss_enabled() const;
	void set_dlss_quality(DlssQuality quality);
	DlssQuality get_dlss_quality() const;
	void set_dlss_preset(DlssPreset preset);
	DlssPreset get_dlss_preset() const;
	// Throws the upscaler's history away on the next frame: call it on camera cuts the automatic
	// detection cannot see (a teleport shorter than dlss/cut_distance, a scene change behind a fade).
	void reset_dlss_history();
#ifdef DEBUG_ENABLED
	// Test hooks (debug builds only, tests/upscaler_tests.gd): upscaler conventions relative to
	// Godot's (+1 = Godot's), a history reset, N frames of UBO / velocity read-backs, and the native
	// recorder's mode (0 record, 1 let Godot's marker dispatch run, 2 record with a negated jitter x).
	void _set_upscaler_test_conventions(double jitter_x, double jitter_y, double motion);
	void _reset_upscaler_history(int phase); // phase >= 0: at that jitter phase
	void _request_upscaler_readbacks(int frames);
	void _set_upscaler_test_native_mode(int mode);
	// 0 = fill far-plane motion vectors (default), 1 = derive every pixel from depth, 2 = off.
	void _set_upscaler_test_velocity_fill(int mode);
	void _set_upscaler_test_cut_thresholds(double angle_degrees, double distance); // 0 disables a test
#endif

	// --- Engine wiring (not exposed) ---
	void disconnect_frame_signals(); // SCENE deinitialisation
	void on_startup(); // main loop exists: connects the frame signals, shows the overlay
	void on_frame(); // GDExtension frame callback, end of every main-loop iteration
	void on_shutdown(); // GDExtension shutdown callback: restores the viewport, frees GPU resources

	static void register_project_settings(); // editor only

protected:
	static void _bind_methods();

private:
	void connect_frame_signals();
	void on_frame_pre_draw(); // Reflex
	void on_upscaler_pre_draw(); // the upscaler's camera tracking
	void on_render_frame_begin(int64_t frame_id);
	void on_render_frame_end(int64_t frame_id);
	void apply_sleep_mode();
	void configure_upscaler();
	void run_upscaler_takeovers(); // render thread
	void release_upscaler(); // render thread
	void register_export_check(); // editor: warn when an export goes without the NVIDIA runtime

	static LatencyUpscaling *singleton_;
	ReflexMode mode_ = REFLEX_MODE_ON;
	double fps_limit_ = 0.0;
	bool flash_marker_ = true;
	bool left_button_was_down_ = false;
	int64_t drawn_frame_id_ = 0; // main thread: frame whose draw this iteration queued
	uint64_t overlay_id_ = 0; // ObjectID of the overlay node
	bool signals_connected_ = false; // Reflex's frame_pre_draw handler
	bool upscaler_signal_connected_ = false;
	bool export_check_registered_ = false; // nvidia_runtime_export_check.gd, owned by the editor's exporter
};

} // namespace lu

VARIANT_ENUM_CAST(lu::LatencyUpscaling::ReflexMode);
VARIANT_ENUM_CAST(lu::LatencyUpscaling::DlssQuality);
VARIANT_ENUM_CAST(lu::LatencyUpscaling::DlssPreset);
