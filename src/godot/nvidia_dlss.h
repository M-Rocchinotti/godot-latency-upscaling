// The NvidiaDLSS engine singleton: the GDScript API and the wiring of Reflex into Godot's frame
// loop. Registered automatically at SCENE initialisation; projects need no setup.
#pragma once

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>

namespace nvdlss {

class NvidiaDLSS : public godot::Object {
	GDCLASS(NvidiaDLSS, godot::Object)

public:
	enum ReflexMode {
		REFLEX_MODE_OFF = 0,
		REFLEX_MODE_ON = 1,
		REFLEX_MODE_ON_BOOST = 2,
	};

	static NvidiaDLSS *get_singleton() { return singleton_; }

	NvidiaDLSS();
	~NvidiaDLSS() override;

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

	// --- Engine wiring (not exposed) ---
	void disconnect_frame_signals(); // SCENE deinitialisation
	void on_startup(); // main loop exists: connects the frame signals, shows the overlay
	void on_frame(); // GDExtension frame callback, end of every main-loop iteration

	static void register_project_settings(); // editor only

protected:
	static void _bind_methods();

private:
	void connect_frame_signals();
	void on_frame_pre_draw();
	void on_render_frame_begin(int64_t frame_id);
	void on_render_frame_end(int64_t frame_id);
	void apply_sleep_mode();

	static NvidiaDLSS *singleton_;
	ReflexMode mode_ = REFLEX_MODE_ON;
	double fps_limit_ = 0.0;
	bool flash_marker_ = true;
	bool left_button_was_down_ = false;
	int64_t drawn_frame_id_ = 0; // main thread: frame whose draw this iteration queued
	uint64_t overlay_id_ = 0; // ObjectID of the overlay node
	bool signals_connected_ = false;
};

} // namespace nvdlss

VARIANT_ENUM_CAST(nvdlss::NvidiaDLSS::ReflexMode);
