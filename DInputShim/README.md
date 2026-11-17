# DInputShim

`dinput.dll` for South Park (1999, PC), part of the RTX Remix mod. It sits between the game and
Windows' DirectInput and:

- lets the game share the mouse and keyboard with the RTX Remix menu and the desktop;
- reads the mouse through raw input and turns off the game's mouse smoothing, which loses all
  movement at high frame rates;
- keeps the Remix bridge's input drain from freezing the game;
- makes the process DPI aware before the game creates its window;
- adds gamepads: an XInput pad, or another pad through DirectInput (DualSense, DualShock 4 and
  others), shown to the game as one virtual joystick;
- plays the game's CD music from WAV files when they are present.

## Installation

Copy `dinput.dll` into `<game dir>`, next to `SouthPark.exe`. Delete it to return to stock
behaviour: nothing is written to `SouthPark.exe` on disk, all changes are made in memory.

`SouthPark.exe` imports `DINPUT.dll` statically, and `dinput.dll` is not a KnownDLL, so Windows
loads the copy in the game folder. The shim loads the real `dinput.dll` from the system directory
by absolute path and forwards every export to it. It has to be a `dinput.dll` proxy: the game
creates DirectInput and acquires its devices long before it loads a video plugin, so the renderer
cannot change any of it.

## Mouse and keyboard sharing

The game takes its mouse and keyboard in exclusive mode. That suppresses the Windows cursor for
the whole process, so the Remix menu has no pointer; the keyboard swallows Alt+Tab; and the game
keeps reading its devices while another window is in front.

The shim patches the COM vtables of the DirectInput objects the game gets back, rather than
wrapping them, so methods it does not change need no reimplementation. Each patched vtable keeps
its own copy of the original methods, because the mouse, the keyboard and later interface
versions each have a different vtable.

| Method | What the shim does |
|---|---|
| `IDirectInput::CreateDevice` | patches the new device; creates the virtual joystick (see Gamepads) |
| `IDirectInput::EnumDevices` | lists the virtual joystick instead of real joysticks |
| `SetCooperativeLevel` | turns any level into non-exclusive + foreground; keeps the game's level if the device rejects that |
| `Acquire` | reports success when the only problem is that another window is in front |
| `GetDeviceState` | returns an all-zero state while input is suspended; answers the game's mouse from raw input (see The mouse) |
| `GetDeviceData` | returns no events while input is suspended, and flushes the queue so nothing replays later |
| `QueryInterface` | patches the interface it returns (not `IID_IUnknown`, whose vtable is too short) |
| `Release` | forgets a destroyed device |

- **Suspended input.** While another process owns the foreground window, or while the Remix menu
  or the F4 overlay is open, the game sees nothing pressed and no movement.
- **Deferred acquire.** The game asks for background access to its keyboard. As a foreground
  device, `Acquire` fails while another window is in front, and the game would then skip the rest
  of its input setup. The shim reports success and acquires on the next read.
- **Reacquire on read.** Foreground devices are unacquired on focus loss. When a read returns
  `DIERR_INPUTLOST` or `DIERR_NOTACQUIRED` and the game has focus, the shim reacquires and reads
  again.
- **Rebinding.** The Remix bridge client also hooks DirectInput and can bind the game's devices to
  the window its D3D9 device presents to. If Remix presents to a window of its own, the devices
  then refuse to acquire while the game window is in front. The shim remembers the cooperative
  level each device took from the game, sets it again on the game window, and acquires.
- **Loss is never reported.** A device that still cannot be acquired reads as empty with `S_OK`.
  The engine answers a lost device by reacquiring in a loop that pumps no window messages, so it
  would never see the focus change that lets the acquire succeed.
- **Cursor.** While the game reads its devices, the pointer is hidden, as the game expects. While
  input is suspended or the game is not in front, the shim shows an arrow (window class cursor,
  `SetCursor`, balanced `ShowCursor`) and undoes all three when the game takes input back.
- **Executable imports.** In `DllMain` the shim rewrites three of the executable's import thunks:
  `ClipCursor` does nothing, so the pointer can leave the game window; `SetCursor(NULL)` shows an
  arrow while the pointer belongs to the desktop; `mouse_event` is dropped while input is
  suspended.

