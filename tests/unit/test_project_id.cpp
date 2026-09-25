#include "dlss/project_id.h"
#include "test_framework.h"

using namespace lu::dlss;

TEST(project_id_guid_likeness_follows_nvidias_format) {
	CHECK(is_guid_like("a0f57b54-1daf-4934-90ae-c4035c19df04")); // the DLSS guide's example
	CHECK(is_guid_like("A0F57B54-1DAF-4934-90AE-C4035C19DF04"));
	CHECK(!is_guid_like(""));
	CHECK(!is_guid_like("a0f57b54-1daf-4934-90ae-c4035c19df0")); // one digit short
	CHECK(!is_guid_like("a0f57b54-1daf-4934-90ae-c4035c19df045")); // one digit long
	CHECK(!is_guid_like("a0f57b541daf-4934-90ae-c4035c19df04-")); // dash misplaced
	CHECK(!is_guid_like("{a0f57b54-1daf-4934-90ae-c4035c19df04}")); // braces are not part of it
	CHECK(!is_guid_like("g0f57b54-1daf-4934-90ae-c4035c19df04")); // not hexadecimal
	CHECK(!is_guid_like(" a0f57b54-1daf-4934-90ae-c4035c19df04"));
	CHECK(!is_guid_like("my game"));
}

TEST(project_id_from_digest_sets_version_and_variant) {
	// SHA-256("abc") = ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad
	const uint8_t abc[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
	const std::string id = project_id_from_digest(abc);
	CHECK_EQ(id, std::string("ba7816bf-8f01-8fea-8141-40de5dae2223")); // 0xcf -> 0x8f, 0x41 -> 0x81
	CHECK(is_guid_like(id));
	uint8_t ones[32];
	uint8_t zeros[32] = {};
	for (uint8_t &b : ones) {
		b = 0xff;
	}
	CHECK_EQ(project_id_from_digest(ones), std::string("ffffffff-ffff-8fff-bfff-ffffffffffff"));
	CHECK_EQ(project_id_from_digest(zeros), std::string("00000000-0000-8000-8000-000000000000"));
}
