#include "dlss/streamline.h"

#include "core/log.h"
#include "dlss/sl_signature.h"
#include "dlss/runtime_files.h"

#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers.h>

#include <cstdio>
#include <cstring>
#include <deque>

namespace lu::dlss {

struct Streamline::Api {
	PFun_slInit *Init = nullptr;
	PFun_slShutdown *Shutdown = nullptr;
	PFun_slIsFeatureSupported *IsFeatureSupported = nullptr;
	PFun_slGetFeatureFunction *GetFeatureFunction = nullptr;
	PFun_slGetFeatureVersion *GetFeatureVersion = nullptr;
	PFun_slGetNewFrameToken *GetNewFrameToken = nullptr;
	PFun_slSetConstants *SetConstants = nullptr;
	PFun_slSetTagForFrame *SetTagForFrame = nullptr;
	PFun_slEvaluateFeature *EvaluateFeature = nullptr;
	PFun_slFreeResources *FreeResources = nullptr;
	// DLSS feature functions (resolved once the device exists).
	PFun_slDLSSGetOptimalSettings *DLSSGetOptimalSettings = nullptr;
	PFun_slDLSSSetOptions *DLSSSetOptions = nullptr;
};

namespace {

std::mutex g_log_mutex;
std::deque<std::string> g_log_tail;

void sl_log_callback(sl::LogType type, const char *message) {
	streamline_log(static_cast<int>(type), message);
}

sl::DLSSMode dlss_mode(upscale::Quality mode) {
	switch (mode) {
		case upscale::Quality::DLAA:
			return sl::DLSSMode::eDLAA;
		case upscale::Quality::Quality:
			return sl::DLSSMode::eMaxQuality;
		case upscale::Quality::Balanced:
			return sl::DLSSMode::eBalanced;
		case upscale::Quality::Performance:
			return sl::DLSSMode::eMaxPerformance;
		case upscale::Quality::UltraPerformance:
			return sl::DLSSMode::eUltraPerformance;
		case upscale::Quality::Auto:
			break;
	}
	return sl::DLSSMode::eMaxQuality;
}

sl::DLSSPreset dlss_preset(upscale::Preset preset) {
	switch (preset) {
		case upscale::Preset::J:
			return sl::DLSSPreset::ePresetJ;
		case upscale::Preset::K:
			return sl::DLSSPreset::ePresetK;
		case upscale::Preset::L:
			return sl::DLSSPreset::ePresetL;
		case upscale::Preset::M:
			return sl::DLSSPreset::ePresetM;
		case upscale::Preset::Default:
			break;
	}
	return sl::DLSSPreset::eDefault;
}

sl::float4x4 to_sl(const float m[4][4]) {
	sl::float4x4 out;
	for (uint32_t r = 0; r < 4; ++r) {
		out.setRow(r, sl::float4(m[r][0], m[r][1], m[r][2], m[r][3]));
	}
	return out;
}

sl::float3 to_sl(const float v[3]) {
	return sl::float3(v[0], v[1], v[2]);
}

sl::Resource to_sl(const EvaluateImage &image) {
	sl::Resource r(sl::ResourceType::eTex2d, image.image, nullptr, image.view, static_cast<uint32_t>(image.layout));
	r.width = image.width;
	r.height = image.height;
	r.nativeFormat = static_cast<uint32_t>(image.format);
	r.mipLevels = 1;
	r.arrayLayers = 1;
	r.flags = 0;
	r.usage = image.usage;
	return r;
}

std::string result_text(const char *what, sl::Result r) {
	return std::string(what) + " failed: " + sl::getResultAsStr(r);
}

// Why Streamline says a GPU cannot run DLSS, in the addon's words.
std::string unsupported_reason(sl::Result r) {
	switch (r) {
		case sl::Result::eErrorNoSupportedAdapterFound:
		case sl::Result::eErrorAdapterNotSupported:
			return "NVIDIA's DLSS runtime does not support this GPU (DLSS needs an NVIDIA RTX GPU)";
		case sl::Result::eErrorDriverOutOfDate:
			return "NVIDIA's DLSS runtime needs a newer NVIDIA driver: update the driver";
		case sl::Result::eErrorOSOutOfDate:
			return "NVIDIA's DLSS runtime needs a newer Windows version";
		case sl::Result::eErrorOSDisabledHWS:
			return "NVIDIA's DLSS runtime needs hardware-accelerated GPU scheduling";
		case sl::Result::eErrorFeatureMissing:
		case sl::Result::eErrorFeatureNotSupported:
			return "the DLSS plugin of the NVIDIA runtime is missing or failed to load";
		default:
			return std::string("NVIDIA's DLSS runtime reports ") + sl::getResultAsStr(r);
	}
}

} // namespace

void streamline_log(int type, const char *message) {
	const sl::LogType log_type = static_cast<sl::LogType>(type);
	if (message == nullptr || log_type == sl::LogType::eInfo) {
		return;
	}
	std::string line = message;
	while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
		line.pop_back();
	}
	{
		std::lock_guard<std::mutex> lock(g_log_mutex);
		g_log_tail.push_back(line);
		while (g_log_tail.size() > 20) {
			g_log_tail.pop_front();
		}
	}
	// Streamline reports expected conditions as errors too (no OTA updater, no native optical flow on
	// this GPU), so its lines only reach the verbose log and the status; the addon reports the
	// failures that matter itself (get_dlss_status, upscaling.dlss_frames.last_error).
	log::info("Streamline: %s", line.c_str());
}

