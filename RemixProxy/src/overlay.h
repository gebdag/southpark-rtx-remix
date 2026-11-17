#pragma once

#include <windows.h>
#include <d3d9.h>

// A small immediate-mode overlay, toggled with F4. It draws pre-transformed triangles with one
// texture -- a monospaced font atlas with a solid cell -- which RTX Remix composites as UI
// (rtx.preTransformedVerticesIsUI). While it is open it takes the mouse and keyboard, and tells
// the input shim (dinput.dll) so the game stops reading its devices.
namespace Overlay {

// Takes over the window's input. Call once the device exists.
void Attach(HWND window);
void SetBackBufferSize(int width, int height);
// Present's destination rectangle: where the back buffer lands in the window, for the mouse.
void OnPresent(const RECT* destination);
// Draws this frame's overlay; call just before the device's EndScene.
void Render(IDirect3DDevice9* device);
bool IsOpen();

} // namespace Overlay

// The drawing and widget kit the overlay's content is built with. Coordinates are back buffer
// pixels. Widgets are only valid while the panel is being built (Overlay::Render calls the
// content functions in lights.cpp at the right time).
namespace Ui {

float Scale();          // 1 at 1080 lines, 2 at 2160
float LineHeight();
float CharWidth();

void Rect(float x0, float y0, float x1, float y1, D3DCOLOR colour);
void Ring(float cx, float cy, float radius, float thickness, D3DCOLOR colour);
void Text(float x, float y, D3DCOLOR colour, const char* text);

void Heading(const char* text);
void Label(const char* fmt, ...);
void LabelDim(const char* fmt, ...);
bool Checkbox(const char* label, bool* value);
bool Button(const char* label);
bool Slider(const char* label, float* value, float minimum, float maximum, const char* format,
            bool logarithmic = false);
void Swatch(D3DCOLOR colour);
// The next widget continues on the current row instead of starting a new one.
void SameLine();

} // namespace Ui
