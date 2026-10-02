#include "godot/latency_upscaling.h"
#include "godot/upscaler.h"

#include "core/log.h"
#include "core/settings.h"
#include "core/version.h"
#include "dlss/dlss_feature.h"
#include "dlss/fg_feature.h"
#include "godot/bootstrap.h"
#include "reflex/latency_stats.h"
#include "reflex/reflex_ll2.h"
#include "vk/dispatch.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace lu {
namespace {

constexpr const char *OVERLAY_SCRIPT = "res://addons/latency_upscaling/overlay/latency_overlay.gd";
constexpr const char *OVERLAY_NODE_NAME = "LatencyUpscalingLatencyOverlay";
constexpr const char *EXPORT_CHECK_SCRIPT = "res://addons/latency_upscaling/nvidia_runtime_export_check.gd";

PackedStringArray to_packed(const std::vector<std::string> &list) {
	PackedStringArray out;
	for (const std::string &item : list) {
		out.push_back(String::utf8(item.c_str()));
	}
	return out;
}

String vk_version_string(uint32_t v) {
	return vformat("%d.%d.%d", static_cast<int>(VK_API_VERSION_MAJOR(v)), static_cast<int>(VK_API_VERSION_MINOR(v)),
			static_cast<int>(VK_API_VERSION_PATCH(v)));
}

String driver_version_string(uint32_t vendor_id, uint32_t v) {
	if (vendor_id == 0x10DE) { // NVIDIA packs 10.8.8.6 bits
		return vformat("%d.%02d", static_cast<int>((v >> 22) & 0x3ff), static_cast<int>((v >> 14) & 0xff));
	}
	return vformat("0x%08x", static_cast<int64_t>(v));
}

std::shared_ptr<reflex::ReflexDevice> active_reflex_device() {
	return reflex::ReflexFeature::get().active_device();
}

Dictionary report_to_dictionary(const reflex::FrameReport &r) {
	Dictionary d;
	d["present_id"] = static_cast<int64_t>(r.present_id);
	d["input_sample_us"] = static_cast<int64_t>(r.input_sample_us);
	d["sim_start_us"] = static_cast<int64_t>(r.sim_start_us);
	d["sim_end_us"] = static_cast<int64_t>(r.sim_end_us);
	d["render_submit_start_us"] = static_cast<int64_t>(r.render_submit_start_us);
	d["render_submit_end_us"] = static_cast<int64_t>(r.render_submit_end_us);
	d["present_start_us"] = static_cast<int64_t>(r.present_start_us);
	d["present_end_us"] = static_cast<int64_t>(r.present_end_us);
	d["driver_start_us"] = static_cast<int64_t>(r.driver_start_us);
	d["driver_end_us"] = static_cast<int64_t>(r.driver_end_us);
	d["os_render_queue_start_us"] = static_cast<int64_t>(r.os_render_queue_start_us);
	d["os_render_queue_end_us"] = static_cast<int64_t>(r.os_render_queue_end_us);
	d["gpu_render_start_us"] = static_cast<int64_t>(r.gpu_render_start_us);
	d["gpu_render_end_us"] = static_cast<int64_t>(r.gpu_render_end_us);
	return d;
}

} // namespace

LatencyUpscaling *LatencyUpscaling::singleton_ = nullptr;

LatencyUpscaling::LatencyUpscaling() {
	singleton_ = this;
	const Settings &settings = boot_state().resolved.settings;
	mode_ = static_cast<ReflexMode>(settings.reflex_mode);
	fps_limit_ = settings.reflex_fps_limit;
	flash_marker_ = settings.reflex_flash_indicator;
	fg_mode_ = static_cast<FrameGenerationMode>(settings.fg_mode);
	mfg_multiplier_ = settings.fg_multiplier;
}

LatencyUpscaling::~LatencyUpscaling() {
	if (singleton_ == this) {
		singleton_ = nullptr;
	}
}

