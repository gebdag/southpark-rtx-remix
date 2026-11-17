#include "device.h"
#include "config.h"
#include "log.h"
#include "lookdev.h"

#include <d3d9.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

// Direct3DCreate9 is resolved at runtime rather than statically imported, so that this
// plugin can pick *which* d3d9.dll it talks to. See LoadD3D9() below -- this is what makes
// RTX Remix reachable, and the reason d3d9.lib is not on the link line.
typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT);

HMODULE g_d3d9Module = 0;
IDirect3D9* g_d3d = 0;
IDirect3DDevice9* g_device = 0;
HWND g_window = 0;
D3DPRESENT_PARAMETERS g_params;
bool g_inScene = false;
bool g_needsReset = false;
Device::Pipeline g_pipeline = Device::PipelineNone;
bool g_frameHadDraw = false;
bool g_worldDrawn = false;
// The viewport depth range last set; negative forces the next SetDepthRange through, as after
// a reset, which puts the viewport back to the device's default.
float g_depthMin = -1.0f;
float g_depthMax = -1.0f;

// Used until the engine sends its own background colour, and whenever the ini says to
// ignore it. Distinctive on purpose, so a black frame reads as "nothing ran".
const D3DCOLOR kFallbackClearColour = D3DCOLOR_XRGB(0x20, 0x30, 0x60);
D3DCOLOR g_clearColour = kFallbackClearColour;

// Where in the window the back buffer goes. With [Display] KeepAspect the back buffer is the
// largest 4:3 rectangle the client area holds -- the engine's picture is always 640x480 --
// and Present places it centred, leaving bars; otherwise it is the whole client area.
RECT g_presentRect;
bool g_presentToRect = false;

void SizeToWindow(HWND window, UINT* width, UINT* height) {
    RECT client = {0, 0, 640, 480};
    if (window) {
        GetClientRect(window, &client);
    }
    int w = client.right - client.left;
    int h = client.bottom - client.top;
    if (w <= 0 || h <= 0) {
        w = 640;
        h = 480;
    }
    g_presentRect.left = 0;
    g_presentRect.top = 0;
    g_presentRect.right = w;
    g_presentRect.bottom = h;
    g_presentToRect = false;
    if (Config::KeepAspect() && w * 3 != h * 4) {
        const int fitW = (w * 3 > h * 4) ? h * 4 / 3 : w;
        const int fitH = (w * 3 > h * 4) ? h : w * 3 / 4;
        g_presentRect.left = (w - fitW) / 2;
        g_presentRect.top = (h - fitH) / 2;
        g_presentRect.right = g_presentRect.left + fitW;
        g_presentRect.bottom = g_presentRect.top + fitH;
        g_presentToRect = true;
        w = fitW;
        h = fitH;
    }
    *width = (UINT)w;
    *height = (UINT)h;
}

void FillPresentParams(HWND window) {
    ZeroMemory(&g_params, sizeof(g_params));
    g_params.Windowed = TRUE;
    g_params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_params.hDeviceWindow = window;
    SizeToWindow(window, &g_params.BackBufferWidth, &g_params.BackBufferHeight);
    g_params.BackBufferFormat = D3DFMT_X8R8G8B8;
    g_params.BackBufferCount = 1;
    g_params.EnableAutoDepthStencil = TRUE;
    g_params.AutoDepthStencilFormat = D3DFMT_D24S8;
    g_params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

// Load the d3d9.dll that sits next to the game executable, in preference to whatever module
// named "d3d9.dll" is already in the process.
//
// RTX Remix ships exactly that way: a d3d9.dll dropped into the game folder. A static import
// cannot reach it. By the time the engine loads this plugin, its DirectDraw startup has
// already pulled C:\Windows\System32\d3d9.dll into the process, and the loader satisfies an
// import by *module name* from what is already resident -- the application-directory search
// that would have found Remix never runs, and every call goes to the stock runtime instead.
//
// A path-qualified LoadLibrary is keyed on the file rather than the name, so it loads the
// local copy even with a same-named module already loaded.
HMODULE LoadSystemD3D9() {
    char path[MAX_PATH];
    const UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n + sizeof("\\d3d9.dll") > MAX_PATH) {
        return 0;
    }
    strcat(path, "\\d3d9.dll");
    HMODULE sys = LoadLibraryA(path);
    LogLine("    d3d9 plain (lookdev): %s%s", path, sys ? "" : " -- failed to load");
    return sys;
}

