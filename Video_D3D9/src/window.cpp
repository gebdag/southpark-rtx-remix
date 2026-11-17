#include "window.h"

#include "config.h"
#include "device.h"
#include "engine.h"
#include "log.h"

namespace {

HWND g_window = 0;
WNDPROC g_engineProc = 0;
Window::Mode g_mode = Window::ModeBorderless;
Window::Placement g_placement;
// Where the framed window last stood, so Alt+Enter back from borderless returns it there
// rather than to the configured spot.
RECT g_windowedRect;
bool g_haveWindowedRect = false;

const DWORD kKeptStyle = WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
const DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW;
const DWORD kBorderlessStyle = WS_POPUP;

// The smallest client area a framed window can be dragged down to.
const int kMinClientWidth = 320;
const int kMinClientHeight = 240;

// The engine's pause and resume routines minimise the window on focus loss and restore it on
// focus gain (SouthPark.exe 0x0047DC50, 0x0047DCC0, 0x0047DD30), each through the copy
// protection's API dispatcher. A window that vanishes whenever another one is clicked cannot
// sit beside anything, so those three calls come here instead and leave the window as it is;
// the pause itself still happens.
const unsigned int kMinimiseOnPauseSite = 0x0047DCB1;
const unsigned int kRestoreOnResumeSites[] = {0x0047DD21, 0x0047DD87};

BOOL WINAPI KeepWindowAsItIs(HWND window, int /*command*/) {
    return IsWindowVisible(window);
}

RECT MonitorArea(bool workArea) {
    MONITORINFO info;
    info.cbSize = sizeof(info);
    GetMonitorInfoA(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &info);
    return workArea ? info.rcWork : info.rcMonitor;
}

RECT ConfiguredWindowedRect() {
    RECT frame = {0, 0, g_placement.width, g_placement.height};
    AdjustWindowRect(&frame, kWindowedStyle, FALSE);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const RECT work = MonitorArea(true);
    const int x = g_placement.x >= 0 ? g_placement.x
                                     : work.left + ((work.right - work.left) - width) / 2;
    const int y = g_placement.y >= 0 ? g_placement.y
                                     : work.top + ((work.bottom - work.top) - height) / 2;
    RECT r = {x, y, x + width, y + height};
    return r;
}

void Apply(Window::Mode mode) {
    if (g_mode == Window::ModeWindowed && mode != Window::ModeWindowed &&
        !IsIconic(g_window) && GetWindowRect(g_window, &g_windowedRect)) {
        g_haveWindowedRect = true;
    }
    g_mode = mode;

    const DWORD kept = (DWORD)GetWindowLongA(g_window, GWL_STYLE) & kKeptStyle;
    RECT r;
    if (mode == Window::ModeWindowed) {
        SetWindowLongA(g_window, GWL_STYLE, kWindowedStyle | kept);
        r = g_haveWindowedRect ? g_windowedRect : ConfiguredWindowedRect();
    } else {
        SetWindowLongA(g_window, GWL_STYLE, kBorderlessStyle | kept);
        r = MonitorArea(false);
    }
    SetWindowPos(g_window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    LogLine("    window: %s, %ldx%ld at %ld,%ld",
            mode == Window::ModeWindowed ? "windowed" : "borderless",
            r.right - r.left, r.bottom - r.top, r.left, r.top);
}

bool IsAltEnter(WPARAM key, LPARAM flags) {
    return key == VK_RETURN && (flags & (1 << 29)) != 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SYSKEYDOWN:
        if (IsAltEnter(wp, lp)) {
            const bool repeat = (lp & (1 << 30)) != 0;
            if (!repeat) {
                Window::ToggleMode();
            }
            return 0;
        }
        break;
    case WM_SYSCHAR:
        // The character half of Alt+Enter; unanswered, DefWindowProc beeps at it.
        if (IsAltEnter(wp, lp)) {
            return 0;
        }
        break;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            Device::OnWindowResized(hwnd);
        }
        break;
    case WM_GETMINMAXINFO: {
        RECT frame = {0, 0, kMinClientWidth, kMinClientHeight};
        AdjustWindowRect(&frame, kWindowedStyle, FALSE);
        MINMAXINFO* info = (MINMAXINFO*)lp;
        info->ptMinTrackSize.x = frame.right - frame.left;
        info->ptMinTrackSize.y = frame.bottom - frame.top;
        return 0;
    }
    case WM_ACTIVATE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        // These are what pause the engine; withheld, the game keeps running behind other
        // windows. The input shim still keeps its devices from reading while unfocused.
        if (!Config::PauseWhenUnfocused()) {
            return DefWindowProcA(hwnd, msg, wp, lp);
        }
        break;
    }
    return CallWindowProcA(g_engineProc, hwnd, msg, wp, lp);
}

void KeepEngineFromMinimising() {
    int patched = Engine::RedirectApiCall(kMinimiseOnPauseSite, (const void*)&KeepWindowAsItIs);
    for (int i = 0; i < (int)(sizeof(kRestoreOnResumeSites) / sizeof(kRestoreOnResumeSites[0]));
         ++i) {
        patched += Engine::RedirectApiCall(kRestoreOnResumeSites[i],
                                           (const void*)&KeepWindowAsItIs);
    }
    LogLine("    window: %d of 3 engine minimise/restore calls redirected", patched);
}

} // namespace

Window::Placement Window::Configured() {
    Placement p;
    p.mode = Config::DisplayWindowed() ? ModeWindowed : ModeBorderless;
    p.width = Config::DisplayWindowWidth();
    p.height = Config::DisplayWindowHeight();
    p.x = Config::DisplayWindowX();
    p.y = Config::DisplayWindowY();
    return p;
}

void Window::Attach(HWND window, const Placement& placement) {
    if (!window) {
        return;
    }
    // The engine keeps its window across a driver reselect, and that calls this again.
    if (window != g_window) {
        g_window = window;
        g_engineProc = (WNDPROC)SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)&WindowProc);
        KeepEngineFromMinimising();
    }
    g_placement = placement;
    // An earlier mode would make Apply save its rect as the user's own windowed position.
    g_mode = placement.mode;
    g_haveWindowedRect = false;
    Apply(placement.mode);
}

void Window::ToggleMode() {
    if (g_window) {
        Apply(g_mode == ModeWindowed ? ModeBorderless : ModeWindowed);
    }
}
