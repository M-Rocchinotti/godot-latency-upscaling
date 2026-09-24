#include "hook/loader_hook.h"

#include "core/log.h"
#include "hook/iat_patch.h"
#include "hook/loader_name.h"
#include "vk/dispatch.h"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace lu::hook {
namespace {

using LoadLibraryAFn = HMODULE(WINAPI *)(LPCSTR);
using LoadLibraryWFn = HMODULE(WINAPI *)(LPCWSTR);
using LoadLibraryExAFn = HMODULE(WINAPI *)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn = HMODULE(WINAPI *)(LPCWSTR, HANDLE, DWORD);

// What each hook calls to perform the real load: the value the host's import slot held before
// patching (normally kernel32's function; another hook if something patched it first).
struct Next {
	std::atomic<LoadLibraryAFn> load_a{ nullptr };
	std::atomic<LoadLibraryWFn> load_w{ nullptr };
	std::atomic<LoadLibraryExAFn> load_ex_a{ nullptr };
	std::atomic<LoadLibraryExWFn> load_ex_w{ nullptr };
};
Next g_next;

// Loading as data or as a resource never runs code, so such requests are not the loader.
constexpr DWORD NON_EXECUTABLE_FLAGS = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;

template <typename Char>
struct ExRequest {
	const Char *name;
	HANDLE file;
	DWORD flags;
};

HMODULE WINAPI hook_LoadLibraryA(LPCSTR name) {
	if (is_vulkan_loader_name(name)) {
		return vk::on_loader_requested([](const void *r) { return g_next.load_a.load()(static_cast<LPCSTR>(r)); }, name);
	}
	return g_next.load_a.load()(name);
}

HMODULE WINAPI hook_LoadLibraryW(LPCWSTR name) {
	if (is_vulkan_loader_name(name)) {
		return vk::on_loader_requested([](const void *r) { return g_next.load_w.load()(static_cast<LPCWSTR>(r)); }, name);
	}
	return g_next.load_w.load()(name);
}

HMODULE WINAPI hook_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
	if ((flags & NON_EXECUTABLE_FLAGS) == 0 && is_vulkan_loader_name(name)) {
		const ExRequest<char> request{ name, file, flags };
		return vk::on_loader_requested(
				[](const void *r) {
					const auto *q = static_cast<const ExRequest<char> *>(r);
					return g_next.load_ex_a.load()(q->name, q->file, q->flags);
				},
				&request);
	}
	return g_next.load_ex_a.load()(name, file, flags);
}

HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
	if ((flags & NON_EXECUTABLE_FLAGS) == 0 && is_vulkan_loader_name(name)) {
		const ExRequest<wchar_t> request{ name, file, flags };
		return vk::on_loader_requested(
				[](const void *r) {
					const auto *q = static_cast<const ExRequest<wchar_t> *>(r);
					return g_next.load_ex_w.load()(q->name, q->file, q->flags);
				},
				&request);
	}
	return g_next.load_ex_w.load()(name, file, flags);
}

template <typename Fn>
bool patch_one(HMODULE exe, HMODULE kernel32, const char *function, Fn hook, std::atomic<Fn> &next, InstallResult &result) {
	void *real = reinterpret_cast<void *>(GetProcAddress(kernel32, function));
	std::string error;
	const std::vector<IatSlot> slots = patch_imports(exe, function, real, reinterpret_cast<void *>(hook), &error);
	if (!error.empty()) {
		log::warning("patching %s failed: %s", function, error.c_str());
		return false;
	}
	if (slots.empty()) {
		return false;
	}
	// Chain to what the slots held before (another hook, if one was installed first). Until this
	// store, `next` is kernel32's own function, which is equally correct. A single shared hook has
	// one chain target, so if the slots held different originals we chain to the real export.
	bool mismatch = false;
	void *target = select_chain_target(slots, real, &mismatch);
	if (mismatch) {
		log::warning("import slots for %s held different originals; chaining %s to the real loader export", function, function);
	}
	next.store(reinterpret_cast<Fn>(target));
	for (const IatSlot &slot : slots) {
		result.patched.push_back(slot.dll + "!" + function);
	}
	return true;
}

} // namespace

bool pin_addon_module() {
	HMODULE self = nullptr;
	return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				   reinterpret_cast<LPCWSTR>(&pin_addon_module), &self) != FALSE;
}

InstallResult install() {
	static std::mutex mutex;
	static InstallResult cached;
	static bool done = false;
	std::lock_guard<std::mutex> lock(mutex);
	if (done) {
		return cached;
	}
	done = true;

	InstallResult result;
	HMODULE exe = GetModuleHandleW(nullptr);
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	if (exe == nullptr || kernel32 == nullptr) {
		result.reason = "could not locate the host executable or kernel32.dll";
		cached = result;
		return cached;
	}
	// Pin first: from the moment an import points here, this DLL must stay loaded.
	if (!pin_addon_module()) {
		result.reason = "could not pin the addon DLL in memory";
		cached = result;
		return cached;
	}
	// Set every `next` pointer before the slots are switched, so a hook can never observe a null
	// `next`: the defaults are kernel32's own functions.
	g_next.load_a.store(reinterpret_cast<LoadLibraryAFn>(GetProcAddress(kernel32, "LoadLibraryA")));
	g_next.load_w.store(reinterpret_cast<LoadLibraryWFn>(GetProcAddress(kernel32, "LoadLibraryW")));
	g_next.load_ex_a.store(reinterpret_cast<LoadLibraryExAFn>(GetProcAddress(kernel32, "LoadLibraryExA")));
	g_next.load_ex_w.store(reinterpret_cast<LoadLibraryExWFn>(GetProcAddress(kernel32, "LoadLibraryExW")));

	const bool a = patch_one<LoadLibraryAFn>(exe, kernel32, "LoadLibraryA", &hook_LoadLibraryA, g_next.load_a, result);
	const bool w = patch_one<LoadLibraryWFn>(exe, kernel32, "LoadLibraryW", &hook_LoadLibraryW, g_next.load_w, result);
	const bool ex_a = patch_one<LoadLibraryExAFn>(exe, kernel32, "LoadLibraryExA", &hook_LoadLibraryExA, g_next.load_ex_a, result);
	const bool ex_w = patch_one<LoadLibraryExWFn>(exe, kernel32, "LoadLibraryExW", &hook_LoadLibraryExW, g_next.load_ex_w, result);
	result.installed = a || w || ex_a || ex_w;
	if (!result.installed) {
		result.reason = "the host executable does not import LoadLibraryA/W or LoadLibraryExA/W from a DLL (unsupported build)";
	}
	cached = result;
	return cached;
}

} // namespace lu::hook