Not covered: objects created through COM (`DllGetClassObject` is a plain pass-through) and
`IDirectInput7::CreateDeviceEx`. South Park uses neither.

## The mouse

**Raw input.** DirectInput 5 feeds a non-exclusive mouse from a low-level mouse hook
(`WH_MOUSE_LL`) on its own thread. Windows removes such a hook without notice when one of its
calls runs past the system's hook timeout, and the device stays acquired: every read then succeeds
with no movement until the device is acquired again. So the shim reads the mouse itself with raw
input, on a thread of its own with a hidden window registered for the mouse (`RIDEV_INPUTSINK`),
and answers the game's mouse `GetDeviceState` (`DIMOUSESTATE` or `DIMOUSESTATE2`) with the
movement and wheel since the last read and the buttons held now. Every interface the game gets for
its mouse is read this way. Movement that arrives while input is suspended is dropped. Windows'
compatibility database applies `IgnoreAltTab` to `SouthPark.exe`, which intercepts
`RegisterRawInputDevices` for the whole process, so the shim calls the function from user32's
export table. If raw input cannot be registered, the mouse is read through DirectInput.

**Smoothing.** The game smooths each mouse read (`FUN_00476910`): an axis becomes the average
speed of the last N reads times the time since the previous read, timed with `GetTickCount`, which
advances in 15-16 ms steps. Above about 64 frames a second most reads see no time pass, and the
result is 0, so the view turns in jerks or not at all. N (`[0x4dc35c]`, 1, never written by the
game) is set to 0 when the game creates its mouse, which skips the smoothing. The site is checked
first; if it differs, smoothing stays on and the log says so.

## Remix menu freeze prevention

The Remix runtime tells the bridge client whether its menu is open by posting the registered
window message `UWM_REMIX_UIACTIVE_MSG` to the game window (bit 0 of `wParam` set means open). It
posts "closed" once at startup and again each time the menu closes. On "closed", the bridge client
drains the mouse it saw the game acquire: it calls `GetDeviceData` in a loop until no events are
left, passing the DirectInput 8 record size (20 bytes). The game's DirectInput 5 devices accept
only the 16-byte record and, for any other size, return `DI_OK` without touching the event count.
The loop never ends, and the game window stops responding.

The bridge only drains a device it has seen acquired and not since unacquired. So the shim
installs a thread-local `WH_GETMESSAGE` hook on the thread that owns the game's input window.
When that thread takes the Remix message off its queue, before it reaches the bridge's window
procedure, the shim unacquires every game device. The bridge finds nothing to drain, and the
game's next read reacquires the device. This works with any bridge build that drains this way,
costs nothing with one that does not, and is always on.

The same message tells the shim whether the Remix menu is open, so it also decides when input is
suspended for the menu. The RemixProxy F4 overlay posts its own registered message,
`UWM_SOUTHPARK_OVERLAY_ACTIVE`, the same way, and the shim suspends input while it is open.

## DPI awareness

In `DllMain` the shim calls `SetProcessDpiAwarenessContext` with per-monitor aware v2.

The engine sizes its window from the screen metrics before any renderer loads. A process that is
not DPI aware gets scaled metrics (for example 1920x1080 on a 3840x2160 monitor at 200%) and a
window that Windows stretches and blurs. The Remix bridge declares the process DPI aware later,
when its device is created, which would leave the window created under one set of coordinates and
managed under another. This module is the only code of the mod that runs before the engine creates
its window, so the awareness is set here.

If a manifest or a compatibility setting has already decided the process's DPI awareness, the call
fails and changes nothing. The log records the resulting awareness and whether the shim set it.

## Gamepads

The game reads a DirectInput joystick itself, but only a few axes, one hat and up to 16 buttons,
and every pad reports a different layout. So the shim shows the game one virtual joystick,
"Gamepad", always present, and hides the real ones from the game's enumeration. Each time the game
polls it, the shim reads a physical pad and fills in one fixed layout.

| Pad kind | How it is read |
|---|---|
| Xbox and other XInput pads | XInput (`xinput1_4`, `xinput1_3` or `xinput9_1_0`), first connected slot |
| DualSense, DualShock 4 (Sony) | DirectInput: left stick X/Y, right stick Z/Rz, buttons Square, Cross, Circle, Triangle, L1, R1, L2, R2, Create, Options, L3, R3 |
| Other DirectInput pads | DirectInput: same axes, buttons A, B, X, Y, LB, RB, LT, RT, Back, Start, L3, R3 |

