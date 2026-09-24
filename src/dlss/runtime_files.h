// Where the NVIDIA runtime for DLSS is expected, and whether its files are there.
//
// The runtime (Streamline + the DLSS library) is licensed by NVIDIA and never ships with the
// addon: the developer fetches it after accepting NVIDIA's licences (the editor plugin's dialog or
// tools/fetch_nvidia_sdk.py; addons/latency_upscaling/nvidia_runtime.json names the files). This
// check only looks at file existence; it never loads anything.
#pragma once

#include <string>
#include <vector>

namespace lu::dlss {

struct RuntimeFiles {
	std::wstring dir;
	std::vector<std::string> missing; // file names (UTF-8) of the Super Resolution set not found in dir
	bool present() const { return missing.empty(); }
};

// The files DLSS Super Resolution needs (nvidia_runtime.json "super_resolution_files").
const std::vector<std::wstring> &super_resolution_files();

RuntimeFiles check_runtime_files(const std::wstring &dir);

// "<folder of the addon's own DLL>\nvidia": addons/latency_upscaling/bin/nvidia in a project.
std::wstring default_runtime_dir();

std::string to_utf8(const std::wstring &text);

} // namespace lu::dlss