Streamline &Streamline::get() {
	static Streamline *instance = new Streamline(); // never destroyed: Vulkan calls may reach it during teardown
	return *instance;
}

Streamline::Api &Streamline::api() const {
	static Api api;
	return api;
}

bool Streamline::load(const std::wstring &runtime_dir, std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (initialized_) {
		return true;
	}
	std::wstring dir = runtime_dir;
	while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
		dir.pop_back();
	}
	const std::wstring interposer = dir + L"\\sl.interposer.dll";
	if (!verify_nvidia_signature(interposer)) {
		reason = "sl.interposer.dll in " + to_utf8(dir) + " does not carry NVIDIA's signature: fetch the runtime again";
		return false;
	}
	HMODULE module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (module == nullptr) {
		char buffer[160];
		std::snprintf(buffer, sizeof(buffer), "sl.interposer.dll could not be loaded (Windows error %lu)", GetLastError());
		reason = buffer;
		return false;
	}
	Api &a = api();
	auto proc = [&](auto &fn, const char *name) {
		fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void *>(GetProcAddress(module, name)));
		return fn != nullptr;
	};
	const bool complete = proc(a.Init, "slInit") && proc(a.Shutdown, "slShutdown") &&
			proc(a.IsFeatureSupported, "slIsFeatureSupported") && proc(a.GetFeatureFunction, "slGetFeatureFunction") &&
			proc(a.GetFeatureVersion, "slGetFeatureVersion") && proc(a.GetNewFrameToken, "slGetNewFrameToken") &&
			proc(a.SetConstants, "slSetConstants") && proc(a.SetTagForFrame, "slSetTagForFrame") &&
			proc(a.EvaluateFeature, "slEvaluateFeature") && proc(a.FreeResources, "slFreeResources");
	if (!complete) {
		FreeLibrary(module);
		a = Api();
		reason = "sl.interposer.dll lacks a Streamline function this addon needs (not Streamline 2.14?)";
		return false;
	}

	static const sl::Feature features[] = { sl::kFeatureDLSS };
	static std::wstring plugin_path;
	plugin_path = dir;
	static const wchar_t *paths[1];
	paths[0] = plugin_path.c_str();
	sl::Preferences pref{};
	pref.showConsole = false;
	pref.logLevel = sl::LogLevel::eDefault;
	pref.pathsToPlugins = paths;
	pref.numPathsToPlugins = 1;
	pref.pathToLogsAndData = nullptr; // no log files next to the game
	pref.logMessageCallback = &sl_log_callback;
	// Interposer mode (not manual hooking): Streamline's vkCreateInstance / vkCreateDevice and its
	// present path run. No command-list state tracking (the layer re-binds Godot's state), frame-based
	// tagging, and no over-the-air updates: only the pinned, verified files run.
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = 1;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "Godot 4.7";
	pref.projectId = "0a1c5b38-6e3f-4d7a-9c21-4b8e5f2d7a90"; // godot-latency-upscaling
	pref.renderAPI = sl::RenderAPI::eVulkan;
	const sl::Result r = a.Init(pref, sl::kSDKVersion);
	if (r != sl::Result::eOk) {
		FreeLibrary(module);
		a = Api();
		reason = result_text("slInit", r);
		return false;
	}
	module_ = module;
	initialized_ = true;
	return true;
}

