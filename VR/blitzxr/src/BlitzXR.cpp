// BlitzXR - OpenXR bridge userlib for BlitzX3D (32-bit)
//
// Frame path: game renders each eye with RenderWorld into a Blitz render-target texture
// (D3D9Ex) -> StretchRect into a D3D9Ex shared texture -> opened in D3D11 ->
// CopyResource into the OpenXR D3D11 swapchain image -> xrEndFrame.
// When the game runs on DXVK instead of the system Direct3D 9, the session is created on DXVK's
// Vulkan device and the render targets' VkImages are copied into Vulkan swapchain images
// (see "DXVK (Vulkan) path" below). The path is picked automatically and written to BlitzXR.log.
//
// Everything is exposed as __stdcall extern "C" functions described in BlitzXR.decls.
// Poses are returned already converted to Blitz conventions:
//   left-handed, +X right, +Y up, +Z forward, Blitz units (metres * world scale),
//   angles in degrees with Blitz's pitch/yaw/roll (positive pitch looks down,
//   positive yaw turns left), so they can be fed to MoveEntity / TurnEntity directly.
//
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

// Vulkan is only used when the game runs on DXVK (Direct3D 9 on Vulkan); everything is loaded at runtime.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

// exports are listed (undecorated) in BlitzXR.def so MSVC and MinGW builds match the .decls
#define BXR_API extern "C"
#define BXR_CALL __stdcall

// ---------------------------------------------------------------------------------------------
// logging / errors
// ---------------------------------------------------------------------------------------------

static std::string g_lastError;
static FILE* g_log = nullptr;

static void logf(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (!g_log) g_log = fopen("BlitzXR.log", "w");
	if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
	OutputDebugStringA("[BlitzXR] ");
	OutputDebugStringA(buf);
	OutputDebugStringA("\n");
}

static bool fail(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_lastError = buf;
	logf("ERROR: %s", buf);
	return false;
}

template <class T> static void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------------------------
// DXVK Direct3D 9 interop (declarations as in DXVK's d3d9_interfaces.h, DXVK 2.x)
// ---------------------------------------------------------------------------------------------

struct __declspec(uuid("d56344f5-8d35-46fd-806d-94c351b472c1")) ID3D9VkInteropTexture : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout, VkImageCreateInfo* pInfo) = 0;
};

struct __declspec(uuid("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")) ID3D9VkInteropDevice : public IUnknown {
	// (these two return nothing in DXVK's d3d9_interfaces.h; declaring them HRESULT read a garbage return value)
	virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev, VkDevice* pDevice) = 0;
	virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueIndex, uint32_t* pQueueFamilyIndex) = 0;
	virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture* pTexture, const VkImageSubresourceRange* pSubresources, VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
	virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
	virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
	virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
	virtual void STDMETHODCALLTYPE LockDevice() = 0;
	virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
	virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9* pResource, DWORD MapFlags) = 0;
};

// Vulkan functions used by the DXVK path (vulkan-1.dll is already loaded by DXVK)
#define BXR_VK_DEVICE_FUNCS(X) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdCopyImage) \
	X(vkCmdBlitImage) \
	X(vkQueueSubmit) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences)

#define BXR_VK_DECLARE(name) static PFN_##name p_##name = nullptr;
BXR_VK_DEVICE_FUNCS(BXR_VK_DECLARE)
#undef BXR_VK_DECLARE
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr = nullptr;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr = nullptr;
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties = nullptr;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties = nullptr;

// ---------------------------------------------------------------------------------------------
// OpenXR function table (loaded at runtime from openxr_loader.dll, so no import lib is needed)
// ---------------------------------------------------------------------------------------------

#define BXR_XR_FUNCS(X) \
	X(xrEnumerateInstanceExtensionProperties) \
	X(xrCreateInstance) \
	X(xrDestroyInstance) \
	X(xrGetInstanceProperties) \
	X(xrGetSystem) \
	X(xrGetSystemProperties) \
	X(xrPollEvent) \
	X(xrResultToString) \
	X(xrEnumerateViewConfigurationViews) \
	X(xrCreateSession) \
	X(xrDestroySession) \
	X(xrBeginSession) \
	X(xrEndSession) \
	X(xrRequestExitSession) \
	X(xrCreateReferenceSpace) \
	X(xrDestroySpace) \
	X(xrLocateSpace) \
	X(xrEnumerateSwapchainFormats) \
	X(xrCreateSwapchain) \
	X(xrDestroySwapchain) \
	X(xrEnumerateSwapchainImages) \
	X(xrAcquireSwapchainImage) \
	X(xrWaitSwapchainImage) \
	X(xrReleaseSwapchainImage) \
	X(xrWaitFrame) \
	X(xrBeginFrame) \
	X(xrEndFrame) \
	X(xrLocateViews) \
	X(xrStringToPath) \
	X(xrCreateActionSet) \
	X(xrDestroyActionSet) \
	X(xrCreateAction) \
	X(xrSuggestInteractionProfileBindings) \
	X(xrAttachSessionActionSets) \
	X(xrSyncActions) \
	X(xrGetActionStateBoolean) \
	X(xrGetActionStateFloat) \
	X(xrGetActionStateVector2f) \
	X(xrGetActionStatePose) \
	X(xrCreateActionSpace) \
	X(xrApplyHapticFeedback)

// graphics-API functions, loaded only for the path in use
#define BXR_XR_GFX_FUNCS(X) \
	X(xrGetD3D11GraphicsRequirementsKHR) \
	X(xrGetVulkanInstanceExtensionsKHR) \
	X(xrGetVulkanDeviceExtensionsKHR) \
	X(xrGetVulkanGraphicsDeviceKHR) \
	X(xrGetVulkanGraphicsRequirementsKHR)

#define BXR_DECLARE(name) static PFN_##name p_##name = nullptr;
BXR_XR_FUNCS(BXR_DECLARE)
BXR_XR_GFX_FUNCS(BXR_DECLARE)
#undef BXR_DECLARE

static HMODULE g_loader = nullptr;
static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr = nullptr;

static bool loadGlobalFuncs() {
	if (!g_loader) g_loader = LoadLibraryA("openxr_loader.dll");
	if (!g_loader) return fail("openxr_loader.dll not found next to the game exe");
	p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loader, "xrGetInstanceProcAddr");
	if (!p_xrGetInstanceProcAddr) return fail("openxr_loader.dll has no xrGetInstanceProcAddr export");
	p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&p_xrEnumerateInstanceExtensionProperties);
	p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&p_xrCreateInstance);
	if (!p_xrEnumerateInstanceExtensionProperties || !p_xrCreateInstance) return fail("OpenXR loader is missing core functions");
	return true;
}

static bool loadInstanceFuncs(XrInstance inst) {
#define BXR_LOAD(name) \
	if (!p_##name && XR_FAILED(p_xrGetInstanceProcAddr(inst, #name, (PFN_xrVoidFunction*)&p_##name))) return fail("missing OpenXR function " #name);
	BXR_XR_FUNCS(BXR_LOAD)
#undef BXR_LOAD
	return true;
}

static void clearFuncs() {
#define BXR_CLEAR(name) p_##name = nullptr;
	BXR_XR_FUNCS(BXR_CLEAR)
	BXR_XR_GFX_FUNCS(BXR_CLEAR)
#undef BXR_CLEAR
}

// ---------------------------------------------------------------------------------------------
// small math (Blitz conventions)
// ---------------------------------------------------------------------------------------------

struct V3 { float x, y, z; };
// Blitz-style matrix: columns i, j, k are the local X, Y, Z axes; M*v = i*v.x + j*v.y + k*v.z
struct M3 { V3 i, j, k; };

static const float RTOD = 57.29577951308232f;

static V3 v3(float x, float y, float z) { V3 v = { x, y, z }; return v; }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 mulMtV(const M3& m, V3 v) { return v3(dot(m.i, v), dot(m.j, v), dot(m.k, v)); }
static M3 mulMtM(const M3& a, const M3& b) { M3 r = { mulMtV(a, b.i), mulMtV(a, b.j), mulMtV(a, b.k) }; return r; }

// OpenXR (right-handed, -Z forward) -> Blitz (left-handed, +Z forward): mirror Z.
static V3 xrToBlitzPos(const XrVector3f& p, float scale) { return v3(p.x * scale, p.y * scale, -p.z * scale); }

static M3 xrToBlitzRot(const XrQuaternionf& q) {
	float x = q.x, y = q.y, z = q.z, w = q.w;
	// standard right-handed rotation matrix, R[row][col]
	float R[3][3] = {
		{ 1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w) },
		{ 2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
		{ 2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y) }
	};
	const float s[3] = { 1, 1, -1 };
	float B[3][3];
	for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) B[r][c] = s[r] * s[c] * R[r][c];
	M3 m;
	m.i = v3(B[0][0], B[1][0], B[2][0]);
	m.j = v3(B[0][1], B[1][1], B[2][1]);
	m.k = v3(B[0][2], B[1][2], B[2][2]);
	return m;
}

// Same decomposition Blitz uses for EntityPitch/Yaw/Roll, so RotateEntity/TurnEntity rebuilds it.
static void blitzEuler(const M3& m, float out[3]) {
	out[0] = -atan2f(m.k.y, sqrtf(m.k.x * m.k.x + m.k.z * m.k.z)) * RTOD; // pitch
	out[1] = -atan2f(m.k.x, m.k.z) * RTOD;                                // yaw
	out[2] = atan2f(m.i.y, m.j.y) * RTOD;                                 // roll
}

// ---------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------

struct Target {
	XrSwapchain swapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageD3D11KHR> images;
	int width = 0, height = 0;
	IDirect3DTexture9* tex9 = nullptr;
	IDirect3DSurface9* surf9 = nullptr;
	ID3D11Texture2D* tex11 = nullptr;
	ID3D11ShaderResourceView* srv11 = nullptr;   // for the alpha-keyed overlay copy
	std::vector<ID3D11RenderTargetView*> rtvs;   // one per swapchain image, made on first use
	bool submitted = false;
	// DXVK (Vulkan) path: tex9 is a plain render target whose VkImage is copied into the swapchain
	std::vector<XrSwapchainImageVulkanKHR> vkImages;
	struct ID3D9VkInteropTexture* vkTex = nullptr;
	IDirect3DTexture9* key9 = nullptr;           // alpha-keyed copy of tex9 for HUD / panel overlays
	IDirect3DSurface9* keySurf9 = nullptr;
	struct ID3D9VkInteropTexture* keyVkTex = nullptr;
};

enum { BTN_BLINK, BTN_SPRINT, BTN_CROUCH, BTN_INTERACT, BTN_INVENTORY, BTN_MENU, BTN_SAVE, BTN_GRIP_L, BTN_GRIP_R, BTN_COUNT };
enum { AX_MOVE_X, AX_MOVE_Y, AX_TURN_X, AX_TURN_Y, AX_COUNT };

static struct State {
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
	bool running = false;
	bool exitRequested = false;
	std::string runtimeName;

	XrSpace localSpace = XR_NULL_HANDLE;
	XrSpace viewSpace = XR_NULL_HANDLE;
	XrSpace baseSpace = XR_NULL_HANDLE; // recentred LOCAL space, all poses are reported in this
	bool spaceChanged = false; // set when the runtime recentres, read once by XR_SpaceChanged

	IDirect3DDevice9Ex* d9 = nullptr;
	IDirect3DQuery9* query9 = nullptr;
	ID3D11Device* d11 = nullptr;
	ID3D11DeviceContext* ctx11 = nullptr;
	ID3D11Query* query11 = nullptr;
	DXGI_FORMAT swapFormat = DXGI_FORMAT_UNKNOWN;

