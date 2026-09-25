#include "dlss/project_id.h"

#include <cctype>
#include <cstdio>

namespace lu::dlss {

bool is_guid_like(const std::string &id) {
	static const int groups[5] = { 8, 4, 4, 4, 12 };
	size_t pos = 0;
	for (int g = 0; g < 5; ++g) {
		for (int i = 0; i < groups[g]; ++i, ++pos) {
			if (pos >= id.size() || !std::isxdigit(static_cast<unsigned char>(id[pos]))) {
				return false;
			}
		}
		if (g < 4) {
			if (pos >= id.size() || id[pos] != '-') {
				return false;
			}
			++pos;
		}
	}
	return pos == id.size();
}

std::string project_id_from_digest(const uint8_t digest[32]) {
	uint8_t b[16];
	for (int i = 0; i < 16; ++i) {
		b[i] = digest[i];
	}
	b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x80); // version 8 (custom, name-based here)
	b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80); // variant 10xx (RFC 9562)
	char out[37];
	std::snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
			b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return out;
}

} // namespace lu::dlss
