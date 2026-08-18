#include "device.h"
#include "log.h"

#include <d3d9.h>
#include <string.h>

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

// The stub clears to a distinctive colour so a black frame reads as "nothing ran"
// rather than "cleared to black", which is the single most useful bring-up signal.
const D3DCOLOR kClearColour = D3DCOLOR_XRGB(0x20, 0x30, 0x60);

void FillPresentParams(HWND window, int width, int height) {
    ZeroMemory(&g_params, sizeof(g_params));
    g_params.Windowed = TRUE;
    g_params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_params.hDeviceWindow = window;
    g_params.BackBufferWidth = width;
    g_params.BackBufferHeight = height;
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
HMODULE LoadD3D9() {
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

bool Reset() {
    g_inScene = false;
    HRESULT hr = g_device->Reset(&g_params);
    if (FAILED(hr)) {
        return false;
    }
    g_needsReset = false;
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

    RECT client;
    int width = 640;
    int height = 480;
    if (window && GetClientRect(window, &client)) {
        if (client.right > client.left && client.bottom > client.top) {
            width = client.right - client.left;
            height = client.bottom - client.top;
        }
    }
    LogLine("    window=%p client=%dx%d", window, width, height);

    FillPresentParams(window, width, height);

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
    g_device->Clear(0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClearColour, 1.0f, 0);
    if (FAILED(g_device->BeginScene())) {
        return false;
    }
    g_inScene = true;
    return true;
}

void Device::EndFrame() {
    if (!g_device || !g_inScene) {
        return;
    }
    g_device->EndScene();
    g_inScene = false;

    HRESULT hr = g_device->Present(0, 0, 0, 0);
    if (hr == D3DERR_DEVICELOST) {
        g_needsReset = true;
    }
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