	// DXVK (Vulkan) path
	bool vk = false;
	struct ID3D9VkInteropDevice* vkInterop = nullptr;
	VkInstance vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice vkPhys = VK_NULL_HANDLE;
	VkDevice vkDevice = VK_NULL_HANDLE;
	VkQueue vkQueue = VK_NULL_HANDLE;
	uint32_t vkQueueIndex = 0, vkQueueFamily = 0;
	VkFormat vkFormat = VK_FORMAT_UNDEFINED;
	bool vkRawCopy = true;          // vkCmdCopyImage (same BGRA layout) or vkCmdBlitImage (channel swap)
	VkCommandPool vkPool = VK_NULL_HANDLE;
	VkCommandBuffer vkCmd = VK_NULL_HANDLE;
	VkFence vkFence = VK_NULL_HANDLE;
	IDirect3DPixelShader9* key9PS = nullptr;
	IDirect3DStateBlock9* key9State = nullptr;
	bool key9Tried = false;

	Target eyes[2];
	Target quad;
	float quadWidth = 2.0f, quadDistance = 2.0f;

	// The quad target is used three ways (overlayMode, set by the submit call each frame):
	//   0 = full floating screen on its own (main menu, loading screens), fixed in front of the origin
	//   1 = HUD over the 3D view, locked to the head, black is transparent
	//   2 = panel over the 3D view (pause menu, inventory), placed where you were looking when it opened
	int overlayMode = 0, lastOverlayMode = 0;
	bool overlayKey = false; // this frame's overlay was drawn on the key colour: only that is see-through (black stays)
	float hudWidth = 1.3f, hudDistance = 1.5f;
	XrPosef panelPose{ { 0, 0, 0, 1 }, { 0, 0, -2 } };
	XrPosef headPoseXr{ { 0, 0, 0, 1 }, { 0, 0, 0 } }; // raw head pose in base space, this frame
	ID3D11VertexShader* keyVS = nullptr;
	ID3D11PixelShader* keyPS = nullptr;
	ID3D11Buffer* keyCB = nullptr;
	ID3D11RasterizerState* keyRS = nullptr;
	bool keyTried = false;
	int endFrameErrors = 0;

	XrFrameState frameState{ XR_TYPE_FRAME_STATE };
	bool frameBegun = false;
	bool posesValid = false;
	XrView views[2]{ { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };

	float worldScale = 1.0f;
	float head[6] = {};       // x y z pitch yaw roll in base space
	float eyeRel[2][6] = {};  // eye relative to head
	float eyeTan[2][4] = {};  // tan left, right, up, down
	float hand[2][6] = {};
	bool handValid[2] = {};
	float grip[2][6] = {};    // grip pose (where the hand holds the controller), same layout as hand
	bool gripValid[2] = {};
	float gripValue[2] = {};  // analog squeeze 0..1
	float triggerValue[2] = {}; // analog trigger 0..1
	int touch[2][2] = { { -1, -1 }, { -1, -1 } }; // [hand][0 index on trigger, 1 thumb on rest/stick/buttons]: 1/0, -1 = no touch sensors

	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction btnAction[BTN_COUNT] = {};
	XrAction moveAction = XR_NULL_HANDLE, turnAction = XR_NULL_HANDLE;
	XrAction poseAction = XR_NULL_HANDLE, hapticAction = XR_NULL_HANDLE;
	XrAction gripPoseAction = XR_NULL_HANDLE, squeezeAction = XR_NULL_HANDLE, triggerAction = XR_NULL_HANDLE;
	XrAction touchAction[2] = {}; // capacitive touch: 0 index finger on the trigger, 1 thumb on any thumb control
	XrPath handPath[2] = {};
	XrSpace handSpace[2] = {};
	XrSpace gripSpace[2] = {};
	bool btnDown[BTN_COUNT] = {}, btnHit[BTN_COUNT] = {};
	bool pointerValid = false;
	float pointerU = 0, pointerV = 0;
	IDirect3DSurface9* resolve9 = nullptr;
	float axis[AX_COUNT] = {};
} S;

// On the DXVK path the OpenXR runtime submits to DXVK's Vulkan queue, which DXVK also uses from its
// own thread. Hold this around every OpenXR call that may touch the queue, and our own submits.
struct VkQueueLock {
	ID3D9VkInteropDevice* i;
	VkQueueLock() : i(S.vk ? S.vkInterop : nullptr) { if (i) i->LockSubmissionQueue(); }
	~VkQueueLock() { if (i) i->ReleaseSubmissionQueue(); }
	VkQueueLock(const VkQueueLock&) = delete;
	VkQueueLock& operator=(const VkQueueLock&) = delete;
};

static bool createTargetVk(Target& t, int w, int h);

static bool xrCheck(XrResult r, const char* what) {
	if (XR_SUCCEEDED(r)) return true;
	char name[XR_MAX_RESULT_STRING_SIZE] = "";
	if (p_xrResultToString && S.instance) p_xrResultToString(S.instance, r, name);
	return fail("%s failed: %s (%d)", what, name, (int)r);
}

static XrPath toPath(const char* s) {
	XrPath p = XR_NULL_PATH;
	p_xrStringToPath(S.instance, s, &p);
	return p;
}

// ---------------------------------------------------------------------------------------------
// D3D11 device on the headset's adapter
// ---------------------------------------------------------------------------------------------

static bool createD3D11(LUID luid, D3D_FEATURE_LEVEL minLevel) {
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return fail("CreateDXGIFactory1 failed");
	IDXGIAdapter1* adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);
		if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
			char name[128];
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
			logf("Headset GPU: %s", name);
			break;
		}
		safeRelease(adapter);
	}
	safeRelease(factory);
	if (!adapter) return fail("could not find the GPU the headset runtime asked for");

	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	UINT count = 0;
	while (count < 4 && levels[count] >= minLevel) ++count;
	if (count == 0) count = 1;
	HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, count, D3D11_SDK_VERSION, &S.d11, nullptr, &S.ctx11);
	safeRelease(adapter);
	if (FAILED(hr)) return fail("D3D11CreateDevice failed (0x%08X)", (unsigned)hr);

	D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
	S.d11->CreateQuery(&qd, &S.query11);
	return true;
}

static bool checkSameAdapter(LUID xrLuid) {
	IDirect3D9* d3d9 = nullptr;
	if (FAILED(S.d9->GetDirect3D(&d3d9))) return true;
	IDirect3D9Ex* d3d9ex = nullptr;
	bool ok = true;
	if (SUCCEEDED(d3d9->QueryInterface(__uuidof(IDirect3D9Ex), (void**)&d3d9ex))) {
		D3DDEVICE_CREATION_PARAMETERS cp;
		LUID luid;
		if (SUCCEEDED(S.d9->GetCreationParameters(&cp)) && SUCCEEDED(d3d9ex->GetAdapterLUID(cp.AdapterOrdinal, &luid))) {
			ok = luid.LowPart == xrLuid.LowPart && luid.HighPart == xrLuid.HighPart;
		}
		d3d9ex->Release();
	}
	d3d9->Release();
	if (!ok) return fail("the game is running on a different GPU than the headset. Set the game to use the same GPU (Windows Graphics settings) and try again");
	return true;
}

// ---------------------------------------------------------------------------------------------
// swapchains + shared textures
// ---------------------------------------------------------------------------------------------

static bool createTarget(Target& t, int w, int h) {
	if (S.vk) return createTargetVk(t, w, h);
	t.width = w; t.height = h;

	XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	sci.format = (int64_t)S.swapFormat;
	sci.sampleCount = 1;
	sci.width = w; sci.height = h;
	sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
	if (!xrCheck(p_xrCreateSwapchain(S.session, &sci, &t.swapchain), "xrCreateSwapchain")) { t.swapchain = XR_NULL_HANDLE; t.width = t.height = 0; return false; }

	uint32_t n = 0;
	p_xrEnumerateSwapchainImages(t.swapchain, 0, &n, nullptr);
	t.images.assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	if (!xrCheck(p_xrEnumerateSwapchainImages(t.swapchain, n, &n, (XrSwapchainImageBaseHeader*)t.images.data()), "xrEnumerateSwapchainImages")) return false;

	HANDLE shared = nullptr;
	HRESULT hr = S.d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t.tex9, &shared);
	if (FAILED(hr) || !shared) return fail("D3D9Ex shared texture creation failed (0x%08X)", (unsigned)hr);
	t.tex9->GetSurfaceLevel(0, &t.surf9);
	hr = S.d11->OpenSharedResource(shared, __uuidof(ID3D11Texture2D), (void**)&t.tex11);
	if (FAILED(hr)) return fail("D3D11 OpenSharedResource failed (0x%08X)", (unsigned)hr);
	// clear to black so an unsubmitted frame never shows garbage
	S.d9->ColorFill(t.surf9, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
	return true;
}

static void destroyTarget(Target& t) {
	if (t.swapchain && p_xrDestroySwapchain) { VkQueueLock lk; p_xrDestroySwapchain(t.swapchain); }
	t.swapchain = XR_NULL_HANDLE;
	t.images.clear();
	t.vkImages.clear();
	safeRelease(t.vkTex);
	safeRelease(t.keyVkTex);
	safeRelease(t.keySurf9);
	safeRelease(t.key9);
	for (auto*& v : t.rtvs) safeRelease(v);
	t.rtvs.clear();
	safeRelease(t.srv11);
	safeRelease(t.tex11);
	safeRelease(t.surf9);
	safeRelease(t.tex9);
	t.submitted = false;
}

static DXGI_FORMAT pickFormat() {
	uint32_t n = 0;
	p_xrEnumerateSwapchainFormats(S.session, 0, &n, nullptr);
	std::vector<int64_t> formats(n);
	p_xrEnumerateSwapchainFormats(S.session, n, &n, formats.data());
	// The game renders in gamma space into BGRA8; an sRGB swapchain of the same family
	// lets the runtime interpret those values correctly and allows a plain CopyResource.
	const DXGI_FORMAT wanted[] = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM };
	for (DXGI_FORMAT w : wanted)
		for (int64_t f : formats)
			if (f == (int64_t)w) return w;
	return DXGI_FORMAT_UNKNOWN;
}

// ---------------------------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------------------------

struct Binding { XrAction action; const char* path; };

static void suggest(const char* profile, const std::vector<Binding>& list) {
	std::vector<XrActionSuggestedBinding> b;
	for (const Binding& x : list) {
		XrPath p = toPath(x.path);
		if (p != XR_NULL_PATH) b.push_back({ x.action, p });
	}
	XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	sb.interactionProfile = toPath(profile);
	sb.countSuggestedBindings = (uint32_t)b.size();
	sb.suggestedBindings = b.data();
	XrResult r = p_xrSuggestInteractionProfileBindings(S.instance, &sb);
	if (XR_FAILED(r)) logf("bindings for %s rejected (%d)", profile, (int)r);
}

static XrAction makeAction(const char* name, const char* loc, XrActionType type, bool perHand) {
	XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
	strncpy(ci.actionName, name, sizeof(ci.actionName) - 1);
	strncpy(ci.localizedActionName, loc, sizeof(ci.localizedActionName) - 1);
	ci.actionType = type;
	if (perHand) { ci.countSubactionPaths = 2; ci.subactionPaths = S.handPath; }
	XrAction a = XR_NULL_HANDLE;
	xrCheck(p_xrCreateAction(S.actionSet, &ci, &a), name);
	return a;
}

