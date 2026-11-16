// The 46 GFXDLL_* exports the South Park engine expects from a video plugin.
//
// Every export is __cdecl, so the caller cleans the stack and a stub may safely declare
// fewer parameters than the engine passes. Only the exports whose arguments or return
// values the renderer uses are typed; the rest take no parameters, log the call and do
// nothing, which is all the engine needs of them.

#include "device.h"
#include "engine.h"
#include "log.h"
#include "lookdev.h"
#include "pcx.h"
#include "texture.h"
#include "geometry.h"
#include "config.h"

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
    if (GetAsyncKeyState(VK_F10) & 1) {
        Geometry::DumpNextFrame();
    }
}

DWORD g_frameStartTicks = 0;
bool g_vsyncEnabled = false;
unsigned int g_frameCount = 0;
int g_frameStatCounter = 0;

} // namespace

// ------------------------------------------------------------------ lifecycle

// Returns 1 on success and 0xFFFFFFFF on failure, matching the original.
GFXAPI unsigned int GFXDLL_InitializeDriver(HWND window, unsigned int a2, unsigned int a3,
                                            unsigned int a4, unsigned int a5, unsigned int a6,
                                            unsigned int* a7, unsigned int a8) {
    LogCall("GFXDLL_InitializeDriver",
            "hwnd=%p, %u, %u, %u, %u, %u, %p, %u",
            window, a2, a3, a4, a5, a6, (void*)a7, a8);

    const unsigned int startup[7] = {a2, a3, a4, a5, a6, (unsigned int)a7, a8};
    Lookdev::OnInitializeDriver(startup);

    // The executable has unpacked itself by the time it drives any export, so this is the
    // first point at which its code can be patched. The lookdev host has no executable.
    if (!Lookdev::IsHost()) {
        Engine::Install();
    }

    if (!Device::Create(window)) {
        return 0xFFFFFFFFu;
    }
    // Args 5 and 6 are the framebuffer size the engine expects to draw into.
    Geometry::SetViewportSize((int)a5, (int)a6);
    g_frameStartTicks = GetTickCount();
    return 1;
}

GFXAPI void GFXDLL_ShutdownDriver(void) {
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
}

GFXAPI void GFXDLL_BeginScene(void) {
    LogCall("GFXDLL_BeginScene");
    Lookdev::OnBeginScene();
    MaybeDumpFrame();
    if (Device::BeginFrame()) {
        Geometry::BeginFrame();
    }
}

// PrintWindow returns black for a D3D9 swapchain, so the renderer takes its own screenshots:
// on F11, and optionally from a fixed frame on ([Debug] ScreenshotFrame / ScreenshotInterval).
namespace {
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
} // namespace

GFXAPI void GFXDLL_EndScene(void) {
    LogCall("GFXDLL_EndScene");
    Geometry::EndFrame();
    Geometry::LogGeometryAudit();
    Lookdev::OnEndScene();
    MaybeScreenshot();
    Device::EndFrame();
    ++g_frameCount;
    if ((++g_frameStatCounter % 300) == 0) {
        Pcx::LogQuadStats();
    }
}

// The engine polls this to pace frames. The original skips the sleep when vsync is on,
// leaving the flip to throttle instead.
GFXAPI int GFXDLL_WaitAndGetFrameMSTime(int targetMs) {
    DWORD now = GetTickCount();
    int elapsed = (int)(now - g_frameStartTicks);
    if (!g_vsyncEnabled && elapsed < targetMs) {
        Sleep(targetMs - elapsed);
    }
    g_frameStartTicks = GetTickCount();
    return elapsed;
}

GFXAPI void GFXDLL_StartFrameTiming(void) {
    g_frameStartTicks = GetTickCount();
}

GFXAPI void GFXDLL_SetVSync(int enabled) {
    LogCall("GFXDLL_SetVSync", "%d", enabled);
    g_vsyncEnabled = (enabled != 0);
    Device::SetVSync(g_vsyncEnabled);
}

GFXAPI void GFXDLL_ShowWindowsDisplay(void) {
    LogCall("GFXDLL_ShowWindowsDisplay");
}

GFXAPI void GFXDLL_changeViewportSize(void) {
    LogCall("GFXDLL_changeViewportSize");
}

// ------------------------------------------------------------------ rendering

GFXAPI void GFXDLL_ProcessDisplayList(unsigned int* displayList) {
    LogCall("GFXDLL_ProcessDisplayList");
    Lookdev::BeginList(displayList);
    Geometry::ProcessDisplayList(displayList);
    Lookdev::EndList();
}

