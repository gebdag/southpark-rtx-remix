// The 46 GFXDLL_* exports the South Park engine expects from a video plugin.
//
// Every export is __cdecl, so the caller cleans the stack and a stub may safely declare
// fewer parameters than the engine passes. Only the exports whose arguments or return
// values the renderer uses are typed; the rest take no parameters, log the call and do
// nothing, which is all the engine needs of them.
//
// In lookdev mode (SP_LOOKDEV=1) the plugin is a tee (see original.h and lookdev.h):
//
//   - the exports that only the visible picture depends on -- 2D, frame pacing, options,
//     texture-set lookups -- go straight to Acclaim's original renderer, through a jump that
//     hands it the engine's arguments untouched, typed or not (ORIGINAL_ONLY below);
//   - start-up, texture enumeration, scenes and display lists run through both: the original
//     draws the game window, this renderer draws the same frames into the Remix window and
//     records them for a capture.
//
// Outside lookdev mode every export is this renderer's own.

#include "device.h"
#include "engine.h"
#include "log.h"
#include "lookdev.h"
#include "original.h"
#include "pcx.h"
#include "texture.h"
#include "window.h"
#include "geometry.h"
#include "normal_cache.h"
#include "config.h"

#include <intrin.h>

#include <stdio.h>

#include <windows.h>

// All exports are __cdecl, which is the default calling convention for this build (/Gd),
// so the .def maps each undecorated name onto the leading-underscore symbol MSVC emits.
#define GFXAPI extern "C"

