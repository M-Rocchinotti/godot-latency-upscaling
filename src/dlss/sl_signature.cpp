// NVIDIA's own signature check for the Streamline modules (sl_security.h, MIT, Streamline 2.14.1),
// in its own translation unit: the header defines UNICODE and non-inline functions.
#include "dlss/sl_signature.h"

#include <sl_security.h>

namespace lu::dlss {

bool verify_nvidia_signature(const std::wstring &path) {
	return sl::security::verifyEmbeddedSignature(path.c_str());
}

} // namespace lu::dlss