void LatencyUpscaling::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_status"), &LatencyUpscaling::get_status);
	ClassDB::bind_method(D_METHOD("is_reflex_supported"), &LatencyUpscaling::is_reflex_supported);
	ClassDB::bind_method(D_METHOD("is_reflex_active"), &LatencyUpscaling::is_reflex_active);
	ClassDB::bind_method(D_METHOD("get_reflex_status"), &LatencyUpscaling::get_reflex_status);
	ClassDB::bind_method(D_METHOD("set_reflex_mode", "mode"), &LatencyUpscaling::set_reflex_mode);
	ClassDB::bind_method(D_METHOD("get_reflex_mode"), &LatencyUpscaling::get_reflex_mode);
	ClassDB::bind_method(D_METHOD("set_reflex_fps_limit", "fps"), &LatencyUpscaling::set_reflex_fps_limit);
	ClassDB::bind_method(D_METHOD("get_reflex_fps_limit"), &LatencyUpscaling::get_reflex_fps_limit);
	ClassDB::bind_method(D_METHOD("get_latency_stats"), &LatencyUpscaling::get_latency_stats);
	ClassDB::bind_method(D_METHOD("get_latency_reports"), &LatencyUpscaling::get_latency_reports);
	ClassDB::bind_method(D_METHOD("trigger_flash"), &LatencyUpscaling::trigger_flash);
	ClassDB::bind_method(D_METHOD("set_overlay_visible", "visible"), &LatencyUpscaling::set_overlay_visible);
	ClassDB::bind_method(D_METHOD("is_overlay_visible"), &LatencyUpscaling::is_overlay_visible);
	ClassDB::bind_method(D_METHOD("get_version"), &LatencyUpscaling::get_version);
	ClassDB::bind_method(D_METHOD("is_dlss_supported"), &LatencyUpscaling::is_dlss_supported);
	ClassDB::bind_method(D_METHOD("is_dlss_active"), &LatencyUpscaling::is_dlss_active);
	ClassDB::bind_method(D_METHOD("get_dlss_status"), &LatencyUpscaling::get_dlss_status);
	ClassDB::bind_method(D_METHOD("set_dlss_enabled", "enabled"), &LatencyUpscaling::set_dlss_enabled);
	ClassDB::bind_method(D_METHOD("is_dlss_enabled"), &LatencyUpscaling::is_dlss_enabled);
	ClassDB::bind_method(D_METHOD("set_dlss_quality", "quality"), &LatencyUpscaling::set_dlss_quality);
	ClassDB::bind_method(D_METHOD("get_dlss_quality"), &LatencyUpscaling::get_dlss_quality);
	ClassDB::bind_method(D_METHOD("set_dlss_preset", "preset"), &LatencyUpscaling::set_dlss_preset);
	ClassDB::bind_method(D_METHOD("get_dlss_preset"), &LatencyUpscaling::get_dlss_preset);
	ClassDB::bind_method(D_METHOD("reset_dlss_history"), &LatencyUpscaling::reset_dlss_history);
	ClassDB::bind_method(D_METHOD("is_frame_generation_supported"), &LatencyUpscaling::is_frame_generation_supported);
	ClassDB::bind_method(D_METHOD("is_frame_generation_active"), &LatencyUpscaling::is_frame_generation_active);
	ClassDB::bind_method(D_METHOD("get_frame_generation_status"), &LatencyUpscaling::get_frame_generation_status);
	ClassDB::bind_method(D_METHOD("set_frame_generation_mode", "mode"), &LatencyUpscaling::set_frame_generation_mode);
	ClassDB::bind_method(D_METHOD("get_frame_generation_mode"), &LatencyUpscaling::get_frame_generation_mode);
	ClassDB::bind_method(D_METHOD("set_mfg_multiplier", "multiplier"), &LatencyUpscaling::set_mfg_multiplier);
	ClassDB::bind_method(D_METHOD("get_mfg_multiplier"), &LatencyUpscaling::get_mfg_multiplier);
	ClassDB::bind_method(D_METHOD("get_max_frame_generation_multiplier"), &LatencyUpscaling::get_max_frame_generation_multiplier);
#ifdef DEBUG_ENABLED
	ClassDB::bind_method(D_METHOD("_set_upscaler_test_conventions", "jitter_x", "jitter_y", "motion"),
			&LatencyUpscaling::_set_upscaler_test_conventions);
	ClassDB::bind_method(D_METHOD("_reset_upscaler_history", "phase"), &LatencyUpscaling::_reset_upscaler_history, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("_request_upscaler_readbacks", "frames"), &LatencyUpscaling::_request_upscaler_readbacks);
	ClassDB::bind_method(D_METHOD("_set_upscaler_test_native_mode", "mode"), &LatencyUpscaling::_set_upscaler_test_native_mode);
	ClassDB::bind_method(D_METHOD("_set_upscaler_test_velocity_fill", "mode"), &LatencyUpscaling::_set_upscaler_test_velocity_fill);
	ClassDB::bind_method(D_METHOD("_set_upscaler_test_cut_thresholds", "angle_degrees", "distance"),
			&LatencyUpscaling::_set_upscaler_test_cut_thresholds);
#endif

	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflex_mode", PROPERTY_HINT_ENUM, "Off,On,On + Boost"), "set_reflex_mode", "get_reflex_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflex_fps_limit", PROPERTY_HINT_RANGE, "0,1000,1"), "set_reflex_fps_limit", "get_reflex_fps_limit");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "overlay_visible"), "set_overlay_visible", "is_overlay_visible");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dlss_enabled"), "set_dlss_enabled", "is_dlss_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "dlss_quality", PROPERTY_HINT_ENUM, "Auto,DLAA,Quality,Balanced,Performance,Ultra Performance"),
			"set_dlss_quality", "get_dlss_quality");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "dlss_preset", PROPERTY_HINT_ENUM, "Default,J,K,L,M"), "set_dlss_preset", "get_dlss_preset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frame_generation", PROPERTY_HINT_ENUM, "Off,On"), "set_frame_generation_mode", "get_frame_generation_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mfg_multiplier", PROPERTY_HINT_ENUM, "Auto:0,2x:2,3x:3,4x:4"), "set_mfg_multiplier", "get_mfg_multiplier");

	BIND_ENUM_CONSTANT(REFLEX_MODE_OFF);
	BIND_ENUM_CONSTANT(REFLEX_MODE_ON);
	BIND_ENUM_CONSTANT(REFLEX_MODE_ON_BOOST);
	BIND_ENUM_CONSTANT(DLSS_QUALITY_AUTO);
	BIND_ENUM_CONSTANT(DLSS_QUALITY_DLAA);
	BIND_ENUM_CONSTANT(DLSS_QUALITY_QUALITY);
	BIND_ENUM_CONSTANT(DLSS_QUALITY_BALANCED);
	BIND_ENUM_CONSTANT(DLSS_QUALITY_PERFORMANCE);
	BIND_ENUM_CONSTANT(DLSS_QUALITY_ULTRA_PERFORMANCE);
	BIND_ENUM_CONSTANT(DLSS_PRESET_DEFAULT);
	BIND_ENUM_CONSTANT(DLSS_PRESET_J);
	BIND_ENUM_CONSTANT(DLSS_PRESET_K);
	BIND_ENUM_CONSTANT(DLSS_PRESET_L);
	BIND_ENUM_CONSTANT(DLSS_PRESET_M);
	BIND_ENUM_CONSTANT(FRAME_GENERATION_OFF);
	BIND_ENUM_CONSTANT(FRAME_GENERATION_ON);
}

