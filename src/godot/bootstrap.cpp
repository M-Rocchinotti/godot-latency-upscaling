#include "godot/bootstrap.h"

#include "core/log.h"
#include "hook/loader_hook.h"
#include "reflex/reflex_ll2.h"
#include "vk/dispatch.h"

#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace lu {
namespace {

BootState g_state;

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

SettingsLayer config_layer(std::vector<std::string> &warnings) {
	SettingsLayer layer;
	layer.name = ADDON_CONFIG_PATH;
	Ref<ConfigFile> config;
	config.instantiate();
	const Error err = config->load(ADDON_CONFIG_PATH);
	if (err != OK) {
		// Built-in defaults are identical to the shipped file, so a missing file (e.g. an export
		// without the editor plugin's export hook) changes nothing.
		log::info("%s not loaded (error %d); using built-in defaults", ADDON_CONFIG_PATH, static_cast<int>(err));
		return layer;
	}
	for (const char *key : all_setting_keys()) {
		const String full(key);
		const String section = full.get_slice("/", 0);
		const String name = full.get_slice("/", 1);
		if (!config->has_section_key(section, name)) {
			continue;
		}
		double number = 0.0;
		if (variant_to_number(config->get_value(section, name), number)) {
			layer.values[key] = number;
		} else {
			warnings.push_back(std::string(ADDON_CONFIG_PATH) + ": '" + key + "' is not a boolean or number");
		}
	}
	return layer;
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

	// Settings: built-in defaults < addon config < project settings < environment.
	std::vector<std::string> read_warnings;
	std::vector<SettingsLayer> layers;
	layers.push_back(config_layer(read_warnings));
	g_state.addon_defaults = resolve_settings({ layers.front() }).settings;
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
	if (g_state.hook_installed) {
		log::info("Vulkan hook installed (%d import slot(s)); Reflex %s", static_cast<int>(g_state.patched_imports.size()),
				g_state.reflex_enabled ? "enabled" : g_state.reflex_reason.c_str());
	} else if (settings.enabled) {
		log::info("Vulkan hook not installed: %s", g_state.hook_reason.c_str());
	}
}

} // namespace lu
