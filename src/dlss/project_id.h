// The project ID Streamline hands to NGX (sl::Preferences::projectId).
//
// NVIDIA's DLSS programming guide (5.2.1): with the CUSTOM engine type the driver checks that the
// project ID is GUID-like ("a0f57b54-1daf-4934-90ae-c4035c19df04"), and it identifies the project,
// like the project ID Unreal keeps in its project settings. Each Godot project therefore gets its own:
//   - latency_upscaling/dlss/project_id, when set to a GUID-like string, or else
//   - one derived from the project's name (application/config/name): a name-based UUID (RFC 9562
//     version 8 layout) from the SHA-256 of "godot-latency-upscaling:" + the name. It is the same on
//     every machine and in every export of the project, and differs between projects with different
//     names. Projects that share a name share it: set your own ID then.
//
// Pure C++: the SHA-256 comes from the caller (Godot's String::sha256_buffer), so the formatting and
// validation are unit-tested (tests/unit/test_project_id.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace lu::dlss {

inline constexpr const char *PROJECT_ID_SEED_PREFIX = "godot-latency-upscaling:";

// 8-4-4-4-12 hexadecimal digits separated by '-', nothing else (either case).
bool is_guid_like(const std::string &id);

// A lowercase UUID from the first 16 bytes of a SHA-256 digest, with the version (8) and the
// variant (10xx) bits set as RFC 9562 requires.
std::string project_id_from_digest(const uint8_t digest[32]);

} // namespace lu::dlss
