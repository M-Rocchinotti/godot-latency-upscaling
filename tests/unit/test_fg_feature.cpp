// Frame Generation's capability logic against injected fake caps: the architecture classifier for
// real adapter names (the signal bootstrap feeds from the gpu_precheck probe), and the reasons the
// API reports for non-RTX-40, non-NVIDIA, old-driver, unsupported-OS and headless GPUs, plus the
// supported path on Ada / Blackwell. On this GTX 1080 dev box the real probe classifies Pascal, so
// Frame Generation stays unavailable with an accurate reason (guarded-off milestone H2).
#include "dlss/fg_feature.h"
#include "core/settings.h"
#include "test_framework.h"

#include <string>

using namespace lu;
using namespace lu::dlss;

namespace {

// A fully capable, really-probed RTX 40 series GPU with a known-good OS: the baseline the refusals
// are derived from (os_known true here so the OS branch can be exercised).
FgCaps rtx40_caps() {
	FgCaps caps;
	caps.probed = true;
	caps.vulkan_device = true;
	caps.vendor_id = 0x10DE;
	caps.gpu_name = "NVIDIA GeForce RTX 4080";
	caps.driver_major = 591;
	caps.driver_minor = 86;
	caps.architecture = NvArchitecture::Ada;
	caps.os_known = true;
	caps.os_supported = true;
	return caps;
}

// The caps the bootstrap probe would build for a given adapter (NVIDIA driver 591.86, OS not yet
// probed, as in milestone H2), so the tests exercise the same path the real probe takes.
FgCaps caps_for(uint32_t vendor_id, const std::string &name) {
	FgCaps caps;
	caps.probed = true;
	caps.vulkan_device = true;
	caps.vendor_id = vendor_id;
	caps.gpu_name = name;
	caps.driver_major = 591;
	caps.driver_minor = 86;
	caps.architecture = classify_nvidia_architecture(vendor_id, name, 0);
	return caps; // os_known stays false, like the real H2 probe
}

const uint32_t NV = 0x10DE;
const uint32_t AMD = 0x1002;
const uint32_t INTEL = 0x8086;

} // namespace

TEST(fg_classifier_places_real_adapter_names) {
	CHECK(classify_nvidia_architecture(NV, "NVIDIA GeForce RTX 5090", 0) == NvArchitecture::Blackwell);
	CHECK(classify_nvidia_architecture(NV, "NVIDIA GeForce RTX 5070 Ti", 0) == NvArchitecture::Blackwell);
	CHECK(classify_nvidia_architecture(NV, "NVIDIA GeForce RTX 4080", 0) == NvArchitecture::Ada);
	CHECK(classify_nvidia_architecture(NV, "NVIDIA GeForce RTX 4060 Laptop GPU", 0) == NvArchitecture::Ada);
	CHECK(classify_nvidia_architecture(NV, "NVIDIA GeForce RTX 3080", 0) == NvArchitecture::PreAda); // Ampere
	CHECK(classify_nvidia_architecture(NV, "NVIDIA GeForce RTX 2070 SUPER", 0) == NvArchitecture::PreAda); // Turing
	CHECK(classify_nvidia_architecture(NV, "NVIDIA GeForce GTX 1080", 0) == NvArchitecture::PreAda); // Pascal (this box)
	CHECK(classify_nvidia_architecture(NV, "NVIDIA GeForce GTX 1660 Ti", 0) == NvArchitecture::PreAda); // Turing GTX
	// Workstation cards: the "Ada" / "Blackwell" keyword wins over the number.
	CHECK(classify_nvidia_architecture(NV, "NVIDIA RTX 6000 Ada Generation", 0) == NvArchitecture::Ada);
	CHECK(classify_nvidia_architecture(NV, "NVIDIA RTX 5880 Ada Generation", 0) == NvArchitecture::Ada);
	// Non-NVIDIA and unplaceable NVIDIA names are Unknown (conservative).
	CHECK(classify_nvidia_architecture(AMD, "AMD Radeon RX 7900 XTX", 0) == NvArchitecture::Unknown);
	CHECK(classify_nvidia_architecture(INTEL, "Intel Arc A770", 0) == NvArchitecture::Unknown);
	CHECK(classify_nvidia_architecture(NV, "NVIDIA RTX A6000", 0) == NvArchitecture::Unknown); // Ampere workstation
}

