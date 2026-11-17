#pragma once

// The game's mouse, read with Windows raw input instead of DirectInput.
//
// DirectInput 5 feeds a mouse from a low-level mouse hook (WH_MOUSE_LL) on its own thread.
// Windows removes such a hook without telling anyone when one of its calls runs past the
// system's hook timeout, and DirectInput keeps the device acquired: from then on every read
// succeeds and reports no movement and no buttons until the device is acquired afresh. Raw
// input is delivered as window messages, so nothing can be dropped that way. The reader runs on
// a thread of its own with a hidden window registered for the mouse (RIDEV_INPUTSINK), adds up
// the movement, and hands it to the game's mouse reads.
namespace RawMouse {

typedef void (*LogFn)(const char* fmt, ...);

// Starts the reader thread, which registers for raw mouse input and logs the outcome through
// `log`. Returns at once: the thread may only get to run once the caller's loader work is
// done, so the game's reads ask Running() each time. Safe to call more than once.
void Start(LogFn log);

// True once the reader has registered for raw input.
bool Running();

// Fills a DIMOUSESTATE (16 bytes) or DIMOUSESTATE2 (20 bytes) with the movement and wheel since
// the last call and the buttons held now, and starts the next interval at zero. Returns false
// for any other size, leaving `data` untouched.
bool Read(void* data, unsigned long size);

// Drops the movement gathered so far: while the game is not reading input, the mouse belongs
// to someone else, and none of it should reach the game later.
void Discard();

// Mouse events received since the reader started.
long Events();

} // namespace RawMouse
