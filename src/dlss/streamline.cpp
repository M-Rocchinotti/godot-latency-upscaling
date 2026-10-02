#include "dlss/streamline.h"

#include "core/log.h"
#include "dlss/fg_feature.h"
#include "dlss/sl_signature.h"
#include "dlss/runtime_files.h"
#include "reflex/reflex_ll2.h"
#include "reflex/sl_reflex_device.h"

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <type_traits>

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
	// Frame Generation feature functions (Phase 3; resolved when FG is permitted and the device
	// exists, but not yet used to drive rendering — a later milestone switches Reflex and evaluates).
	PFun_slDLSSGSetOptions *DLSSGSetOptions = nullptr;
	PFun_slDLSSGGetState *DLSSGGetState = nullptr;
	PFun_slReflexSetOptions *ReflexSetOptions = nullptr;
	PFun_slReflexSleep *ReflexSleep = nullptr;
	PFun_slReflexGetState *ReflexGetState = nullptr;
	PFun_slPCLSetMarker *PCLSetMarker = nullptr;
	PFun_slPCLSetOptions *PCLSetOptions = nullptr;
	PFun_slPCLGetState *PCLGetState = nullptr;
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

std::vector<uint32_t> streamline_features_to_load(bool fg_permitted) {
	std::vector<uint32_t> features = { sl::kFeatureDLSS };
	if (fg_permitted) {
		// DLSS Frame Generation plus the Reflex and PC-latency plugins Streamline requires for it.
		features.push_back(sl::kFeatureDLSS_G);
		features.push_back(sl::kFeatureReflex);
		features.push_back(sl::kFeaturePCL);
	}
	return features;
}

Streamline &Streamline::get() {
	static Streamline *instance = new Streamline(); // never destroyed: Vulkan calls may reach it during teardown
	return *instance;
}

Streamline::Api &Streamline::api() const {
	static Api api;
	return api;
}

bool Streamline::load(const std::wstring &runtime_dir, const std::string &project_id, bool fg_permitted, std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	fg_permitted_ = fg_permitted;
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

	// DLSS Super Resolution always; the Frame Generation plugins too when fg is permitted (Phase 3,
	// off by default). Static storage: Streamline may keep the pointer past slInit. sl::Feature is
	// uint32_t, so the pure helper's vector is the same type.
	static std::vector<sl::Feature> features;
	features = streamline_features_to_load(fg_permitted);
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
	pref.featuresToLoad = features.data();
	pref.numFeaturesToLoad = static_cast<uint32_t>(features.size());
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "Godot 4.7";
	static std::string project;
	project = project_id; // Streamline may keep the pointer
	pref.projectId = project.c_str(); // per project: latency_upscaling/dlss/project_id or derived from its name
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
	device_ready_ = false;
	fg_runtime_ready_ = false;
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

bool Streamline::request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) {
	// In interposer mode Streamline's own vkCreateDevice adds everything its features need downstream
	// of the addon: DLSS Super Resolution's and, when sl.dlss_g is loaded, Frame Generation's device
	// extensions, features and queues — including the optical-flow queue and VK_NV_optical_flow. The
	// addon must NOT add the optical-flow queue itself.
	if (!adapter_supported(caps.handle, reason)) {
		return false;
	}
	// But Godot 4.7 blanket-creates one queue in every family, including the optical-flow family, and
	// the interposer appends its own there in Frame Generation mode: the family has one queue, so they
	// collide and vkCreateDevice is invalid (the RTX 5080 crash). Ask the patch to drop Godot's
	// optical-flow-family queue so the interposer's is the only one. The device patch no-ops when no
	// optical-flow family exists; the dispatch layer remaps Godot's vkGetDeviceQueue for it.
	bool fg_permitted = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		fg_permitted = fg_permitted_;
	}
	if (fg_permitted) {
		request.drop_optical_flow_queue = true;
	}
	return true;
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
	// Phase 3: resolve the Frame Generation plugins' functions now that the device is set (required by
	// slGetFeatureFunction). Guarded off: this never fails DLSS and nothing drives rendering with them.
	if (fg_permitted_) {
		resolve_frame_generation_functions();
		if (fg_runtime_ready_) {
			// Hand the resolved sl.reflex / sl.pcl functions to the Reflex feature so it drives Reflex
			// through Streamline (Frame Generation needs that). The VK_NV_low_latency2 path was kept out
			// of the device (ReflexFeature::request_device declined it when fg was permitted).
			reflex::SlReflexApi reflex_api;
			reflex_api.get_new_frame_token = a.GetNewFrameToken;
			reflex_api.set_options = a.ReflexSetOptions;
			reflex_api.sleep = a.ReflexSleep;
			reflex_api.get_state = a.ReflexGetState;
			reflex_api.set_marker = a.PCLSetMarker;
			reflex::ReflexFeature::get().enable_streamline_reflex(reflex_api);

			// Report the real multiplier ceiling from the runtime (e.g. numFramesToGenerateMax 5 -> 6x
			// on an RTX 50 series), overriding the architecture guess. Best-effort: on any error the
			// guess stands.
			if (a.DLSSGGetState != nullptr) {
				sl::DLSSGState state{};
				const sl::ViewportHandle viewport(0);
				if (a.DLSSGGetState(viewport, state, nullptr) == sl::Result::eOk && state.numFramesToGenerateMax > 0) {
					FgFeature::get().set_runtime_max_multiplier(fg_multiplier_from_frames_to_generate(state.numFramesToGenerateMax));
				}
			}
		}
	}
	return true;
}

