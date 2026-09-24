#include "hook/iat_patch.h"

#include <cstring>

namespace nvdlss::hook {
namespace {

bool write_pointer(void **slot, void *value) {
	// Keep execute permission if the page has it: some linkers place the IAT in a page shared
	// with code, which another thread may be running right now.
	MEMORY_BASIC_INFORMATION page{};
	if (VirtualQuery(slot, &page, sizeof(page)) == 0) {
		return false;
	}
	constexpr DWORD EXECUTABLE = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	const DWORD writable = (page.Protect & EXECUTABLE) != 0 ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
	DWORD old_protect = 0;
	if (!VirtualProtect(slot, sizeof(void *), writable, &old_protect)) {
		return false;
	}
	// Pointer-sized aligned store: other threads calling through the slot see either the old
	// or the new value, never a torn one.
	InterlockedExchangePointer(reinterpret_cast<PVOID volatile *>(slot), value);
	DWORD ignored = 0;
	VirtualProtect(slot, sizeof(void *), old_protect, &ignored);
	return true;
}

const IMAGE_NT_HEADERS *nt_headers(HMODULE module, std::string *error) {
	const auto *base = reinterpret_cast<const BYTE *>(module);
	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		*error = "not a PE image (DOS signature)";
		return nullptr;
	}
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) {
		*error = "not a PE32+ image";
		return nullptr;
	}
	return nt;
}

} // namespace

std::vector<IatSlot> patch_imports(HMODULE module, const char *function, void *real_function, void *replacement,
		std::string *error) {
	std::string local_error;
	if (error == nullptr) {
		error = &local_error;
	}
	error->clear();
	std::vector<IatSlot> found;
	if (module == nullptr || function == nullptr || replacement == nullptr) {
		*error = "invalid arguments";
		return found;
	}
	const IMAGE_NT_HEADERS *nt = nt_headers(module, error);
	if (nt == nullptr) {
		return found;
	}
	const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (dir.VirtualAddress == 0 || dir.Size == 0) {
		return found;
	}
	auto *base = reinterpret_cast<BYTE *>(module);
	for (auto *desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); desc->Name != 0;
			++desc) {
		auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(base + desc->FirstThunk);
		const IMAGE_THUNK_DATA *names = desc->OriginalFirstThunk != 0
				? reinterpret_cast<const IMAGE_THUNK_DATA *>(base + desc->OriginalFirstThunk)
				: nullptr;
		for (size_t i = 0; iat[i].u1.Function != 0; ++i) {
			bool match = false;
			if (names != nullptr) {
				if (!IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
					const auto *by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names[i].u1.AddressOfData);
					match = std::strcmp(reinterpret_cast<const char *>(by_name->Name), function) == 0;
				}
			} else {
				match = real_function != nullptr && reinterpret_cast<void *>(iat[i].u1.Function) == real_function;
			}
			if (match) {
				IatSlot slot;
				slot.slot = reinterpret_cast<void **>(&iat[i].u1.Function);
				slot.original = *slot.slot;
				slot.dll = reinterpret_cast<const char *>(base + desc->Name);
				found.push_back(slot);
			}
		}
	}
	for (size_t i = 0; i < found.size(); ++i) {
		if (!write_pointer(found[i].slot, replacement)) {
			*error = "VirtualProtect failed on the import table";
			found.resize(i);
			restore_imports(found);
			found.clear();
			return found;
		}
	}
	return found;
}

void restore_imports(const std::vector<IatSlot> &slots) {
	for (const IatSlot &slot : slots) {
		write_pointer(slot.slot, slot.original);
	}
}

} // namespace nvdlss::hook