// ---------------------------------------------------------------------------------------------
// Frame-loop wiring. One iteration of Godot 4.7's Main::iteration(), and what happens where:
//
//   [OS run loop] DisplayServer::process_events()      input is sampled here
//   Main::iteration()
//     physics steps, SceneTree process_frame ...       simulation
//     RenderingServer::draw()
//       emit frame_pre_draw          -> on_frame_pre_draw(): [TRIGGER_FLASH] SIMULATION_END(N),
//                                       then queue on_render_frame_begin(N) on the render thread
//       (render thread) _draw()      -> on_render_frame_begin(N): RENDERSUBMIT_START(N)
//         ... vkQueueSubmit          -> submissions tagged with present ID N
//         vkQueuePresentKHR          -> RENDERSUBMIT_END(N) PRESENT_START(N), VkPresentIdKHR = N,
//                                       PRESENT_END(N)   (reflex/reflex_ll2.cpp)
//     GDExtension frame callback     -> on_frame(): queue on_render_frame_end(N) behind _draw(N)
//                                       (RENDERSUBMIT_END(N) if no present sent it), then
//                                       vkLatencySleepNV + wait, then SIMULATION_START(N+1),
//                                       INPUT_SAMPLE(N+1)
//     ScriptServer::frame, AudioServer::update, OS::add_frame_delay
//   [OS run loop] DisplayServer::process_events()      input for N+1
//
// The sleep sits at the last point an extension can reach before input sampling, which is
// where NVIDIA's Reflex guide puts it ("sleep, then sample input, then simulate"); the NVIDIA
// Godot fork calls slReflexSleep at the same point (end of Main::iteration, main.cpp:5154).
// Details and the reasoning per marker: docs/ARCHITECTURE.md.
// ---------------------------------------------------------------------------------------------

void LatencyUpscaling::connect_frame_signals() {
	// Engine singletons are only reachable by name once Godot has registered them, which happens
	// after the SCENE initialisation level: hence this runs from the startup callback. Reflex (its
	// markers) and the upscaler (the current camera, before the frame renders) each connect their
	// own frame_pre_draw handler, so neither depends on the other being on.
	if (!Engine::get_singleton()->has_singleton("RenderingServer")) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!signals_connected_ && boot_state().reflex_enabled) {
		rs->connect("frame_pre_draw", callable_mp(this, &LatencyUpscaling::on_frame_pre_draw));
		signals_connected_ = true;
	}
	if (!upscaler_signal_connected_ && Upscaler::get().backend() != UpscaleBackend::None) {
		rs->connect("frame_pre_draw", callable_mp(this, &LatencyUpscaling::on_upscaler_pre_draw));
		upscaler_signal_connected_ = true;
	}
}

void LatencyUpscaling::disconnect_frame_signals() {
	if (!Engine::get_singleton()->has_singleton("RenderingServer")) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (signals_connected_) {
		rs->disconnect("frame_pre_draw", callable_mp(this, &LatencyUpscaling::on_frame_pre_draw));
		signals_connected_ = false;
	}
	if (upscaler_signal_connected_) {
		rs->disconnect("frame_pre_draw", callable_mp(this, &LatencyUpscaling::on_upscaler_pre_draw));
		upscaler_signal_connected_ = false;
	}
}

void LatencyUpscaling::on_upscaler_pre_draw() {
	Upscaler::get().pre_draw();
}

// The NVIDIA runtime ships with Windows exports through the editor plugin's export plugin, so an
// export made while the plugin is disabled goes without it. This check runs in every editor session
// (the extension is loaded whether or not the plugin is enabled) and warns at export time. It needs
// no editor bindings: EditorPlugin::add_export_plugin only forwards to the editor's exporter, so a
// short-lived EditorPlugin instance outside the scene tree is enough to reach it. The exporter then
// owns the check and releases it with the editor; it is never removed at the extension's shutdown
// (doing so crashed the editor at exit in about one run in six: tests/run_package.py).
void LatencyUpscaling::register_export_check() {
	if (export_check_registered_ || !Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	const Ref<Resource> script = ResourceLoader::get_singleton()->load(EXPORT_CHECK_SCRIPT);
	const Variant host = ClassDBSingleton::get_singleton()->instantiate("EditorPlugin");
	Object *host_object = host;
	if (script.is_null() || host_object == nullptr) {
		log::warning("could not register the export check (%s)", script.is_null() ? "script missing" : "no EditorPlugin");
		return;
	}
	host_object->call("add_export_plugin", script->call("new"));
	host_object->call("free");
	export_check_registered_ = true;
}

void LatencyUpscaling::on_startup() {
	register_export_check();
	configure_upscaler();
	connect_frame_signals();
	if (boot_state().resolved.settings.overlay_enabled) {
		set_overlay_visible(true);
	}
	if (is_reflex_active() && Engine::get_singleton()->get_max_fps() > 0) {
		log::warning("Engine.max_fps is %d: Godot's limiter sleeps after the Reflex sleep and adds latency. "
					 "Use LatencyUpscaling.reflex_fps_limit (or latency_upscaling/reflex/fps_limit) instead.",
				Engine::get_singleton()->get_max_fps());
	}
}

void LatencyUpscaling::on_frame() {
	Upscaler &upscaler = Upscaler::get();
	upscaler.update();
	if (upscaler.takeover_pending()) {
		// Behind this iteration's _draw(): the buffers Godot configured for it are taken over
		// before the next frame renders (research 2.2; see godot/upscaler.h).
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LatencyUpscaling::run_upscaler_takeovers));
	}
	std::shared_ptr<reflex::ReflexDevice> device = active_reflex_device();
	if (!device) {
		return;
	}
	if (drawn_frame_id_ != 0) {
		// Queued behind this iteration's _draw(): right after it on a separate render thread, at
		// once (after the present) in the default model. frame_post_draw cannot be used: with a
		// separate render thread it is deferred and may arrive during the next frame's draw.
		RenderingServer::get_singleton()->call_on_render_thread(
				callable_mp(this, &LatencyUpscaling::on_render_frame_end).bind(drawn_frame_id_));
		drawn_frame_id_ = 0;
	}
	reflex::FrameSequencer &sequencer = device->sequencer();
	sequencer.close_open_simulation(); // a frame that never drew: close it before sleeping
	device->sleep();
	sequencer.begin_simulation();
}