static bool createActions() {
	XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(asci.actionSetName, "gameplay");
	strcpy(asci.localizedActionSetName, "Gameplay");
	if (!xrCheck(p_xrCreateActionSet(S.instance, &asci, &S.actionSet), "xrCreateActionSet")) return false;

	S.handPath[0] = toPath("/user/hand/left");
	S.handPath[1] = toPath("/user/hand/right");

	XrAction* B = S.btnAction;
	B[BTN_BLINK] = makeAction("blink", "Blink", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	B[BTN_SPRINT] = makeAction("sprint", "Sprint", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	B[BTN_CROUCH] = makeAction("crouch", "Crouch", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	B[BTN_INTERACT] = makeAction("interact", "Interact", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	B[BTN_INVENTORY] = makeAction("inventory", "Inventory", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	B[BTN_MENU] = makeAction("menu", "Pause menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	B[BTN_SAVE] = makeAction("save", "Save game", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	B[BTN_GRIP_L] = makeAction("grip_left", "Left grip", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	B[BTN_GRIP_R] = makeAction("grip_right", "Right grip", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	S.moveAction = makeAction("move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
	S.turnAction = makeAction("turn", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
	S.poseAction = makeAction("hand_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT, true);
	S.hapticAction = makeAction("haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
	S.gripPoseAction = makeAction("grip_pose", "Hand grip pose", XR_ACTION_TYPE_POSE_INPUT, true);
	S.squeezeAction = makeAction("squeeze_value", "Grip amount (hand model)", XR_ACTION_TYPE_FLOAT_INPUT, true);
	S.triggerAction = makeAction("trigger_value", "Trigger amount (hand model)", XR_ACTION_TYPE_FLOAT_INPUT, true);
	S.touchAction[0] = makeAction("index_touch", "Index finger on trigger (hand model)", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
	S.touchAction[1] = makeAction("thumb_touch", "Thumb on controls (hand model)", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
	XrAction* T = S.touchAction;

	const char* L = "/user/hand/left";
	const char* R = "/user/hand/right";
	// (the string ring must hold every path built for one profile's binding list, so keep it large)
	auto P = [](const char* hand, const char* rest) { static std::string s[256]; static int i = 0; i = (i + 1) % 256; s[i] = std::string(hand) + rest; return s[i].c_str(); };
	std::vector<Binding> common = {
		{ S.poseAction, P(L, "/input/aim/pose") }, { S.poseAction, P(R, "/input/aim/pose") },
		{ S.gripPoseAction, P(L, "/input/grip/pose") }, { S.gripPoseAction, P(R, "/input/grip/pose") },
		{ S.hapticAction, P(L, "/output/haptic") }, { S.hapticAction, P(R, "/output/haptic") },
	};

	// Meta Quest / Oculus Touch (also what Virtual Desktop presents)
	{
		std::vector<Binding> b = common;
		b.insert(b.end(), {
			{ S.moveAction, P(L, "/input/thumbstick") }, { S.turnAction, P(R, "/input/thumbstick") },
			{ B[BTN_BLINK], P(L, "/input/trigger/value") }, { B[BTN_INTERACT], P(R, "/input/trigger/value") },
			{ B[BTN_SPRINT], P(L, "/input/thumbstick/click") }, { B[BTN_CROUCH], P(R, "/input/a/click") },
			{ B[BTN_INVENTORY], P(R, "/input/b/click") }, { B[BTN_SAVE], P(L, "/input/y/click") },
			{ B[BTN_MENU], P(L, "/input/menu/click") },
			{ B[BTN_GRIP_L], P(L, "/input/squeeze/value") }, { B[BTN_GRIP_R], P(R, "/input/squeeze/value") },
			{ S.squeezeAction, P(L, "/input/squeeze/value") }, { S.squeezeAction, P(R, "/input/squeeze/value") },
			{ S.triggerAction, P(L, "/input/trigger/value") }, { S.triggerAction, P(R, "/input/trigger/value") },
			{ T[0], P(L, "/input/trigger/touch") }, { T[0], P(R, "/input/trigger/touch") },
			{ T[1], P(L, "/input/thumbrest/touch") }, { T[1], P(R, "/input/thumbrest/touch") },
			{ T[1], P(L, "/input/thumbstick/touch") }, { T[1], P(R, "/input/thumbstick/touch") },
			{ T[1], P(L, "/input/x/touch") }, { T[1], P(L, "/input/y/touch") },
			{ T[1], P(R, "/input/a/touch") }, { T[1], P(R, "/input/b/touch") },
		});
		suggest("/interaction_profiles/oculus/touch_controller", b);
	}
	// Valve Index
	{
		std::vector<Binding> b = common;
		b.insert(b.end(), {
			{ S.moveAction, P(L, "/input/thumbstick") }, { S.turnAction, P(R, "/input/thumbstick") },
			{ B[BTN_BLINK], P(L, "/input/trigger/click") }, { B[BTN_INTERACT], P(R, "/input/trigger/click") },
			{ B[BTN_SPRINT], P(L, "/input/thumbstick/click") }, { B[BTN_CROUCH], P(R, "/input/a/click") },
			{ B[BTN_INVENTORY], P(R, "/input/b/click") }, { B[BTN_SAVE], P(L, "/input/a/click") },
			{ B[BTN_MENU], P(L, "/input/b/click") },
			{ B[BTN_GRIP_L], P(L, "/input/squeeze/value") }, { B[BTN_GRIP_R], P(R, "/input/squeeze/value") },
			{ S.squeezeAction, P(L, "/input/squeeze/value") }, { S.squeezeAction, P(R, "/input/squeeze/value") },
			{ S.triggerAction, P(L, "/input/trigger/value") }, { S.triggerAction, P(R, "/input/trigger/value") },
			{ T[0], P(L, "/input/trigger/touch") }, { T[0], P(R, "/input/trigger/touch") },
			{ T[1], P(L, "/input/thumbstick/touch") }, { T[1], P(R, "/input/thumbstick/touch") },
			{ T[1], P(L, "/input/trackpad/touch") }, { T[1], P(R, "/input/trackpad/touch") },
			{ T[1], P(L, "/input/a/touch") }, { T[1], P(R, "/input/a/touch") },
			{ T[1], P(L, "/input/b/touch") }, { T[1], P(R, "/input/b/touch") },
		});
		suggest("/interaction_profiles/valve/index_controller", b);
	}
	// HTC Vive wands
	{
		std::vector<Binding> b = common;
		b.insert(b.end(), {
			{ S.moveAction, P(L, "/input/trackpad") }, { S.turnAction, P(R, "/input/trackpad") },
			{ B[BTN_BLINK], P(L, "/input/trigger/click") }, { B[BTN_INTERACT], P(R, "/input/trigger/click") },
			{ B[BTN_SPRINT], P(L, "/input/trackpad/click") }, { B[BTN_CROUCH], P(R, "/input/trackpad/click") },
			{ B[BTN_INVENTORY], P(R, "/input/menu/click") }, { B[BTN_MENU], P(L, "/input/menu/click") },
			{ B[BTN_GRIP_L], P(L, "/input/squeeze/click") }, { B[BTN_GRIP_R], P(R, "/input/squeeze/click") },
			{ S.squeezeAction, P(L, "/input/squeeze/click") }, { S.squeezeAction, P(R, "/input/squeeze/click") },
			{ S.triggerAction, P(L, "/input/trigger/value") }, { S.triggerAction, P(R, "/input/trigger/value") },
		});
		suggest("/interaction_profiles/htc/vive_controller", b);
	}
	// Windows Mixed Reality
	{
		std::vector<Binding> b = common;
		b.insert(b.end(), {
			{ S.moveAction, P(L, "/input/thumbstick") }, { S.turnAction, P(R, "/input/thumbstick") },
			{ B[BTN_BLINK], P(L, "/input/trigger/value") }, { B[BTN_INTERACT], P(R, "/input/trigger/value") },
			{ B[BTN_SPRINT], P(L, "/input/thumbstick/click") }, { B[BTN_CROUCH], P(R, "/input/trackpad/click") },
			{ B[BTN_INVENTORY], P(R, "/input/menu/click") }, { B[BTN_MENU], P(L, "/input/menu/click") },
			{ B[BTN_SAVE], P(L, "/input/trackpad/click") },
			{ B[BTN_GRIP_L], P(L, "/input/squeeze/click") }, { B[BTN_GRIP_R], P(R, "/input/squeeze/click") },
			{ S.squeezeAction, P(L, "/input/squeeze/click") }, { S.squeezeAction, P(R, "/input/squeeze/click") },
			{ S.triggerAction, P(L, "/input/trigger/value") }, { S.triggerAction, P(R, "/input/trigger/value") },
		});
		suggest("/interaction_profiles/microsoft/motion_controller", b);
	}
	// generic fallback
	{
		std::vector<Binding> b = {
			{ S.poseAction, P(L, "/input/aim/pose") }, { S.poseAction, P(R, "/input/aim/pose") },
			{ S.hapticAction, P(L, "/output/haptic") }, { S.hapticAction, P(R, "/output/haptic") },
			{ B[BTN_BLINK], P(L, "/input/select/click") }, { B[BTN_INTERACT], P(R, "/input/select/click") },
			{ B[BTN_MENU], P(L, "/input/menu/click") }, { B[BTN_INVENTORY], P(R, "/input/menu/click") },
			{ S.gripPoseAction, P(L, "/input/grip/pose") }, { S.gripPoseAction, P(R, "/input/grip/pose") },
			{ S.triggerAction, P(L, "/input/select/click") }, { S.triggerAction, P(R, "/input/select/click") },
		};
		suggest("/interaction_profiles/khr/simple_controller", b);
	}

	for (int h = 0; h < 2; ++h) {
		XrActionSpaceCreateInfo sci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		sci.action = S.poseAction;
		sci.subactionPath = S.handPath[h];
		sci.poseInActionSpace.orientation.w = 1;
		p_xrCreateActionSpace(S.session, &sci, &S.handSpace[h]);
		sci.action = S.gripPoseAction;
		if (S.gripPoseAction) p_xrCreateActionSpace(S.session, &sci, &S.gripSpace[h]);
	}

	XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	ai.countActionSets = 1;
	ai.actionSets = &S.actionSet;
	return xrCheck(p_xrAttachSessionActionSets(S.session, &ai), "xrAttachSessionActionSets");
}

static void syncActions() {
	for (int i = 0; i < BTN_COUNT; ++i) S.btnHit[i] = false;
	if (!S.running || !S.actionSet) return;
	XrActiveActionSet active = { S.actionSet, XR_NULL_PATH };
	XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
	si.countActiveActionSets = 1;
	si.activeActionSets = &active;
	if (XR_FAILED(p_xrSyncActions(S.session, &si))) return;

	for (int i = 0; i < BTN_COUNT; ++i) {
		XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
		gi.action = S.btnAction[i];
		XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
		if (XR_SUCCEEDED(p_xrGetActionStateBoolean(S.session, &gi, &st)) && st.isActive) {
			S.btnDown[i] = st.currentState != XR_FALSE;
			S.btnHit[i] = st.currentState && st.changedSinceLastSync;
		}
		else {
			S.btnDown[i] = false;
		}
	}
	XrAction axes[2] = { S.moveAction, S.turnAction };
	for (int a = 0; a < 2; ++a) {
		XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
		gi.action = axes[a];
		XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
		if (XR_SUCCEEDED(p_xrGetActionStateVector2f(S.session, &gi, &st)) && st.isActive) {
			S.axis[a * 2] = st.currentState.x;
			S.axis[a * 2 + 1] = st.currentState.y;
		}
		else {
			S.axis[a * 2] = S.axis[a * 2 + 1] = 0;
		}
	}
	// capacitive touch per hand; -1 when the controller has no touch sensor bound for it (Vive, WMR, simple)
	for (int a = 0; a < 2; ++a) {
		for (int h = 0; h < 2; ++h) {
			S.touch[h][a] = -1;
			if (!S.touchAction[a]) continue;
			XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
			gi.action = S.touchAction[a];
			gi.subactionPath = S.handPath[h];
			XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
			if (XR_SUCCEEDED(p_xrGetActionStateBoolean(S.session, &gi, &st)) && st.isActive)
				S.touch[h][a] = st.currentState ? 1 : 0;
		}
	}
	// analog squeeze / trigger per hand (only drive the hand models' finger curl)
	XrAction fl[2] = { S.squeezeAction, S.triggerAction };
	float* out[2] = { S.gripValue, S.triggerValue };
	for (int a = 0; a < 2; ++a) {
		for (int h = 0; h < 2; ++h) {
			out[a][h] = 0;
			if (!fl[a]) continue;
			XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
			gi.action = fl[a];
			gi.subactionPath = S.handPath[h];
			XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
			if (XR_SUCCEEDED(p_xrGetActionStateFloat(S.session, &gi, &st)) && st.isActive) {
				float v = st.currentState;
				out[a][h] = v < 0 ? 0 : (v > 1 ? 1 : v);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// spaces / poses
// ---------------------------------------------------------------------------------------------

// Recentring is left to the VR software (e.g. holding the Oculus / system button): all poses are in the
// runtime's LOCAL space, which the runtime moves to your current head position and facing when you
// recentre there. XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING then tells the game (XR_SpaceChanged)
// so it can re-capture your standing height.

static void poseToBlitz(const XrPosef& p, float out[6], V3* posOut = nullptr, M3* rotOut = nullptr) {
	V3 pos = xrToBlitzPos(p.position, S.worldScale);
	M3 rot = xrToBlitzRot(p.orientation);
	out[0] = pos.x; out[1] = pos.y; out[2] = pos.z;
	blitzEuler(rot, out + 3);
	if (posOut) *posOut = pos;
	if (rotOut) *rotOut = rot;
}

// quaternion helpers (OpenXR convention)
static XrVector3f qrot(const XrQuaternionf& q, const XrVector3f& v) {
	// v' = v + 2w(u x v) + 2 u x (u x v)
	XrVector3f u = { q.x, q.y, q.z };
	XrVector3f t = { 2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x) };
	return { v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
	         v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
	         v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
}
static XrQuaternionf qconj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }

static float quadAspect() { return S.quad.width ? (float)S.quad.height / S.quad.width : 0.5625f; }

// Where the pointable screen is this frame (in base space), or false if nothing can be pointed at.
static bool pointerTarget(XrPosef& pose, float& w) {
	if (S.lastOverlayMode == 2) { pose = S.panelPose; w = S.quadWidth; return true; }
	if (S.lastOverlayMode == 0) { pose = { { 0, 0, 0, 1 }, { 0, 0, -S.quadDistance } }; w = S.quadWidth; return true; }
	return false; // the head-locked HUD isn't interactive
}

// Aim ray of a controller against the current screen; sets pointerU/V (0..1, top-left origin).
static void updatePointer(const XrPosef& p) {
	XrPosef qp; float w;
	if (!pointerTarget(qp, w)) return;
	XrQuaternionf inv = qconj(qp.orientation);
	XrVector3f rel = { p.position.x - qp.position.x, p.position.y - qp.position.y, p.position.z - qp.position.z };
	XrVector3f o = qrot(inv, rel);                                   // ray origin in screen space
	XrVector3f d = qrot(inv, qrot(p.orientation, { 0, 0, -1 }));     // aim direction in screen space
	if (d.z > -0.05f || o.z <= 0) return;                            // must point at the front face
	float t = -o.z / d.z;
	float hx = o.x + d.x * t, hy = o.y + d.y * t;
	float h = w * quadAspect();
	float u = hx / w + 0.5f, v = 0.5f - hy / h;
	if (u < 0 || u > 1 || v < 0 || v > 1) return;
	S.pointerU = u; S.pointerV = v; S.pointerValid = true;
}

// Put the panel in front of the head, level, facing it.
static void placePanel() {
	const XrQuaternionf& q = S.headPoseXr.orientation;
	float fx = -(2 * (q.x * q.z + q.y * q.w));
	float fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
	float len = sqrtf(fx * fx + fz * fz);
	if (len < 0.001f) { fx = 0; fz = -1; } else { fx /= len; fz /= len; }
	float yaw = atan2f(-fx, -fz);
	S.panelPose.orientation = { 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f) };
	S.panelPose.position = { S.headPoseXr.position.x + fx * S.quadDistance,
	                         S.headPoseXr.position.y - 0.1f,
	                         S.headPoseXr.position.z + fz * S.quadDistance };
}

static void locate() {
	S.posesValid = false;
	S.pointerValid = false;
	XrTime t = S.frameState.predictedDisplayTime;
	XrSpace base = S.baseSpace ? S.baseSpace : S.localSpace;

	XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
	li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	li.displayTime = t;
	li.space = base;
	XrViewState vs{ XR_TYPE_VIEW_STATE };
	uint32_t n = 0;
	S.views[0] = { XR_TYPE_VIEW };
	S.views[1] = { XR_TYPE_VIEW };
	if (XR_FAILED(p_xrLocateViews(S.session, &li, &vs, 2, &n, S.views)) || n != 2) return;
	if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;

	XrSpaceLocation hl{ XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(p_xrLocateSpace(S.viewSpace, base, t, &hl))) return;

	S.headPoseXr = hl.pose;
	V3 hp; M3 hr;
	poseToBlitz(hl.pose, S.head, &hp, &hr);
	for (int e = 0; e < 2; ++e) {
		V3 ep = xrToBlitzPos(S.views[e].pose.position, S.worldScale);
		M3 er = xrToBlitzRot(S.views[e].pose.orientation);
		V3 off = mulMtV(hr, sub(ep, hp));
		M3 rel = mulMtM(hr, er);
		S.eyeRel[e][0] = off.x; S.eyeRel[e][1] = off.y; S.eyeRel[e][2] = off.z;
		blitzEuler(rel, S.eyeRel[e] + 3);
		const XrFovf& f = S.views[e].fov;
		S.eyeTan[e][0] = tanf(f.angleLeft);
		S.eyeTan[e][1] = tanf(f.angleRight);
		S.eyeTan[e][2] = tanf(f.angleUp);
		S.eyeTan[e][3] = tanf(f.angleDown);
	}
	for (int h = 0; h < 2; ++h) {
		S.handValid[h] = false;
		if (!S.handSpace[h]) continue;
		XrSpaceLocation l{ XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(p_xrLocateSpace(S.handSpace[h], base, t, &l)) &&
			(l.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (l.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
			poseToBlitz(l.pose, S.hand[h]);
			S.handValid[h] = true;
			if (h == 1 || !S.pointerValid) updatePointer(l.pose);
		}
		S.gripValid[h] = false;
		if (!S.gripSpace[h]) continue;
		XrSpaceLocation g{ XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(p_xrLocateSpace(S.gripSpace[h], base, t, &g)) &&
			(g.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (g.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
			poseToBlitz(g.pose, S.grip[h]);
			S.gripValid[h] = true;
		}
	}
	S.posesValid = true;
}

// ---------------------------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------------------------

static void pollEvents() {
	if (!S.instance) return;
	XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
	while (p_xrPollEvent(S.instance, &ev) == XR_SUCCESS) {
		switch (ev.type) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			const XrEventDataSessionStateChanged& e = *(XrEventDataSessionStateChanged*)&ev;
			S.sessionState = e.state;
			logf("session state %d", (int)e.state);
			if (e.state == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				XrResult br;
				{ VkQueueLock lk; br = p_xrBeginSession(S.session, &bi); }
				if (xrCheck(br, "xrBeginSession")) S.running = true;
			}
			else if (e.state == XR_SESSION_STATE_STOPPING) {
				VkQueueLock lk;
				p_xrEndSession(S.session);
				S.running = false;
			}
			else if (e.state == XR_SESSION_STATE_EXITING || e.state == XR_SESSION_STATE_LOSS_PENDING) {
				S.running = false;
				S.exitRequested = true;
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			S.running = false;
			S.exitRequested = true;
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			// the runtime recentred (or the tracking origin moved); LOCAL follows it by itself
			logf("reference space changed (recentred by the VR software)");
			S.spaceChanged = true;
			break;
		default:
			break;
		}
		ev = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

// ---------------------------------------------------------------------------------------------
// frame submission helpers
// ---------------------------------------------------------------------------------------------

static void waitD3D9() {
	if (!S.query9) S.d9->CreateQuery(D3DQUERYTYPE_EVENT, &S.query9);
	if (!S.query9) return;
	S.query9->Issue(D3DISSUE_END);
	BOOL done = FALSE;
	for (int spins = 0; spins < 200000; ++spins) {
		HRESULT hr = S.query9->GetData(&done, sizeof(done), D3DGETDATA_FLUSH);
		if (hr == S_OK) break;
		if (hr != S_FALSE) break; // device lost etc.
		if (spins > 64) Sleep(0);
	}
}

static void waitD3D11() {
	if (!S.query11) return;
	S.ctx11->End(S.query11);
	S.ctx11->Flush();
	BOOL done = FALSE;
	for (int spins = 0; spins < 200000; ++spins) {
		if (S.ctx11->GetData(S.query11, &done, sizeof(done), 0) == S_OK) break;
		if (spins > 64) Sleep(0);
	}
}

static bool copyToSwapchain(Target& t) {
	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (XR_FAILED(p_xrAcquireSwapchainImage(t.swapchain, &ai, &idx))) return false;
	XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wi.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(p_xrWaitSwapchainImage(t.swapchain, &wi))) return false;
	S.ctx11->CopyResource(t.images[idx].texture, t.tex11);
	XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	p_xrReleaseSwapchainImage(t.swapchain, &ri);
	return true;
}

// ---- alpha-keyed copy: black becomes transparent so the HUD and panels float over the 3D view (alphaScale > 0),
// or, in key mode (alphaScale < 0), only the key colour (0,0,12) does and black stays opaque
typedef HRESULT(WINAPI* PFN_D3DCompileX)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static const char* g_keyHLSL =
	"cbuffer C : register(b0) { float alphaScale; float srgbDecode; float2 pad; };\n"
	"Texture2D tex : register(t0);\n"
	"float4 vs(uint id : SV_VertexID) : SV_Position {\n"
	"  float2 uv = float2((id << 1) & 2, id & 2);\n"
	"  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
	"}\n"
	"float3 toLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }\n"
	"float4 ps(float4 pos : SV_Position) : SV_Target {\n"
	"  float4 c = tex.Load(int3(pos.xy, 0));\n"
	"  float3 K = float3(0, 0, 12.0 / 255.0);\n" // the key colour the game clears its 2D layer to (VR.bb VR_KEY_*)
	"  float a = saturate(max(c.r, max(c.g, c.b)) * alphaScale);\n" // black is see-through, dark greys partly
	"  float3 p = c.rgb;\n"
	"  if (alphaScale < 0) {\n" // key mode: only the key colour is see-through, black stays opaque
	"    a = saturate(distance(c.rgb, K) / 0.035);\n"
	"    p = max(c.rgb - (1.0 - a) * K, 0.0);\n" // premultiplied: take the key colour back out of edge pixels
	"  }\n"
	"  float3 rgb = srgbDecode > 0.5 ? toLinear(p) : p;\n"
	"  return float4(rgb, a);\n" // premultiplied: the black background already scales the colour
	"}\n";

static bool initKeyShader() {
	if (S.keyTried) return S.keyPS != nullptr;
	S.keyTried = true;
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	if (!dc) dc = LoadLibraryA("d3dcompiler_43.dll");
	PFN_D3DCompileX compile = dc ? (PFN_D3DCompileX)GetProcAddress(dc, "D3DCompile") : nullptr;
	if (!compile) { logf("d3dcompiler not found, HUD and panels will be opaque"); return false; }
	ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
	size_t len = strlen(g_keyHLSL);
	if (FAILED(compile(g_keyHLSL, len, "key", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
		FAILED(compile(g_keyHLSL, len, "key", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
		logf("overlay shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
		safeRelease(err); safeRelease(vsb); safeRelease(psb);
		return false;
	}
	S.d11->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &S.keyVS);
	S.d11->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &S.keyPS);
	safeRelease(vsb); safeRelease(psb); safeRelease(err);
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	S.d11->CreateBuffer(&bd, nullptr, &S.keyCB);
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
	S.d11->CreateRasterizerState(&rd, &S.keyRS);
	if (!S.keyVS || !S.keyPS || !S.keyCB || !S.keyRS) { logf("overlay shader setup failed, HUD and panels will be opaque"); safeRelease(S.keyPS); return false; }
	logf("overlay alpha key ready");
	return true;
}

static void releaseKeyShader() {
	safeRelease(S.keyVS); safeRelease(S.keyPS); safeRelease(S.keyCB); safeRelease(S.keyRS);
	S.keyTried = false;
}

// Like copyToSwapchain, but derives alpha from brightness. Falls back to a plain copy.
static bool keyToSwapchain(Target& t, float alphaScale) {
	if (!initKeyShader()) return copyToSwapchain(t);
	if (!t.srv11) {
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		if (FAILED(S.d11->CreateShaderResourceView(t.tex11, &sd, &t.srv11))) {
			static bool warned = false;
			if (!warned) { logf("overlay SRV creation failed, using a plain copy"); warned = true; }
			return copyToSwapchain(t);
		}
	}
	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (XR_FAILED(p_xrAcquireSwapchainImage(t.swapchain, &ai, &idx))) return false;
	XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wi.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(p_xrWaitSwapchainImage(t.swapchain, &wi))) return false;
	if (t.rtvs.size() != t.images.size()) t.rtvs.assign(t.images.size(), nullptr);
	if (!t.rtvs[idx]) {
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = S.swapFormat;
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		S.d11->CreateRenderTargetView(t.images[idx].texture, &rd, &t.rtvs[idx]);
	}
	if (t.rtvs[idx]) {
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(S.ctx11->Map(S.keyCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			float* f = (float*)m.pData;
			f[0] = alphaScale;
			f[1] = (S.swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? 1.0f : 0.0f;
			f[2] = f[3] = 0;
			S.ctx11->Unmap(S.keyCB, 0);
		}
		D3D11_VIEWPORT vp = { 0, 0, (float)t.width, (float)t.height, 0, 1 };
		S.ctx11->OMSetRenderTargets(1, &t.rtvs[idx], nullptr);
		S.ctx11->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		S.ctx11->OMSetDepthStencilState(nullptr, 0);
		S.ctx11->RSSetState(S.keyRS);
		S.ctx11->RSSetViewports(1, &vp);
		S.ctx11->IASetInputLayout(nullptr);
		S.ctx11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		S.ctx11->VSSetShader(S.keyVS, nullptr, 0);
		S.ctx11->PSSetShader(S.keyPS, nullptr, 0);
		S.ctx11->PSSetConstantBuffers(0, 1, &S.keyCB);
		S.ctx11->PSSetShaderResources(0, 1, &t.srv11);
		S.ctx11->Draw(3, 0);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		S.ctx11->PSSetShaderResources(0, 1, &nullSrv);
		S.ctx11->OMSetRenderTargets(0, nullptr, nullptr);
	}
	else {
		S.ctx11->CopyResource(t.images[idx].texture, t.tex11);
	}
	XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	p_xrReleaseSwapchainImage(t.swapchain, &ri);
	return true;
}

// =============================================================================================
// DXVK (Vulkan) path
//
// When the game runs on DXVK (Direct3D 9 implemented on Vulkan), there is no D3D9Ex/D3D11 sharing.
// Instead the OpenXR session is created on DXVK's own Vulkan device and queue (XR_KHR_vulkan_enable),
// the eye / screen textures are plain D3D9 render targets, and each frame their VkImages are copied
// into the OpenXR swapchain images on DXVK's queue. The HUD / panel alpha key is done as a small
// Direct3D 9 pixel-shader pass (DXVK runs it), so no SPIR-V is needed.
// =============================================================================================

static ID3D9VkInteropDevice* queryInterop(IDirect3DDevice9* d) {
	ID3D9VkInteropDevice* i = nullptr;
	if (!d || FAILED(d->QueryInterface(__uuidof(ID3D9VkInteropDevice), (void**)&i))) return nullptr;
	return i;
}

static ID3D9VkInteropTexture* queryInteropTex(IDirect3DTexture9* tex, IDirect3DSurface9* surf) {
	ID3D9VkInteropTexture* t = nullptr;
	if (tex && SUCCEEDED(tex->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&t)) && t) return t;
	t = nullptr;
	if (surf && SUCCEEDED(surf->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&t)) && t) return t;
	return nullptr;
}

static bool loadVkFuncs() {
	HMODULE vk = LoadLibraryA("vulkan-1.dll"); // already loaded by DXVK; this only takes a reference
	if (!vk) return fail("vulkan-1.dll not found");
	p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
	if (!p_vkGetInstanceProcAddr) return fail("vulkan-1.dll has no vkGetInstanceProcAddr");
	p_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)p_vkGetInstanceProcAddr(S.vkInstance, "vkGetDeviceProcAddr");
	p_vkEnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)p_vkGetInstanceProcAddr(S.vkInstance, "vkEnumerateDeviceExtensionProperties");
	p_vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)p_vkGetInstanceProcAddr(S.vkInstance, "vkGetPhysicalDeviceProperties");
	if (!p_vkGetDeviceProcAddr) return fail("could not load vkGetDeviceProcAddr");
#define BXR_VK_LOAD(name) \
	p_##name = (PFN_##name)p_vkGetDeviceProcAddr(S.vkDevice, #name); \
	if (!p_##name) return fail("missing Vulkan function " #name);
	BXR_VK_DEVICE_FUNCS(BXR_VK_LOAD)
#undef BXR_VK_LOAD
	return true;
}

// OpenXR returns extension lists as one space-separated string
static std::string xrVkExtList(bool device) {
	uint32_t n = 0;
	XrResult r = device ? p_xrGetVulkanDeviceExtensionsKHR(S.instance, S.system, 0, &n, nullptr)
	                    : p_xrGetVulkanInstanceExtensionsKHR(S.instance, S.system, 0, &n, nullptr);
	if (XR_FAILED(r) || n == 0) return "";
	std::string s(n, '\0');
	r = device ? p_xrGetVulkanDeviceExtensionsKHR(S.instance, S.system, n, &n, &s[0])
	           : p_xrGetVulkanInstanceExtensionsKHR(S.instance, S.system, n, &n, &s[0]);
	if (XR_FAILED(r)) return "";
	s.resize(strlen(s.c_str()));
	return s;
}

// Take DXVK's Vulkan objects, check them against what the runtime wants, and make our command buffer.
static bool vkSetupFromDxvk() {
	S.vkInterop->GetVulkanHandles(&S.vkInstance, &S.vkPhys, &S.vkDevice);
	if (!S.vkInstance || !S.vkDevice)
		return fail("DXVK did not return its Vulkan device");
	S.vkInterop->GetSubmissionQueue(&S.vkQueue, &S.vkQueueIndex, &S.vkQueueFamily);
	if (!S.vkQueue)
		return fail("DXVK did not return its Vulkan queue");
	if (!loadVkFuncs()) return false;
	if (p_vkGetPhysicalDeviceProperties) {
		VkPhysicalDeviceProperties pp;
		p_vkGetPhysicalDeviceProperties(S.vkPhys, &pp);
		logf("DXVK GPU: %s (Vulkan %u.%u), queue family %u index %u", pp.deviceName,
			VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), S.vkQueueFamily, S.vkQueueIndex);
	}

	XrGraphicsRequirementsVulkanKHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
	if (!xrCheck(p_xrGetVulkanGraphicsRequirementsKHR(S.instance, S.system, &req), "xrGetVulkanGraphicsRequirementsKHR")) return false;
	logf("runtime supports Vulkan %u.%u to %u.%u", XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
		XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));

	VkPhysicalDevice want = VK_NULL_HANDLE;
	if (!xrCheck(p_xrGetVulkanGraphicsDeviceKHR(S.instance, S.system, S.vkInstance, &want), "xrGetVulkanGraphicsDeviceKHR")) return false;
	if (want != S.vkPhys)
		return fail("DXVK is running on a different GPU than the headset. Set the game to use the same GPU (Windows Graphics settings) and try again");

	// DXVK created its Vulkan instance and device itself, so the extensions the runtime asks for can't
	// be added; report them so a session failure can be traced.
	std::string ie = xrVkExtList(false), de = xrVkExtList(true);
	logf("runtime asks for Vulkan instance extensions: %s", ie.empty() ? "(none)" : ie.c_str());
	logf("runtime asks for Vulkan device extensions: %s", de.empty() ? "(none)" : de.c_str());
	if (!de.empty() && p_vkEnumerateDeviceExtensionProperties) {
		uint32_t n = 0;
		p_vkEnumerateDeviceExtensionProperties(S.vkPhys, nullptr, &n, nullptr);
		std::vector<VkExtensionProperties> have(n);
		p_vkEnumerateDeviceExtensionProperties(S.vkPhys, nullptr, &n, have.data());
		size_t p = 0;
		while (p < de.size()) {
			size_t q = de.find(' ', p);
			if (q == std::string::npos) q = de.size();
			std::string name = de.substr(p, q - p);
			bool ok = name.empty();
			for (auto& e : have) if (name == e.extensionName) ok = true;
			if (!ok) logf("WARNING: the GPU does not offer %s, which the runtime asks for", name.c_str());
			p = q + 1;
		}
	}

	VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = S.vkQueueFamily;
	if (p_vkCreateCommandPool(S.vkDevice, &pci, nullptr, &S.vkPool) != VK_SUCCESS) return fail("vkCreateCommandPool failed");
	VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool = S.vkPool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	if (p_vkAllocateCommandBuffers(S.vkDevice, &ai, &S.vkCmd) != VK_SUCCESS) return fail("vkAllocateCommandBuffers failed");
	VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	if (p_vkCreateFence(S.vkDevice, &fi, nullptr, &S.vkFence) != VK_SUCCESS) return fail("vkCreateFence failed");
	return true;
}

static bool pickVkFormat() {
	uint32_t n = 0;
	p_xrEnumerateSwapchainFormats(S.session, 0, &n, nullptr);
	std::vector<int64_t> formats(n);
	p_xrEnumerateSwapchainFormats(S.session, n, &n, formats.data());
	auto has = [&](VkFormat f) { for (int64_t x : formats) if (x == (int64_t)f) return true; return false; };
	// Same as the D3D11 path: the game's gamma-space BGRA8 pixels are copied unchanged into an sRGB
	// swapchain, which tells the runtime they are already gamma encoded.
	const VkFormat copyable[] = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM };
	for (VkFormat f : copyable) if (has(f)) { S.vkFormat = f; S.vkRawCopy = true; logf("swapchain format VkFormat %d (copy)", (int)f); return true; }
	const VkFormat blitted[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
	for (VkFormat f : blitted) if (has(f)) {
		S.vkFormat = f; S.vkRawCopy = false;
		logf("swapchain format VkFormat %d (blit; the runtime offers no BGRA8, colours may be slightly off)", (int)f);
		return true;
	}
	return fail("the runtime offers no 8-bit RGBA/BGRA Vulkan swapchain format");
}

static bool createTargetVk(Target& t, int w, int h) {
	t.width = w; t.height = h;
	XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	sci.format = (int64_t)S.vkFormat;
	sci.sampleCount = 1;
	sci.width = w; sci.height = h;
	sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
	XrResult r;
	{ VkQueueLock lk; r = p_xrCreateSwapchain(S.session, &sci, &t.swapchain); }
	if (!xrCheck(r, "xrCreateSwapchain")) { t.swapchain = XR_NULL_HANDLE; t.width = t.height = 0; return false; }

	uint32_t n = 0;
	p_xrEnumerateSwapchainImages(t.swapchain, 0, &n, nullptr);
	t.vkImages.assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
	if (!xrCheck(p_xrEnumerateSwapchainImages(t.swapchain, n, &n, (XrSwapchainImageBaseHeader*)t.vkImages.data()), "xrEnumerateSwapchainImages")) return false;

	HRESULT hr = S.d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t.tex9, nullptr);
	if (FAILED(hr)) return fail("D3D9 render target creation failed (0x%08X)", (unsigned)hr);
	t.tex9->GetSurfaceLevel(0, &t.surf9);
	t.vkTex = queryInteropTex(t.tex9, t.surf9);
	if (!t.vkTex) return fail("DXVK gave no Vulkan image for a render target (ID3D9VkInteropTexture)");
	S.d9->ColorFill(t.surf9, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
	return true;
}

// ---- alpha key as a Direct3D 9 pass (runs on DXVK): t.tex9 -> t.key9 with alpha from brightness
static const char* g_key9HLSL =
	"sampler2D s : register(s0);\n"
	"float4 p : register(c0);\n"
	"float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
	"  float4 c = tex2D(s, uv);\n"
	"  float3 K = float3(0, 0, 12.0 / 255.0);\n" // key colour, as in the D3D11 path
	"  if (p.x < 0) {\n" // key mode: only the key colour is see-through, black stays opaque
	"    float a = saturate(distance(c.rgb, K) / 0.035);\n"
	"    return float4(max(c.rgb - (1.0 - a) * K, 0.0), a);\n"
	"  }\n"
	"  return float4(c.rgb, saturate(max(c.r, max(c.g, c.b)) * p.x));\n" // premultiplied, like the D3D11 path
	"}\n";

static bool initKey9() {
	if (S.key9Tried) return S.key9PS != nullptr;
	S.key9Tried = true;
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	if (!dc) dc = LoadLibraryA("d3dcompiler_43.dll");
	PFN_D3DCompileX compile = dc ? (PFN_D3DCompileX)GetProcAddress(dc, "D3DCompile") : nullptr;
	if (!compile) { logf("d3dcompiler not found, HUD hidden and panels opaque"); return false; }
	ID3DBlob *psb = nullptr, *err = nullptr;
	if (FAILED(compile(g_key9HLSL, strlen(g_key9HLSL), "key9", nullptr, nullptr, "main", "ps_2_0", 0, 0, &psb, &err))) {
		logf("overlay shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
		safeRelease(err); safeRelease(psb);
		return false;
	}
	S.d9->CreatePixelShader((const DWORD*)psb->GetBufferPointer(), &S.key9PS);
	safeRelease(psb); safeRelease(err);
	S.d9->CreateStateBlock(D3DSBT_ALL, &S.key9State);
	if (!S.key9PS || !S.key9State) { logf("overlay shader setup failed, HUD hidden and panels opaque"); safeRelease(S.key9PS); safeRelease(S.key9State); return false; }
	logf("overlay alpha key ready (Direct3D 9 pass)");
	return true;
}

static bool key9Pass(Target& t, float alphaScale) {
	if (!initKey9() || !t.tex9) return false;
	if (!t.key9) {
		if (FAILED(S.d9->CreateTexture(t.width, t.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t.key9, nullptr))) return false;
		t.key9->GetSurfaceLevel(0, &t.keySurf9);
		t.keyVkTex = queryInteropTex(t.key9, t.keySurf9);
		if (!t.keySurf9 || !t.keyVkTex) { safeRelease(t.keyVkTex); safeRelease(t.keySurf9); safeRelease(t.key9); return false; }
	}

	// save everything the game had set, draw one full-screen quad, put it all back
	S.key9State->Capture();
	IDirect3DSurface9* oldRT = nullptr, * oldDS = nullptr;
	S.d9->GetRenderTarget(0, &oldRT);
	S.d9->GetDepthStencilSurface(&oldDS);

	S.d9->SetRenderTarget(0, t.keySurf9);
	S.d9->SetDepthStencilSurface(nullptr);
	D3DVIEWPORT9 vp = { 0, 0, (DWORD)t.width, (DWORD)t.height, 0.0f, 1.0f };
	S.d9->SetViewport(&vp);
	S.d9->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
	S.d9->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	S.d9->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	S.d9->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	S.d9->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	S.d9->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	S.d9->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	S.d9->SetRenderState(D3DRS_FOGENABLE, FALSE);
	S.d9->SetRenderState(D3DRS_LIGHTING, FALSE);
	S.d9->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	S.d9->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	S.d9->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	S.d9->SetVertexShader(nullptr);
	S.d9->SetPixelShader(S.key9PS);
	S.d9->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	S.d9->SetTexture(0, t.tex9);
	S.d9->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	S.d9->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	S.d9->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	S.d9->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	S.d9->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	S.d9->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
	float c[4] = { alphaScale, 0, 0, 0 };
	S.d9->SetPixelShaderConstantF(0, c, 1);

	struct V { float x, y, z, rhw, u, v; };
	float w = (float)t.width - 0.5f, h = (float)t.height - 0.5f;
	V q[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w, -0.5f, 0, 1, 1, 0 }, { -0.5f, h, 0, 1, 0, 1 }, { w, h, 0, 1, 1, 1 } };
	bool ok = false;
	if (SUCCEEDED(S.d9->BeginScene())) {
		ok = SUCCEEDED(S.d9->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
		S.d9->EndScene();
	}

	S.d9->SetRenderTarget(0, oldRT);
	S.d9->SetDepthStencilSurface(oldDS);
	safeRelease(oldRT);
	safeRelease(oldDS);
	S.key9State->Apply();
	return ok;
}

struct VkCopyJob {
	Target* t;
	ID3D9VkInteropTexture* src;
	VkImage img = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	uint32_t idx = 0;
	bool acquired = false;
};

// Copy each job's D3D9 texture into its next swapchain image on DXVK's queue, then release the images.
static void vkCopyToSwapchains(std::vector<VkCopyJob>& jobs) {
	const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	std::vector<VkCopyJob> ready;
	for (auto& j : jobs) {
		if (!j.src || !j.t->swapchain) continue;
		VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		if (FAILED(j.src->GetVulkanImageInfo(&j.img, &j.layout, &ci)) || !j.img) continue;
		ready.push_back(j);
	}
	if (ready.empty()) return;

	// DXVK moves the source images into TRANSFER_SRC and submits all pending game rendering
	for (auto& j : ready) S.vkInterop->TransitionTextureLayout(j.src, &range, j.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	S.vkInterop->FlushRenderingCommands();

	for (auto& j : ready) {
		XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrResult r;
		{ VkQueueLock lk; r = p_xrAcquireSwapchainImage(j.t->swapchain, &ai, &j.idx); }
		if (XR_FAILED(r) || j.idx >= j.t->vkImages.size()) continue;
		j.acquired = true;
		XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wi.timeout = XR_INFINITE_DURATION;
		p_xrWaitSwapchainImage(j.t->swapchain, &wi);
	}

	VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	p_vkBeginCommandBuffer(S.vkCmd, &bi);
	for (auto& j : ready) {
		if (!j.acquired) continue;
		VkImage dst = j.t->vkImages[j.idx].image;
		VkImageMemoryBarrier pre[2] = { { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER }, { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER } };
		pre[0].srcAccessMask = 0;
		pre[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		pre[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		pre[0].srcQueueFamilyIndex = pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre[0].image = dst;
		pre[0].subresourceRange = range;
		pre[1].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		pre[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		pre[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		pre[1].srcQueueFamilyIndex = pre[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre[1].image = j.img;
		pre[1].subresourceRange = range;
		p_vkCmdPipelineBarrier(S.vkCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre);

		const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		uint32_t w = (uint32_t)j.t->width, h = (uint32_t)j.t->height;
		if (S.vkRawCopy) {
			VkImageCopy c{};
			c.srcSubresource = layers;
			c.dstSubresource = layers;
			c.extent = { w, h, 1 };
			p_vkCmdCopyImage(S.vkCmd, j.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		}
		else {
			VkImageBlit b{};
			b.srcSubresource = layers;
			b.dstSubresource = layers;
			b.srcOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
			b.dstOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
			p_vkCmdBlitImage(S.vkCmd, j.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
		}

		VkImageMemoryBarrier post{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		post.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		post.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // what OpenXR expects at release
		post.srcQueueFamilyIndex = post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post.image = dst;
		post.subresourceRange = range;
		p_vkCmdPipelineBarrier(S.vkCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &post);
	}
	p_vkEndCommandBuffer(S.vkCmd);

	VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1;
	si.pCommandBuffers = &S.vkCmd;
	VkResult vr;
	{ VkQueueLock lk; vr = p_vkQueueSubmit(S.vkQueue, 1, &si, S.vkFence); }
	if (vr == VK_SUCCESS) {
		p_vkWaitForFences(S.vkDevice, 1, &S.vkFence, VK_TRUE, UINT64_MAX);
		p_vkResetFences(S.vkDevice, 1, &S.vkFence);
	}
	else {
		static int warned = 0;
		if (warned++ < 3) logf("vkQueueSubmit failed (%d)", (int)vr);
	}

	// hand the sources back to DXVK in the layout it keeps them in
	for (auto& j : ready) S.vkInterop->TransitionTextureLayout(j.src, &range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, j.layout);

	VkQueueLock lk;
	for (auto& j : ready) {
		if (!j.acquired) continue;
		XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		p_xrReleaseSwapchainImage(j.t->swapchain, &ri);
	}
}

static void shutdownVk() {
	if (S.vkFence && p_vkDestroyFence) p_vkDestroyFence(S.vkDevice, S.vkFence, nullptr);
	if (S.vkPool && p_vkDestroyCommandPool) p_vkDestroyCommandPool(S.vkDevice, S.vkPool, nullptr);
	S.vkFence = VK_NULL_HANDLE;
	S.vkPool = VK_NULL_HANDLE;
	S.vkCmd = VK_NULL_HANDLE;
	safeRelease(S.key9State);
	safeRelease(S.key9PS);
	S.key9Tried = false;
	safeRelease(S.vkInterop);
	S.vk = false;
	S.vkInstance = VK_NULL_HANDLE;
	S.vkPhys = VK_NULL_HANDLE;
	S.vkDevice = VK_NULL_HANDLE;
	S.vkQueue = VK_NULL_HANDLE;
}

static void stretchInto(IDirect3DSurface9* src, IDirect3DSurface9* dst, bool cropToAspect) {
	if (!src || !dst) return;
	D3DSURFACE_DESC sd, dd;
	src->GetDesc(&sd);
	dst->GetDesc(&dd);
	if (dd.MultiSampleType != D3DMULTISAMPLE_NONE && sd.MultiSampleType == D3DMULTISAMPLE_NONE) {
		static bool warned = false;
		if (!warned) { logf("StretchRect into a multisampled surface is not supported; use CopyRectStretch for the mirror"); warned = true; }
		return;
	}
	if (sd.MultiSampleType != D3DMULTISAMPLE_NONE) {
		D3DSURFACE_DESC rd = {};
		if (S.resolve9) S.resolve9->GetDesc(&rd);
		if (!S.resolve9 || rd.Width != sd.Width || rd.Height != sd.Height || rd.Format != sd.Format) {
			safeRelease(S.resolve9);
			HRESULT hr = S.d9->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &S.resolve9, nullptr);
			if (FAILED(hr)) { logf("resolve surface creation failed (0x%08X)", (unsigned)hr); return; }
		}
		HRESULT hr = S.d9->StretchRect(src, nullptr, S.resolve9, nullptr, D3DTEXF_NONE);
		if (FAILED(hr)) { static bool warned = false; if (!warned) { logf("multisample resolve failed (0x%08X)", (unsigned)hr); warned = true; } return; }
		src = S.resolve9;
	}
	RECT sr = { 0, 0, (LONG)sd.Width, (LONG)sd.Height };
	if (cropToAspect && sd.Height && dd.Height) {
		float sa = (float)sd.Width / sd.Height, da = (float)dd.Width / dd.Height;
		if (sa < da) { // source is taller: crop top/bottom
			LONG h = (LONG)(sd.Width / da);
			sr.top = (LONG)(sd.Height - h) / 2; sr.bottom = sr.top + h;
		}
		else {
			LONG w = (LONG)(sd.Height * da);
			sr.left = (LONG)(sd.Width - w) / 2; sr.right = sr.left + w;
		}
	}
	HRESULT hr = S.d9->StretchRect(src, &sr, dst, nullptr, D3DTEXF_LINEAR);
	if (FAILED(hr)) { static int warned = 0; if (warned++ < 3) logf("StretchRect %ux%u -> %ux%u failed (0x%08X)", sd.Width, sd.Height, dd.Width, dd.Height, (unsigned)hr); }
}

static void shutdownAll() {
	if (S.session && S.running && p_xrEndSession) { VkQueueLock lk; p_xrEndSession(S.session); }
	S.running = false;
	destroyTarget(S.eyes[0]);
	destroyTarget(S.eyes[1]);
	destroyTarget(S.quad);
	for (int h = 0; h < 2; ++h) if (S.handSpace[h] && p_xrDestroySpace) { p_xrDestroySpace(S.handSpace[h]); S.handSpace[h] = XR_NULL_HANDLE; }
	for (int h = 0; h < 2; ++h) {
		if (S.gripSpace[h] && p_xrDestroySpace) p_xrDestroySpace(S.gripSpace[h]);
		S.gripSpace[h] = XR_NULL_HANDLE;
		S.gripValid[h] = false;
		S.gripValue[h] = S.triggerValue[h] = 0;
	}
	S.gripPoseAction = S.squeezeAction = S.triggerAction = XR_NULL_HANDLE; // destroyed with the action set
	S.touchAction[0] = S.touchAction[1] = XR_NULL_HANDLE;
	for (int h = 0; h < 2; ++h) S.touch[h][0] = S.touch[h][1] = -1;
	if (S.baseSpace && p_xrDestroySpace) p_xrDestroySpace(S.baseSpace);
	if (S.viewSpace && p_xrDestroySpace) p_xrDestroySpace(S.viewSpace);
	if (S.localSpace && p_xrDestroySpace) p_xrDestroySpace(S.localSpace);
	S.baseSpace = S.viewSpace = S.localSpace = XR_NULL_HANDLE;
	if (S.actionSet && p_xrDestroyActionSet) p_xrDestroyActionSet(S.actionSet);
	S.actionSet = XR_NULL_HANDLE;
	if (S.session && p_xrDestroySession) { VkQueueLock lk; p_xrDestroySession(S.session); }
	S.session = XR_NULL_HANDLE;
	if (S.instance && p_xrDestroyInstance) p_xrDestroyInstance(S.instance);
	S.instance = XR_NULL_HANDLE;
	shutdownVk();
	releaseKeyShader();
	safeRelease(S.query11);
	safeRelease(S.ctx11);
	safeRelease(S.d11);
	safeRelease(S.query9);
	safeRelease(S.resolve9);
	if (S.d9) { S.d9->Release(); S.d9 = nullptr; }
	clearFuncs();
	S.frameBegun = false;
	S.posesValid = false;
	S.spaceChanged = false;
}

// =============================================================================================
// exported API
// =============================================================================================

// XR_Init%(d3d9device%, appName$, resolutionScale#) -> 1 on success, 0 on failure (see XR_GetError$)
BXR_API int BXR_CALL XR_Init(int d3d9Device, const char* appName, float resScale) {
	if (S.instance) return 1;
	g_lastError.clear();
	if (!d3d9Device) return fail("no Direct3D 9 device (call after Graphics3D)"), 0;
	if (!loadGlobalFuncs()) return 0;

	// --- graphics path: a DXVK device (Direct3D 9 on Vulkan) exposes its Vulkan objects through
	// ID3D9VkInteropDevice; the system Direct3D 9 doesn't, and uses the D3D9Ex -> D3D11 bridge.
	bool useVk = false;
	if (ID3D9VkInteropDevice* probe = queryInterop((IDirect3DDevice9*)(intptr_t)d3d9Device)) { useVk = true; probe->Release(); }
	logf("graphics path: %s", useVk ? "DXVK Vulkan" : "D3D11 bridge");
	{
		// which Direct3D 9 the game runs on (the DLL the device's code is in), and whether DXVK was asked for
		HMODULE mod = nullptr;
		char dll[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, *(LPCSTR*)(intptr_t)d3d9Device, &mod))
			GetModuleFileNameA(mod, dll, sizeof(dll));
		logf("Direct3D 9: %s (-dxvk on the command line: %s)", dll, strstr(GetCommandLineA(), "-dxvk") ? "yes" : "no");
	}
	const char* gfxExt = useVk ? XR_KHR_VULKAN_ENABLE_EXTENSION_NAME : XR_KHR_D3D11_ENABLE_EXTENSION_NAME;

	// --- instance
	uint32_t n = 0;
	p_xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
	std::vector<XrExtensionProperties> exts(n, { XR_TYPE_EXTENSION_PROPERTIES });
	p_xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
	bool hasGfx = false;
	for (auto& e : exts) if (!strcmp(e.extensionName, gfxExt)) hasGfx = true;
	if (!hasGfx) return fail("the active OpenXR runtime does not support %s for 32-bit apps (%s)", useVk ? "Vulkan" : "Direct3D 11", gfxExt), 0;

	const char* enabled[] = { gfxExt };
	XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
	strncpy(ici.applicationInfo.applicationName, appName && *appName ? appName : "BlitzXR", XR_MAX_APPLICATION_NAME_SIZE - 1);
	ici.applicationInfo.applicationVersion = 1;
	strcpy(ici.applicationInfo.engineName, "BlitzX3D");
	ici.applicationInfo.engineVersion = 1;
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = 1;
	ici.enabledExtensionNames = enabled;
	XrResult r = p_xrCreateInstance(&ici, &S.instance);
	if (XR_FAILED(r)) { fail("xrCreateInstance failed (%d). Is a headset runtime (SteamVR, Meta, Virtual Desktop) running?", (int)r); return 0; }
	if (!loadInstanceFuncs(S.instance)) { shutdownAll(); return 0; }

	XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
	p_xrGetInstanceProperties(S.instance, &ip);
	S.runtimeName = ip.runtimeName;
	logf("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

	XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!xrCheck(p_xrGetSystem(S.instance, &sgi, &S.system), "xrGetSystem (is the headset connected?)")) { shutdownAll(); return 0; }

	// --- graphics + session
	S.d9 = (IDirect3DDevice9Ex*)(intptr_t)d3d9Device;
	S.d9->AddRef();
	XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
	sci.systemId = S.system;
	if (useVk) {
#define BXR_LOAD_GFX(name) \
		if (XR_FAILED(p_xrGetInstanceProcAddr(S.instance, #name, (PFN_xrVoidFunction*)&p_##name)) || !p_##name) { fail("missing OpenXR function " #name); shutdownAll(); return 0; }
		BXR_LOAD_GFX(xrGetVulkanInstanceExtensionsKHR)
		BXR_LOAD_GFX(xrGetVulkanDeviceExtensionsKHR)
		BXR_LOAD_GFX(xrGetVulkanGraphicsDeviceKHR)
		BXR_LOAD_GFX(xrGetVulkanGraphicsRequirementsKHR)
		S.vkInterop = queryInterop(S.d9);
		S.vk = S.vkInterop != nullptr;
		if (!S.vk || !vkSetupFromDxvk()) { shutdownAll(); return 0; }
		XrGraphicsBindingVulkanKHR gb{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
		gb.instance = S.vkInstance;
		gb.physicalDevice = S.vkPhys;
		gb.device = S.vkDevice;
		gb.queueFamilyIndex = S.vkQueueFamily;
		gb.queueIndex = S.vkQueueIndex;
		sci.next = &gb;
		XrResult r2;
		{ VkQueueLock lk; r2 = p_xrCreateSession(S.instance, &sci, &S.session); }
		if (!xrCheck(r2, "xrCreateSession (Vulkan on DXVK's device; see the extension lines above)")) { shutdownAll(); return 0; }
	}
	else {
		BXR_LOAD_GFX(xrGetD3D11GraphicsRequirementsKHR)
#undef BXR_LOAD_GFX
		XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
		if (!xrCheck(p_xrGetD3D11GraphicsRequirementsKHR(S.instance, S.system, &req), "xrGetD3D11GraphicsRequirementsKHR")) { shutdownAll(); return 0; }
		if (!checkSameAdapter(req.adapterLuid) || !createD3D11(req.adapterLuid, req.minFeatureLevel)) { shutdownAll(); return 0; }
		XrGraphicsBindingD3D11KHR gb{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
		gb.device = S.d11;
		sci.next = &gb;
		if (!xrCheck(p_xrCreateSession(S.instance, &sci, &S.session), "xrCreateSession")) { shutdownAll(); return 0; }
	}

	XrReferenceSpaceCreateInfo rci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rci.poseInReferenceSpace.orientation.w = 1;
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	p_xrCreateReferenceSpace(S.session, &rci, &S.localSpace);
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	p_xrCreateReferenceSpace(S.session, &rci, &S.viewSpace);
	if (!S.localSpace || !S.viewSpace) { fail("could not create reference spaces"); shutdownAll(); return 0; }

	// --- swapchains
	if (S.vk) {
		if (!pickVkFormat()) { shutdownAll(); return 0; }
	}
	else {
		S.swapFormat = pickFormat();
		if (S.swapFormat == DXGI_FORMAT_UNKNOWN) { fail("runtime offers no BGRA8 swapchain format"); shutdownAll(); return 0; }
	}
	XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	n = 0;
	if (!xrCheck(p_xrEnumerateViewConfigurationViews(S.instance, S.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv), "xrEnumerateViewConfigurationViews")) { shutdownAll(); return 0; }
	if (resScale <= 0.1f || resScale > 2.0f) resScale = 1.0f;
	for (int e = 0; e < 2; ++e) {
		int w = (int)(vcv[e].recommendedImageRectWidth * resScale);
		int h = (int)(vcv[e].recommendedImageRectHeight * resScale);
		w = std::min<int>(std::max(w, 64), (int)vcv[e].maxImageRectWidth);
		h = std::min<int>(std::max(h, 64), (int)vcv[e].maxImageRectHeight);
		if (!createTarget(S.eyes[e], w, h)) { shutdownAll(); return 0; }
	}
	logf("eye resolution %dx%d (scale %.2f)", S.eyes[0].width, S.eyes[0].height, resScale);
	{
		int qw = 1280, qh = 720;
		IDirect3DSurface9* bb = nullptr;
		if (SUCCEEDED(S.d9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
			D3DSURFACE_DESC bd;
			if (SUCCEEDED(bb->GetDesc(&bd)) && bd.Width >= 320 && bd.Height >= 200) { qw = (int)bd.Width; qh = (int)bd.Height; }
			bb->Release();
		}
		if (qw > 2560) { qh = qh * 2560 / qw; qw = 2560; }
		if (!createTarget(S.quad, qw, qh)) {
			destroyTarget(S.quad);
			if (!createTarget(S.quad, 1280, 720)) { shutdownAll(); return 0; }
		}
		logf("floating screen %dx%d", S.quad.width, S.quad.height);
	}

	if (!createActions()) logf("controller actions unavailable, continuing without them");
	return 1;
}

BXR_API void BXR_CALL XR_Shutdown() {
	if (S.instance) shutdownAll();
	if (g_loader) { FreeLibrary(g_loader); g_loader = nullptr; }
}

static std::string g_retStr;
BXR_API const char* BXR_CALL XR_GetError() { g_retStr = g_lastError; return g_retStr.c_str(); }
BXR_API const char* BXR_CALL XR_RuntimeName() { g_retStr = S.runtimeName; return g_retStr.c_str(); }

BXR_API int BXR_CALL XR_EyeWidth() { return S.eyes[0].width; }
BXR_API int BXR_CALL XR_EyeHeight() { return S.eyes[0].height; }
BXR_API void BXR_CALL XR_SetWorldScale(float unitsPerMetre) { if (unitsPerMetre > 0) S.worldScale = unitsPerMetre; }
BXR_API void BXR_CALL XR_SetQuad(float widthMetres, float distanceMetres) {
	if (widthMetres > 0.1f) S.quadWidth = widthMetres;
	if (distanceMetres > 0.1f) S.quadDistance = distanceMetres;
}

BXR_API int BXR_CALL XR_IsRunning() { pollEvents(); return S.running ? 1 : 0; }
BXR_API int BXR_CALL XR_ExitRequested() { return S.exitRequested ? 1 : 0; }
BXR_API int BXR_CALL XR_FrameBegun() { return S.frameBegun ? 1 : 0; }

// XR_BeginFrame%() -> 1 if the game should render the eyes this frame.
// Blocks until the runtime wants a new frame (this paces the game to the headset).
// Calling it again before XR_EndFrame does nothing and returns the same answer.
BXR_API int BXR_CALL XR_BeginFrame() {
	if (!S.instance) return 0;
	if (S.frameBegun) return (S.frameState.shouldRender && S.posesValid) ? 1 : 0;
	pollEvents();
	if (!S.running) { syncActions(); return 0; }

	S.frameState = { XR_TYPE_FRAME_STATE };
	XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
	if (!xrCheck(p_xrWaitFrame(S.session, &fwi, &S.frameState), "xrWaitFrame")) return 0;
	XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
	XrResult br;
	{ VkQueueLock lk; br = p_xrBeginFrame(S.session, &fbi); }
	if (XR_FAILED(br)) return 0;
	S.frameBegun = true;
	S.eyes[0].submitted = S.eyes[1].submitted = S.quad.submitted = false;
	syncActions();
	locate();
	return (S.frameState.shouldRender && S.posesValid) ? 1 : 0;
}

// head pose in the recentred tracking space: 0..2 = x,y,z (Blitz units), 3..5 = pitch,yaw,roll (degrees)
BXR_API float BXR_CALL XR_Head(int i) { return (i >= 0 && i < 6) ? S.head[i] : 0.0f; }
// eye pose relative to the head, same layout. Apply with MoveEntity + TurnEntity on the head camera.
BXR_API float BXR_CALL XR_EyeOffset(int eye, int i) { return (eye >= 0 && eye < 2 && i >= 0 && i < 6) ? S.eyeRel[eye][i] : 0.0f; }
// per-eye field of view as view-space tangents: 0 = left (negative), 1 = right, 2 = up, 3 = down (negative)
BXR_API float BXR_CALL XR_EyeFov(int eye, int i) { return (eye >= 0 && eye < 2 && i >= 0 && i < 4) ? S.eyeTan[eye][i] : 0.0f; }
// controller aim pose in the recentred tracking space (same layout as XR_Head)
BXR_API float BXR_CALL XR_Hand(int hand, int i) { return (hand >= 0 && hand < 2 && i >= 0 && i < 6) ? S.hand[hand][i] : 0.0f; }
BXR_API int BXR_CALL XR_HandValid(int hand) { return (hand >= 0 && hand < 2 && S.handValid[hand]) ? 1 : 0; }
// controller grip pose (centre of the hand holding the controller, OpenXR /input/grip/pose), same layout as XR_Head:
// x y z (Blitz units, recentred tracking space) pitch yaw roll. Grip-space axes in Blitz terms: +X out of the left
// palm / into the right palm, +Z out of the thumb side of the fist (along the controller handle), +Y = up from there.
BXR_API float BXR_CALL XR_Grip(int hand, int i) { return (hand >= 0 && hand < 2 && i >= 0 && i < 6) ? S.grip[hand][i] : 0.0f; }
BXR_API int BXR_CALL XR_GripValid(int hand) { return (hand >= 0 && hand < 2 && S.gripValid[hand]) ? 1 : 0; }
// analog squeeze / trigger, 0..1 (click-only controllers give 0 or 1)
BXR_API float BXR_CALL XR_GripValue(int hand) { return (hand >= 0 && hand < 2) ? S.gripValue[hand] : 0.0f; }
BXR_API float BXR_CALL XR_TriggerValue(int hand) { return (hand >= 0 && hand < 2) ? S.triggerValue[hand] : 0.0f; }
// capacitive touch: what 0 = index finger on the trigger, 1 = thumb on the thumb rest / stick / face buttons / trackpad.
// 1 touching, 0 not touching, -1 this controller has no touch sensor for it (keep the non-touch hand pose)
BXR_API int BXR_CALL XR_Touch(int hand, int what) { return (hand >= 0 && hand < 2 && what >= 0 && what < 2) ? S.touch[hand][what] : -1; }

// Copy a rendered eye (any Blitz render-target buffer, via BufferD3D9Surface) for submission.
BXR_API void BXR_CALL XR_SubmitEye(int eye, int surface) {
	if (!S.frameBegun || eye < 0 || eye > 1 || !surface) return;
	Target& t = S.eyes[eye];
	if (!t.swapchain || !t.surf9) return;
	stretchInto((IDirect3DSurface9*)(intptr_t)surface, t.surf9, false);
	t.submitted = true;
}

// Show a flat 2D surface (menus, loading screens) as a floating screen this frame.
BXR_API void BXR_CALL XR_SubmitQuad(int surface) {
	if (!S.frameBegun || !surface) return;
	if (!S.quad.swapchain || !S.quad.surf9) return;
	IDirect3DSurface9* src = (IDirect3DSurface9*)(intptr_t)surface;
	stretchInto(src, S.quad.surf9, false);
	S.quad.submitted = true;
	S.overlayMode = 0;
}

// Show a 2D surface over the 3D view this frame (call after both XR_SubmitEye).
// mode 1 = HUD locked to the head; mode 2 = panel that stays where it was opened.
// Black is transparent in both. mode 3 = like 2, but the surface was cleared to the key colour (0,0,12): only that
// is transparent and black stays black (the game uses it for its pause menu and inventory).
BXR_API void BXR_CALL XR_SubmitOverlay(int surface, int mode) {
	if (!S.frameBegun || !surface || mode < 1 || mode > 3) return;
	S.overlayKey = (mode == 3);
	if (mode == 3) mode = 2;
	if (!S.quad.swapchain || !S.quad.surf9) return;
	if (mode == 2 && S.lastOverlayMode != 2) placePanel();
	stretchInto((IDirect3DSurface9*)(intptr_t)surface, S.quad.surf9, false);
	S.quad.submitted = true;
	S.overlayMode = mode;
}

// HUD size and distance in metres
BXR_API void BXR_CALL XR_SetHud(float widthMetres, float distanceMetres) {
	if (widthMetres > 0.1f) S.hudWidth = widthMetres;
	if (distanceMetres > 0.1f) S.hudDistance = distanceMetres;
}

// Move the open panel back in front of the head.
BXR_API void BXR_CALL XR_ResetPanel() { placePanel(); }

// Copy what an eye saw into a Blitz buffer (e.g. the back buffer) for the desktop mirror.
BXR_API void BXR_CALL XR_MirrorEye(int eye, int dstSurface) {
	if (eye < 0 || eye > 1 || !dstSurface || !S.eyes[eye].surf9) return;
	stretchInto(S.eyes[eye].surf9, (IDirect3DSurface9*)(intptr_t)dstSurface, true);
}

BXR_API void BXR_CALL XR_EndFrame() {
	if (!S.frameBegun) return;
	S.frameBegun = false;

	bool stereo = S.frameState.shouldRender && S.posesValid && S.eyes[0].submitted && S.eyes[1].submitted
		&& S.eyes[0].swapchain && S.eyes[1].swapchain;
	bool quad = S.frameState.shouldRender && S.quad.submitted && S.quad.swapchain;
	int mode = S.overlayMode;
	if (quad && mode != 0 && !stereo) mode = 0; // an overlay without a 3D view is just a floating screen
	if (quad && mode == 0 && stereo) stereo = false;
	if (S.vk && (stereo || quad)) {
		bool keyed = false;
		if (quad && mode != 0) {
			keyed = key9Pass(S.quad, S.overlayKey ? -1.0f : (mode == 1 ? 3.0f : 8.0f));
			if (!keyed && mode == 1) {
				// an opaque HUD would black out the view, so leave it out
				static bool warned = false;
				if (!warned) { logf("HUD hidden: the overlay alpha key is not available"); warned = true; }
				quad = false;
			}
		}
		std::vector<VkCopyJob> jobs;
		if (stereo) {
			jobs.push_back({ &S.eyes[0], S.eyes[0].vkTex });
			jobs.push_back({ &S.eyes[1], S.eyes[1].vkTex });
		}
		if (quad) jobs.push_back({ &S.quad, keyed ? S.quad.keyVkTex : S.quad.vkTex });
		if (!jobs.empty()) vkCopyToSwapchains(jobs);
	}
	else if (stereo || quad) {
		waitD3D9(); // D3D9 copies must be finished before D3D11 reads the shared textures
		if (stereo) { copyToSwapchain(S.eyes[0]); copyToSwapchain(S.eyes[1]); }
		if (quad) {
			if (mode == 0) copyToSwapchain(S.quad);
			else keyToSwapchain(S.quad, S.overlayKey ? -1.0f : (mode == 1 ? 3.0f : 8.0f));
		}
		waitD3D11(); // and D3D11 must be done before D3D9 overwrites them next frame
	}

	XrSpace base = S.baseSpace ? S.baseSpace : S.localSpace;
	XrCompositionLayerProjectionView pv[2];
	XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad ql{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader* layers[2];
	uint32_t layerCount = 0;

	if (stereo) {
		for (int e = 0; e < 2; ++e) {
			pv[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
			pv[e].pose = S.views[e].pose;
			pv[e].fov = S.views[e].fov;
			pv[e].subImage.swapchain = S.eyes[e].swapchain;
			pv[e].subImage.imageRect.extent = { S.eyes[e].width, S.eyes[e].height };
		}
		proj.space = base;
		proj.viewCount = 2;
		proj.views = pv;
		layers[layerCount++] = (XrCompositionLayerBaseHeader*)&proj;
	}
	if (quad) {
		float w = S.quadWidth;
		ql.space = base;
		ql.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		ql.subImage.swapchain = S.quad.swapchain;
		ql.subImage.imageRect.extent = { S.quad.width, S.quad.height };
		ql.pose.orientation.w = 1;
		ql.pose.position = { 0, 0, -S.quadDistance };
		if (mode == 1) {
			ql.space = S.viewSpace;
			ql.pose.position = { 0, 0, -S.hudDistance };
			w = S.hudWidth;
		}
		else if (mode == 2) {
			ql.pose = S.panelPose;
		}
		if (mode != 0) ql.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		ql.size = { w, w * S.quad.height / (float)S.quad.width };
		layers[layerCount++] = (XrCompositionLayerBaseHeader*)&ql;
	}
	S.lastOverlayMode = quad ? mode : -1;
	S.overlayMode = 0;

	XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
	fei.displayTime = S.frameState.predictedDisplayTime;
	fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	fei.layerCount = layerCount;
	fei.layers = layers;
	XrResult er;
	{ VkQueueLock lk; er = p_xrEndFrame(S.session, &fei); }
	if (XR_FAILED(er)) {
		// log the first few only; a persistent failure would otherwise fill the disk
		if (S.endFrameErrors < 5) xrCheck(er, "xrEndFrame");
		else if (S.endFrameErrors == 5) logf("(further xrEndFrame errors not logged)");
		++S.endFrameErrors;
	}
}

// Recentring is done by the VR software now; this only asks the game to re-capture the standing height.
BXR_API void BXR_CALL XR_Recenter() { S.spaceChanged = true; }
// 1 once after each recentre by the VR software (or XR_Recenter), otherwise 0
BXR_API int BXR_CALL XR_SpaceChanged() { bool c = S.spaceChanged; S.spaceChanged = false; return c ? 1 : 0; }
BXR_API int BXR_CALL XR_PointerValid() { return S.pointerValid ? 1 : 0; }
BXR_API float BXR_CALL XR_PointerX() { return S.pointerU; }
BXR_API float BXR_CALL XR_PointerY() { return S.pointerV; }

// input. Buttons: 0 blink, 1 sprint, 2 crouch, 3 interact, 4 inventory, 5 menu, 6 save (Y), 7 left grip, 8 right grip
BXR_API int BXR_CALL XR_ButtonDown(int b) { return (b >= 0 && b < BTN_COUNT && S.btnDown[b]) ? 1 : 0; }
// true only during the frame the button went down (refreshed by each XR_BeginFrame)
BXR_API int BXR_CALL XR_ButtonHit(int b) { return (b >= 0 && b < BTN_COUNT && S.btnHit[b]) ? 1 : 0; }
// axes: 0 move x, 1 move y, 2 turn x, 3 turn y (-1..1)
BXR_API float BXR_CALL XR_Axis(int a) { return (a >= 0 && a < AX_COUNT) ? S.axis[a] : 0.0f; }

BXR_API void BXR_CALL XR_Haptic(int hand, float amplitude, int milliseconds) {
	if (!S.running || !S.hapticAction || hand < 0 || hand > 1) return;
	XrHapticVibration v{ XR_TYPE_HAPTIC_VIBRATION };
	v.amplitude = amplitude;
	v.duration = (XrDuration)milliseconds * 1000000;
	v.frequency = XR_FREQUENCY_UNSPECIFIED;
	XrHapticActionInfo hi{ XR_TYPE_HAPTIC_ACTION_INFO };
	hi.action = S.hapticAction;
	hi.subactionPath = S.handPath[hand];
	p_xrApplyHapticFeedback(S.session, &hi, (XrHapticBaseHeader*)&v);
}
