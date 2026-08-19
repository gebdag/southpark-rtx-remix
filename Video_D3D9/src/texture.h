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

// Decodes one frame of a set into a D3D9 texture on first use and caches it.
//
// `subA` and `subB` are the two 16-bit fields of display-list opcode 0xa9's second word.
// They select which sub-chunk of the image and of the palette to use -- this is how the
// engine animates a texture (blinking eyes, flowing water) without re-uploading anything.
// Which field drives the image depends on the format, so both are passed through and the
// decision is made where the format byte is already in hand. Null when the set is unknown
// or its payload is not decodable.
IDirect3DTexture9* GetTexture(int index, int subA, int subB);

// One-shot census of every enumerated set: format, size, sub-chunk counts and name.
void LogSetCensus();

int Count();

// Mean colour of the decoded texels of a set's first cached frame, as 0x00RRGGBB, and -1 when
// nothing of it has been decoded. This is the texture as the renderer actually built it,
// before any tint -- the only way to tell "the palette decoded wrong" from "the right texture
// was tinted the wrong colour".
int MeanColour(int index);

// The ASCII name stored in chunk 3, or "" when the set has none. Empty for an unknown index.
const char* Name(int index);

// One line describing what a draw actually bound: name, format, dimensions, which frame and
// palette were chosen, and how much of the decoded image is transparent. That last number is
// the one that says whether a wall you can see through is see-through because of its texture.
void Describe(int index, int subA, int subB, char* out, int capacity);

// Format byte from chunk 0, or -1 for an unknown set. Format 3 is the only one whose palette
// carries graduated alpha.
int Format(int index);

// Whether a set was tried and could not be decoded. A set that never decodes draws
// untextured, which is indistinguishable on screen from geometry that asked for no texture.
bool DecodeFailed(int index);

// Whether a set is one of the sky textures named in the ini. The sky is ordinary world
// geometry to the engine, so the only thing distinguishing it is the name in chunk 3.
bool IsSkySet(int index);

// Drops every decoded frame but keeps the blobs, so the next draw decodes afresh. The engine
// asks for this through GFXDLL_FlushTextureCache, which the original honours by jumping into
// its own cache code -- a stale decoded frame is an animation that stops moving.
void FlushDecoded();

void ReleaseAll();

// Writes a whole set blob to disk for offline analysis.
void DumpSet(int index, const char* path);

} // namespace Texture