void LatencyUpscaling::on_frame_pre_draw() {
	std::shared_ptr<reflex::ReflexDevice> device = active_reflex_device();
	if (!device) {
		return;
	}
	reflex::FrameSequencer &sequencer = device->sequencer();
	if (flash_marker_) {
		// NVIDIA: send the flash marker for left clicks; the driver decides whether to show the
		// Reflex Flash Indicator. Sampled here, inside the simulation of the frame.
		const bool down = Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT);
		if (down && !left_button_was_down_) {
			sequencer.request_flash();
		}
		left_button_was_down_ = down;
	}
	const uint64_t frame_id = sequencer.end_simulation();
	if (frame_id != 0) {
		// Runs immediately in the default thread model; with a separate render thread it runs
		// right before this frame's _draw(), which was not queued yet.
		drawn_frame_id_ = static_cast<int64_t>(frame_id);
		RenderingServer::get_singleton()->call_on_render_thread(
				callable_mp(this, &LatencyUpscaling::on_render_frame_begin).bind(drawn_frame_id_));
	}
}

void LatencyUpscaling::on_render_frame_begin(int64_t frame_id) {
	std::shared_ptr<reflex::ReflexDevice> device = active_reflex_device();
	if (!device) {
		return;
	}
	device->sequencer().render_begin(static_cast<uint64_t>(frame_id));
}

void LatencyUpscaling::on_render_frame_end(int64_t frame_id) {
	std::shared_ptr<reflex::ReflexDevice> device = active_reflex_device();
	if (!device) {
		return;
	}
	device->sequencer().render_end(static_cast<uint64_t>(frame_id));
}

// ---------------------------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// Upscaling (Phase 2)
// ---------------------------------------------------------------------------------------------

void LatencyUpscaling::configure_upscaler() {
	const BootState &boot = boot_state();
	const Settings &s = boot.resolved.settings;
	UpscaleBackend backend = UpscaleBackend::None;
	std::string reason;
	if (boot.editor) {
		reason = "not in the editor (games started from it use it)";
	} else if (!boot.hook_installed) {
		reason = boot.hook_reason;
	} else {
		const dlss::DlssDeviceStatus dlss_status = dlss::DlssFeature::get().status();
		if (dlss_status.available) {
			backend = UpscaleBackend::Dlss;
		} else {
			reason = dlss_status.reason;
		}
	}
#ifdef DEBUG_ENABLED
	if ((boot.test_upscaler == "reference" || boot.test_upscaler == "native") && !boot.editor) {
		backend = UpscaleBackend::Reference; // test backend (tests/upscaler_tests.gd)
		reason.clear();
	}
	const bool native = boot.test_upscaler == "native" && !boot.editor;
#else
	const bool native = false;
#endif
	Upscaler::get().configure(backend, reason, s.dlss_quality, s.dlss_preset, s.dlss_enabled, native);
	Upscaler::get().set_cut_thresholds(upscale::CutThresholds{ s.dlss_cut_angle_degrees, s.dlss_cut_distance });
}

void LatencyUpscaling::run_upscaler_takeovers() {
	Upscaler::get().run_takeovers();
}

void LatencyUpscaling::release_upscaler() {
	Upscaler::get().release_gpu_resources();
}

void LatencyUpscaling::on_shutdown() {
	Upscaler::get().shutdown();
	if (Engine::get_singleton()->has_singleton("RenderingServer")) {
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->call_on_render_thread(callable_mp(this, &LatencyUpscaling::release_upscaler));
		rs->force_sync();
	}
}

bool LatencyUpscaling::is_dlss_supported() const {
	return Upscaler::get().backend() == UpscaleBackend::Dlss;
}

bool LatencyUpscaling::is_dlss_active() const {
	return is_dlss_supported() && Upscaler::get().is_active();
}

String LatencyUpscaling::get_dlss_status() const {
	const Upscaler &u = Upscaler::get();
	if (u.backend() == UpscaleBackend::None) {
		return String::utf8(("unavailable: " + u.reason()).c_str());
	}
	if (!u.is_active()) {
		return String::utf8(("off: " + u.reason()).c_str());
	}
	const Dictionary st = u.status();
	const Vector2i render = st["render_size"];
	const Vector2i target = st["target_size"];
	return vformat("active (%s%s, %dx%d -> %dx%d)", st["resolved_quality"],
			u.backend() == UpscaleBackend::Dlss ? String() : String(", ") + upscale_backend_name(u.backend()) + " backend",
			render.x, render.y, target.x, target.y);
}

void LatencyUpscaling::set_dlss_enabled(bool enabled) {
	Upscaler::get().set_enabled(enabled);
}

bool LatencyUpscaling::is_dlss_enabled() const {
	return Upscaler::get().is_enabled();
}

void LatencyUpscaling::set_dlss_quality(DlssQuality quality) {
	ERR_FAIL_COND_MSG(quality < DLSS_QUALITY_AUTO || quality > DLSS_QUALITY_ULTRA_PERFORMANCE,
			"DLSS quality must be one of the DLSS_QUALITY_* constants.");
	Upscaler::get().set_quality(static_cast<upscale::Quality>(quality));
}

