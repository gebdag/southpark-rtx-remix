#pragma once

#include <windows.h>

// Owns the D3D9 device for the whole plugin. The engine creates the window and hands
// it to GFXDLL_InitializeDriver, so the plugin only ever attaches a swapchain to an
// existing HWND -- it never creates or owns one.
struct IDirect3DDevice9;

namespace Device {

bool Create(HWND window);

// Bring the D3D9 runtime into the process without creating a device yet.
//
// This exists for RTX Remix. The Remix bridge installs its DirectInput and window-procedure
// detours when its d3d9.dll loads, and can only drive its own menu with input it detoured
// *before* the engine took it. Loading at device-creation time is far too late: the engine
// has already registered its window class and taken the mouse, so the bridge logs
// "No winproc detected" and its menu never gets a usable cursor. Calling this from the very
// first export the engine invokes is the earliest point the plugin controls.
//
// Safe to call repeatedly; only the first call loads anything. Deliberately not called from
// DllMain -- LoadLibrary under the loader lock risks deadlock.
void PreloadD3D9();

// World geometry and the 2D layer want incompatible device state -- different vertex
// formats, depth behaviour and texture stages -- and the engine interleaves them freely
// within a frame. Each claims the device before drawing; a true return means the other
// pipeline had it and the caller must install its own state again. This keeps each path
// owning its state instead of depending on what frame setup happened to leave behind.
enum Pipeline {
    PipelineNone,
    PipelineWorld,
    Pipeline2D,
};
bool ClaimPipeline(Pipeline pipeline);

// Null until Create succeeds. Subsystems that own D3D resources must tolerate this.
IDirect3DDevice9* Get();
void Destroy();
bool IsReady();

// Clear + BeginScene. Returns false if the device is not usable this frame.
bool BeginFrame();

// EndScene + Present. Handles a lost device by flagging it for reset next frame.
void EndFrame();

void SetVSync(bool enabled);

// Background colour for the frame clear, as the engine supplies it through display-list
// opcode 0xf8. Packed 0xAABBGGRR, matching the byte order the original unpacks.
void SetClearColour(unsigned int packedAbgr);

int Width();
int Height();

} // namespace Device