void Streamline::resolve_frame_generation_functions() {
	Api &a = api();
	auto get = [&a](sl::Feature feature, const char *name, auto &slot) -> bool {
		void *function = nullptr;
		if (a.GetFeatureFunction(feature, name, function) != sl::Result::eOk || function == nullptr) {
			return false;
		}
		slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(function);
		return true;
	};
	const bool ok = get(sl::kFeatureDLSS_G, "slDLSSGSetOptions", a.DLSSGSetOptions) &&
			get(sl::kFeatureDLSS_G, "slDLSSGGetState", a.DLSSGGetState) &&
			get(sl::kFeatureReflex, "slReflexSetOptions", a.ReflexSetOptions) &&
			get(sl::kFeatureReflex, "slReflexSleep", a.ReflexSleep) &&
			get(sl::kFeatureReflex, "slReflexGetState", a.ReflexGetState) &&
			get(sl::kFeaturePCL, "slPCLSetMarker", a.PCLSetMarker) &&
			get(sl::kFeaturePCL, "slPCLSetOptions", a.PCLSetOptions) &&
			get(sl::kFeaturePCL, "slPCLGetState", a.PCLGetState);
	fg_runtime_ready_ = ok;
	if (ok) {
		log::info("Streamline: Frame Generation plugins loaded and resolved (not yet driving rendering)");
	} else {
		// Expected on anything that is not an RTX 40 series+: DLSS Super Resolution is unaffected.
		log::warning("Streamline: Frame Generation was permitted but its plugin functions could not be resolved; "
					 "Frame Generation stays off (Reflex and DLSS are unaffected)");
	}
}

void Streamline::shutdown() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		return;
	}
	// Before the device goes away (Streamline's rule). The module stays loaded: Vulkan calls of the
	// teardown still go through it.
	const bool had_fg_reflex = fg_runtime_ready_;
	api().Shutdown();
	initialized_ = false;
	device_ready_ = false;
	fg_runtime_ready_ = false;
	if (had_fg_reflex) {
		reflex::ReflexFeature::get().disable_streamline_reflex(); // the Reflex functions are gone now
		FgFeature::get().set_runtime_max_multiplier(0); // back to the architecture guess
	}
}

bool Streamline::frame_generation_ready() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return fg_runtime_ready_;
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

StreamlineChainFeature &StreamlineChainFeature::get() {
	static StreamlineChainFeature *instance = new StreamlineChainFeature();
	return *instance;
}

bool StreamlineChainFeature::request_device(const vk::PhysicalDeviceCaps &caps, vk::DeviceRequest &request, std::string &reason) {
	const bool core = caps.effective_api_version >= VK_API_VERSION_1_3;
	if (!core && !caps.has_extension(VK_EXT_PRIVATE_DATA_EXTENSION_NAME)) {
		reason = "the device offers no private data (Streamline reports a validation error, nothing else)";
		return false;
	}
	if (!core) {
		request.add_extension(VK_EXT_PRIVATE_DATA_EXTENSION_NAME);
	}
	request.features.private_data = true;
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
