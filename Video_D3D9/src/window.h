#pragma once

#include <windows.h>

// Owns the game window once a renderer is chosen.
//
// The engine creates its window itself, before any renderer loads: a borderless popup the
// size of the screen, which it minimises whenever it loses focus. This module takes that
// window over -- its style, size and position, windowed or borderless with Alt+Enter between
// them -- and runs its own window procedure in front of the engine's, which still receives
// every message the game needs.
namespace Window {

enum Mode {
    ModeBorderless,   // covers the monitor the window is on
    ModeWindowed,     // framed, movable and resizable
};

struct Placement {
    Mode mode;
    int width;        // windowed client size
    int height;
    int x;            // windowed top-left on the desktop; -1 centres it on its monitor
    int y;
};

// The placement [Display] in Video_D3D9.ini asks for.
Placement Configured();

// Takes over the engine's window. Must run before a D3D9 device is created on it: the RTX
// Remix bridge subclasses the window when its device comes up and later unhooks by restoring
// the procedure it found, so this one has to be in place first or the bridge removes it.
void Attach(HWND window, const Placement& placement);

// Switches between windowed and borderless, as Alt+Enter does.
void ToggleMode();

} // namespace Window
