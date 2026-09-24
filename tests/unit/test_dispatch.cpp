// The dispatch layer's proxy vkGetInstanceProcAddr against the real Vulkan loader (vulkan-1.dll,
// which every NVIDIA/AMD/Intel driver installs; the test fails loudly without it).
//
// The proxy returns its own hooks for the few functions it intercepts, but only where the loader
// itself has an answer: a hooked name the loader does not provide must stay null, or the host
// would believe in a function that does not exist. With VK_NULL_HANDLE as the instance the loader
// only resolves global commands, which makes that case reachable without creating an instance.
#include "vk/dispatch.h"
#include "test_framework.h"

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lu_proxy_vkGetInstanceProcAddr(VkInstance instance, const char *name);

namespace {

HMODULE load_real_loader(const void *) {
	return LoadLibraryW(L"vulkan-1.dll");
}

} // namespace

TEST(dispatch_proxy_keeps_the_loaders_not_available_answers) {
	const HMODULE module = lu::vk::on_loader_requested(&load_real_loader, nullptr);
	CHECK(module != nullptr); // needs vulkan-1.dll
	if (module == nullptr) {
		return;
	}
	CHECK(lu::vk::snapshot().loader_intercepted);
	// A global, hooked command: the proxy answers with its hook (so it is not trivially null).
	CHECK(lu_proxy_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") != nullptr);
	CHECK(lu_proxy_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties") != nullptr);
	// Hooked, but not global: without an instance the loader returns null, and so must the proxy.
	CHECK(lu_proxy_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice") == nullptr);
	CHECK(lu_proxy_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkQueuePresentKHR") == nullptr);
	CHECK(lu_proxy_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateSwapchainKHR") == nullptr);
	// Unknown names stay null.
	CHECK(lu_proxy_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkNotARealFunction") == nullptr);
	CHECK(lu_proxy_vkGetInstanceProcAddr(VK_NULL_HANDLE, nullptr) == nullptr);
}
