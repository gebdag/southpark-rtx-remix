#pragma once

#include <windows.h>

// Owns the D3D9 device for the whole plugin. The engine creates the window and hands
// it to GFXDLL_InitializeDriver, so the plugin only ever attaches a swapchain to an
// existing HWND -- it never creates or owns one.
struct IDirect3DDevice9;

namespace Device {

bool Create(HWND window);

// Null until Create succeeds. Subsystems that own D3D resources must tolerate this.
IDirect3DDevice9* Get();
void Destroy();
bool IsReady();

// Clear + BeginScene. Returns false if the device is not usable this frame.
bool BeginFrame();

// EndScene + Present. Handles a lost device by flagging it for reset next frame.
void EndFrame();

void SetVSync(bool enabled);

int Width();
int Height();

} // namespace Device
