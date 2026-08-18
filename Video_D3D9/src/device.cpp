#include "device.h"
#include "log.h"

#include <d3d9.h>

namespace {

IDirect3D9* g_d3d = 0;
IDirect3DDevice9* g_device = 0;
HWND g_window = 0;
D3DPRESENT_PARAMETERS g_params;
bool g_inScene = false;
bool g_needsReset = false;

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

    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
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