LatencyUpscaling::DlssQuality LatencyUpscaling::get_dlss_quality() const {
	return static_cast<DlssQuality>(Upscaler::get().quality());
}

void LatencyUpscaling::set_dlss_preset(DlssPreset preset) {
	ERR_FAIL_COND_MSG(preset < DLSS_PRESET_DEFAULT || preset > DLSS_PRESET_M, "DLSS preset must be one of the DLSS_PRESET_* constants.");
	Upscaler::get().set_preset(static_cast<upscale::Preset>(preset));
}

LatencyUpscaling::DlssPreset LatencyUpscaling::get_dlss_preset() const {
	return static_cast<DlssPreset>(Upscaler::get().preset());
}

void LatencyUpscaling::reset_dlss_history() {
	Upscaler::get().reset_history();
}

// ---------------------------------------------------------------------------------------------
// Frame Generation (Phase 3; guarded off). The capability is reported by dlss::FgFeature (stubbed
// in milestone H1, so unavailable on every GPU); the mode and multiplier are stored here but have
// no runtime effect yet. A later, RTX-only milestone loads the Streamline Frame Generation feature,
// switches Reflex to sl.reflex and makes these take effect.
// ---------------------------------------------------------------------------------------------

bool LatencyUpscaling::is_frame_generation_supported() const {
	return dlss::FgFeature::get().status().supported;
}

bool LatencyUpscaling::is_frame_generation_active() const {
	return dlss::FgFeature::get().status().active;
}

String LatencyUpscaling::get_frame_generation_status() const {
	const dlss::FgStatus s = dlss::FgFeature::get().status();
	if (!s.supported) {
		return String("unavailable: ") + String::utf8(s.reason.c_str());
	}
	if (!s.active) {
		return String("off: ") + String::utf8(s.reason.c_str());
	}
	return vformat("active (%dx)", s.max_multiplier);
}

void LatencyUpscaling::set_frame_generation_mode(FrameGenerationMode mode) {
	ERR_FAIL_COND_MSG(mode < FRAME_GENERATION_OFF || mode > FRAME_GENERATION_ON,
			"Frame Generation mode must be FRAME_GENERATION_OFF or FRAME_GENERATION_ON.");
	fg_mode_ = mode;
}

LatencyUpscaling::FrameGenerationMode LatencyUpscaling::get_frame_generation_mode() const {
	return fg_mode_;
}

void LatencyUpscaling::set_mfg_multiplier(int multiplier) {
	ERR_FAIL_COND_MSG(!is_valid_mfg_multiplier(multiplier), "Multi Frame Generation multiplier must be 0 (Auto), 2, 3 or 4.");
	mfg_multiplier_ = multiplier;
}

int LatencyUpscaling::get_mfg_multiplier() const {
	return mfg_multiplier_;
}

int LatencyUpscaling::get_max_frame_generation_multiplier() const {
	return dlss::FgFeature::get().status().max_multiplier;
}

#ifdef DEBUG_ENABLED
void LatencyUpscaling::_set_upscaler_test_conventions(double jitter_x, double jitter_y, double motion) {
	Upscaler::get().set_test_conventions(static_cast<float>(jitter_x), static_cast<float>(jitter_y), static_cast<float>(motion));
}

void LatencyUpscaling::_reset_upscaler_history(int phase) {
	Upscaler::get().request_history_reset(phase);
}

void LatencyUpscaling::_request_upscaler_readbacks(int frames) {
	Upscaler::get().request_readbacks(frames);
}

void LatencyUpscaling::_set_upscaler_test_native_mode(int mode) {
	Upscaler::get().set_test_native_mode(mode);
}

void LatencyUpscaling::_set_upscaler_test_velocity_fill(int mode) {
	Upscaler::get().set_test_velocity_fill(mode);
}

void LatencyUpscaling::_set_upscaler_test_cut_thresholds(double angle_degrees, double distance) {
	Upscaler::get().set_cut_thresholds(upscale::CutThresholds{ angle_degrees, distance });
}
#endif

String LatencyUpscaling::get_version() const {
	return LU_VERSION;
}

void LatencyUpscaling::apply_sleep_mode() {
	reflex::SleepMode mode;
	mode.mode = static_cast<lu::ReflexMode>(mode_);
	mode.minimum_interval_us = fps_limit_to_interval_us(fps_limit_);
	reflex::ReflexFeature::get().set_sleep_mode(mode);
}

void LatencyUpscaling::set_reflex_mode(ReflexMode mode) {
	ERR_FAIL_COND_MSG(mode < REFLEX_MODE_OFF || mode > REFLEX_MODE_ON_BOOST, "Reflex mode must be REFLEX_MODE_OFF, REFLEX_MODE_ON or REFLEX_MODE_ON_BOOST.");
	mode_ = mode;
	apply_sleep_mode();
}

LatencyUpscaling::ReflexMode LatencyUpscaling::get_reflex_mode() const {
	return mode_;
}

void LatencyUpscaling::set_reflex_fps_limit(double fps) {
	ERR_FAIL_COND_MSG(!is_valid_fps_limit(fps), vformat("Reflex frame-rate limit must be 0 (none) or between %d and %d.",
														 static_cast<int>(MIN_FPS_LIMIT), static_cast<int>(MAX_FPS_LIMIT)));
	fps_limit_ = fps;
	apply_sleep_mode();
}

double LatencyUpscaling::get_reflex_fps_limit() const {
	return fps_limit_;
}

bool LatencyUpscaling::is_reflex_supported() const {
	return reflex::ReflexFeature::get().status().supported;
}

