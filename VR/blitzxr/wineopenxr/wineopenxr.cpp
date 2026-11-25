// Tiny wineopenxr.dll for DXVK on Windows (32-bit, next to the game exe).
//
// DXVK creates the Vulkan instance and device itself and asks a wineopenxr.dll (which Proton has and Windows doesn't)
// for the extensions an OpenXR runtime needs to have enabled on them. Without these, Virtual Desktop's OpenXR runtime
// calls functions that aren't there (vkGetImageMemoryRequirements2KHR) and crashes. The lists are what OpenXR runtimes
// return from xrGetVulkanInstanceExtensionsKHR / xrGetVulkanDeviceExtensionsKHR; all have been part of Vulkan drivers
// on Windows for years.
#include <windows.h>
#include <stdint.h>
#include <string.h>

static const char g_instance[] =
	"VK_KHR_external_memory_capabilities VK_KHR_external_semaphore_capabilities VK_KHR_external_fence_capabilities "
	"VK_KHR_get_physical_device_properties2";
static const char g_device[] =
	"VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2 VK_KHR_external_memory VK_KHR_external_memory_win32 "
	"VK_KHR_timeline_semaphore VK_KHR_external_semaphore VK_KHR_external_semaphore_win32";

static int give(const char* s, uint32_t cap, uint32_t* count, char* out) {
	uint32_t n = (uint32_t)strlen(s) + 1;
	if (count) *count = n;
	if (!out) return 0;
	if (cap < n) return -1;
	memcpy(out, s, n);
	return 0;
}

extern "C" int WINAPI GetInstanceExt(uint32_t cap, uint32_t* count, char* out) { return give(g_instance, cap, count, out); }
extern "C" int WINAPI GetDeviceExt(uint32_t cap, uint32_t* count, char* out) { return give(g_device, cap, count, out); }