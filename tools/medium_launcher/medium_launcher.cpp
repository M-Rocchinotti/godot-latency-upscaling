// medium_launcher: runs a command at medium integrity when this process is elevated.
//
// The Vulkan loader ignores VK_ADD_LAYER_PATH and VK_LAYER_PATH in elevated (high-integrity)
// processes (loader_secure_getenv). Where UAC is off, as on the RTX test PC, every process of an
// administrator is elevated, so a validation run would silently lose its layer. This launcher
// duplicates its own token, lowers the copy's integrity level to medium (lowering needs no
// privilege; nothing on the machine changes: no setting, no registry) and starts the command with
// it (CreateProcessAsUserW). The child inherits the environment and this process' STARTUPINFO show
// state (tools/rtx_remote.py starts the launcher hidden or as a window without focus). The launcher
// waits and exits with the child's exit code; it has no window of its own (GUI subsystem).
//
//     build: python tools/build.py medium_launcher  -> build/tools/medium_launcher.exe
//     run:   medium_launcher.exe <program> [arguments...]
#include <windows.h>

#include <cwchar>
#include <string>

namespace {

// The command line after the launcher's own program name (quoted or not), as Windows passed it.
std::wstring rest_of_command_line() {
	const wchar_t *p = GetCommandLineW();
	if (*p == L'"') {
		for (++p; *p && *p != L'"'; ++p) {
		}
		if (*p == L'"') {
			++p;
		}
	} else {
		for (; *p && *p != L' ' && *p != L'\t'; ++p) {
		}
	}
	while (*p == L' ' || *p == L'\t') {
		++p;
	}
	return p;
}

int fail(const wchar_t *what) {
	wchar_t message[256];
	std::swprintf(message, 256, L"medium_launcher: %ls failed (Windows error %lu)\n", what, GetLastError());
	OutputDebugStringW(message);
	return 125;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
	std::wstring command = rest_of_command_line();
	if (command.empty()) {
		return fail(L"no command given:");
	}
	HANDLE token = nullptr;
	HANDLE primary = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT,
				&token)) {
		return fail(L"OpenProcessToken");
	}
	if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
		CloseHandle(token);
		return fail(L"DuplicateTokenEx");
	}
	CloseHandle(token);
	SID_IDENTIFIER_AUTHORITY label_authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
	PSID medium = nullptr;
	if (!AllocateAndInitializeSid(&label_authority, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0, 0, 0, 0, 0, &medium)) {
		CloseHandle(primary);
		return fail(L"AllocateAndInitializeSid");
	}
	TOKEN_MANDATORY_LABEL label = {};
	label.Label.Attributes = SE_GROUP_INTEGRITY;
	label.Label.Sid = medium;
	const BOOL lowered = SetTokenInformation(primary, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(medium));
	FreeSid(medium);
	if (!lowered) {
		CloseHandle(primary);
		return fail(L"SetTokenInformation(TokenIntegrityLevel)");
	}
	STARTUPINFOW own = {};
	own.cb = sizeof(own);
	GetStartupInfoW(&own);
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	if (own.dwFlags & STARTF_USESHOWWINDOW) {
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = own.wShowWindow;
	}
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessAsUserW(primary, nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		CloseHandle(primary);
		return fail(L"CreateProcessAsUserW");
	}
	CloseHandle(primary);
	CloseHandle(pi.hThread);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 125;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return static_cast<int>(code);
}