HMODULE LoadD3D9() {
    // Path-qualified, because a bare "d3d9.dll" would find Remix in the application folder.
    if (Lookdev::UsePlainD3D9()) {
        return LoadSystemD3D9();
    }
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(0, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char* slash = strrchr(path, '\\');
        if (slash && (size_t)(slash - path) + sizeof("\\d3d9.dll") < MAX_PATH) {
            strcpy(slash + 1, "d3d9.dll");
            HMODULE local = LoadLibraryA(path);
            if (local) {
                LogLine("    d3d9 from game folder: %s", path);
                return local;
            }
            LogLine("    no d3d9.dll beside the executable (%s), err=%lu -- falling back to the "
                    "system runtime; RTX Remix will not see this process",
                    path, (unsigned long)GetLastError());
        }
    }
    HMODULE sys = LoadLibraryA("d3d9.dll");
    if (sys) {
        char actual[MAX_PATH];
        if (GetModuleFileNameA(sys, actual, MAX_PATH)) {
            LogLine("    d3d9 fallback: %s", actual);
        }
    }
    return sys;
}

// Geometry is streamed through one dynamic vertex/index buffer pair rather than drawn with
// DrawPrimitiveUP: RTX Remix rasterises sky draws into its sky layer from the buffers bound
// to the draw, and a UP draw's temporary buffers are gone by the time it does. Each draw
// appends behind the previous one (NOOVERWRITE) and the ring restarts with DISCARD when full.
const UINT kStreamVertexBytes = 1u << 20;
const UINT kStreamIndexBytes = 1u << 18;
IDirect3DVertexBuffer9* g_streamVertices = 0;
IDirect3DIndexBuffer9* g_streamIndices = 0;
UINT g_streamVertexEnd = 0;
UINT g_streamIndexEnd = 0;

void ReleaseStream() {
    if (g_streamVertices) {
        g_streamVertices->Release();
        g_streamVertices = 0;
    }
    if (g_streamIndices) {
        g_streamIndices->Release();
        g_streamIndices = 0;
    }
}

bool EnsureStream() {
    if (g_streamVertices && g_streamIndices) {
        return true;
    }
    ReleaseStream();
    const DWORD usage = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
    HRESULT hr = g_device->CreateVertexBuffer(kStreamVertexBytes, usage, 0, D3DPOOL_DEFAULT,
                                              &g_streamVertices, 0);
    if (SUCCEEDED(hr)) {
        hr = g_device->CreateIndexBuffer(kStreamIndexBytes, usage, D3DFMT_INDEX16,
                                         D3DPOOL_DEFAULT, &g_streamIndices, 0);
    }
    if (FAILED(hr)) {
        LogLine("    stream buffers could not be created hr=0x%08lx", (unsigned long)hr);
        ReleaseStream();
        return false;
    }
    g_streamVertexEnd = kStreamVertexBytes;  // the first draw discards
    g_streamIndexEnd = kStreamIndexBytes;
    return true;
}

// Reserves `bytes` in a ring at a multiple of `align`, returning the offset and the lock flag.
UINT ReserveInRing(UINT* end, UINT capacity, UINT bytes, UINT align, DWORD* lockFlags) {
    UINT offset = (*end + align - 1) / align * align;
    *lockFlags = D3DLOCK_NOOVERWRITE;
    if (offset + bytes > capacity) {
        offset = 0;
        *lockFlags = D3DLOCK_DISCARD;
    }
    *end = offset + bytes;
    return offset;
}