GFXAPI void GFXDLL_SetZPlanes(float zNear, float zFar) {
    LogCall("GFXDLL_SetZPlanes", "near=%f far=%f", zNear, zFar);
    Lookdev::OnSetZPlanes(zNear, zFar);
    Geometry::SetZPlanes(zNear, zFar);
}

GFXAPI void GFXDLL_SetColormask(void) {
    LogCall("GFXDLL_SetColormask");
}

GFXAPI void GFXDLL_CheckBloodColor(void) {
    LogCall("GFXDLL_CheckBloodColor");
}

// --------------------------------------------------------------- capabilities

// Reporting hardware capability is what steers the engine onto the textured-quad 2D
// path instead of the software direct-blit path that the D3D renderer leaves unimplemented.
GFXAPI int GFXDLL_CanXXX(unsigned int capFlags) {
    LogCall("GFXDLL_CanXXX", "flags=0x%08x", capFlags);
    return 1;
}

// The engine passes two output buffers, a source string and a capacity, then uses the
// return value as the number of drivers found. The original enumerates DirectDraw devices,
// keeps only those whose D3D device description is "Direct3D HAL", and writes a 60-byte
// name field per record, null-terminated at +0x3b, followed by the a3 string at +0x3c.
//
// We expose exactly one driver. The buffers are dumped first because the record stride and
// the purpose of a1 are still unknown -- the executable that allocates them is packed.
GFXAPI unsigned int GFXDLL_GetDriverInfo(void* a1, char* a2, const char* a3, int maxEntries) {
    // Driver enumeration is the first export the engine calls, which makes it the earliest
    // moment the D3D9 runtime can be brought in. RTX Remix needs that head start to detour
    // input before the engine claims it -- see Device::PreloadD3D9.
    Device::PreloadD3D9();
    LogCall("GFXDLL_GetDriverInfo", "a1=%p, a2=%p, a3=%p, max=%d", a1, (void*)a2, (const void*)a3, maxEntries);
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

GFXAPI int* GFXDLL_SetCapsBits(void) {
    LogCall("GFXDLL_SetCapsBits");
    return 0;
}

GFXAPI int GFXDLL_GetOptionStatus(int index) {
    if (index < 0 || index >= (int)(sizeof(kOptionStatus) / sizeof(kOptionStatus[0]))) {
        return 0;
    }
    return kOptionStatus[index];
}

GFXAPI unsigned int GFXDLL_ToggleOption(int option) {
    LogCall("GFXDLL_ToggleOption", "%d", option);
    return 0;
}

// The original writes a value the engine reads back; both source globals are zero at
// load, which makes 0 and 0x53 the correct fresh-start pair.
GFXAPI void GFXDLL_SetDLLVariables(unsigned int* out1, unsigned int* out2) {
    LogCall("GFXDLL_SetDLLVariables", "%p, %p", (void*)out1, (void*)out2);
    if (out1) {
        *out1 = 0;
    }
    if (out2) {
        *out2 = 0x53;
    }
}

GFXAPI int GFXDLL_DoSettingsDialog(void) {
    LogCall("GFXDLL_DoSettingsDialog");
    return 0;
}

// ------------------------------------------------------------------- textures

// The original takes the set count, the window it draws its progress text on, and a value it
// keeps; this renderer needs none of them, but a lookdev capture records them for a replay
// through the original.
//
// The original returns 1 when its converted store (win32.tex) is already current, and then
// expects no EnumerateTexture calls: its table for them is never allocated. This renderer keeps
// no store, so it always asks for the sets.
GFXAPI int GFXDLL_StartTextureEnumeration(unsigned int count, HWND window, unsigned int a3) {
    LogCall("GFXDLL_StartTextureEnumeration", "%u, %p, %u", count, window, a3);
    Lookdev::OnStartTextureEnumeration(count, a3);
    Texture::StartEnumeration();
    return 0;
}

GFXAPI void GFXDLL_EnumerateTexture(unsigned int* blob, unsigned int a2, HWND window) {
    Lookdev::OnEnumerateTexture(a2);
    Texture::Enumerate(blob);
}

GFXAPI void GFXDLL_FinishTextureEnumeration(HWND window) {
    LogCall("GFXDLL_FinishTextureEnumeration");
    Texture::FinishEnumeration();
    Texture::LogSetCensus();
}

GFXAPI void GFXDLL_FlushTextureCache(void) {
    LogCall("GFXDLL_FlushTextureCache");
    Lookdev::OnFlushTextureCache();
    Texture::FlushDecoded();
}

GFXAPI void GFXDLL_SetTextureFileName(const char* name) {
    LogCall("GFXDLL_SetTextureFileName", "\"%s\"", name ? name : "(null)");
    Lookdev::OnSetTextureFileName(name);
}

// Returns the copy of the set kept since enumeration (the engine's own buffer is reused) --
// the engine dereferences the result, so a null or invented pointer crashes the game.
GFXAPI void* GFXDLL_GetTextureSetPtr(int index, int* outWidth, int* outHeight) {
    void* set = Texture::GetSet(index, outWidth, outHeight);
    LogCall("GFXDLL_GetTextureSetPtr", "%d -> %p (%dx%d)", index, set,
            outWidth ? *outWidth : -1, outHeight ? *outHeight : -1);
    return set;
}

// ------------------------------------------------------------------- PCX / 2D

GFXAPI int GFXDLL_LoadPCX(const char* name) {
    LogCall("GFXDLL_LoadPCX", "\"%s\"", name ? name : "(null)");
    return Pcx::Load(name);
}

GFXAPI int GFXDLL_CreatePCX(int width, int height) {
    LogCall("GFXDLL_CreatePCX", "%d x %d", width, height);
    return Pcx::Create(width, height);
}

GFXAPI void GFXDLL_UnloadPCX(int handle) {
    LogCall("GFXDLL_UnloadPCX", "%d", handle);
    Pcx::Unload(handle);
}

GFXAPI void GFXDLL_DrawPCX(void) {
    LogCall("GFXDLL_DrawPCX");
}

GFXAPI void GFXDLL_DrawQuadPCX(int handle, const float* positions, const float* colours,
                               const float* uvs) {
    LogCall("GFXDLL_DrawQuadPCX");
    Pcx::DrawQuad(handle, positions, colours, uvs);
}

GFXAPI void GFXDLL_BlitPCXToPCX(void) {
    LogCall("GFXDLL_BlitPCXToPCX");
}

GFXAPI void GFXDLL_DirtyPCX(int handle, int x0, int y0, int x1, int y1) {
    Pcx::MarkDirty(handle, x0, y0, x1, y1);
}

GFXAPI int GFXDLL_GetWidthPCX(int handle) {
    return Pcx::Width(handle);
}

GFXAPI int GFXDLL_GetHeightPCX(int handle) {
    return Pcx::Height(handle);
}

// Both return pointers straight into the engine's own image object: the engine draws into
// the RGBA buffer itself and calls UpdateRGBAPCX when it wants the result on screen.
GFXAPI void* GFXDLL_GetIndexedPCX(int handle) {
    LogCall("GFXDLL_GetIndexedPCX", "%d", handle);
    return Pcx::IndexedPixels(handle);
}

GFXAPI void* GFXDLL_GetRGBAPCX(int handle) {
    LogCall("GFXDLL_GetRGBAPCX", "%d -> %p", handle, Pcx::RgbaPixels(handle));
    return Pcx::RgbaPixels(handle);
}

GFXAPI void GFXDLL_UpdateRGBAPCX(int handle) {
    Pcx::Update(handle);
}

GFXAPI void GFXDLL_SetBlendModePCX(int mode) {
    LogCall("GFXDLL_SetBlendModePCX");
    Pcx::SetBlendMode(mode);
}

GFXAPI void GFXDLL_SetAllocatePCXImageFile(Pcx::AllocFn fn) {
    LogCall("GFXDLL_SetAllocatePCXImageFile", "%p", (void*)fn);
    Pcx::SetAlloc(fn);
}

GFXAPI void GFXDLL_SetDeallocatePCXImageFile(Pcx::DeallocFn fn) {
    LogCall("GFXDLL_SetDeallocatePCXImageFile", "%p", (void*)fn);
    Pcx::SetDealloc(fn);
}

GFXAPI void GFXDLL_SetsaveDataAsPCX(void* fn) {
    LogCall("GFXDLL_SetsaveDataAsPCX", "%p", fn);
}

// ------------------------------------------------------------ framebuffer I/O

GFXAPI void GFXDLL_LockFrameBufferPCX(void) {
    LogCall("GFXDLL_LockFrameBufferPCX");
}

GFXAPI void GFXDLL_UnlockFrameBufferPCX(void) {
    LogCall("GFXDLL_UnlockFrameBufferPCX");
}

GFXAPI void GFXDLL_GetScreen(void) {
    LogCall("GFXDLL_GetScreen");
}

GFXAPI void GFXDLL_DumpScreen(void) {
    LogCall("GFXDLL_DumpScreen");
}