void Streamline::unload() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		return;
	}
	api().Shutdown();
	FreeLibrary(module_);
	module_ = nullptr;
	initialized_ = false;
	api() = Api();
}

bool Streamline::loaded() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return initialized_;
}

HMODULE Streamline::interposer() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return module_;
}

bool Streamline::adapter_supported(VkPhysicalDevice device, std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		reason = "Streamline is not loaded";
		return false;
	}
	sl::AdapterInfo adapter{};
	adapter.vkPhysicalDevice = device;
	const sl::Result r = api().IsFeatureSupported(sl::kFeatureDLSS, adapter);
	if (r != sl::Result::eOk) {
		reason = unsupported_reason(r);
		return false;
	}
	return true;
}

bool Streamline::request_instance_extensions(std::vector<std::string> &, std::string &reason) {
	// Streamline's vkCreateInstance adds its own.
	if (!loaded()) {
		reason = "Streamline is not loaded";
		return false;
	}
	return true;
}

bool Streamline::request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &, std::string &reason) {
	// Streamline's vkCreateDevice adds DLSS's extensions, features and queues itself.
	return adapter_supported(caps.handle, reason);
}

bool Streamline::initialize(const vk::DeviceContext &, std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		reason = "Streamline is not loaded";
		return false;
	}
	Api &a = api();
	void *optimal = nullptr;
	void *set_options = nullptr;
	sl::Result r = a.GetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", optimal);
	if (r == sl::Result::eOk) {
		r = a.GetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", set_options);
	}
	if (r != sl::Result::eOk || optimal == nullptr || set_options == nullptr) {
		reason = result_text("resolving the DLSS functions", r);
		return false;
	}
	a.DLSSGetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings *>(optimal);
	a.DLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions *>(set_options);
	sl::FeatureVersion version{};
	if (a.GetFeatureVersion(sl::kFeatureDLSS, version) == sl::Result::eOk) {
		char buffer[96];
		std::snprintf(buffer, sizeof(buffer), "DLSS %u.%u.%u through Streamline %u.%u.%u", version.versionNGX.major,
				version.versionNGX.minor, version.versionNGX.build, version.versionSL.major, version.versionSL.minor,
				version.versionSL.build);
		version_ = buffer;
	}
	device_ready_ = true;
	return true;
}

void Streamline::shutdown() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		return;
	}
	// Before the device goes away (Streamline's rule). The module stays loaded: Vulkan calls of the
	// teardown still go through it.
	api().Shutdown();
	initialized_ = false;
	device_ready_ = false;
}

bool Streamline::optimal_render_size(ImageSize target, upscale::Quality mode, ImageSize &render) const {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!device_ready_) {
		return false;
	}
	sl::DLSSOptions options{};
	options.mode = dlss_mode(mode);
	options.outputWidth = target.width;
	options.outputHeight = target.height;
	sl::DLSSOptimalSettings settings{};
	if (api().DLSSGetOptimalSettings(options, settings) != sl::Result::eOk || settings.optimalRenderWidth == 0) {
		return false;
	}
	render.width = settings.optimalRenderWidth;
	render.height = settings.optimalRenderHeight;
	return true;
}

