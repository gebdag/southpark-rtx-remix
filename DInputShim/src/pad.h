#pragma once

#include <windows.h>

// Gamepads for South Park: one virtual DirectInput joystick, fed by whatever pad is connected.
//
// The game reads a DirectInput joystick itself, but only its X, Y and Z axes, one hat and up
// to 16 buttons, and every pad reports a different layout through DirectInput -- Xbox pads put
// both triggers on one axis. So the game is shown one joystick of our own, always present, and
// that joystick is filled from an XInput pad (Xbox) or, failing that, a DirectInput pad
// (DualSense, DualShock 4, others), mapped to one layout:
//
//   right stick X  turn          left stick Y   walk
//   left stick X   strafe        right stick Y  look
//   RT / X         fire          LT / B         secondary fire
//   A              jump          Y              taunt
//   RB / LB        next / previous weapon       R3  center view
//   D-pad          look up/down, strafe          Start  Esc (pause, back)
//
// Turning and looking go through the N64 game's own stick handling, which is still in the
// executable: turning a rate, looking a position that springs back to level. In menus the
// D-pad and left stick step one item per press, A confirms, B goes back. Small patches to the
// executable make all of that reachable (pad.cpp, "executable patches").
namespace Pad {

typedef bool (*BlockedFn)();
typedef void (*LogFn)(const char* format, ...);
typedef HRESULT (WINAPI* DirectInputCreateAFn)(HINSTANCE, DWORD, void**, void*);

// Once, when the game creates DirectInput: patches the executable and remembers how to read
// pads. `inputBlocked` says when the game must see a centred pad (menus, focus elsewhere).
// `iniPath` holds [Input] PadTurnSensitivity and PadLookSensitivity, re-read whenever the file
// changes.
void Install(DirectInputCreateAFn realCreate, BlockedFn inputBlocked, LogFn log, const char* iniPath);
bool Installed();

// [Input] PadTurnSensitivity or PadLookSensitivity, set without touching the file: the F4
// overlay applies its changes this way and writes the ini only when asked to save. False for
// any other key.
bool SetSetting(const char* section, const char* key, float value);

// True for the factory and device the pad layer created for its own reading. They share
// dinput's vtables with the game's, so the shim's hooks see them too and must leave them
// alone: the focus and menu rules are for the game's devices.
bool IsOwnFactory(void* factory);
bool IsOwnDevice(void* device);

// Joystick enumeration: offers the virtual joystick in place of the real ones.
void EnumerateJoysticks(void* callback, void* context);
bool IsVirtualJoystick(const GUID* guid);
HRESULT CreateVirtualJoystick(void** out);

// The mouse moved: mouse look takes the view back from the pad's right stick.
void OnMouseMoved();

// Start is Esc. In play the game takes Esc from its buffered DirectInput keyboard; in its
// menus from window messages. A real key produces both, and so does Start: this appends the
// Esc events to a buffered keyboard read (`buffer` holds `*count` records of `recordSize` bytes,
// room for `capacity`), and the key messages are posted to the game window as Start changes.
void AddKeyEvents(void* buffer, DWORD recordSize, DWORD* count, DWORD capacity);

} // namespace Pad
