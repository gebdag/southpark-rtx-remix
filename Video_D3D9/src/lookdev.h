#pragma once

#include <windows.h>

// Lookdev: record frames from the running game at the plugin boundary, so the lookdev host
// (patches\SouthPark\lookdev) can replay them through any GFXDLL plugin -- this renderer,
// into RTX Remix or plain D3D9, or one of the game's original renderers as a reference.
//
// A frame is fully described by what crosses the GFXDLL boundary: the display lists, the
// engine memory they point at (matrices, vertex blocks, sub-lists, skip masks, the 0x03
// viewport/texgen/light blocks), the texture sets enumerated at load, the arguments of the
// start-up exports, and the last command of every state opcode seen before the capture. The
// capture records exactly that, and the host puts the memory back at the addresses the
// engine had and replays the state commands ahead of the first frame.
//
// The mode comes from the environment, set by whoever starts the process, so a normal game
// start is untouched:
//
//   SP_LOOKDEV=1        the game, as a tee: Acclaim's original renderer draws the game window
//                       (the ground truth, see original.h), this renderer draws the same
//                       frames through RTX Remix into a second window, and F9 or a request
//                       file captures
//   SP_LOOKDEV_HOST=1   the lookdev host: no SouthPark.exe to patch
//   SP_LOOKDEV_PLAIN=1  the host: use the system d3d9.dll instead of the game folder's
//                       (RTX Remix)
namespace Lookdev {

// Reads the environment. Called first thing in DllMain, before the log is opened, because
// the host writes its own log rather than the game's.
void DetectMode();

bool IsHost();
bool IsTee();
bool UsePlainD3D9();

// Name of the log beside the DLL for this mode.
const char* LogFileName();

// ----------------------------------------------------------- start-up exports
//
// Remembered from the moment the engine makes them, since a capture is taken long after.

void OnInitializeDriver(HWND window, const unsigned int args[7]);   // arguments 2..8
void OnSetTextureFileName(const char* name);
void OnStartTextureEnumeration(unsigned int count, unsigned int arg3);
void OnEnumerateTexture(unsigned int arg2);

// ------------------------------------------------------------------- the Remix window

// In tee mode: the window this renderer draws into, beside the game window. Sized and placed
// by [Lookdev] RemixWindow* in the ini; movable and resizable like any window.
HWND CreateRemixWindow();
void DestroyRemixWindow();

// Handles its messages. The game's own loop only pumps the game window's.
void PumpRemixWindow();

// ------------------------------------------------------------------- frames

// True while a frame is being captured. Read on every memory access the interpreter makes,
// so it is a plain flag rather than a call.
extern bool g_recording;

void OnBeginScene();
void OnEndScene();
void OnSetZPlanes(float zNear, float zFar);
void OnFlushTextureCache();

// Bracket one top-level GFXDLL_ProcessDisplayList call. Memory noted in between is copied
// when the call returns, which is when the engine may next rewrite it.
void BeginList(const void* list);
void EndList();

// True in the game's lookdev mode: the interpreter reports every command it walks, so a
// capture can start from the state the engine left behind.
extern bool g_trackState;

void TrackCommand(unsigned int word0, unsigned int word1);

inline void NoteCommand(unsigned int word0, unsigned int word1) {
    if (g_trackState) {
        TrackCommand(word0, word1);
    }
}

void RecordRead(const void* address, unsigned int bytes);

// The interpreter read `bytes` of engine memory at `address`.
inline void NoteRead(const void* address, unsigned int bytes) {
    if (g_recording) {
        RecordRead(address, bytes);
    }
}

} // namespace Lookdev