bool Reset() {
    g_inScene = false;
    ReleaseStream();
    HRESULT hr = g_device->Reset(&g_params);
    if (FAILED(hr)) {
        return false;
    }
    g_needsReset = false;
    g_depthMin = g_depthMax = -1.0f;
    LogLine("    device reset ok (%dx%d)", g_params.BackBufferWidth, g_params.BackBufferHeight);
    return true;
}

} // namespace

bool Device::Create(HWND window) {
    if (g_device) {
        LogLine("    Create called with a device already live -- ignoring");
        return true;
    }

    if (!g_d3d9Module) {
        g_d3d9Module = LoadD3D9();
    }
    if (!g_d3d9Module) {
        LogLine("    FATAL: no d3d9.dll could be loaded");
        return false;
    }
    PFN_Direct3DCreate9 createD3D9 =
        (PFN_Direct3DCreate9)GetProcAddress(g_d3d9Module, "Direct3DCreate9");
    if (!createD3D9) {
        LogLine("    FATAL: d3d9.dll has no Direct3DCreate9");
        return false;
    }

    g_d3d = createD3D9(D3D_SDK_VERSION);
    if (!g_d3d) {
        LogLine("    FATAL: Direct3DCreate9 failed");
        return false;
    }

    FillPresentParams(window);
    const int width = (int)g_params.BackBufferWidth;
    const int height = (int)g_params.BackBufferHeight;
    LogLine("    window=%p back buffer %dx%d at %ld,%ld", window, width, height,
            g_presentRect.left, g_presentRect.top);

    // SOFTWARE_VERTEXPROCESSING keeps device creation working on any driver.
    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                     &g_params, &g_device);
    if (FAILED(hr)) {
        LogLine("    FATAL: CreateDevice failed hr=0x%08lx", (unsigned long)hr);
        g_d3d->Release();
        g_d3d = 0;
        return false;
    }

    g_window = window;
    LogLine("    D3D9 device created %dx%d", width, height);
    return true;
}

void Device::Destroy() {
    if (g_device) {
        if (g_inScene) {
            g_device->EndScene();
            g_inScene = false;
        }
        ReleaseStream();
        g_device->Release();
        g_device = 0;
    }
    if (g_d3d) {
        g_d3d->Release();
        g_d3d = 0;
    }
    g_window = 0;
    LogLine("    D3D9 device destroyed");
}

void Device::PreloadD3D9() {
    if (!g_d3d9Module) {
        g_d3d9Module = LoadD3D9();
    }
}

bool Device::FrameHasDrawn() {
    return g_frameHadDraw;
}

void Device::ClaimFirstDraw() {
    g_frameHadDraw = true;
}

void Device::NoteWorldDraw() {
    g_worldDrawn = true;
}

bool Device::WorldHasDrawn() {
    return g_worldDrawn;
}

void Device::SetDepthRange(float minZ, float maxZ) {
    if (!g_device || (minZ == g_depthMin && maxZ == g_depthMax)) {
        return;
    }
    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = g_params.BackBufferWidth;
    vp.Height = g_params.BackBufferHeight;
    vp.MinZ = minZ;
    vp.MaxZ = maxZ;
    if (SUCCEEDED(g_device->SetViewport(&vp))) {
        g_depthMin = minZ;
        g_depthMax = maxZ;
    }
}

