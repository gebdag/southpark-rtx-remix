// South Park's d3d9.dll proxy: sits between the renderer (Video_D3D.dll) and the RTX Remix bridge
// client, renamed d3d9_remix.dll. Everything passes straight through; the proxy adds the game's
// dynamic lights through the Remix API and an overlay that shows them (F4).
//
// Only Direct3DCreate9 is implemented here -- every other export is forwarded by the .def. The
// device is not wrapped: five slots of its vtable are patched, which is all the proxy needs and
// leaves the bridge's own objects untouched.

#include "lights.h"
#include "log.h"
#include "overlay.h"
#include "remix.h"

#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

// The bridge client's other exports, forwarded unchanged.
#pragma comment(linker, "/export:Direct3DCreate9Ex=d3d9_remix.Direct3DCreate9Ex")
#pragma comment(linker, "/export:D3DPERF_BeginEvent=d3d9_remix.D3DPERF_BeginEvent")
#pragma comment(linker, "/export:D3DPERF_EndEvent=d3d9_remix.D3DPERF_EndEvent")
#pragma comment(linker, "/export:D3DPERF_GetStatus=d3d9_remix.D3DPERF_GetStatus")
#pragma comment(linker, "/export:D3DPERF_QueryRepeatFrame=d3d9_remix.D3DPERF_QueryRepeatFrame")
#pragma comment(linker, "/export:D3DPERF_SetMarker=d3d9_remix.D3DPERF_SetMarker")
#pragma comment(linker, "/export:D3DPERF_SetOptions=d3d9_remix.D3DPERF_SetOptions")
#pragma comment(linker, "/export:D3DPERF_SetRegion=d3d9_remix.D3DPERF_SetRegion")
#pragma comment(linker, "/export:DebugSetLevel=d3d9_remix.DebugSetLevel")
#pragma comment(linker, "/export:DebugSetMute=d3d9_remix.DebugSetMute")
#pragma comment(linker, "/export:Direct3D9EnableMaximizedWindowedModeShim=d3d9_remix.Direct3D9EnableMaximizedWindowedModeShim")
#pragma comment(linker, "/export:remixapi_InitializeLibrary=d3d9_remix.remixapi_InitializeLibrary")
#pragma comment(linker, "/export:remixapi_RegisterCallbacks=d3d9_remix.remixapi_RegisterCallbacks")

namespace {

char g_directory[MAX_PATH];
HMODULE g_bridge = 0;

typedef IDirect3D9* (WINAPI* Direct3DCreate9Fn)(UINT);

// Vtable slots, counting IUnknown's three first.
const int kSlotCreateDevice = 16;    // IDirect3D9
const int kSlotReset = 16;           // IDirect3DDevice9 from here down
const int kSlotPresent = 17;
const int kSlotBeginScene = 41;
const int kSlotEndScene = 42;
const int kSlotSetTransform = 44;

typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* SceneFn)(IDirect3DDevice9*);
typedef HRESULT(STDMETHODCALLTYPE* SetTransformFn)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);

CreateDeviceFn g_createDevice = 0;
ResetFn g_reset = 0;
PresentFn g_present = 0;
SceneFn g_beginScene = 0;
SceneFn g_endScene = 0;
SetTransformFn g_setTransform = 0;

// Replaces one vtable slot and returns what it held; a slot already ours is left alone.
void* Patch(void* object, int slot, void* replacement) {
    void** table = *(void***)object;
    if (table[slot] == replacement) {
        return 0;
    }
    DWORD previous = 0;
    if (!VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &previous)) {
        return 0;
    }
    void* original = table[slot];
    table[slot] = replacement;
    VirtualProtect(&table[slot], sizeof(void*), previous, &previous);
    return original;
}

void BackBufferSize(HWND window, const D3DPRESENT_PARAMETERS* params) {
    int width = (int)params->BackBufferWidth;
    int height = (int)params->BackBufferHeight;
    if (width == 0 || height == 0) {
        RECT client = {};
        GetClientRect(window, &client);
        width = client.right;
        height = client.bottom;
    }
    Overlay::SetBackBufferSize(width, height);
}

