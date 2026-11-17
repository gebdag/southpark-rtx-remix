#pragma once

#include <windows.h>

// Acclaim's original Video_D3D.dll, running inside the game as the ground truth for lookdev.
//
// In lookdev mode (SP_LOOKDEV=1) this renderer is a tee: the engine's calls go to the original,
// which draws the game window as the game looked in 1999, while this renderer draws the same
// frames into a second window through RTX Remix (see exports.cpp).
//
// The original needs DirectDraw and a Direct3D 3 device, which Windows 11 no longer provides,
// so it runs on dgVoodoo2 from <game>\lookdev\ref\ (nothing goes into the game folder).
// SouthPark.exe imports the system ddraw.dll itself, so the original's own DirectDraw imports
// would bind to that one; Load() points them at dgVoodoo's instead.
namespace Original {

// Loads dgVoodoo2 and the original once. False, with the reason logged, when either is
// missing or the original's imports cannot be redirected.
bool Load();

// True once Load() has succeeded.
bool Active();

// One of the original's exports, by name; null if it has none.
FARPROC Export(const char* name);

// Where the original keeps its converted texture store, in place of the engine's
// "win32.tex" in the game folder.
const char* TextureStorePath();

} // namespace Original