namespace {

// Mirrors the original DLL's option table at 0x10021fdc, read as signed bytes.
const signed char kOptionStatus[16] = {
    0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, -128, 0, 0, 0
};

// F10 arms a one-frame batch-by-batch dump, F11 takes a screenshot. Both are edge-triggered
// off GetAsyncKeyState's low bit, which reports "pressed since the last call" and so works
// even while the game holds the devices.
void MaybeDumpFrame() {
    if ((GetAsyncKeyState(VK_F10) & 1) || Lookdev::TakeDumpRequest()) {
        Geometry::DumpNextFrame();
    }
}

DWORD g_frameStartTicks = 0;
bool g_vsyncEnabled = false;
unsigned int g_frameCount = 0;
int g_frameStatCounter = 0;

// PrintWindow returns black for a D3D9 swapchain, so the renderer takes its own screenshots:
// on F11, and optionally from a fixed frame on ([Debug] ScreenshotFrame / ScreenshotInterval).
void MaybeScreenshot() {
    const bool hotkey = (GetAsyncKeyState(VK_F11) & 1) != 0;
    const int first = Config::ScreenshotFrame();
    const int interval = Config::ScreenshotInterval();
    bool automatic = false;
    if (first > 0 && (int)g_frameCount >= first) {
        const int since = (int)g_frameCount - first;
        automatic = (since == 0) || (interval > 0 && (since % interval) == 0);
    }
    if (!hotkey && !automatic) {
        return;
    }
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%sshot_%05u.bmp", Config::BaseDir(), g_frameCount);
    path[sizeof(path) - 1] = 0;
    Device::RequestScreenshot(path);
}

// ============================================================ this renderer's own exports
//
// The ones the tee hands to the original in lookdev mode. ORIGINAL_ONLY below exports each
// under its GFXDLL_ name.

int Ours_CanXXX(unsigned int capFlags) {
    // Reporting hardware capability is what steers the engine onto the textured-quad 2D
    // path instead of the software direct-blit path that the D3D renderer leaves unimplemented.
    LogCall("GFXDLL_CanXXX", "flags=0x%08x", capFlags);
    return 1;
}

int* Ours_SetCapsBits(void) {
    LogCall("GFXDLL_SetCapsBits");
    return 0;
}

int Ours_GetOptionStatus(int index) {
    if (index < 0 || index >= (int)(sizeof(kOptionStatus) / sizeof(kOptionStatus[0]))) {
        return 0;
    }
    return kOptionStatus[index];
}

unsigned int Ours_ToggleOption(int option) {
    LogCall("GFXDLL_ToggleOption", "%d", option);
    return 0;
}

// The original writes a value the engine reads back; both source globals are zero at
// load, which makes 0 and 0x53 the correct fresh-start pair.
void Ours_SetDLLVariables(unsigned int* out1, unsigned int* out2) {
    LogCall("GFXDLL_SetDLLVariables", "%p, %p", (void*)out1, (void*)out2);
    if (out1) {
        *out1 = 0;
    }
    if (out2) {
        *out2 = 0x53;
    }
}

int Ours_DoSettingsDialog(void) {
    LogCall("GFXDLL_DoSettingsDialog");
    return 0;
}

// The engine polls this to pace frames. The original skips the sleep when vsync is on,
// leaving the flip to throttle instead.
int Ours_WaitAndGetFrameMSTime(int targetMs) {
    DWORD now = GetTickCount();
    int elapsed = (int)(now - g_frameStartTicks);
    if (!g_vsyncEnabled && elapsed < targetMs) {
        Sleep(targetMs - elapsed);
    }
    g_frameStartTicks = GetTickCount();
    return elapsed;
}

void Ours_StartFrameTiming(void) {
    g_frameStartTicks = GetTickCount();
}

void Ours_SetVSync(int enabled) {
    LogLine("    GFXDLL_SetVSync(%d): present interval %s (Remix decides by rtx.enableVsync)", enabled,
            enabled ? "one" : "immediate");
    g_vsyncEnabled = (enabled != 0);
    Device::SetVSync(g_vsyncEnabled);
}

void Ours_ShowWindowsDisplay(void) {
    LogCall("GFXDLL_ShowWindowsDisplay");
}

void Ours_changeViewportSize(void) {
    LogCall("GFXDLL_changeViewportSize");
}

void Ours_SetColormask(void) {
    LogCall("GFXDLL_SetColormask");
}

void Ours_CheckBloodColor(void) {
    LogCall("GFXDLL_CheckBloodColor");
}

// Returns the copy of the set kept since enumeration (the engine's own buffer is reused) --
// the engine dereferences the result, so a null or invented pointer crashes the game.
void* Ours_GetTextureSetPtr(int index, int* outWidth, int* outHeight) {
    void* set = Texture::GetSet(index, outWidth, outHeight);
    LogCall("GFXDLL_GetTextureSetPtr", "%d -> %p (%dx%d)", index, set,
            outWidth ? *outWidth : -1, outHeight ? *outHeight : -1);
    return set;
}

int Ours_LoadPCX(const char* name) {
    LogCall("GFXDLL_LoadPCX", "\"%s\"", name ? name : "(null)");
    return Pcx::Load(name);
}

int Ours_CreatePCX(int width, int height) {
    LogCall("GFXDLL_CreatePCX", "%d x %d", width, height);
    return Pcx::Create(width, height);
}

void Ours_UnloadPCX(int handle) {
    LogCall("GFXDLL_UnloadPCX", "%d", handle);
    Pcx::Unload(handle);
}

void Ours_DrawPCX(void) {
    LogCall("GFXDLL_DrawPCX");
}

// Reached by a jump from the export, so its return address is the engine's call site.
void Ours_DrawQuadPCX(int handle, const float* positions, const float* colours,
                      const float* uvs) {
    LogCall("GFXDLL_DrawQuadPCX");
    Pcx::DrawQuad(handle, positions, colours, uvs, (unsigned int)_ReturnAddress());
}

void Ours_BlitPCXToPCX(void) {
    LogCall("GFXDLL_BlitPCXToPCX");
}

void Ours_DirtyPCX(int handle, int x0, int y0, int x1, int y1) {
    Pcx::MarkDirty(handle, x0, y0, x1, y1);
}

int Ours_GetWidthPCX(int handle) {
    return Pcx::Width(handle);
}

int Ours_GetHeightPCX(int handle) {
    return Pcx::Height(handle);
}

// Both return pointers straight into the engine's own image object: the engine draws into
// the RGBA buffer itself and calls UpdateRGBAPCX when it wants the result on screen.
void* Ours_GetIndexedPCX(int handle) {
    LogCall("GFXDLL_GetIndexedPCX", "%d", handle);
    return Pcx::IndexedPixels(handle);
}

void* Ours_GetRGBAPCX(int handle) {
    LogCall("GFXDLL_GetRGBAPCX", "%d -> %p", handle, Pcx::RgbaPixels(handle));
    return Pcx::RgbaPixels(handle);
}

void Ours_UpdateRGBAPCX(int handle) {
    Pcx::Update(handle);
}

void Ours_SetBlendModePCX(int mode) {
    LogCall("GFXDLL_SetBlendModePCX");
    Pcx::SetBlendMode(mode);
}

void Ours_SetAllocatePCXImageFile(Pcx::AllocFn fn) {
    LogCall("GFXDLL_SetAllocatePCXImageFile", "%p", (void*)fn);
    Pcx::SetAlloc(fn);
}

void Ours_SetDeallocatePCXImageFile(Pcx::DeallocFn fn) {
    LogCall("GFXDLL_SetDeallocatePCXImageFile", "%p", (void*)fn);
    Pcx::SetDealloc(fn);
}

void Ours_SetsaveDataAsPCX(void* fn) {
    LogCall("GFXDLL_SetsaveDataAsPCX", "%p", fn);
}

void Ours_LockFrameBufferPCX(void) {
    LogCall("GFXDLL_LockFrameBufferPCX");
}

void Ours_UnlockFrameBufferPCX(void) {
    LogCall("GFXDLL_UnlockFrameBufferPCX");
}

void Ours_GetScreen(void) {
    LogCall("GFXDLL_GetScreen");
}

void Ours_DumpScreen(void) {
    LogCall("GFXDLL_DumpScreen");
}

} // namespace