TEST(fg_max_multiplier_by_architecture) {
	CHECK_EQ(max_multiplier_for(NvArchitecture::Ada), 2); // Frame Generation (2x)
	CHECK_EQ(max_multiplier_for(NvArchitecture::Blackwell), 4); // Multi Frame Generation (up to 4x)
	CHECK_EQ(max_multiplier_for(NvArchitecture::PreAda), 1); // no Frame Generation
	CHECK_EQ(max_multiplier_for(NvArchitecture::Unknown), 1);
}

TEST(fg_check_accepts_ada_and_blackwell) {
	CHECK_EQ(check_frame_generation(rtx40_caps()), std::string());
	// Real adapter names through the classifier, OS not probed (H2): still supported.
	CHECK_EQ(check_frame_generation(caps_for(NV, "NVIDIA GeForce RTX 4080")), std::string());
	CHECK_EQ(check_frame_generation(caps_for(NV, "NVIDIA GeForce RTX 5090")), std::string());
}

TEST(fg_check_explains_every_refusal) {
	// Not probed (e.g. the editor): the honest, player-actionable default.
	FgCaps unprobed;
	CHECK_EQ(check_frame_generation(unprobed), std::string("needs a GeForce RTX 40 series GPU"));

	// No Vulkan device (headless / non-Vulkan renderer): nothing to generate.
	FgCaps headless = rtx40_caps();
	headless.vulkan_device = false;
	CHECK(check_frame_generation(headless).find("no Vulkan device") == 0);

	// Not an NVIDIA GPU: AMD and Intel.
	CHECK(check_frame_generation(caps_for(AMD, "AMD Radeon RX 7900 XTX")).find("AMD Radeon RX 7900 XTX is not an NVIDIA GPU") == 0);
	CHECK(check_frame_generation(caps_for(INTEL, "Intel Arc A770")).find("is not an NVIDIA GPU") != std::string::npos);

	// NVIDIA, but older than Ada: Pascal (this box), Turing and Ampere.
	const std::string pascal = check_frame_generation(caps_for(NV, "NVIDIA GeForce GTX 1080"));
	CHECK(pascal.find("NVIDIA GeForce GTX 1080 does not support Frame Generation") == 0);
	CHECK(pascal.find("GeForce RTX 40 series GPU or newer") != std::string::npos);
	CHECK(check_frame_generation(caps_for(NV, "NVIDIA GeForce RTX 2070")).find("does not support Frame Generation") != std::string::npos);
	CHECK(check_frame_generation(caps_for(NV, "NVIDIA GeForce RTX 3090")).find("does not support Frame Generation") != std::string::npos);

	// Ada, but the driver predates native Vulkan optical flow: the reason notes the interop fallback.
	FgCaps old_driver = rtx40_caps();
	old_driver.driver_major = FG_MIN_DRIVER_MAJOR;
	old_driver.driver_minor = FG_MIN_DRIVER_MINOR - 1;
	const std::string old_reason = check_frame_generation(old_driver);
	CHECK(old_reason.find("below 527.64") != std::string::npos);
	CHECK(old_reason.find("native Vulkan optical flow") != std::string::npos);
	CHECK(old_reason.find("interop") != std::string::npos);
	old_driver.driver_minor = FG_MIN_DRIVER_MINOR; // the minimum itself passes
	CHECK(check_frame_generation(old_driver).empty());

	// Ada and a current driver, but the OS / HAGS requirement is known-bad.
	FgCaps bad_os = rtx40_caps();
	bad_os.os_supported = false; // os_known stays true
	CHECK(check_frame_generation(bad_os).find("Windows 10 20H1") != std::string::npos);
	// When the OS is not yet probed (os_known false, as in H2), it does not refuse a capable GPU.
	FgCaps os_unknown = rtx40_caps();
	os_unknown.os_known = false;
	os_unknown.os_supported = false;
	CHECK(check_frame_generation(os_unknown).empty());
}

