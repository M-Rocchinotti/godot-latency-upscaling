#include "core/settings.h"
#include "hook/loader_name.h"
#include "test_framework.h"

using namespace lu;

TEST(settings_builtin_defaults) {
	const ResolvedSettings r = resolve_settings({});
	CHECK(r.settings.enabled);
	CHECK(!r.settings.enable_in_editor);
	CHECK(r.settings.reflex_mode == ReflexMode::On); // NVIDIA's QA checklist: default On
	CHECK_EQ(r.settings.reflex_fps_limit, 0.0);
	CHECK(r.settings.reflex_flash_indicator);
	CHECK(!r.settings.overlay_enabled);
	CHECK(r.settings.dlss_enabled); // NVIDIA UI guidelines: on (Auto) where DLSS is available
	CHECK(r.settings.dlss_quality == lu::upscale::Quality::Auto);
	CHECK(r.settings.dlss_preset == lu::upscale::Preset::Default);
	CHECK_EQ(r.settings.dlss_cut_angle_degrees, 60.0);
	CHECK_EQ(r.settings.dlss_cut_distance, 10.0);
	CHECK(!r.settings.fg_enabled); // Phase 3: Frame Generation is guarded off by default
	CHECK(r.settings.fg_mode == FrameGenerationMode::Off);
	CHECK_EQ(r.settings.fg_multiplier, 0); // 0 = Auto
	CHECK(r.warnings.empty());
}

TEST(settings_fg_values_are_validated) {
	SettingsLayer good{ "project setting", { { keys::FG_ENABLED, 1.0 }, { keys::FG_MODE, 1.0 }, { keys::FG_MULTIPLIER, 3.0 } } };
	ResolvedSettings r = resolve_settings({ good });
	CHECK(r.settings.fg_enabled);
	CHECK(r.settings.fg_mode == FrameGenerationMode::On);
	CHECK_EQ(r.settings.fg_multiplier, 3);
	CHECK(r.warnings.empty());
	// 0 (Auto), 2, 3 and 4 are the only valid multipliers; mode is 0 or 1.
	SettingsLayer bad{ "env", { { keys::FG_MODE, 2.0 }, { keys::FG_MULTIPLIER, 1.0 }, { keys::FG_ENABLED, 0.5 } } };
	r = resolve_settings({ good, bad });
	CHECK(r.settings.fg_mode == FrameGenerationMode::On); // bad values ignored, the good layer stands
	CHECK_EQ(r.settings.fg_multiplier, 3);
	CHECK(r.settings.fg_enabled);
	CHECK_EQ(r.warnings.size(), size_t(3));
	SettingsLayer bad_multiplier{ "env", { { keys::FG_MULTIPLIER, 5.0 } } };
	r = resolve_settings({ bad_multiplier });
	CHECK_EQ(r.settings.fg_multiplier, 0); // 5 rejected, the default stands
	CHECK_EQ(r.warnings.size(), size_t(1));
	CHECK(is_valid_mfg_multiplier(0) && is_valid_mfg_multiplier(2) && is_valid_mfg_multiplier(3) && is_valid_mfg_multiplier(4));
	CHECK(!is_valid_mfg_multiplier(1) && !is_valid_mfg_multiplier(5) && !is_valid_mfg_multiplier(-1));
}

