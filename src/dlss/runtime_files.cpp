#include "dlss/runtime_files.h"

#include <windows.h>

namespace lu::dlss {

const std::vector<std::wstring> &super_resolution_files() {
	static const std::vector<std::wstring> files = {
		L"sl.interposer.dll",
		L"sl.common.dll",
		L"sl.dlss.dll",
		L"nvngx_dlss.dll",
	};
	return files;
}

RuntimeFiles check_runtime_files(const std::wstring &dir) {
	RuntimeFiles result;
	result.dir = dir;
	for (const std::wstring &name : super_resolution_files()) {
		std::wstring path = dir;
		if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
			path += L'\\';
		}
		path += name;
		const DWORD attributes = GetFileAttributesW(path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			result.missing.push_back(to_utf8(name));
		}
	}
	return result;
}

std::wstring default_runtime_dir() {
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&default_runtime_dir), &self)) {
		return std::wstring();
	}
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		const DWORD length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
		if (length == 0) {
			return std::wstring();
		}
		if (length < path.size()) {
			path.resize(length);
			break;
		}
		path.resize(path.size() * 2);
	}
	const size_t slash = path.find_last_of(L"\\/");
	return (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"nvidia";
}

std::string to_utf8(const std::wstring &text) {
	if (text.empty()) {
		return std::string();
	}
	const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
	return out;
}

} // namespace lu::dlss