TEST(fg_runtime_max_multiplier_overrides_the_architecture_guess) {
	// numFramesToGenerateMax maps to one real frame plus the generated ones.
	CHECK_EQ(fg_multiplier_from_frames_to_generate(0), 1); // no Frame Generation
	CHECK_EQ(fg_multiplier_from_frames_to_generate(1), 2); // 2x (Ada)
	CHECK_EQ(fg_multiplier_from_frames_to_generate(3), 4); // 4x
	CHECK_EQ(fg_multiplier_from_frames_to_generate(5), 6); // 6x (the RTX 50 series value)

	FgFeature feature;
	feature.configure(caps_for(NV, "NVIDIA GeForce RTX 5090")); // Blackwell: guess 4
	CHECK_EQ(feature.status().max_multiplier, 4);
	feature.set_runtime_max_multiplier(6); // the runtime reports a higher ceiling (6x)
	CHECK_EQ(feature.status().max_multiplier, 6);
	feature.set_runtime_max_multiplier(0); // cleared (e.g. at shutdown) -> back to the guess
	CHECK_EQ(feature.status().max_multiplier, 4);

	// On an unsupported GPU the runtime value never applies: the dev-box behaviour (1) is unchanged.
	feature.configure(caps_for(NV, "NVIDIA GeForce GTX 1080"));
	feature.set_runtime_max_multiplier(6);
	CHECK(!feature.status().supported);
	CHECK_EQ(feature.status().max_multiplier, 1);
}

TEST(fg_num_frames_to_generate_maps_the_multiplier) {
	CHECK_EQ(fg_num_frames_to_generate(2, 5), 1u); // 2x -> 1 generated frame
	CHECK_EQ(fg_num_frames_to_generate(3, 5), 2u); // 3x -> 2
	CHECK_EQ(fg_num_frames_to_generate(4, 5), 3u); // 4x -> 3
	CHECK_EQ(fg_num_frames_to_generate(0, 5), 5u); // Auto -> the runtime ceiling (6x on an RTX 50)
	CHECK_EQ(fg_num_frames_to_generate(4, 1), 1u); // capped to a 2x-only ceiling
	CHECK_EQ(fg_num_frames_to_generate(0, 1), 1u); // Auto on a 2x-only GPU
	CHECK_EQ(fg_num_frames_to_generate(0, 0), 1u); // ceiling unknown -> at least one when on
}

TEST(fg_availability_decides_when_to_drive) {
	CHECK(fg_availability(false, true, true, false) == FgAvailability::Unavailable); // not RTX 40+
	CHECK(fg_availability(true, false, true, false) == FgAvailability::Off); // supported but off
	CHECK(fg_availability(true, true, false, false) == FgAvailability::NeedsUpscaler); // on, no DLSS/DLAA
	CHECK(fg_availability(true, true, true, true) == FgAvailability::NeedsVSyncOff); // on, upscaling, VSync on
	CHECK(fg_availability(true, true, true, false) == FgAvailability::Driving); // on, upscaling, VSync off
}