TEST(settings_cut_thresholds_are_validated) {
	SettingsLayer good{ "project setting", { { keys::DLSS_CUT_ANGLE, 45.0 }, { keys::DLSS_CUT_DISTANCE, 0.0 } } };
	ResolvedSettings r = resolve_settings({ good });
	CHECK_EQ(r.settings.dlss_cut_angle_degrees, 45.0);
	CHECK_EQ(r.settings.dlss_cut_distance, 0.0); // 0 = translation never counts as a cut
	CHECK(r.warnings.empty());
	SettingsLayer edge{ "project setting", { { keys::DLSS_CUT_ANGLE, 180.0 }, { keys::DLSS_CUT_DISTANCE, 2.5 } } };
	r = resolve_settings({ edge });
	CHECK_EQ(r.settings.dlss_cut_angle_degrees, 180.0);
	CHECK_EQ(r.settings.dlss_cut_distance, 2.5);
	SettingsLayer bad{ "env", { { keys::DLSS_CUT_ANGLE, 181.0 }, { keys::DLSS_CUT_DISTANCE, -1.0 } } };
	r = resolve_settings({ good, bad });
	CHECK_EQ(r.settings.dlss_cut_angle_degrees, 45.0); // bad values ignored
	CHECK_EQ(r.settings.dlss_cut_distance, 0.0);
	CHECK_EQ(r.warnings.size(), size_t(2));
	SettingsLayer negative_angle{ "env", { { keys::DLSS_CUT_ANGLE, -5.0 } } };
	r = resolve_settings({ negative_angle });
	CHECK_EQ(r.settings.dlss_cut_angle_degrees, 60.0);
	CHECK_EQ(r.warnings.size(), size_t(1));
}

TEST(settings_dlss_values_are_validated) {
	SettingsLayer good{ "project setting", { { keys::DLSS_QUALITY, 4.0 }, { keys::DLSS_PRESET, 2.0 }, { keys::DLSS_ENABLED, 0.0 } } };
	ResolvedSettings r = resolve_settings({ good });
	CHECK(r.settings.dlss_quality == lu::upscale::Quality::Performance);
	CHECK(r.settings.dlss_preset == lu::upscale::Preset::K);
	CHECK(!r.settings.dlss_enabled);
	CHECK(r.warnings.empty());
	SettingsLayer bad{ "env", { { keys::DLSS_QUALITY, 6.0 }, { keys::DLSS_PRESET, 1.5 }, { keys::DLSS_ENABLED, 2.0 } } };
	r = resolve_settings({ good, bad });
	CHECK(r.settings.dlss_quality == lu::upscale::Quality::Performance); // bad values ignored
	CHECK(r.settings.dlss_preset == lu::upscale::Preset::K);
	CHECK(!r.settings.dlss_enabled);
	CHECK_EQ(r.warnings.size(), size_t(3));
}

TEST(settings_later_layers_win_and_sources_are_recorded) {
	SettingsLayer cfg{ "cfg", { { keys::REFLEX_MODE, 2.0 }, { keys::REFLEX_FPS_LIMIT, 120.0 } } };
	SettingsLayer project{ "project setting", { { keys::REFLEX_MODE, 0.0 } } };
	const ResolvedSettings r = resolve_settings({ cfg, project });
	CHECK(r.settings.reflex_mode == ReflexMode::Off);
	CHECK_EQ(r.settings.reflex_fps_limit, 120.0);
	CHECK_EQ(r.source_of.at(keys::REFLEX_MODE), std::string("project setting"));
	CHECK_EQ(r.source_of.at(keys::REFLEX_FPS_LIMIT), std::string("cfg"));
}

TEST(settings_environment_kill_switch_wins) {
	SettingsLayer project{ "project setting", { { keys::ENABLED, 1.0 } } };
	SettingsLayer env{ "environment", { { keys::ENABLED, 0.0 } } };
	const ResolvedSettings r = resolve_settings({ project, env });
	CHECK(!r.settings.enabled);
	CHECK_EQ(r.source_of.at(keys::ENABLED), std::string("environment"));
}

TEST(settings_invalid_values_are_ignored_with_a_warning) {
	SettingsLayer bad{ "cfg", { { keys::REFLEX_MODE, 3.0 }, { keys::REFLEX_FPS_LIMIT, 5.0 }, { keys::ENABLED, 0.5 },
									  { "reflex/unknown", 1.0 } } };
	const ResolvedSettings r = resolve_settings({ bad });
	CHECK(r.settings.reflex_mode == ReflexMode::On);
	CHECK_EQ(r.settings.reflex_fps_limit, 0.0);
	CHECK(r.settings.enabled);
	CHECK_EQ(r.warnings.size(), static_cast<size_t>(4));
	CHECK(r.source_of.empty());
}

