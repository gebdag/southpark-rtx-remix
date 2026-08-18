#pragma once

// Runtime options, read once from Video_D3D9.ini beside the DLL.
//
// These exist because the right answer differs between plain rasterisation and RTX Remix.
// A path tracer wants geometry present and double-sided so light transport can find it,
// where the engine assumes a rasteriser that discards back faces. Rather than pick one, the
// behaviour is switchable and the file documents itself.
namespace Config {

// Reads the ini next to `self`, writing a commented default file if none exists.
void Load(void* self);

// --------------------------------------------------------------------------- culling

bool DisableBackfaceCulling();

// Ignore the per-triangle skip bitmask installed by opcodes 0xa8/0xa7.
bool DisableTriangleSkipMask();

// Draw 2D quads even when a corner leaves the screen. The original drops the whole quad.
bool DisableOffscreenQuadReject();

// ------------------------------------------------------------------------------- sky

// Clear to the colour the engine supplies through display-list opcode 0xf8 rather than to
// a fixed colour.
bool UseEngineBackgroundColour();

// Draw sky-textured batches with no depth test and no depth write. That is how a skydome
// should be drawn in any case, and it is the signal Remix's sky auto-detection looks for.
bool DepthlessSky();

// Comma-separated texture-set names treated as sky, matched against the name in chunk 3.
const char* SkyTextureNames();

// ------------------------------------------------------------------------------- log

// Log every engine call. Off by default: the 2D layer alone makes ~1.6M calls a run, which
// turns the log into gigabytes and throttles the frame rate.
bool LogEveryCall();

} // namespace Config