bool Streamline::evaluate(VkCommandBuffer command_buffer, const EvaluateInputs &in, std::string &error) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!device_ready_) {
		error = "DLSS is not initialised on the device";
		return false;
	}
	Api &a = api();
	sl::FrameToken *token = nullptr;
	uint32_t frame_index = static_cast<uint32_t>(frame_++);
	sl::Result r = a.GetNewFrameToken(token, &frame_index);
	if (r != sl::Result::eOk || token == nullptr) {
		error = result_text("slGetNewFrameToken", r);
		return false;
	}
	const sl::ViewportHandle viewport(in.viewport);

	sl::DLSSOptions options{};
	options.mode = dlss_mode(in.mode);
	options.outputWidth = in.output.width;
	options.outputHeight = in.output.height;
	options.colorBuffersHDR = sl::Boolean::eTrue; // linear scene colour, before tonemapping
	options.useAutoExposure = sl::Boolean::eTrue; // Godot has no exposure texture without auto exposure
	const sl::DLSSPreset preset = dlss_preset(in.preset);
	options.dlaaPreset = preset;
	options.qualityPreset = preset;
	options.balancedPreset = preset;
	options.performancePreset = preset;
	options.ultraPerformancePreset = preset;
	options.ultraQualityPreset = preset;
	r = a.DLSSSetOptions(viewport, options);
	if (r != sl::Result::eOk) {
		error = result_text("slDLSSSetOptions", r);
		return false;
	}

	sl::Constants c{};
	c.cameraViewToClip = to_sl(in.view_to_clip);
	c.clipToCameraView = to_sl(in.clip_to_view);
	sl::float4x4 identity;
	for (uint32_t i = 0; i < 4; ++i) {
		identity.setRow(i, sl::float4(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f, i == 3 ? 1.0f : 0.0f));
	}
	c.clipToLensClip = identity;
	c.clipToPrevClip = to_sl(in.clip_to_prev_clip);
	c.prevClipToClip = to_sl(in.prev_clip_to_clip);
	c.jitterOffset = sl::float2(in.jitter_x, in.jitter_y);
	c.mvecScale = sl::float2(in.mvec_scale_x, in.mvec_scale_y);
	c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
	c.cameraPos = to_sl(in.camera_pos);
	c.cameraUp = to_sl(in.camera_up);
	c.cameraRight = to_sl(in.camera_right);
	c.cameraFwd = to_sl(in.camera_forward);
	c.cameraNear = in.camera_near;
	c.cameraFar = in.camera_far;
	c.cameraFOV = in.camera_fov_y;
	c.cameraAspectRatio = in.camera_aspect;
	c.depthInverted = in.depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	c.cameraMotionIncluded = sl::Boolean::eTrue;
	c.motionVectors3D = sl::Boolean::eFalse;
	c.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	c.orthographicProjection = in.orthographic ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	c.motionVectorsDilated = sl::Boolean::eFalse;
	c.motionVectorsJittered = sl::Boolean::eFalse;
	r = a.SetConstants(c, *token, viewport);
	if (r != sl::Result::eOk) {
		error = result_text("slSetConstants", r);
		return false;
	}

	sl::Resource color = to_sl(in.color);
	sl::Resource depth = to_sl(in.depth);
	sl::Resource velocity = to_sl(in.velocity);
	sl::Resource output = to_sl(in.output);
	sl::ResourceTag tags[] = {
		sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag(&velocity, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent),
	};
	r = a.SetTagForFrame(*token, viewport, tags, 4, command_buffer);
	if (r != sl::Result::eOk) {
		error = result_text("slSetTagForFrame", r);
		return false;
	}
	const sl::BaseStructure *inputs[] = { &viewport };
	r = a.EvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, command_buffer);
	if (r != sl::Result::eOk) {
		error = result_text("slEvaluateFeature", r);
		return false;
	}
	return true;
}

std::string Streamline::version() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return version_;
}

std::vector<std::string> Streamline::log_tail() const {
	std::lock_guard<std::mutex> lock(g_log_mutex);
	return std::vector<std::string>(g_log_tail.begin(), g_log_tail.end());
}

} // namespace lu::dlss