TEST(settings_environment_override_is_parsed) {
	std::vector<std::string> warnings;
	const SettingsLayer layer = parse_settings_override("env", " general/enable_in_editor = true ;reflex/mode=2;; reflex/fps_limit=144.5 ;overlay/enabled=false", warnings);
	CHECK(warnings.empty());
	CHECK_EQ(layer.values.size(), static_cast<size_t>(4));
	CHECK_EQ(layer.values.at(keys::ENABLE_IN_EDITOR), 1.0);
	CHECK_EQ(layer.values.at(keys::REFLEX_MODE), 2.0);
	CHECK_EQ(layer.values.at(keys::REFLEX_FPS_LIMIT), 144.5);
	CHECK_EQ(layer.values.at(keys::OVERLAY_ENABLED), 0.0);
	const ResolvedSettings r = resolve_settings({ layer });
	CHECK(r.settings.enable_in_editor);
	CHECK(r.settings.reflex_mode == ReflexMode::OnBoost);
}

TEST(settings_environment_override_rejects_malformed_entries) {
	std::vector<std::string> warnings;
	const SettingsLayer layer = parse_settings_override("env", "reflex/mode;=1;reflex/fps_limit=fast;general/enabled=0", warnings);
	CHECK_EQ(warnings.size(), static_cast<size_t>(3));
	CHECK_EQ(layer.values.size(), static_cast<size_t>(1));
	CHECK_EQ(layer.values.at(keys::ENABLED), 0.0);
	CHECK(parse_settings_override("env", "", warnings).values.empty());
}

TEST(settings_fps_limit_conversion) {
	CHECK_EQ(fps_limit_to_interval_us(0.0), 0u);
	CHECK_EQ(fps_limit_to_interval_us(60.0), 16667u);
	CHECK_EQ(fps_limit_to_interval_us(1000.0), 1000u);
	CHECK_EQ(fps_limit_to_interval_us(10.0), 100000u);
	CHECK_EQ(fps_limit_to_interval_us(9.0), 0u); // invalid: below the minimum
	CHECK_EQ(fps_limit_to_interval_us(-60.0), 0u);
	CHECK(is_valid_fps_limit(0.0));
	CHECK(!is_valid_fps_limit(1001.0));
	CHECK(!is_valid_fps_limit(std::nan("")));
}

TEST(loader_name_matches_only_the_vulkan_loader) {
	using hook::is_vulkan_loader_name;
	CHECK(is_vulkan_loader_name("vulkan-1.dll"));
	CHECK(is_vulkan_loader_name("VULKAN-1.DLL"));
	CHECK(is_vulkan_loader_name("vulkan-1"));
	CHECK(is_vulkan_loader_name("C:\\Windows\\System32\\vulkan-1.dll"));
	CHECK(is_vulkan_loader_name("C:/games/bin/Vulkan-1.dll"));
	CHECK(is_vulkan_loader_name(L"vulkan-1.dll"));
	CHECK(is_vulkan_loader_name(L"D:\\x\\VULKAN-1.dll"));
	CHECK(!is_vulkan_loader_name(static_cast<const char *>(nullptr)));
	CHECK(!is_vulkan_loader_name(""));
	CHECK(!is_vulkan_loader_name("vulkan-1.dll.bak"));
	CHECK(!is_vulkan_loader_name("myvulkan-1.dll"));
	CHECK(!is_vulkan_loader_name("vulkan-1.dl"));
	CHECK(!is_vulkan_loader_name("d3d12.dll"));
	CHECK(!is_vulkan_loader_name(L"sl.interposer.dll"));
}
