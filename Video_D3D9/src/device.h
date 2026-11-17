#pragma once

#include <windows.h>

// Owns the D3D9 device for the whole plugin. The device draws into a window it is handed --
// the engine's, which window.h shapes, or lookdev's Remix window -- and sizes its back buffer
// from that window's client area.
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
    Pipeline2D,           // screen-space quads on top of the scene: the UI
    Pipeline2DBackdrop,   // screen-space quads behind it, drawn as sky
};
bool ClaimPipeline(Pipeline pipeline);

// The synthesised skybox covers the whole screen at the far plane, so it is only drawn while
// the frame is still empty; a 2D backdrop can legitimately go down first. Ask with
// FrameHasDrawn, and claim only once something has actually been drawn -- a claim spent on
// an attempt that then fails costs the frame its backdrop.
bool FrameHasDrawn();
void ClaimFirstDraw();

// Whether any world geometry has been drawn this frame. The engine's 2D layer means two
// different things depending on the answer: before the world it is a backdrop (the menu's
// night sky), after it the UI on top.
void NoteWorldDraw();
bool WorldHasDrawn();

// The viewport's depth range, over the whole back buffer. RTX Remix treats a draw whose
// viewport starts at depth 1 as sky (rtx.skyMinZThreshold, 1 by default) and rasterises it
// into its sky layer instead of path tracing it; the rasteriser draws it at the far plane,
// behind everything, however late in the frame it comes.
void SetDepthRange(float minZ, float maxZ);

// Draws an indexed triangle list with the vertex format the caller has set. Every draw goes
// through here: the geometry is streamed into device-owned buffers, never drawn "UP", because
// RTX Remix rasterises sky draws from the buffers bound to the draw after the draw itself.
bool DrawTriangles(const void* vertices, unsigned int stride, unsigned int vertexCount,
                   const unsigned short* indices, unsigned int indexCount);

// Null until Create succeeds. Subsystems that own D3D resources must tolerate this.
IDirect3DDevice9* Get();
void Destroy();
bool IsReady();

// Clear + BeginScene. Returns false if the device is not usable this frame.
bool BeginFrame();

// EndScene + Present. Handles a lost device by flagging it for reset next frame.
void EndFrame();

// The window `window` changed size. If it is the one the device draws into, the back buffer
// follows it at the start of the next frame. Ignored for any other window.
void OnWindowResized(HWND window);

void SetVSync(bool enabled);

// Background colour for the frame clear, as the engine supplies it through display-list
// opcode 0xf8. Packed 0xAABBGGRR, matching the byte order the original unpacks.
void SetClearColour(unsigned int packedAbgr);

// The colour the frame is being cleared to, as a D3DCOLOR. The synthesised skybox paints
// itself with this so the backdrop is one colour whether it is reached as cleared pixels or
// as geometry.
unsigned int ClearColour();

int Width();
int Height();

// Copies the back buffer to a 24-bit BMP. PrintWindow returns black for a D3D9 swapchain, so
// the renderer takes its own screenshots.
// The capture happens inside EndFrame, after EndScene and before Present, which is the only
// window where the finished frame is both complete and still on the render target.
void RequestScreenshot(const char* path);

} // namespace Device
