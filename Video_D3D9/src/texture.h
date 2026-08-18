#pragma once

// Texture sets are blobs the engine hands to GFXDLL_EnumerateTexture during startup, each in
// one reused staging buffer, so every set is copied as it arrives. The engine later asks for
// them back by index through GFXDLL_GetTextureSetPtr and dereferences the result, so it gets
// the stored copy.
//
// The blob starts with a chunk count and a table of chunk offsets at +4; chunk 0 carries the
// width and height as single bytes at +2 and +3.
struct IDirect3DTexture9;

namespace Texture {

void StartEnumeration();
void Enumerate(void* blob);
void FinishEnumeration();

// Returns the stored blob and fills its dimensions. Null for an unknown index.
void* GetSet(int index, int* outWidth, int* outHeight);

// Decodes the set's palettised image into a D3D9 texture on first use and caches it.
// Null when the set is unknown or its payload is not decodable.
IDirect3DTexture9* GetTexture(int index);

int Count();

void ReleaseAll();

// Writes a whole set blob to disk for offline analysis.
void DumpSet(int index, const char* path);

} // namespace Texture