An XInput pad is preferred. DirectInput pads with Microsoft's vendor ID are skipped, because
XInput reads them. One pad is used at a time. DirectInput pads give their triggers through the
digital L2/R2 buttons, not the trigger axes: until a pad sends its first report, DirectInput reads
every axis as 0, which in the stick range is a half-pressed trigger.

### Layout

| Pad | In play |
|---|---|
| Left stick | walk (up/down), strafe (left/right) |
| Right stick | turn (left/right), look (up/down) |
| RT or X | fire |
| LT or B | secondary fire |
| A | jump |
| Y | taunt |
| RB / LB | next / previous weapon |
| R3 | center view |
| D-pad | look up/down, strafe |
| Start | Esc (pause menu) |
| Back, L3 | not bound |

Turning and looking go through the N64 controller code that is still in the executable. Turning is
a rate. Looking is a position from -90 to +90 degrees that springs back to level when the stick is
released. The game squares the stick value, so the centre is gentle. Mouse look and the pad share
the view: the right stick takes it, any mouse movement takes it back.

The buttons above are the game's default joystick bindings, set by the shim. Bindings saved from
the game's own Joystick Config screen take precedence. The shim also turns the game's AlwaysLook
option on by default. Start produces Esc in both places the game reads it: as a key event in its
buffered DirectInput keyboard (used in play) and as `WM_KEYDOWN`/`WM_KEYUP` posted to the game
window (used in its menus).