TEST(fg_ui_recomposition_needs_fg_driving_and_both_layers) {
	// Both the HUD-less colour and the UI layer present while Frame Generation drives: recompose, and
	// both get tagged (the clean-UI path, milestone H6).
	FgUiRecomposition both = fg_ui_recomposition(true, true, true);
	CHECK(both.enabled);
	CHECK(both.tag_hud_less);
	CHECK(both.tag_ui);

	// Frame Generation not driving: present-only even with both layers (nothing extra tagged), so the
	// non-FG and FG-off paths are byte-identical to before H6.
	FgUiRecomposition not_driving = fg_ui_recomposition(false, true, true);
	CHECK(!not_driving.enabled);
	CHECK(!not_driving.tag_hud_less);
	CHECK(!not_driving.tag_ui);

	// Only one layer provided (the game wired one but not the other): present-only, never a half-tagged
	// recomposition that DLSS-G would reject.
	CHECK(!fg_ui_recomposition(true, true, false).enabled); // HUD-less only
	CHECK(!fg_ui_recomposition(true, false, true).enabled); // UI only

	// Neither provided (the default): present-only, no regression.
	FgUiRecomposition neither = fg_ui_recomposition(true, false, false);
	CHECK(!neither.enabled);
	CHECK(!neither.tag_hud_less);
	CHECK(!neither.tag_ui);
}

TEST(fg_permitted_requires_both_the_settings_and_the_hardware) {
	// The pure gate bootstrap applies (Blocker #1): Frame Generation is permitted at startup only when
	// the developer enabled it (fg/enabled), chose a non-Off mode, AND the first adapter can actually
	// run Frame Generation. The hardware term is the same predicate the API reports.
	const bool rtx40 = check_frame_generation(caps_for(NV, "NVIDIA GeForce RTX 4080")).empty();
	const bool gtx1080 = check_frame_generation(caps_for(NV, "NVIDIA GeForce GTX 1080")).empty();
	const bool amd = check_frame_generation(caps_for(AMD, "AMD Radeon RX 7900 XTX")).empty();
	CHECK(rtx40); // the FG-capable adapter supports it
	CHECK(!gtx1080); // the dev box (Pascal) does not
	CHECK(!amd);

	// FG-capable hardware + the game's project.godot (fg/enabled=true, fg/mode=On): permitted.
	CHECK(fg_permitted(true, FrameGenerationMode::On, rtx40));

	// The crux of Blocker #1: the SAME enabled settings DO NOT permit Frame Generation on a non-FG GPU,
	// so adding fg/enabled=true to project.godot leaves the GTX 1080 / RTX 20-30 / AMD / Intel player
	// byte-identical to Frame Generation off — fg_permitted is false, so the Reflex LL2 path stays, no
	// FG plugins are requested and the device is not modified.
	CHECK(!fg_permitted(true, FrameGenerationMode::On, gtx1080));
	CHECK(!fg_permitted(true, FrameGenerationMode::On, amd));

	// Each settings term is still required even on FG-capable hardware.
	CHECK(!fg_permitted(false, FrameGenerationMode::On, rtx40)); // fg/enabled off
	CHECK(!fg_permitted(true, FrameGenerationMode::Off, rtx40)); // fg/mode Off
}

TEST(fg_feature_reports_the_configured_caps) {
	// Not probed (milestone H1 / the editor): unavailable everywhere, max multiplier 1.
	FgFeature feature;
	feature.configure(FgCaps{});
	FgStatus s = feature.status();
	CHECK(!s.supported);
	CHECK(!s.active);
	CHECK_EQ(s.reason, std::string("needs a GeForce RTX 40 series GPU"));
	CHECK_EQ(s.max_multiplier, 1);

	// This GTX 1080 dev box (what the real H2 probe classifies): unavailable, naming the GPU.
	feature.configure(caps_for(NV, "NVIDIA GeForce GTX 1080"));
	s = feature.status();
	CHECK(!s.supported);
	CHECK(s.reason.find("NVIDIA GeForce GTX 1080 does not support Frame Generation") == 0);
	CHECK_EQ(s.max_multiplier, 1);

	// A Blackwell GPU: supported (still not active, no runtime loaded), multiplier ceiling 4.
	feature.configure(caps_for(NV, "NVIDIA GeForce RTX 5090"));
	s = feature.status();
	CHECK(s.supported);
	CHECK(!s.active);
	CHECK_EQ(s.max_multiplier, 4);
}