bool LatencyUpscaling::is_reflex_active() const {
	return reflex::ReflexFeature::get().status().active;
}

String LatencyUpscaling::get_reflex_status() const {
	const reflex::ReflexFeature::Status s = reflex::ReflexFeature::get().status();
	if (s.active) {
		return s.reason.empty() ? String("active") : String("active (") + String::utf8(s.reason.c_str()) + ")";
	}
	return String("unavailable: ") + String::utf8(s.reason.c_str());
}

void LatencyUpscaling::trigger_flash() {
	if (std::shared_ptr<reflex::ReflexDevice> device = active_reflex_device()) {
		device->sequencer().request_flash();
	}
}

Dictionary LatencyUpscaling::get_status() const {
	const BootState &boot = boot_state();
	const vk::LayerSnapshot layer = vk::snapshot();
	RenderingServer *rs = RenderingServer::get_singleton();
	const String driver = rs != nullptr ? rs->get_current_rendering_driver_name() : String();
	const String method = rs != nullptr ? rs->get_current_rendering_method() : String();
	String display;
	if (Object *ds = Engine::get_singleton()->get_singleton("DisplayServer")) {
		display = ds->call("get_name");
	}

	Dictionary d;
	d["version"] = LU_VERSION;
	d["godot_version"] = String::utf8(boot.godot_version.c_str());
	d["editor"] = boot.editor;

	// Why the layer is (not) in front of the loader, most specific reason first.
	String hook_reason;
	if (!boot.hook_installed) {
		hook_reason = String::utf8(boot.hook_reason.c_str());
	} else if (!layer.loader_intercepted) {
		if (display == "headless") {
			hook_reason = "headless display server: no Vulkan device";
		} else {
			hook_reason = vformat("the Vulkan loader was never requested (rendering driver '%s', method '%s'); the addon needs the Vulkan driver", driver, method);
		}
	} else if (!layer.loader_problem.empty()) {
		hook_reason = String::utf8(layer.loader_problem.c_str());
	} else if (layer.devices_created == 0) {
		hook_reason = "no Vulkan device has been created";
	}
	Dictionary hook;
	hook["installed"] = boot.hook_installed;
	hook["active"] = hook_reason.is_empty();
	hook["reason"] = hook_reason;
	hook["patched_imports"] = to_packed(boot.patched_imports);
	d["hook"] = hook;

	Dictionary vulkan;
	vulkan["loader_intercepted"] = layer.loader_intercepted;
	vulkan["instances_created"] = layer.instances_created;
	vulkan["instance_layers"] = to_packed(layer.instance_layers);
	vulkan["devices_created"] = layer.devices_created;
	vulkan["local_devices_created"] = layer.local_devices_created;
	vulkan["device_handle"] = static_cast<int64_t>(layer.device_handle);
	vulkan["gpu"] = String::utf8(layer.gpu_name.c_str());
	vulkan["vendor_id"] = static_cast<int64_t>(layer.vendor_id);
	vulkan["driver_version"] = layer.devices_created > 0 ? driver_version_string(layer.vendor_id, layer.driver_version) : String();
	vulkan["api_version"] = layer.devices_created > 0 ? vk_version_string(layer.api_version) : String();
	vulkan["device_extensions"] = to_packed(layer.device_extensions);
	vulkan["added_device_extensions"] = to_packed(layer.added_device_extensions);
	vulkan["note"] = String::utf8(layer.device_note.c_str());
	vulkan["loader_override"] = String::utf8(layer.loader_override.c_str()); // "sl.interposer.dll" with DLSS
	vulkan["loader_override_active"] = layer.loader_override_active;
	vulkan["loader_override_problem"] = String::utf8(layer.loader_override_problem.c_str());
	d["vulkan"] = vulkan;

	const reflex::ReflexFeature::Status rs_status = reflex::ReflexFeature::get().status();
	Dictionary reflex_info;
	reflex_info["requested"] = rs_status.requested;
	reflex_info["supported"] = rs_status.supported;
	reflex_info["active"] = rs_status.active;
	reflex_info["reason"] = String::utf8(rs_status.reason.c_str());
	reflex_info["mode"] = static_cast<int>(mode_);
	reflex_info["fps_limit"] = fps_limit_;
	if (std::shared_ptr<reflex::ReflexDevice> device = active_reflex_device()) {
		const reflex::DeviceState st = device->state();
		reflex_info["sleep_enabled"] = st.sleep_enabled;
		reflex_info["sleep_mode_error"] = String::utf8(st.sleep_mode_error.c_str());
		Dictionary counters;
		for (size_t i = 0; i < reflex::MARKER_COUNT; ++i) {
			counters[reflex::marker_name(static_cast<reflex::Marker>(i))] = static_cast<int64_t>(st.counters.markers[i]);
		}
		counters["sleeps"] = static_cast<int64_t>(st.counters.sleeps);
		counters["sleeps_skipped"] = static_cast<int64_t>(st.counters.sleeps_skipped);
		counters["sleep_timeouts"] = static_cast<int64_t>(st.counters.sleep_timeouts);
		counters["presents_stamped"] = static_cast<int64_t>(st.counters.presents_stamped);
		counters["submits_tagged"] = static_cast<int64_t>(st.counters.submits_tagged);
		counters["swapchains_with_latency_mode"] = static_cast<int64_t>(st.counters.swapchains_with_latency_mode);
		counters["other_swapchains"] = static_cast<int64_t>(st.counters.other_swapchains);
		counters["present_lock_waits_over_1ms"] = static_cast<int64_t>(st.counters.present_lock_waits_over_1ms);
		counters["present_lock_wait_max_us"] = static_cast<int64_t>(st.counters.present_lock_wait_max_us);
		counters["latency_sleep_call_max_us"] = static_cast<int64_t>(st.counters.latency_sleep_call_max_us);
		reflex_info["counters"] = counters;
	}
	d["reflex"] = reflex_info;

	Dictionary rendering;
	rendering["driver"] = driver;
	rendering["method"] = method;
	rendering["display_server"] = display;
	d["rendering"] = rendering;

	Dictionary settings;
	const Settings &s = boot.resolved.settings;
	auto source = [&](const char *key) {
		const auto it = boot.resolved.source_of.find(key);
		return String::utf8(it != boot.resolved.source_of.end() ? it->second.c_str() : "built-in default");
	};
	auto entry = [&](const char *key, const Variant &value) {
		Dictionary e;
		e["value"] = value;
		e["source"] = source(key);
		settings[key] = e;
	};
	entry(keys::ENABLED, s.enabled);
	entry(keys::ENABLE_IN_EDITOR, s.enable_in_editor);
	entry(keys::REFLEX_MODE, static_cast<int>(s.reflex_mode));
	entry(keys::REFLEX_FPS_LIMIT, s.reflex_fps_limit);
	entry(keys::REFLEX_FLASH_INDICATOR, s.reflex_flash_indicator);
	entry(keys::OVERLAY_ENABLED, s.overlay_enabled);
	entry(keys::DLSS_ENABLED, s.dlss_enabled);
	entry(keys::DLSS_QUALITY, static_cast<int>(s.dlss_quality));
	entry(keys::DLSS_PRESET, static_cast<int>(s.dlss_preset));
	entry(keys::DLSS_CUT_ANGLE, s.dlss_cut_angle_degrees);
	entry(keys::DLSS_CUT_DISTANCE, s.dlss_cut_distance);
	entry(keys::FG_ENABLED, s.fg_enabled);
	entry(keys::FG_MODE, static_cast<int>(s.fg_mode));
	entry(keys::FG_MULTIPLIER, s.fg_multiplier);
	d["settings"] = settings;
	Dictionary upscaling = Upscaler::get().status();
	Dictionary dlss;
	const dlss::DlssDeviceStatus dlss_status = dlss::DlssFeature::get().status();
	dlss["device_checked"] = dlss_status.device_seen;
	dlss["available"] = dlss_status.available;
	dlss["reason"] = String::utf8(dlss_status.reason.c_str());
	dlss["tensor_core_hint"] = dlss_status.tensor_core_hint;
	dlss["runtime_dir"] = String::utf8(boot.dlss_runtime_dir.c_str());
	PackedStringArray runtime_missing;
	for (const std::string &name : boot.dlss_runtime_missing) {
		runtime_missing.push_back(String::utf8(name.c_str()));
	}
	dlss["runtime_missing"] = runtime_missing;
	// Why the NVIDIA runtime was not loaded, even where the GPU check already rules DLSS out (e.g. an
	// interposer without NVIDIA's signature).
	dlss["runtime_reason"] = String::utf8(boot.dlss_runtime_reason.c_str());
	dlss["project_id"] = String::utf8(boot.dlss_project_id.c_str());
	dlss["project_id_source"] = String::utf8(boot.dlss_project_id_source.c_str());
	upscaling["dlss"] = dlss;
	// Frame Generation (Phase 3; guarded off, so supported/active are false on every GPU in this
	// version). mode / multiplier are the live settings; max_multiplier is the hardware's ceiling.
	Dictionary fg;
	const dlss::FgStatus fg_status = dlss::FgFeature::get().status();
	fg["supported"] = fg_status.supported;
	fg["active"] = fg_status.active;
	fg["mode"] = static_cast<int>(fg_mode_);
	fg["multiplier"] = mfg_multiplier_;
	fg["max_multiplier"] = fg_status.max_multiplier;
	fg["reason"] = String::utf8(fg_status.reason.c_str());
	upscaling["frame_generation"] = fg;
	d["upscaling"] = upscaling;
	d["warnings"] = to_packed(boot.resolved.warnings);
	return d;
}