bool Device::DrawTriangles(const void* vertices, unsigned int stride, unsigned int vertexCount,
                           const unsigned short* indices, unsigned int indexCount) {
    if (!g_device || stride == 0 || indexCount < 3 || !EnsureStream()) {
        return false;
    }
    const UINT vertexBytes = stride * vertexCount;
    const UINT indexBytes = indexCount * sizeof(unsigned short);
    if (vertexBytes > kStreamVertexBytes || indexBytes > kStreamIndexBytes) {
        LogLine("    draw of %u vertices / %u indices exceeds the stream buffers",
                vertexCount, indexCount);
        return false;
    }

    DWORD vertexLock = 0;
    DWORD indexLock = 0;
    const UINT vertexOffset = ReserveInRing(&g_streamVertexEnd, kStreamVertexBytes, vertexBytes,
                                            stride, &vertexLock);
    const UINT indexOffset = ReserveInRing(&g_streamIndexEnd, kStreamIndexBytes, indexBytes,
                                           sizeof(unsigned short), &indexLock);
    void* dst = 0;
    if (FAILED(g_streamVertices->Lock(vertexOffset, vertexBytes, &dst, vertexLock))) {
        return false;
    }
    memcpy(dst, vertices, vertexBytes);
    g_streamVertices->Unlock();
    if (FAILED(g_streamIndices->Lock(indexOffset, indexBytes, &dst, indexLock))) {
        return false;
    }
    memcpy(dst, indices, indexBytes);
    g_streamIndices->Unlock();

    g_device->SetStreamSource(0, g_streamVertices, 0, stride);
    g_device->SetIndices(g_streamIndices);
    const HRESULT hr = g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                                                      (INT)(vertexOffset / stride), 0, vertexCount,
                                                      indexOffset / sizeof(unsigned short),
                                                      indexCount / 3);
    if (FAILED(hr)) {
        LogLine("    DrawIndexedPrimitive failed hr=0x%08lx", (unsigned long)hr);
        return false;
    }
    return true;
}

bool Device::ClaimPipeline(Pipeline pipeline) {
    if (g_pipeline == pipeline) {
        return false;
    }
    g_pipeline = pipeline;
    return true;
}

IDirect3DDevice9* Device::Get() {
    return g_device;
}

bool Device::IsReady() {
    return g_device != 0;
}

bool Device::BeginFrame() {
    if (!g_device) {
        return false;
    }

    if (g_needsReset && !Reset()) {
        return false;
    }

    HRESULT hr = g_device->TestCooperativeLevel();
    if (hr == D3DERR_DEVICELOST) {
        return false;
    }
    if (hr == D3DERR_DEVICENOTRESET && !Reset()) {
        return false;
    }

    // A fresh frame inherits nothing: whichever pipeline draws first installs its state.
    g_pipeline = PipelineNone;
    g_frameHadDraw = false;
    g_worldDrawn = false;
    g_device->Clear(0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, g_clearColour, 1.0f, 0);
    if (FAILED(g_device->BeginScene())) {
        return false;
    }
    g_inScene = true;
    return true;
}

namespace {

char g_screenshotPath[MAX_PATH];

bool WriteScreenshot(const char* path) {
    if (!g_device || !path) {
        return false;
    }
    IDirect3DSurface9* back = 0;
    if (FAILED(g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) {
        LogLine("    screenshot: no back buffer");
        return false;
    }
    D3DSURFACE_DESC desc;
    back->GetDesc(&desc);

    IDirect3DSurface9* sys = 0;
    HRESULT hr = g_device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                       D3DPOOL_SYSTEMMEM, &sys, 0);
    if (FAILED(hr) || !sys) {
        LogLine("    screenshot: CreateOffscreenPlainSurface failed hr=0x%08lx",
                (unsigned long)hr);
        back->Release();
        return false;
    }
    hr = g_device->GetRenderTargetData(back, sys);
    back->Release();
    if (FAILED(hr)) {
        LogLine("    screenshot: GetRenderTargetData failed hr=0x%08lx", (unsigned long)hr);
        sys->Release();
        return false;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(sys->LockRect(&lr, 0, D3DLOCK_READONLY))) {
        sys->Release();
        return false;
    }

    // 24-bit BMP: no dependencies, and every viewer and converter reads it.
    const int w = (int)desc.Width;
    const int h = (int)desc.Height;
    const int rowBytes = (w * 3 + 3) & ~3;
    const unsigned int pixelBytes = (unsigned int)rowBytes * (unsigned int)h;
    unsigned char header[54];
    memset(header, 0, sizeof(header));
    header[0] = 'B'; header[1] = 'M';
    const unsigned int fileSize = 54 + pixelBytes;
    memcpy(header + 2, &fileSize, 4);
    const unsigned int offset = 54;
    memcpy(header + 10, &offset, 4);
    const unsigned int infoSize = 40;
    memcpy(header + 14, &infoSize, 4);
    memcpy(header + 18, &w, 4);
    memcpy(header + 22, &h, 4);
    const unsigned short planes = 1, bits = 24;
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &bits, 2);
    memcpy(header + 34, &pixelBytes, 4);

