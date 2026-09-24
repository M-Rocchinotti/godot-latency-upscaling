#pragma once

#include <string>

namespace lu::dlss {

// True when the file at `path` (a full path) carries a valid Authenticode signature plus NVIDIA's
// secondary signature, the check Streamline itself applies before loading a module.
bool verify_nvidia_signature(const std::wstring &path);

} // namespace lu::dlss