Dictionary LatencyUpscaling::get_latency_stats() const {
	Dictionary d;
	std::shared_ptr<reflex::ReflexDevice> device = active_reflex_device();
	const reflex::LatencyStats stats = reflex::compute_latency_stats(device ? device->latency_reports() : std::vector<reflex::FrameReport>());
	d["frames"] = stats.frames;
	d["latest_present_id"] = static_cast<int64_t>(stats.latest_present_id);
	d["sim_start_to_gpu_end_ms"] = stats.sim_start_to_gpu_end_ms;
	d["input_sample_to_gpu_end_ms"] = stats.input_sample_to_gpu_end_ms;
	d["render_submit_start_to_gpu_end_ms"] = stats.render_submit_start_to_gpu_end_ms;
	d["simulation_ms"] = stats.simulation_ms;
	d["render_submit_ms"] = stats.render_submit_ms;
	d["present_ms"] = stats.present_ms;
	d["driver_ms"] = stats.driver_ms;
	d["os_render_queue_ms"] = stats.os_render_queue_ms;
	d["gpu_render_ms"] = stats.gpu_render_ms;
	d["frame_interval_ms"] = stats.frame_interval_ms;
	d["gpu_frame_interval_ms"] = stats.gpu_frame_interval_ms;
	d["mode"] = static_cast<int>(mode_);
	d["fps_limit"] = fps_limit_;
	if (device) {
		const reflex::DeviceState st = device->state();
		d["reflex_sleep_ms"] = st.counters.average_sleep_ms;
		d["reflex_sleep_enabled"] = st.sleep_enabled;
	} else {
		d["reflex_sleep_ms"] = 0.0;
		d["reflex_sleep_enabled"] = false;
	}
	return d;
}