In menus, the D-pad or the left stick moves one item per press; held, it repeats after 0.4 s, then
every 0.13 s. The left stick counts as pressed past half deflection and as released below about a
third. A confirms and B goes back (the menus' own Esc), each once per press.

### Dead zones and sensitivity

| Setting | Value | Configurable |
|---|---|---|
| Stick dead zone | radial, 0.24 of full deflection, rescaled so the stick still reaches full | no |
| Trigger threshold (XInput) | 0.12 of full travel | no |
| `PadTurnSensitivity` | default 0.60, range 0.05 to 1 | yes |
| `PadLookSensitivity` | default 1.00, range 0.05 to 1 | yes |

The sensitivities scale the stick before the game sees it, so full deflection reaches that
fraction of the N64 range. Full turn in that range is 576 degrees a second; at 0.6, after the game
squares the value, the turn tops out near 210 degrees a second. A value outside the range is
ignored.

Both live in `[Input]` of `Video_D3D9.ini` (see Settings). While it reads the pad, the shim checks
the file's write time at most twice a second and reloads both values when it changes. The
RemixProxy F4 overlay sets them while the game runs through the shim's export
`SouthPark_SetSetting(section, key, value)`, which does not write the file.

### Finding pads

Looking for a pad is slow: DirectInput's enumeration walks every HID device, and an empty XInput
slot is slow to answer. So the search runs on a thread of its own, once a second, only while the
game has no pad. It checks XInput slots 0 to 3 and enumerates attached DirectInput joysticks
through its own DirectInput instance; the game thread only opens what it found. Once an XInput pad
is in use, the search for DirectInput pads stops. A pad that disconnects starts the search again,
so a pad can be connected at any time.

XInput is loaded, and the search thread started, on the first pad read, not inside the game's
`DirectInputCreateA` call: XInput loaded there makes the game's mouse setup fail.

The shim's own DirectInput objects share vtables with the game's; the hooks recognise and skip
them. While input is suspended, the virtual joystick reads as centred, with no buttons.

### Game patches and builds

When the game first calls `DirectInputCreateA` (its code is decrypted by then), the shim patches
the executable in memory: the joystick axis reader (four full, unscaled axes for walk, strafe, turn
and look), AlwaysLook (look through the N64 stick value, on by default), the default button
bindings, the two menu joystick readers (replaced by the menu navigation above), and one call in
the mouse poller (so the pad can hold the view).

The addresses are those of the US build. `common/game_build.h` identifies the running build from
the executable's header timestamp and, on other builds, finds the same places by signature search.
Every site is checked for its expected bytes first. If any check fails, nothing is patched, the
log says so, and the game keeps its stock joystick handling.

## Music from WAV files

The game's soundtrack is the disc's CD audio, tracks 2 to 14, played through MCI's `cdaudio`
device. When the tracks exist as `<game dir>\music\track02.wav` to `<game dir>\music\track14.wav`
(numbered as on the disc), the shim answers the game's `cdaudio` device itself and plays the files.
The drive is not used. With no WAV files, nothing is hooked and the game uses the drive.

The files must be:

- RIFF/WAVE, with a `fmt ` chunk before the `data` chunk;
- uncompressed PCM (`WAVE_FORMAT_PCM`) in a format Windows audio output accepts. 44.1 kHz, 16-bit
  stereo is the CD's own format;
- complete tracks. The shim reports each track's length from its sample count, and the game checks
  the number of tracks (14) and the length of track 2 (5:24 to 5:27).

The track count reported is the highest track number found. A track missing from the folder ends
at once; the others still play.

What the shim answers:

- **Open.** An `MCI_OPEN` of `cdaudio`, by device type ID or by name, gets a device ID of the
  shim's. Every other device and every other open goes to Windows.
- **Commands:** close, set time format (MSF, TMSF, milliseconds), status (number of tracks, length,
  position, mode, current track, track type, media present, ready, time format), play with
  from/to, stop, pause, resume, seek, device capabilities, info. Anything else returns
  `MCIERR_UNSUPPORTED_FUNCTION`.
- **Notifications:** `MM_MCINOTIFY` with success when a played range ends, superseded when a new
  play replaces it, aborted when it is stopped.

The hook is a hot patch on winmm's `mciSendCommandA`, so it also sees calls that do not go through
the executable's import table. If that function lacks the expected hot-patch prologue, the shim
logs it and leaves the CD audio to the drive. A player thread streams each file through `waveOut`
with four 250 ms buffers.

## Extra Load Game slots

Load Game steps through slots 0 to 8 with Left and Right: "Slot 1" to "Slot 8" read
`savegame.000` to `savegame.007`, and slot 8, "Last Autosave", reads `savegame.008`. The game
names the file `savegame.00%i`, so later slots would read `savegame.009`, `savegame.0010` and so
on. When such files exist in the game folder, the shim extends the screen to the highest of them:
Right past "Last Autosave" goes on to "Slot 10", and Left from "Slot 1" goes to the last slot.
Without them nothing is patched. Save Game keeps its own eight slots and never writes these. This
is a developer aid for reaching any level; the mod ships no such saves.

## Settings

Read from `Video_D3D9.ini` in the game folder (the renderer's settings file, so the mod has one),
section `[Input]`. All keys are optional.

| Key | Default | Effect |
|---|---|---|
| `ForceNonExclusiveInput` | 1 | rewrite every cooperative level to non-exclusive, foreground |
| `SuspendInputWhenUnfocused` | 1 | the game reads no input while another window is in front |
| `RemixMenuSuspendsInput` | 1 | the game reads no input while the Remix menu or the F4 overlay is open |
| `KeepCursorVisible` | 1 | show the pointer while input is suspended; stop the game confining it |
| `Controller` | 1 | the gamepad layer; 0 leaves the game's own joystick handling alone |
| `PadTurnSensitivity` | 0.60 | see Dead zones and sensitivity |
| `PadLookSensitivity` | 1.00 | see Dead zones and sensitivity |

The freeze prevention is always on. The music is on whenever the WAV files exist.

## Log file

`<game dir>\DInputShim.log`, recreated at every start. It records the settings, the DPI awareness,
the redirected import thunks, each cooperative level the shim rewrote, the raw mouse reader and the
mouse smoothing, any extra Load Game slots, the Remix menu and overlay
opening and closing, the game build and whether the pad patches were applied, which pad is in use
and when it connects or disconnects, the music tracks found and played, and a summary of counts
when the game exits.

## Build

Requirements: Visual Studio 2022 with the "Desktop development with C++" workload.

```
DInputShim\build.bat
```

The script finds Visual Studio with `vswhere`, sets up the 32-bit (x86) toolchain (the game is a
32-bit process), and compiles `src\shim.cpp`, `src\pad.cpp`, `src\cdaudio.cpp`, `src\rawmouse.cpp`
and `src\loadslots.cpp` with the exports
from `dinput.def` and the headers in `..\common`. The output is `DInputShim\build\dinput.dll`.
