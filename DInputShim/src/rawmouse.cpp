#include "rawmouse.h"

#include <windows.h>
#include <string.h>

namespace {

CRITICAL_SECTION g_lock;
LONG g_x = 0, g_y = 0, g_wheel = 0;
BYTE g_buttons[5] = {};
long g_events = 0;
volatile LONG g_started = 0;
volatile bool g_running = false;
RawMouse::LogFn g_log = 0;

const USHORT kButtonDown[5] = {RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
                               RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
const USHORT kButtonUp[5] = {RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP,
                             RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};

void Accumulate(const RAWMOUSE& m) {
    EnterCriticalSection(&g_lock);
    ++g_events;
    // Only relative movement turns the view; absolute positions (tablets, remote sessions)
    // place a pointer rather than move one.
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_x += m.lLastX;
        g_y += m.lLastY;
    }
    if (m.usButtonFlags & RI_MOUSE_WHEEL) {
        g_wheel += (SHORT)m.usButtonData;
    }
    for (int i = 0; i < 5; ++i) {
        if (m.usButtonFlags & kButtonDown[i]) {
            g_buttons[i] = 0x80;
        }
        if (m.usButtonFlags & kButtonUp[i]) {
            g_buttons[i] = 0;
        }
    }
    LeaveCriticalSection(&g_lock);
}

// Windows' compatibility database applies IgnoreAltTab to SouthPark.exe. That fix intercepts
// RegisterRawInputDevices for the whole process (the call lands in AcGenral.dll and fails with
// ERROR_INVALID_PARAMETER), so the genuine function is looked up in user32's own export table,
// which the compatibility layer leaves alone.
typedef BOOL (WINAPI* RegisterRawInputDevicesFn)(PCRAWINPUTDEVICE, UINT, UINT);

RegisterRawInputDevicesFn GenuineRegisterRawInputDevices() {
    const BYTE* base = (const BYTE*)GetModuleHandleA("user32.dll");
    if (!base) {
        return 0;
    }
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const IMAGE_EXPORT_DIRECTORY* exports = (const IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
    const DWORD* names = (const DWORD*)(base + exports->AddressOfNames);
    const WORD* ordinals = (const WORD*)(base + exports->AddressOfNameOrdinals);
    const DWORD* functions = (const DWORD*)(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        if (lstrcmpA((const char*)(base + names[i]), "RegisterRawInputDevices") == 0) {
            return (RegisterRawInputDevicesFn)(base + functions[ordinals[i]]);
        }
    }
    return 0;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INPUT) {
        RAWINPUT input;
        UINT size = sizeof(input);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            input.header.dwType == RIM_TYPEMOUSE) {
            Accumulate(input.data.mouse);
        }
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

DWORD WINAPI ReaderThread(void*) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = &WindowProc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "SouthParkRawMouse";
    RegisterClassA(&wc);
    // Never shown. A message-only window would do for most raw input, but RIDEV_INPUTSINK is
    // documented for an ordinary window.
    HWND window = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, 0, 0, wc.hInstance, 0);
    if (!window) {
        g_log("raw mouse: no window (err %lu); the mouse is read through DirectInput", GetLastError());
        return 0;
    }
    RAWINPUTDEVICE device = {1, 2, RIDEV_INPUTSINK, window};   // generic desktop, mouse
    const RegisterRawInputDevicesFn registerDevices = GenuineRegisterRawInputDevices();
    if (!registerDevices || !registerDevices(&device, 1, sizeof(device))) {
        g_log("raw mouse: registration failed (err %lu); the mouse is read through DirectInput",
              GetLastError());
        return 0;
    }
    g_running = true;
    g_log("raw mouse: reading on thread %lu", GetCurrentThreadId());
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        DispatchMessageA(&msg);
    }
    return 0;
}

} // namespace

void RawMouse::Start(LogFn log) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        return;
    }
    g_log = log;
    InitializeCriticalSection(&g_lock);
    HANDLE thread = CreateThread(0, 0, &ReaderThread, 0, 0, 0);
    if (!thread) {
        g_log("raw mouse: no thread (err %lu); the mouse is read through DirectInput", GetLastError());
        return;
    }
    CloseHandle(thread);
}

bool RawMouse::Running() {
    return g_running;
}

bool RawMouse::Read(void* data, unsigned long size) {
    const unsigned long kStateSize = 3 * sizeof(LONG) + 4;   // DIMOUSESTATE
    const unsigned long kState2Size = 3 * sizeof(LONG) + 8;  // DIMOUSESTATE2
    if (!data || (size != kStateSize && size != kState2Size)) {
        return false;
    }
    LONG* axes = (LONG*)data;
    BYTE* buttons = (BYTE*)data + 3 * sizeof(LONG);
    memset(data, 0, size);
    EnterCriticalSection(&g_lock);
    axes[0] = g_x;
    axes[1] = g_y;
    axes[2] = g_wheel;
    memcpy(buttons, g_buttons, size == kStateSize ? 4 : 5);
    g_x = g_y = g_wheel = 0;
    LeaveCriticalSection(&g_lock);
    return true;
}

void RawMouse::Discard() {
    if (!g_running) {
        return;
    }
    EnterCriticalSection(&g_lock);
    g_x = g_y = g_wheel = 0;
    LeaveCriticalSection(&g_lock);
}

long RawMouse::Events() {
    return g_events;
}