    FILE* f = fopen(path, "wb");
    if (!f) {
        sys->UnlockRect();
        sys->Release();
        return false;
    }
    fwrite(header, 1, sizeof(header), f);
    unsigned char* row = (unsigned char*)malloc(rowBytes);
    if (row) {
        memset(row, 0, rowBytes);
        // BMP rows run bottom-up.
        for (int y = h - 1; y >= 0; --y) {
            const unsigned char* src = (const unsigned char*)lr.pBits + (size_t)y * lr.Pitch;
            for (int x = 0; x < w; ++x) {
                row[x * 3 + 0] = src[x * 4 + 0];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + 2];
            }
            fwrite(row, 1, rowBytes, f);
        }
        free(row);
    }
    fclose(f);
    sys->UnlockRect();
    sys->Release();
    LogLine("    screenshot: %dx%d -> %s", w, h, path);
    return true;
}

} // namespace

void Device::RequestScreenshot(const char* path) {
    lstrcpynA(g_screenshotPath, path ? path : "", MAX_PATH);
}

void Device::EndFrame() {
    if (!g_device || !g_inScene) {
        // A screenshot names the frame it was asked for, so a request that outlives a
        // skipped frame would write the wrong frame's picture under that name.
        g_screenshotPath[0] = 0;
        return;
    }
    g_device->EndScene();
    g_inScene = false;

    if (g_screenshotPath[0]) {
        WriteScreenshot(g_screenshotPath);
        g_screenshotPath[0] = 0;
    }

    HRESULT hr = g_device->Present(0, g_presentToRect ? &g_presentRect : 0, 0, 0);
    if (hr == D3DERR_DEVICELOST) {
        g_needsReset = true;
    }
}

void Device::OnWindowResized(HWND window) {
    if (!g_device || window != g_window) {
        return;
    }
    UINT width = 0;
    UINT height = 0;
    SizeToWindow(window, &width, &height);
    if (width != g_params.BackBufferWidth || height != g_params.BackBufferHeight) {
        g_params.BackBufferWidth = width;
        g_params.BackBufferHeight = height;
        g_needsReset = true;
    }
}

void Device::SetClearColour(unsigned int packedAbgr) {
    if (!Config::UseEngineBackgroundColour()) {
        return;
    }
    // The original splits the operand into r, g, b, a from the low byte upwards.
    const unsigned int r = packedAbgr & 0xff;
    const unsigned int g = (packedAbgr >> 8) & 0xff;
    const unsigned int b = (packedAbgr >> 16) & 0xff;
    g_clearColour = D3DCOLOR_XRGB(r, g, b);
}

unsigned int Device::ClearColour() {
    return (unsigned int)g_clearColour;
}

void Device::SetVSync(bool enabled) {
    UINT interval = enabled ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    if (g_params.PresentationInterval == interval) {
        return;
    }
    g_params.PresentationInterval = interval;
    if (g_device) {
        g_needsReset = true;
    }
}

int Device::Width() {
    return (int)g_params.BackBufferWidth;
}

int Device::Height() {
    return (int)g_params.BackBufferHeight;
}