// ============================================================ exports routed whole
//
// Each makes sure the routes are bound, then jumps through g_route_<name>: this renderer's
// function, or in lookdev mode the original's. A jump rather than a call passes on the
// engine's arguments and the callee's return value exactly, whatever they are; the binding
// call in front saves every register, so the jump finds them as the engine left them.

namespace {
void EnsureRoutes();
}

#define ORIGINAL_ONLY(name)                                         \
    namespace { void* g_route_GFXDLL_##name = (void*)&Ours_##name; } \
    GFXAPI __declspec(naked) void GFXDLL_##name(void) {             \
        __asm pushad                                                \
        __asm call EnsureRoutes                                     \
        __asm popad                                                 \
        __asm jmp dword ptr [g_route_GFXDLL_##name]                 \
    }

#define ORIGINAL_ONLY_EXPORTS(X)                                                  \
    X(CanXXX) X(SetCapsBits) X(GetOptionStatus) X(ToggleOption) X(SetDLLVariables) \
    X(DoSettingsDialog) X(WaitAndGetFrameMSTime) X(StartFrameTiming) X(SetVSync)   \
    X(ShowWindowsDisplay) X(changeViewportSize) X(SetColormask) X(CheckBloodColor) \
    X(GetTextureSetPtr) X(LoadPCX) X(CreatePCX) X(UnloadPCX) X(DrawPCX)            \
    X(DrawQuadPCX) X(BlitPCXToPCX) X(DirtyPCX) X(GetWidthPCX) X(GetHeightPCX)      \
    X(GetIndexedPCX) X(GetRGBAPCX) X(UpdateRGBAPCX) X(SetBlendModePCX)             \
    X(SetAllocatePCXImageFile) X(SetDeallocatePCXImageFile) X(SetsaveDataAsPCX)    \
    X(LockFrameBufferPCX) X(UnlockFrameBufferPCX) X(GetScreen) X(DumpScreen)

ORIGINAL_ONLY_EXPORTS(ORIGINAL_ONLY)

// ============================================================ exports run by both

namespace {

typedef unsigned int (__cdecl* GetDriverInfoFn)(void*, char*, const char*, int);
typedef unsigned int (__cdecl* InitializeDriverFn)(HWND, unsigned int, unsigned int,
                                                   unsigned int, unsigned int, unsigned int,
                                                   unsigned int*, unsigned int);
typedef void (__cdecl* VoidFn)(void);
typedef void (__cdecl* ProcessDisplayListFn)(unsigned int*);
typedef void (__cdecl* SetZPlanesFn)(float, float);
typedef int (__cdecl* StartEnumerationFn)(unsigned int, HWND, unsigned int);
typedef void (__cdecl* EnumerateTextureFn)(unsigned int*, unsigned int, HWND);
typedef void (__cdecl* FinishEnumerationFn)(HWND);
typedef void (__cdecl* SetTextureFileNameFn)(const char*);

// The original's side of the shared exports; all null outside lookdev mode.
struct OriginalExports {
    GetDriverInfoFn getDriverInfo;
    InitializeDriverFn initializeDriver;
    VoidFn shutdownDriver;
    VoidFn beginScene;
    VoidFn endScene;
    ProcessDisplayListFn processDisplayList;
    SetZPlanesFn setZPlanes;
    VoidFn flushTextureCache;
    StartEnumerationFn startEnumeration;
    EnumerateTextureFn enumerateTexture;
    FinishEnumerationFn finishEnumeration;
    SetTextureFileNameFn setTextureFileName;
} g_original = {};

// When the original's converted texture store is already current, it loads it in
// StartTextureEnumeration and expects neither the sets nor the finish.
bool g_originalStoreCurrent = false;

// Driver records as the engine keeps them (0x007e49c0 onwards) and the original fills them.
const unsigned int kDriverRecordSize = 0xfc;
const unsigned int kRecordFullscreen = 0xf8;

// Loads the original and routes the exports to it.
bool BindOriginal() {
    if (!Original::Load()) {
        return false;
    }
#define BIND_ROUTE(name) g_route_GFXDLL_##name = (void*)Original::Export("GFXDLL_" #name);
    ORIGINAL_ONLY_EXPORTS(BIND_ROUTE)
#undef BIND_ROUTE
    g_original.getDriverInfo = (GetDriverInfoFn)Original::Export("GFXDLL_GetDriverInfo");
    g_original.initializeDriver = (InitializeDriverFn)Original::Export("GFXDLL_InitializeDriver");
    g_original.shutdownDriver = (VoidFn)Original::Export("GFXDLL_ShutdownDriver");
    g_original.beginScene = (VoidFn)Original::Export("GFXDLL_BeginScene");
    g_original.endScene = (VoidFn)Original::Export("GFXDLL_EndScene");
    g_original.processDisplayList =
        (ProcessDisplayListFn)Original::Export("GFXDLL_ProcessDisplayList");
    g_original.setZPlanes = (SetZPlanesFn)Original::Export("GFXDLL_SetZPlanes");
    g_original.flushTextureCache = (VoidFn)Original::Export("GFXDLL_FlushTextureCache");
    g_original.startEnumeration =
        (StartEnumerationFn)Original::Export("GFXDLL_StartTextureEnumeration");
    g_original.enumerateTexture = (EnumerateTextureFn)Original::Export("GFXDLL_EnumerateTexture");
    g_original.finishEnumeration =
        (FinishEnumerationFn)Original::Export("GFXDLL_FinishTextureEnumeration");
    g_original.setTextureFileName =
        (SetTextureFileNameFn)Original::Export("GFXDLL_SetTextureFileName");
    return true;
}

// The tee needs the original renderer and dgVoodoo2 in <game>\lookdev\ref; without them there
// is no ground truth to show.
void ReportMissingOriginal() {
    MessageBoxA(0,
                "Lookdev mode could not start Acclaim's original renderer.\n\n"
                "It needs the original renderer and dgVoodoo2 in <game>\\lookdev\\ref. "
                "Video_D3D9.log has the details.",
                "South Park lookdev", MB_ICONERROR);
}

// In lookdev mode, binds the original on whichever export the engine calls first. That has to
// be per load: the engine loads every plugin to list its drivers, unloads them, and loads the
// chosen one afresh, which then sees no driver enumeration at all. Outside lookdev mode, and
// after the first call, it does nothing.
bool g_routesSettled = false;

void EnsureRoutes() {
    if (g_routesSettled) {
        return;
    }
    g_routesSettled = true;
    if (Lookdev::IsTee() && !BindOriginal()) {
        ReportMissingOriginal();
    }
}

} // namespace

// ------------------------------------------------------------------ lifecycle

// The engine passes a window, the driver-record buffer, the plugin's own base name and the
// record capacity, then uses the return value as the number of drivers found. The original
// enumerates DirectDraw devices, keeps only those whose D3D device description is
// "Direct3D HAL", and writes a 60-byte name field per record, null-terminated at +0x3b,
// followed by the a3 string at +0x3c.
//
// This renderer exposes exactly one driver; the tee reports the original's.
GFXAPI unsigned int GFXDLL_GetDriverInfo(void* a1, char* a2, const char* a3, int maxEntries) {
    // Driver enumeration is the first export the engine calls, which makes it the earliest
    // moment the D3D9 runtime can be brought in. RTX Remix needs that head start to detour
    // input before the engine claims it -- see Device::PreloadD3D9.
    Device::PreloadD3D9();
    LogCall("GFXDLL_GetDriverInfo", "a1=%p, a2=%p, a3=%p, max=%d", a1, (void*)a2, (const void*)a3, maxEntries);
    EnsureRoutes();
    if (g_original.getDriverInfo) {
        // The engine passes record +0xf8 to InitializeDriver as its fullscreen flag
        // (SouthPark.exe 0x004790bc), and the original sets it for a hardware device -- which
        // takes the whole display to 640x480 exclusive mode. Cleared, the engine runs the game
        // in a window, as it does on this renderer; InitializeDriver below asks the original
        // for the fullscreen path itself, which dgVoodoo2 keeps in that window.
        const unsigned int drivers = g_original.getDriverInfo(a1, a2, a3, maxEntries);
        for (unsigned int i = 0; i < drivers; ++i) {
            *(unsigned int*)(a2 + i * kDriverRecordSize + kRecordFullscreen) = 0;
        }
        return drivers;
    }

    LogHex("a1 before", a1, 64);
    LogHex("a2 before", a2, 128);
    if (a3 && !IsBadReadPtr(a3, 1)) {
        LogLine("    a3 string = \"%s\"", a3);
    }

    if (maxEntries < 1 || !a2 || IsBadWritePtr(a2, 0x40)) {
        LogLine("    no room for a driver record -- reporting 0");
        return 0;
    }
    memset(a2, 0, 0x3c);
    lstrcpynA(a2, "Direct3D9 Renderer (D3D)", 0x3b);
    a2[0x3b] = 0;
    if (a3 && !IsBadReadPtr(a3, 1) && !IsBadWritePtr(a2 + 0x3c, 0x40)) {
        lstrcpynA(a2 + 0x3c, a3, 0x3f);
    }

    LogHex("a2 after", a2, 128);
    return 1;
}

// Returns 1 on success and 0xFFFFFFFF on failure, matching the original. The tee returns the
// original's answer; this renderer's own device then goes into the Remix window.
GFXAPI unsigned int GFXDLL_InitializeDriver(HWND window, unsigned int a2, unsigned int a3,
                                            unsigned int a4, unsigned int a5, unsigned int a6,
                                            unsigned int* a7, unsigned int a8) {
    EnsureRoutes();
    LogCall("GFXDLL_InitializeDriver",
            "hwnd=%p, %u, %u, %u, %u, %u, %p, %u",
            window, a2, a3, a4, a5, a6, (void*)a7, a8);

    unsigned int result = 1;
    HWND target = window;
    if (g_original.initializeDriver) {
        // Fullscreen requested: only on that path does the original get the video-memory render
        // target its Direct3D device needs (else D3DERR_SURFACENOTINVIDMEM). See GetDriverInfo.
        const unsigned int fullscreen = 1;
        result = g_original.initializeDriver(window, a2, a3, fullscreen, a5, a6, a7, a8);
    }

    const unsigned int startup[7] = {a2, a3, a4, a5, a6, (unsigned int)a7, a8};
    Lookdev::OnInitializeDriver(window, startup);

    // The executable has unpacked itself by the time it drives any export, so this is the
    // first point at which its code can be patched. The lookdev host has no executable, and
    // no engine window.
    if (!Lookdev::IsHost()) {
        Engine::Install();
        Window::Attach(window, g_original.initializeDriver ? Lookdev::GameWindowPlacement()
                                                           : Window::Configured());
    }

    if (g_original.initializeDriver) {
        target = Lookdev::CreateRemixWindow();
    }
    if (!Device::Create(target)) {
        // In the tee the original still runs; only the Remix window stays empty.
        return g_original.initializeDriver ? result : 0xFFFFFFFFu;
    }
    // Args 5 and 6 are the framebuffer size the engine expects to draw into.
    Geometry::SetViewportSize((int)a5, (int)a6);
    g_frameStartTicks = GetTickCount();
    return result;
}

GFXAPI void GFXDLL_ShutdownDriver(void) {
    EnsureRoutes();
    LogCall("GFXDLL_ShutdownDriver");
    Geometry::LogTextureStats();
    Texture::LogMouthVariants();
    Engine::LogState();
    Geometry::ReleaseResources();
    Pcx::ReleaseAll();
    // Every cached texture holds a reference on the device, so releasing the device without
    // them first leaves the whole set -- and the device -- resident. A driver reselect then
    // creates a second device alongside the first.
    Texture::ReleaseAll();
    Device::Destroy();
    if (g_original.shutdownDriver) {
        g_original.shutdownDriver();
        Lookdev::DestroyRemixWindow();
    }
}

GFXAPI void GFXDLL_BeginScene(void) {
    EnsureRoutes();
    LogCall("GFXDLL_BeginScene");
    Lookdev::OnBeginScene();
    Config::RefreshLive();
    Engine::OnFrame();
    MaybeDumpFrame();
    if (Device::BeginFrame()) {
        Geometry::BeginFrame();
    }
    if (g_original.beginScene) {
        g_original.beginScene();
    }
}

// The F4 overlay's live settings ([Display] FieldOfView and HudAtSides, [Sky] StarrySky,
// [FullMap] KeepAroundCamera and DrawDistancePercent). 1 when the key is one of them; the input
// shim exports the same function for its keys.
extern "C" __declspec(dllexport) int __cdecl SouthPark_SetSetting(const char* section, const char* key,
                                                                  float value) {
    return section && key && Config::SetLive(section, key, value) ? 1 : 0;
}

// For the Remix proxy (d3d9.dll): how many of `count` points (x, y, z each) have level
// geometry above them, or -1 when the renderer holds no geometry around any of them. The proxy
// calls it from Present, on the thread that renders.
extern "C" __declspec(dllexport) int __cdecl SouthPark_CoverAbove(const float* points, int count) {
    return NormalCache::CoverAbove(points, count);
}

GFXAPI void GFXDLL_EndScene(void) {
    EnsureRoutes();
    LogCall("GFXDLL_EndScene");
    Geometry::EndFrame();
    // 2D quads still waiting belong to a frame that drew no world: a 2D screen.
    Pcx::DrawPendingAsScreen();
    Geometry::LogGeometryAudit();
    Lookdev::OnEndScene();
    MaybeScreenshot();
    Device::EndFrame();
    ++g_frameCount;
    if ((++g_frameStatCounter % 300) == 0) {
        Pcx::LogQuadStats();
    }
    if (g_original.endScene) {
        g_original.endScene();
        Lookdev::PumpRemixWindow();
    }
}

// ------------------------------------------------------------------ rendering

GFXAPI void GFXDLL_ProcessDisplayList(unsigned int* displayList) {
    EnsureRoutes();
    LogCall("GFXDLL_ProcessDisplayList");
    Lookdev::BeginList(displayList);
    Geometry::ProcessDisplayList(displayList);
    Lookdev::EndList();
    if (g_original.processDisplayList) {
        g_original.processDisplayList(displayList);
    }
}

GFXAPI void GFXDLL_SetZPlanes(float zNear, float zFar) {
    EnsureRoutes();
    LogCall("GFXDLL_SetZPlanes", "near=%f far=%f", zNear, zFar);
    Lookdev::OnSetZPlanes(zNear, zFar);
    Geometry::SetZPlanes(zNear, zFar);
    if (g_original.setZPlanes) {
        g_original.setZPlanes(zNear, zFar);
    }
}

// ------------------------------------------------------------------- textures

// The original takes the set count, the window it draws its progress text on, and a value it
// keeps; this renderer needs none of them, but a lookdev capture records them for a replay
// through the original.
//
// The original returns 1 when its converted store (win32.tex) is already current, and then
// expects no EnumerateTexture calls: its table for them is never allocated. This renderer keeps
// no store, so it always asks the engine for the sets -- in the tee too, and then passes them
// on to the original only when it wants them.
GFXAPI int GFXDLL_StartTextureEnumeration(unsigned int count, HWND window, unsigned int a3) {
    EnsureRoutes();
    LogCall("GFXDLL_StartTextureEnumeration", "%u, %p, %u", count, window, a3);
    Lookdev::OnStartTextureEnumeration(count, a3);
    Texture::StartEnumeration();
    if (g_original.startEnumeration) {
        g_originalStoreCurrent = g_original.startEnumeration(count, window, a3) == 1;
    }
    return 0;
}

GFXAPI void GFXDLL_EnumerateTexture(unsigned int* blob, unsigned int a2, HWND window) {
    EnsureRoutes();
    Lookdev::OnEnumerateTexture(a2);
    Texture::Enumerate(blob);
    if (g_original.enumerateTexture && !g_originalStoreCurrent) {
        g_original.enumerateTexture(blob, a2, window);
    }
}

GFXAPI void GFXDLL_FinishTextureEnumeration(HWND window) {
    EnsureRoutes();
    LogCall("GFXDLL_FinishTextureEnumeration");
    Texture::FinishEnumeration();
    Texture::LogSetCensus();
    if (g_original.finishEnumeration && !g_originalStoreCurrent) {
        g_original.finishEnumeration(window);
    }
}

GFXAPI void GFXDLL_FlushTextureCache(void) {
    EnsureRoutes();
    LogCall("GFXDLL_FlushTextureCache");
    Lookdev::OnFlushTextureCache();
    Texture::FlushDecoded();
    if (g_original.flushTextureCache) {
        g_original.flushTextureCache();
    }
}

// The tee keeps the original's store in lookdev\ref rather than the game folder.
GFXAPI void GFXDLL_SetTextureFileName(const char* name) {
    EnsureRoutes();
    LogCall("GFXDLL_SetTextureFileName", "\"%s\"", name ? name : "(null)");
    Lookdev::OnSetTextureFileName(name);
    if (g_original.setTextureFileName) {
        g_original.setTextureFileName(Original::TextureStorePath());
    }
}
