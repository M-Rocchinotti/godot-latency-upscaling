// DLSS Frame Generation (Phase 3) capability + status holder.
//
// Frame Generation (and Multi Frame Generation on GeForce RTX 50) needs a GeForce RTX 40 series
// (Ada) GPU or newer, a recent NVIDIA driver and Windows 10 20H1+ with hardware-accelerated GPU
// scheduling. Milestone H2 fills FgCaps with the REAL GPU the plugin sees at start-up (vendor,
// architecture and driver from the gpu_precheck adapter probe, dlss::check_adapters /
// dlss::nvidia_driver_version, reused), still guarded off: no Streamline Frame Generation feature
// is loaded and the Reflex backend is not switched (that is H3/H5). The OS / HAGS requirement is
// not probed yet (its real gate belongs to the milestone that loads Frame Generation), so os_known
// is false and a capable GPU is not refused on OS grounds here.
//
// check_frame_generation() is the real capability logic. It is unit-tested with injected fake caps
// (dlss::FgCaps); bootstrap.cpp builds the real caps. The status wording mirrors dlss::DlssFeature /
// get_dlss_status(): a bare reason here, "unavailable: " / "off: " prefixed by the public API.
//
// Pure C++: no Godot types, so the rules and the architecture classifier are unit-tested.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace lu::dlss {

// The NVIDIA GPU architecture, as far as Frame Generation cares. Frame Generation needs Ada or
// newer; Multi Frame Generation (3x / 4x) needs Blackwell. "Unknown" is the honest value until a
// later milestone really queries it; it counts as unsupported.
enum class NvArchitecture : int { Unknown = 0, PreAda = 1, Ada = 2, Blackwell = 3 };

// Frame Generation's minimum NVIDIA driver (README requirements: 520, 527.64+ for native Vulkan
// optical flow; the stricter number is used so the Vulkan path is covered).
inline constexpr uint32_t FG_MIN_DRIVER_MAJOR = 527;
inline constexpr uint32_t FG_MIN_DRIVER_MINOR = 64;

// What Frame Generation's decision needs. Milestone H2 fills the GPU fields from the adapter the
// plugin probes at start-up; the OS / HAGS gate is deferred (os_known false), so it does not refuse
// a capable GPU yet.
struct FgCaps {
	bool probed = false; // the GPU capability was really queried (false: not probed, e.g. the editor)
	bool vulkan_device = false; // a Vulkan device exists (headless / non-Vulkan: nothing to generate)
	uint32_t vendor_id = 0; // 0x10DE for NVIDIA
	uint32_t device_id = 0; // PCI device ID (a secondary signal for the architecture classifier)
	std::string gpu_name;
	uint32_t driver_major = 0;
	uint32_t driver_minor = 0;
	NvArchitecture architecture = NvArchitecture::Unknown;
	bool os_known = false; // the OS / HAGS requirement was determined (false in milestone H2)
	bool os_supported = false; // Windows 10 20H1+ with HAGS on (only consulted when os_known)
};

// The NVIDIA GPU architecture from the adapter's vendor, name and device ID (pure, unit-tested).
// Non-NVIDIA -> Unknown. GeForce RTX 50xx -> Blackwell, RTX 40xx (or an "Ada" name) -> Ada, RTX
// 30xx / 20xx and every GTX -> PreAda; an NVIDIA name it cannot place -> Unknown (conservative).
NvArchitecture classify_nvidia_architecture(uint32_t vendor_id, const std::string &name, uint32_t device_id);

// The highest frame-generation multiplier an architecture offers: 1 = no Frame Generation, 2 = Ada
// Frame Generation, 4 = Blackwell Multi Frame Generation. A conservative guess used until the
// runtime reports the real ceiling.
int max_multiplier_for(NvArchitecture architecture);

// The frame multiplier for a Streamline numFramesToGenerateMax (sl_dlss_g.h DLSSGState): one real
// frame plus that many generated, so the multiplier is numFramesToGenerateMax + 1 (5 -> 6x). 0 (no
// generated frames) maps to 1 (no Frame Generation).
int fg_multiplier_from_frames_to_generate(uint32_t num_frames_to_generate_max);

// "" when Frame Generation could run on these caps, otherwise a player-actionable reason (without a
// leading "unavailable: "; the public API adds that, mirroring get_dlss_status()).
std::string check_frame_generation(const FgCaps &caps);

struct FgStatus {
	bool supported = false; // the hardware / driver / OS could run Frame Generation
	bool active = false; // Frame Generation is generating frames now (never in milestone H1)
	std::string reason; // why not (when !supported or !active)
	int max_multiplier = 1; // the highest multiplier the hardware offers (1 = none)
};

// Process-wide holder, configured once at CORE start-up (bootstrap.cpp). Guarded off in milestone
// H1: configure() is called with a stub (not probed), so status().supported is false everywhere.
class FgFeature {
public:
	static FgFeature &get();

	// CORE initialisation. `caps`: the capability for this run (milestone H1 passes a stub that was
	// not probed, so Frame Generation reports "unavailable" regardless of the GPU).
	void configure(const FgCaps &caps);

	// The runtime's real multiplier ceiling, from slDLSSGGetState().numFramesToGenerateMax once Frame
	// Generation is loaded (RTX only). 0 clears it (fall back to the architecture guess). It overrides
	// the guess only while Frame Generation is supported.
	void set_runtime_max_multiplier(int multiplier);

	FgStatus status() const;

private:
	mutable std::mutex mutex_;
	FgCaps caps_;
	int runtime_max_multiplier_ = 0; // 0 = not reported yet; use the architecture guess
};

} // namespace lu::dlss
