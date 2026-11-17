#pragma once

struct IDirect3DTexture9;

// The engine's 2D image subsystem. Despite the name, the plugin never parses PCX files:
// the engine owns the archive (Data.mrg) and the decoder, and hands the plugin a C++
// image object through callbacks registered before initialisation. The plugin's job is to
// keep a GPU texture in sync with the pixels the engine writes into that object.
//
// Handles are indices into a fixed table, matching the original's 2048-entry array.
namespace Pcx {

// Registered by GFXDLL_SetAllocatePCXImageFile / SetDeallocatePCXImageFile before init.
typedef void* (__cdecl *AllocFn)();
typedef void  (__cdecl *DeallocFn)(void* imageFile);

void SetAlloc(AllocFn fn);
void SetDealloc(DeallocFn fn);

// Loads by name, or bumps the reference count if already loaded. Returns -1 on failure.
int  Load(const char* name);

// Creates a blank image with no backing file, via vtable slot 3 of the engine's image
// object. The engine draws into it through RgbaPixels and expects a valid handle back --
// returning failure here gets dereferenced as if it were one.
int  Create(int width, int height);

void Unload(int handle);

int  Width(int handle);
int  Height(int handle);

// Pointers into the engine's own image object. The engine draws directly into the RGBA
// buffer and then calls Update to have the result re-uploaded.
void* RgbaPixels(int handle);
void* IndexedPixels(int handle);

void MarkDirty(int handle, int x0, int y0, int x1, int y1);
void Update(int handle);

IDirect3DTexture9* Texture(int handle);

// The engine's entire 2D layer -- HUD, menus, fonts, fades -- is this one call, made once
// per quad. Positions are 4 x {x, y, z} in the engine's 640x480 bottom-up screen space;
// colours are 4 x {r, g, b, a} in 0..1; uvs are 4 x {u, v}.
void DrawQuad(int handle, const float* positions, const float* colours, const float* uvs);

// Quads drawn before any world geometry wait until the frame says what they were. The world
// pipeline draws them as the frame's backdrop, at the far plane where RTX Remix takes them as
// sky, just before its first batch; a frame that ends without drawing any world was a 2D
// screen, and draws them as UI at its end.
void DrawPendingAsBackdrop();
void DrawPendingAsScreen();

// Blend mode for subsequent quads, as set by GFXDLL_SetBlendModePCX. Only mode 1 modulates
// by the vertex colour; the rest draw the texture flat, with or without blending.
void SetBlendMode(int mode);

void LogQuadStats();

// UVs are scaled when a texture had to be padded to power-of-two dimensions.
float UScale(int handle);
float VScale(int handle);

void ReleaseAll();

} // namespace Pcx
