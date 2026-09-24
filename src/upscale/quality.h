// Upscaling quality modes and presets (Phase 2), as NVIDIA defines them for DLSS.
//
// Scaling ratios (output / render, per axis): DLSS Programming Guide, section 3.7.1
// ("Ultra Performance 3.0, Performance 2.0, Balanced 1.724, Quality 1.5, DLAA 1.0"). Once NGX is
// loaded, its optimal-settings query replaces these defaults.
// "Auto": RTX UI Developer Guidelines, DLSS Super Resolution table: output below 1920x1080 ->
// DLSS off; up to 2560x1440 -> Quality; up to 3840x2160 -> Performance; above -> Ultra Performance.
// Presets: DLSS Programming Guide 3.12 (Default, J, K, L, M; E/F deprecated, G/H/I reserved).
// Sharpening is deprecated in DLSS (guide 3.11), so there is no sharpness setting.
//
// Pure C++, unit-tested.
#pragma once

#include <cstdint>

namespace lu::upscale {

enum class Quality : int { Auto = 0, DLAA = 1, Quality = 2, Balanced = 3, Performance = 4, UltraPerformance = 5 };
inline constexpr int QUALITY_COUNT = 6;

enum class Preset : int { Default = 0, J = 1, K = 2, L = 3, M = 4 };
inline constexpr int PRESET_COUNT = 5;

struct Size {
	uint32_t width = 0;
	uint32_t height = 0;
};

// The concrete mode "Auto" stands for at this output size. Returns false when NVIDIA recommends
// DLSS off (output below 1920x1080); `mode` is then left unchanged.
bool resolve_auto(Size output, Quality &mode);

// Output / render ratio per axis of a concrete mode (Auto must be resolved first; it maps to 1.0).
double scaling_ratio(Quality mode);

// The render scale Godot's Viewport.scaling_3d_scale takes for a concrete mode (1 / ratio).
float render_scale(Quality mode);

// Render size for an output size and a concrete mode (rounded, at least 1x1). Godot derives its
// own internal size from render_scale(); this is the value NGX's defaults correspond to.
Size render_size(Size output, Quality mode);

const char *quality_name(Quality mode); // "Auto", "DLAA", "Quality", ...
const char *preset_name(Preset preset); // "Default", "J", ...

} // namespace lu::upscale
