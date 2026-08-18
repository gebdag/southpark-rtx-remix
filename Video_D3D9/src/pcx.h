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

// UVs are scaled when a texture had to be padded to power-of-two dimensions.
float UScale(int handle);
float VScale(int handle);

void ReleaseAll();

} // namespace Pcx