void __cdecl OnRemixBeginScene() {
    Lights::SubmitFrame();
}

// ------------------------------------------------------------------------------ device hooks

HRESULT STDMETHODCALLTYPE HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    const HRESULT hr = g_reset(device, params);
    if (SUCCEEDED(hr) && params) {
        BackBufferSize(params->hDeviceWindow, params);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                                        HWND window, const RGNDATA* dirty) {
    Overlay::OnPresent(destination);
    return g_present(device, source, destination, window, dirty);
}

HRESULT STDMETHODCALLTYPE HookedBeginScene(IDirect3DDevice9* device) {
    Lights::OnBeginScene();
    return g_beginScene(device);   // the bridge calls OnRemixBeginScene from in here
}

HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice9* device) {
    Overlay::Render(device);
    return g_endScene(device);
}

HRESULT STDMETHODCALLTYPE HookedSetTransform(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state,
                                             const D3DMATRIX* matrix) {
    Lights::OnSetTransform(state, matrix);
    return g_setTransform(device, state, matrix);
}

void HookDevice(IDirect3DDevice9* device) {
    if (void* p = Patch(device, kSlotReset, (void*)&HookedReset)) g_reset = (ResetFn)p;
    if (void* p = Patch(device, kSlotPresent, (void*)&HookedPresent)) g_present = (PresentFn)p;
    if (void* p = Patch(device, kSlotBeginScene, (void*)&HookedBeginScene)) g_beginScene = (SceneFn)p;
    if (void* p = Patch(device, kSlotEndScene, (void*)&HookedEndScene)) g_endScene = (SceneFn)p;
    if (void* p = Patch(device, kSlotSetTransform, (void*)&HookedSetTransform)) g_setTransform = (SetTransformFn)p;
}

HRESULT STDMETHODCALLTYPE HookedCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus,
                                             DWORD flags, D3DPRESENT_PARAMETERS* params,
                                             IDirect3DDevice9** device) {
    const HRESULT hr = g_createDevice(d3d, adapter, type, focus, flags, params, device);
    if (FAILED(hr) || !device || !*device) {
        return hr;
    }
    HookDevice(*device);
    const HWND window = (params && params->hDeviceWindow) ? params->hDeviceWindow : focus;
    if (params) {
        BackBufferSize(window, params);
    }
    // After the bridge subclassed the window inside CreateDevice, so the overlay sees input first.
    Overlay::Attach(window);
    // The bridge's server is running once a device exists, which is when it accepts the API.
    if (Remix::Connect(g_bridge, &OnRemixBeginScene)) {
        Lights::OnConnected();
    }
    Log("device %p created and hooked", (void*)*device);
    return hr;
}

bool LoadBridge() {
    if (g_bridge) {
        return true;
    }
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%sd3d9_remix.dll", g_directory);
    path[sizeof(path) - 1] = 0;
    g_bridge = LoadLibraryA(path);
    Log(g_bridge ? "loaded %s" : "FATAL: could not load %s", path);
    return g_bridge != 0;
}

} // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    if (!LoadBridge()) {
        return 0;
    }
    const auto create = (Direct3DCreate9Fn)GetProcAddress(g_bridge, "Direct3DCreate9");
    IDirect3D9* d3d = create ? create(sdkVersion) : 0;
    if (d3d) {
        if (void* p = Patch(d3d, kSlotCreateDevice, (void*)&HookedCreateDevice)) {
            g_createDevice = (CreateDeviceFn)p;
        }
    }
    return d3d;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        GetModuleFileNameA(self, g_directory, MAX_PATH);
        if (char* slash = strrchr(g_directory, '\\')) {
            slash[1] = 0;
        }
        LogOpen(g_directory);
        Log("=== South Park Remix proxy ===");

        // One settings file for the whole port: the renderer's.
        char ini[MAX_PATH];
        _snprintf(ini, sizeof(ini), "%sVideo_D3D9.ini", g_directory);
        ini[sizeof(ini) - 1] = 0;
        Lights::Init(ini);
    }
    return TRUE;
}
