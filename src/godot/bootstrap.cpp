#include "godot/bootstrap.h"

#include "core/log.h"
#include "core/testing.h"
#include "dlss/dlss_feature.h"
#include "dlss/gpu_precheck.h"
#include "dlss/runtime_files.h"
#include "dlss/streamline.h"
#include "hook/loader_hook.h"
#include "reflex/reflex_ll2.h"
#include "upscale/upscale_feature.h"
#include "vk/dispatch.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

using namespace godot;

namespace lu {
namespace {

BootState g_state;

// Loads Streamline when a GPU can run DLSS (or `force`): a temporary instance lists the GPUs, the
// addon's own checks come first, Streamline's verdict second. Returns the backend with the host's
// Vulkan routed through Streamline, or nullptr with `reason`.
dlss::UpscalerProvider *load_streamline(const std::wstring &runtime_dir, bool force, std::string &reason) {
	dlss::Streamline &streamline = dlss::Streamline::get();
	bool load_failed = false;
	std::string error;
	const std::vector<dlss::AdapterVerdict> verdicts = dlss::check_adapters(
			[&](const vk::PhysicalDeviceCaps &caps, std::string &why) {
				why = dlss::check_device(caps, g_state.hidden_extensions);
				if (!why.empty() && !force) {
					return false;
				}
				if (load_failed || (!streamline.loaded() && !streamline.load(runtime_dir, why))) {
					load_failed = true;
					return false;
				}
				std::string unsupported;
				const bool supported = streamline.adapter_supported(caps.handle, unsupported);
				if (why.empty()) {
					why = unsupported;
				}
				return supported || force;
			},
			error);
	const auto accepted = std::find_if(verdicts.begin(), verdicts.end(), [](const dlss::AdapterVerdict &v) { return v.accepted; });
	if (accepted == verdicts.end()) {
		streamline.unload();
		if (!error.empty()) {
			reason = error;
		} else if (verdicts.empty()) {
			reason = "Vulkan reports no GPU";
		} else {
			reason = verdicts.front().reason; // the first GPU is the one Godot picks by default
		}
		return nullptr;
	}
	vk::set_loader_override(streamline.interposer(), "sl.interposer.dll");
	log::info("DLSS: NVIDIA runtime loaded for %s; Vulkan goes through Streamline", accepted->name.c_str());
	return &streamline;
}

void godot_log_sink(log::Level level, const char *message) {
	switch (level) {
		case log::Level::Info:
			UtilityFunctions::print(String::utf8(message));
			break;
		case log::Level::Warning:
			UtilityFunctions::push_warning(String::utf8(message));
			break;
		case log::Level::Error:
			UtilityFunctions::push_error(String::utf8(message));
			break;
	}
}

bool variant_to_number(const Variant &value, double &out) {
	switch (value.get_type()) {
		case Variant::BOOL:
			out = static_cast<bool>(value) ? 1.0 : 0.0;
			return true;
		case Variant::INT:
			out = static_cast<double>(static_cast<int64_t>(value));
			return true;
		case Variant::FLOAT:
			out = static_cast<double>(value);
			return true;
		default:
			return false;
	}
}

SettingsLayer project_layer(std::vector<std::string> &warnings) {
	SettingsLayer layer;
	layer.name = "project setting";
	ProjectSettings *ps = ProjectSettings::get_singleton();
	for (const char *key : all_setting_keys()) {
		const String name = String(PROJECT_SETTING_PREFIX) + key;
		if (!ps->has_setting(name)) {
			continue;
		}
		double number = 0.0;
		if (variant_to_number(ps->get_setting(name), number)) {
			layer.values[key] = number;
		} else {
			warnings.push_back(std::string("project setting '") + name.utf8().get_data() + "' is not a boolean or number");
		}
	}
	return layer;
}

#ifdef DEBUG_ENABLED
std::vector<std::string> split_list(const String &text) {
	std::vector<std::string> out;
	const PackedStringArray parts = text.split(",", false);
	for (int i = 0; i < parts.size(); ++i) {
		const String item = parts[i].strip_edges();
		if (!item.is_empty()) {
			out.emplace_back(item.utf8().get_data());
		}
	}
	return out;
}
#endif

} // namespace

const BootState &boot_state() {
	return g_state;
}

void bootstrap_core() {
	if (g_state.ran) {
		return;
	}
	g_state.ran = true;
	OS *os = OS::get_singleton();
	Engine *engine = Engine::get_singleton();

	log::set_sink(&godot_log_sink);
	log::set_verbose(os->get_environment(ENV_VERBOSE) == "1" || os->is_stdout_verbose());
	if (!hook::pin_addon_module()) {
		log::warning("could not pin the addon DLL in memory; unloading it at exit may crash the editor");
	}

	// Settings resolve as: built-in defaults < project settings < environment. The store for
	// overrides is project settings, which Godot always writes into project.godot (hence into
	// project.binary in an export) and which are already parsed before CORE-level GDExtensions
	// initialise (main.cpp registers core extensions "after globals setup"). This works whether
	// or not the editor plugin is enabled and needs no separate config file to be exported.
	std::vector<std::string> read_warnings;
	g_state.addon_defaults = resolve_settings({}).settings; // built-in defaults, for the editor UI
	std::vector<SettingsLayer> layers;
	layers.push_back(project_layer(read_warnings));
	const String env_settings = os->get_environment(ENV_SETTINGS);
	if (!env_settings.is_empty()) {
		layers.push_back(parse_settings_override(std::string("environment variable ") + ENV_SETTINGS,
				env_settings.utf8().get_data(), read_warnings));
	}
	if (os->get_environment(ENV_DISABLE) == "1") {
		SettingsLayer env;
		env.name = std::string("environment variable ") + ENV_DISABLE;
		env.values[keys::ENABLED] = 0.0;
		layers.push_back(env);
	}
	g_state.resolved = resolve_settings(layers);
	g_state.resolved.warnings.insert(g_state.resolved.warnings.begin(), read_warnings.begin(), read_warnings.end());
	for (const std::string &warning : g_state.resolved.warnings) {
		log::warning("%s", warning.c_str());
	}
	const Settings &settings = g_state.resolved.settings;

	g_state.editor = engine->is_editor_hint();
	const Dictionary version = engine->get_version_info();
	int major = version.get("major", 0);
	int minor = version.get("minor", 0);
	int patch = version.get("patch", 0);
#ifdef DEBUG_ENABLED
	const String fake_version = os->get_environment(ENV_TEST_GODOT_VERSION);
	if (!fake_version.is_empty()) {
		const PackedStringArray parts = fake_version.split(".");
		major = parts.size() > 0 ? static_cast<int>(parts[0].to_int()) : 0;
		minor = parts.size() > 1 ? static_cast<int>(parts[1].to_int()) : 0;
		patch = parts.size() > 2 ? static_cast<int>(parts[2].to_int()) : 0;
	}
	g_state.hidden_extensions = split_list(os->get_environment(ENV_TEST_HIDE_EXTENSIONS));
	testing::set_faults(split_list(os->get_environment(ENV_TEST_FAULT)));
	g_state.test_upscaler = os->get_environment(ENV_TEST_UPSCALER).strip_edges().utf8().get_data();
	g_state.test_readback_frames = static_cast<int>(os->get_environment(ENV_TEST_READBACK_FRAMES).to_int());
#endif
	g_state.godot_version = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
	// A RenderingServer only exists at CORE level when the extension is loaded into a running
	// engine: the editor loads a freshly added addon while its Vulkan device already exists.
	g_state.late_load = engine->has_singleton("RenderingServer");

	if (!settings.enabled) {
		const auto it = g_state.resolved.source_of.find(keys::ENABLED);
		g_state.hook_reason = "disabled by " + (it != g_state.resolved.source_of.end() ? it->second : std::string("settings"));
	} else if (major != 4 || minor != 7) {
		// The hook depends on 4.7 internals (volk's loader call, device-creation order).
		g_state.hook_reason = "Godot " + g_state.godot_version + " is not supported: this build targets Godot 4.7.x";
	} else if (g_state.late_load) {
		g_state.hook_reason =
				"the addon was loaded after the Vulkan device was created (added while the editor was running): restart the editor to activate it";
	} else {
		const hook::InstallResult result = hook::install();
		g_state.hook_installed = result.installed;
		g_state.hook_reason = result.reason;
		g_state.patched_imports = result.patched;
	}

	reflex::ReflexFeature &reflex = reflex::ReflexFeature::get();
	if (!g_state.hook_installed) {
		g_state.reflex_reason = g_state.hook_reason;
	} else if (g_state.editor && !settings.enable_in_editor) {
		g_state.reflex_reason = std::string("inactive in the editor (games started from it use it; set ") +
				PROJECT_SETTING_PREFIX + keys::ENABLE_IN_EDITOR + " to use it in the editor itself)";
	} else {
		g_state.reflex_enabled = true;
	}
	reflex.configure(g_state.reflex_enabled, g_state.reflex_reason, settings, g_state.hidden_extensions);
	if (g_state.hook_installed) {
		vk::register_feature(&reflex);
		if (g_state.reflex_enabled) {
			vk::set_queue_hooks(&reflex);
		}
	}
	// Phase 2, DLSS. The runtime (Streamline + DLSS, fetched by the developer after accepting NVIDIA's
	// licences) is only loaded when its files are there and a GPU passes the checks; then the host's
	// Vulkan calls go through Streamline's interposer (see dlss/streamline.h). Otherwise nothing of
	// NVIDIA's is loaded and the API reports why DLSS is unavailable.
	if (g_state.hook_installed && !g_state.editor) {
		const dlss::RuntimeFiles runtime = dlss::check_runtime_files(dlss::default_runtime_dir());
		g_state.dlss_runtime_dir = dlss::to_utf8(runtime.dir);
		g_state.dlss_runtime_missing = runtime.missing;
		std::string no_backend;
		dlss::UpscalerProvider *backend = nullptr;
		if (!settings.dlss_enabled) {
			const auto it = g_state.resolved.source_of.find(keys::DLSS_ENABLED);
			no_backend = "disabled by " + (it != g_state.resolved.source_of.end() ? it->second : std::string("settings")) +
					" at startup (the NVIDIA runtime is only loaded when DLSS is enabled when the game starts)";
		} else if (!runtime.present()) {
			std::string missing;
			for (const std::string &name : runtime.missing) {
				missing += (missing.empty() ? "" : ", ") + name;
			}
			no_backend = "the NVIDIA DLSS runtime is not installed (" + g_state.dlss_runtime_dir + " lacks " + missing +
					"): get it with Project > Tools > \"Latency & Upscaling: Get the NVIDIA DLSS Runtime\" or tools/fetch_nvidia_sdk.py";
		} else {
			bool force = false;
#ifdef DEBUG_ENABLED
			// Tests: route through Streamline even without a DLSS-capable GPU (its pass-through path).
			force = os->get_environment(ENV_TEST_FORCE_STREAMLINE) == "1";
#endif
			backend = load_streamline(runtime.dir, force, no_backend);
		}
		g_state.dlss_streamline = backend != nullptr;
		dlss::DlssFeature::get().configure(backend, no_backend, g_state.hidden_extensions);
		vk::register_feature(&dlss::DlssFeature::get());
		if (backend != nullptr) {
			vk::enable_command_interception(); // DLSS evaluates at the upscaler's marker dispatch
		}
	}

	// Phase 2: recording an upscaler into Godot's command buffer needs the vkCmd* hooks, which
	// must be in place before Godot resolves its Vulkan functions, i.e. now. Only the native test
	// backend uses it so far (DLSS joins in the DLSS step).
	bool native_recording = false;
#ifdef DEBUG_ENABLED
	native_recording = g_state.test_upscaler == "native" && !g_state.editor;
#endif
	upscale::UpscaleFeature &upscale_feature = upscale::UpscaleFeature::get();
	upscale_feature.configure(native_recording, g_state.hidden_extensions);
	if (g_state.hook_installed && native_recording) {
		vk::enable_command_interception();
		vk::register_feature(&upscale_feature);
	}
	if (g_state.hook_installed) {
		log::info("Vulkan hook installed (%d import slot(s)); Reflex %s", static_cast<int>(g_state.patched_imports.size()),
				g_state.reflex_enabled ? "enabled" : g_state.reflex_reason.c_str());
	} else if (settings.enabled) {
		log::info("Vulkan hook not installed: %s", g_state.hook_reason.c_str());
	}
}

} // namespace lu