Array LatencyUpscaling::get_latency_reports() const {
	Array out;
	if (std::shared_ptr<reflex::ReflexDevice> device = active_reflex_device()) {
		for (const reflex::FrameReport &r : device->latency_reports()) {
			out.push_back(report_to_dictionary(r));
		}
	}
	return out;
}

bool LatencyUpscaling::is_overlay_visible() const {
	return overlay_id_ != 0 && ObjectDB::get_instance(overlay_id_) != nullptr;
}

void LatencyUpscaling::set_overlay_visible(bool visible) {
	if (Engine::get_singleton()->is_editor_hint() || visible == is_overlay_visible()) {
		return;
	}
	if (!visible) {
		ObjectDB::get_instance(overlay_id_)->call("queue_free");
		overlay_id_ = 0;
		return;
	}
	MainLoop *loop = Engine::get_singleton()->get_main_loop();
	Object *root = loop != nullptr && loop->has_method("get_root") ? static_cast<Object *>(loop->call("get_root")) : nullptr;
	ERR_FAIL_NULL_MSG(root, "The latency overlay needs a SceneTree main loop.");
	Ref<Resource> script = ResourceLoader::get_singleton()->load(OVERLAY_SCRIPT);
	ERR_FAIL_COND_MSG(script.is_null(), vformat("Could not load %s.", OVERLAY_SCRIPT));
	Object *overlay = script->call("new");
	ERR_FAIL_NULL_MSG(overlay, "Could not instantiate the latency overlay.");
	overlay->set("name", OVERLAY_NODE_NAME);
	root->call_deferred("add_child", overlay);
	overlay_id_ = overlay->get_instance_id();
}

void LatencyUpscaling::register_project_settings() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const Settings &defaults = boot_state().addon_defaults;
	struct Definition {
		const char *key;
		Variant value;
		PropertyHint hint;
		const char *hint_string;
		bool restart;
	};
	const Definition definitions[] = {
		{ keys::ENABLED, defaults.enabled, PROPERTY_HINT_NONE, "", true },
		{ keys::ENABLE_IN_EDITOR, defaults.enable_in_editor, PROPERTY_HINT_NONE, "", true },
		{ keys::REFLEX_MODE, static_cast<int>(defaults.reflex_mode), PROPERTY_HINT_ENUM, "Off,On,On + Boost", false },
		{ keys::REFLEX_FPS_LIMIT, defaults.reflex_fps_limit, PROPERTY_HINT_RANGE, "0,1000,1", false },
		{ keys::REFLEX_FLASH_INDICATOR, defaults.reflex_flash_indicator, PROPERTY_HINT_NONE, "", false },
		{ keys::OVERLAY_ENABLED, defaults.overlay_enabled, PROPERTY_HINT_NONE, "", false },
		{ keys::DLSS_ENABLED, defaults.dlss_enabled, PROPERTY_HINT_NONE, "", false },
		{ keys::DLSS_QUALITY, static_cast<int>(defaults.dlss_quality), PROPERTY_HINT_ENUM,
				"Auto,DLAA,Quality,Balanced,Performance,Ultra Performance", false },
		{ keys::DLSS_PRESET, static_cast<int>(defaults.dlss_preset), PROPERTY_HINT_ENUM, "Default,J,K,L,M", false },
		{ keys::DLSS_CUT_ANGLE, defaults.dlss_cut_angle_degrees, PROPERTY_HINT_RANGE, "0,180,0.1,degrees", false },
		{ keys::DLSS_CUT_DISTANCE, defaults.dlss_cut_distance, PROPERTY_HINT_RANGE, "0,1000,0.01,or_greater,suffix:m", false },
		// Frame Generation (Phase 3, guarded off). fg/enabled is a permit-at-startup flag like a later
		// milestone's runtime load, so it needs a restart; the mode and multiplier are run-time values.
		{ keys::FG_ENABLED, defaults.fg_enabled, PROPERTY_HINT_NONE, "", true },
		{ keys::FG_MODE, static_cast<int>(defaults.fg_mode), PROPERTY_HINT_ENUM, "Off,On", false },
		{ keys::FG_MULTIPLIER, defaults.fg_multiplier, PROPERTY_HINT_ENUM, "Auto:0,2x:2,3x:3,4x:4", false },
	};
	// The project ID for NGX: a string, so not part of the numeric settings above. Empty means
	// "derived from application/config/name" (dlss/project_id.h).
	const String project_id = PROJECT_ID_SETTING;
	if (!ps->has_setting(project_id)) {
		ps->set_setting(project_id, String());
	}
	ps->set_initial_value(project_id, String());
	Dictionary id_info;
	id_info["name"] = project_id;
	id_info["type"] = Variant::STRING;
	id_info["hint"] = PROPERTY_HINT_PLACEHOLDER_TEXT;
	id_info["hint_string"] = "derived from application/config/name";
	ps->add_property_info(id_info);
	ps->set_as_basic(project_id, true);
	ps->set_restart_if_changed(project_id, true);

	for (const Definition &def : definitions) {
		const String name = String(PROJECT_SETTING_PREFIX) + def.key;
		// Like GLOBAL_DEF: a value equal to the initial value is not written to project.godot, so
		// the settings only appear there once a developer changes them.
		if (!ps->has_setting(name)) {
			ps->set_setting(name, def.value);
		}
		ps->set_initial_value(name, def.value);
		Dictionary info;
		info["name"] = name;
		info["type"] = def.value.get_type();
		info["hint"] = def.hint;
		info["hint_string"] = def.hint_string;
		ps->add_property_info(info);
		ps->set_as_basic(name, true);
		ps->set_restart_if_changed(name, def.restart);
	}
}

} // namespace lu
