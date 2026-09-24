#include "hook/iat_patch.h"
#include "test_framework.h"

#include <cstring>

using namespace nvdlss::hook;

namespace {

DWORD WINAPI fake_process_id() {
	return 4242;
}

// Calls the import through this executable's IAT. noinline: the call must really go through
// the import slot each time instead of being folded by the optimizer.
__declspec(noinline) DWORD call_process_id() {
	return GetCurrentProcessId();
}

void *real_process_id() {
	return reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId"));
}

} // namespace

TEST(iat_patch_redirects_and_restores_an_import) {
	const DWORD real_pid = call_process_id();
	std::string error;
	const std::vector<IatSlot> slots = patch_imports(GetModuleHandleW(nullptr), "GetCurrentProcessId", real_process_id(),
			reinterpret_cast<void *>(&fake_process_id), &error);
	CHECK(error.empty());
	CHECK(!slots.empty());
	CHECK_EQ(call_process_id(), static_cast<DWORD>(4242));
	for (const IatSlot &slot : slots) {
		CHECK(_stricmp(slot.dll.c_str(), "KERNEL32.dll") == 0 || slot.dll.find("api-ms-win") == 0);
		CHECK(slot.original != nullptr);
	}
	restore_imports(slots);
	CHECK_EQ(call_process_id(), real_pid);
}

TEST(iat_patch_ignores_functions_that_are_not_imported) {
	std::string error;
	const std::vector<IatSlot> slots = patch_imports(GetModuleHandleW(nullptr), "NoSuchFunctionAnywhere", nullptr,
			reinterpret_cast<void *>(&fake_process_id), &error);
	CHECK(slots.empty());
	CHECK(error.empty());
}

TEST(iat_patch_rejects_a_non_pe_module) {
	alignas(16) static unsigned char not_an_image[4096] = {};
	std::string error;
	const std::vector<IatSlot> slots = patch_imports(reinterpret_cast<HMODULE>(not_an_image), "LoadLibraryA", nullptr,
			reinterpret_cast<void *>(&fake_process_id), &error);
	CHECK(slots.empty());
	CHECK(!error.empty());
}

TEST(iat_patch_rejects_null_arguments) {
	std::string error;
	CHECK(patch_imports(nullptr, "LoadLibraryA", nullptr, reinterpret_cast<void *>(&fake_process_id), &error).empty());
	CHECK(!error.empty());
}
