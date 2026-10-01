// Frame Generation's capability logic (dlss::check_frame_generation) against injected fake caps:
// the reasons the API reports for non-RTX-40, non-NVIDIA, old-driver and unsupported-OS GPUs, the
// supported path on Ada / Blackwell, and the stubbed holder (not probed -> unavailable everywhere,
// which is the guarded-off milestone H1 state on this GTX 1080 dev box).
#include "dlss/fg_feature.h"
#include "test_framework.h"

#include <string>

using namespace lu;
using namespace lu::dlss;

namespace {

// A fully capable, really-probed RTX 40 series GPU: the baseline the refusals are derived from.
FgCaps rtx40_caps() {
	FgCaps caps;
	caps.probed = true;
	caps.vulkan_device = true;
	caps.vendor_id = 0x10DE;
	caps.gpu_name = "NVIDIA GeForce RTX 4080";
	caps.driver_major = 591;
	caps.driver_minor = 86;
	caps.architecture = NvArchitecture::Ada;
	caps.os_supported = true;
	return caps;
}

} // namespace

TEST(fg_max_multiplier_by_architecture) {
	CHECK_EQ(max_multiplier_for(NvArchitecture::Ada), 2); // Frame Generation (2x)
	CHECK_EQ(max_multiplier_for(NvArchitecture::Blackwell), 4); // Multi Frame Generation (up to 4x)
	CHECK_EQ(max_multiplier_for(NvArchitecture::PreAda), 1); // no Frame Generation
	CHECK_EQ(max_multiplier_for(NvArchitecture::Unknown), 1);
}

TEST(fg_check_accepts_ada_and_blackwell) {
	CHECK_EQ(check_frame_generation(rtx40_caps()), std::string());
	FgCaps blackwell = rtx40_caps();
	blackwell.architecture = NvArchitecture::Blackwell;
	blackwell.gpu_name = "NVIDIA GeForce RTX 5090";
	CHECK_EQ(check_frame_generation(blackwell), std::string());
}

TEST(fg_check_explains_every_refusal) {
	// Not probed yet (milestone H1 on every box): the honest, player-actionable default.
	FgCaps unprobed;
	CHECK_EQ(check_frame_generation(unprobed), std::string("needs a GeForce RTX 40 series GPU"));

	// No Vulkan device (headless / non-Vulkan renderer): nothing to generate.
	FgCaps headless = rtx40_caps();
	headless.vulkan_device = false;
	CHECK(check_frame_generation(headless).find("no Vulkan device") == 0);

	// Not an NVIDIA GPU.
	FgCaps amd = rtx40_caps();
	amd.vendor_id = 0x1002;
	amd.gpu_name = "AMD Radeon RX 7900 XTX";
	CHECK(check_frame_generation(amd).find("AMD Radeon RX 7900 XTX is not an NVIDIA GPU") == 0);
	CHECK(check_frame_generation(amd).find("GeForce RTX 40 series") != std::string::npos);

	// NVIDIA, but older than Ada: the reference GTX 1080 (Pascal) and an RTX 30 (Ampere).
	FgCaps gtx1080 = rtx40_caps();
	gtx1080.gpu_name = "NVIDIA GeForce GTX 1080";
	gtx1080.architecture = NvArchitecture::PreAda;
	CHECK(check_frame_generation(gtx1080).find("NVIDIA GeForce GTX 1080 does not support Frame Generation") == 0);
	CHECK(check_frame_generation(gtx1080).find("GeForce RTX 40 series GPU or newer") != std::string::npos);
	FgCaps unknown_arch = rtx40_caps();
	unknown_arch.architecture = NvArchitecture::Unknown;
	CHECK(check_frame_generation(unknown_arch).find("does not support Frame Generation") != std::string::npos);

	// Ada, but the driver is too old.
	FgCaps old_driver = rtx40_caps();
	old_driver.driver_major = FG_MIN_DRIVER_MAJOR;
	old_driver.driver_minor = FG_MIN_DRIVER_MINOR - 1;
	CHECK(check_frame_generation(old_driver).find("is older than") != std::string::npos);
	old_driver.driver_minor = FG_MIN_DRIVER_MINOR; // the minimum itself passes
	CHECK(check_frame_generation(old_driver).empty());

	// Ada and a current driver, but the OS / HAGS requirement is not met.
	FgCaps bad_os = rtx40_caps();
	bad_os.os_supported = false;
	CHECK(check_frame_generation(bad_os).find("Windows 10 20H1") != std::string::npos);
}

TEST(fg_feature_is_unavailable_until_probed) {
	// Milestone H1: configure() is called with a stub that was not probed, so Frame Generation is
	// unavailable on every GPU and the max multiplier is 1 (as on this GTX 1080 dev box).
	FgFeature feature;
	feature.configure(FgCaps{});
	FgStatus s = feature.status();
	CHECK(!s.supported);
	CHECK(!s.active);
	CHECK_EQ(s.reason, std::string("needs a GeForce RTX 40 series GPU"));
	CHECK_EQ(s.max_multiplier, 1);

	// A later milestone will configure it with real, probed caps: the supported path (still not
	// active, since no runtime is loaded) and the hardware's multiplier ceiling.
	feature.configure(rtx40_caps());
	s = feature.status();
	CHECK(s.supported);
	CHECK(!s.active);
	CHECK_EQ(s.max_multiplier, 2);
}
