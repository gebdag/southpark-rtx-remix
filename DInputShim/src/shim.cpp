// dinput.dll shim for South Park (1999) -- makes the game share input with the desktop.
//
// The game takes the mouse and keyboard through DirectInput in EXCLUSIVE mode. That has
// three consequences the RTX Remix port cannot live with:
//
//   * the OS cursor is suppressed for the whole process, so the Alt+X Remix overlay has
//     nothing to point with -- and it is genuinely suppressed, not merely hidden:
//     SetCursorPos still reports success while synthetic mouse_event(MOUSEEVENTF_MOVE)
//     calls leave the pointer where it is;
//   * an exclusively acquired keyboard swallows Alt+Tab;
//   * the game keeps reading its devices while another window has focus, so clicking away
//     leaves it turning and firing in the background.
//
// None of this is fixable from the video plugin. The engine calls DirectInputCreateA during
// startup, long before it loads a renderer, so every call the plugin could redirect has
// already happened by the time it exists.
//
// A DLL the executable imports statically, on the other hand, is initialised before the
// process runs a single instruction of its own. The executable imports DINPUT.dll, and
// dinput.dll is not a KnownDLL, so a copy in the application directory wins the loader's
// search. This is that copy: it forwards every export to the real dinput.dll in system32 and
// patches a handful of methods on the interfaces the game gets back.
//
// Patching the COM vtables rather than wrapping the objects means no method we do not care
// about has to be reimplemented -- which matters, because dinput hands out several interface
// versions with different vtable lengths. Each patched vtable is remembered with its
// original method pointers, since the mouse and the keyboard are different classes inside
// dinput and do not share one.

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pad.h"

namespace {

// --------------------------------------------------------------------------- logging

char g_logPath[MAX_PATH];
CRITICAL_SECTION g_logLock;
bool g_logReady = false;

void Log(const char* fmt, ...) {
    if (!g_logReady) {
        return;
    }
    EnterCriticalSection(&g_logLock);
    FILE* f = fopen(g_logPath, "a");
    if (f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------------------- config
//
// Read from the renderer's Video_D3D9.ini so the port has one settings file rather than two.

char g_iniPath[MAX_PATH];
bool g_forceNonExclusive = true;
bool g_suspendWhenUnfocused = true;
bool g_menuSuspendsInput = true;
bool g_keepCursorVisible = true;
bool g_controller = true;
void* g_keyboard = 0;
void* g_mouse = 0;

bool ReadBool(const char* section, const char* key, bool fallback) {
    return GetPrivateProfileIntA(section, key, fallback ? 1 : 0, g_iniPath) != 0;
}

// --------------------------------------------------------------------------- forwarding

HMODULE g_real = 0;

typedef HRESULT (WINAPI *DirectInputCreateAFn)(HINSTANCE, DWORD, void**, void*);
typedef HRESULT (WINAPI *DirectInputCreateWFn)(HINSTANCE, DWORD, void**, void*);
typedef HRESULT (WINAPI *DirectInputCreateExFn)(HINSTANCE, DWORD, const IID&, void**, void*);
typedef HRESULT (WINAPI *DllGetClassObjectFn)(const CLSID&, const IID&, void**);
typedef HRESULT (WINAPI *NoArgHrFn)(void);

DirectInputCreateAFn  g_realCreateA = 0;
DirectInputCreateWFn  g_realCreateW = 0;
DirectInputCreateExFn g_realCreateEx = 0;

// Loads the genuine dinput.dll by absolute path. A plain LoadLibraryA("dinput.dll") would
// find this file again -- it is the one sitting in the application directory.
bool LoadReal() {
    if (g_real) {
        return true;
    }
    char path[MAX_PATH];
    const UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n + sizeof("\\dinput.dll") >= MAX_PATH) {
        return false;
    }
    lstrcatA(path, "\\dinput.dll");
    g_real = LoadLibraryA(path);
    if (!g_real) {
        Log("FATAL: could not load %s (err %lu)", path, (unsigned long)GetLastError());
        return false;
    }
    Log("real dinput: %s", path);
    return true;
}

FARPROC Real(const char* name) {
    return LoadReal() ? GetProcAddress(g_real, name) : 0;
}

// ------------------------------------------------------------------------ focus state

// True while the Remix menu, or the Remix proxy's overlay, is open and wants the game's input.
// Set from their own menu-state messages (see RemixMenuMessageHook), so neither can drift.
bool g_inputSuspendedByMenu = false;
bool g_inputSuspendedByOverlay = false;

bool MenuOwnsInput() {
    return g_inputSuspendedByMenu || g_inputSuspendedByOverlay;
}

// The cursor belongs to whoever is entitled to the mouse. While the game is reading its
// devices that is the game, which hides the pointer -- an arrow parked in the middle of
// gameplay is exactly what the game's exclusive grab prevents. It is put back only while
// input is suspended: overlay open, or another window in the foreground.
int g_cursorShown = 0;
// Starts released so the first UpdateCursor(false) genuinely runs: the window class may
// already carry a cursor, and the resting state has to be installed rather than assumed.
bool g_cursorReleased = true;       // the pointer currently belongs to the desktop
HCURSOR g_gameCursor = 0;           // the last cursor the game asked for
HWND g_inputWindow = 0;             // the window the game gave to SetCooperativeLevel
int g_devicesPatched = 0;
int g_coopCallsRewritten = 0;
long g_statesBlocked = 0;
long g_reacquires = 0;
long g_deferredAcquires = 0;

bool GameHasFocus() {
    const HWND fg = GetForegroundWindow();
    if (!fg) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ShowCursor keeps an internal counter, so the show and the hide have to be balanced rather
// than repeated -- calling it every frame either climbs forever or never takes effect. The
// window class cursor moves with it: a class with no cursor shows nothing over its client
// area whatever anyone else wants, and a class with one shows it whether or not it is
// wanted, so it is installed and removed alongside.
void UpdateCursor(bool release) {
    if (!g_keepCursorVisible || release == g_cursorReleased) {
        return;
    }
    g_cursorReleased = release;
    if (release) {
        if (g_inputWindow) {
            SetClassLongPtrA(g_inputWindow, GCLP_HCURSOR, (LONG_PTR)LoadCursorA(0, IDC_ARROW));
        }
        SetCursor(LoadCursorA(0, IDC_ARROW));
        if (g_cursorShown < 1) {
            ShowCursor(TRUE);
            ++g_cursorShown;
        }
    } else {
        if (g_inputWindow) {
            SetClassLongPtrA(g_inputWindow, GCLP_HCURSOR, 0);
        }
        // Hand the game back whatever it last asked for, which is usually nothing at all.
        SetCursor(g_gameCursor);
        while (g_cursorShown > 0) {
            ShowCursor(FALSE);
            --g_cursorShown;
        }
    }
}

// True when the game must not see any input on this call.
bool InputBlocked() {
    const bool focused = GameHasFocus();
    const bool blocked = (g_suspendWhenUnfocused && !focused) || MenuOwnsInput();
    UpdateCursor(blocked || !focused);
    return blocked;
}

// ------------------------------------------------------------------- vtable patching

typedef HRESULT (STDMETHODCALLTYPE *QueryInterfaceFn)(void*, const IID&, void**);
typedef ULONG (STDMETHODCALLTYPE *ReleaseFn)(void*);
typedef HRESULT (STDMETHODCALLTYPE *CreateDeviceFn)(void*, const GUID*, void**, void*);
typedef HRESULT (STDMETHODCALLTYPE *EnumDevicesFn)(void*, DWORD, void*, void*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *AcquireFn)(void*);
typedef HRESULT (STDMETHODCALLTYPE *UnacquireFn)(void*);
typedef HRESULT (STDMETHODCALLTYPE *SetCooperativeLevelFn)(void*, HWND, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *GetDeviceStateFn)(void*, DWORD, void*);
typedef HRESULT (STDMETHODCALLTYPE *GetDeviceDataFn)(void*, DWORD, void*, DWORD*, DWORD);

// Vtable slots, counting the three IUnknown methods first.
const int kSlotQueryInterface      = 0;
const int kSlotRelease             = 2;
const int kSlotCreateDevice        = 3;    // IDirectInput
const int kSlotEnumDevices         = 4;
const int kSlotAcquire             = 7;    // IDirectInputDevice from here down
const int kSlotUnacquire           = 8;
const int kSlotGetDeviceState      = 9;
const int kSlotGetDeviceData       = 10;
const int kSlotSetCooperativeLevel = 13;
const int kMaxTrackedSlots         = 14;

const DWORD DISCL_EXCLUSIVE    = 0x00000001;
const DWORD DISCL_NONEXCLUSIVE = 0x00000002;
const DWORD DISCL_FOREGROUND   = 0x00000004;
const DWORD DISCL_BACKGROUND   = 0x00000008;

const HRESULT DIERR_INPUTLOST   = (HRESULT)0x8007001EL;
const HRESULT DIERR_NOTACQUIRED = (HRESULT)0x8007000CL;

// One record per patched vtable, holding the methods it displaced. dinput's mouse and
// keyboard are separate classes with separate vtables, and QueryInterface can hand back a
// third for a later interface version, so a single saved "original" would call the wrong
// implementation the moment a second device appeared.
struct Vtable {
    void** table;
    void* original[kMaxTrackedSlots];
};
const int kMaxVtables = 16;
Vtable g_vtables[kMaxVtables];
int g_vtableCount = 0;

Vtable* FindVtable(void* object) {
    if (!object) {
        return 0;
    }
    void** table = *(void***)object;
    for (int i = 0; i < g_vtableCount; ++i) {
        if (g_vtables[i].table == table) {
            return &g_vtables[i];
        }
    }
    return 0;
}

Vtable* EnsureVtable(void* object) {
    Vtable* found = FindVtable(object);
    if (found || g_vtableCount >= kMaxVtables) {
        return found;
    }
    Vtable& v = g_vtables[g_vtableCount++];
    memset(&v, 0, sizeof(v));
    v.table = *(void***)object;
    return &v;
}

// Replaces one slot, remembering what was there. Returns false if the slot is unreadable or
// already ours.
bool PatchMethod(Vtable* v, int slot, void* replacement) {
    if (!v || slot >= kMaxTrackedSlots) {
        return false;
    }
    if (IsBadReadPtr(v->table, (slot + 1) * sizeof(void*))) {
        return false;
    }
    if (v->original[slot]) {
        return true;                       // already patched
    }
    DWORD previous = 0;
    if (!VirtualProtect(&v->table[slot], sizeof(void*), PAGE_READWRITE, &previous)) {
        return false;
    }
    v->original[slot] = v->table[slot];
    v->table[slot] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&v->table[slot], sizeof(void*), previous, &ignored);
    return true;
}

void PatchDevice(void* device);
void PatchFactory(void* factory);

// A QueryInterface result is only worth patching if the vtable behind it is at least as long
// as the slots being written. Every DirectInput device interface (A, 2A, 7A, 8A) has at
// least 18 methods and every factory interface at least 8, so the one dangerous answer is
// IID_IUnknown: three methods, after which slot 3, 9, 10 or 13 lands on whatever follows the
// vtable in dinput's .rdata -- and IsBadReadPtr cannot tell the difference, because that
// memory is perfectly readable.
// Spelled out rather than taken from uuid.lib, which this module has no other reason to link.
const IID kIidUnknown = { 0x00000000, 0x0000, 0x0000,
                          { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

bool IsUnknownIid(const IID& iid) {
    return IsEqualIID(iid, kIidUnknown) != 0;
}

// The cooperative level each device last took from the game, so it can be put back. The
// Remix bridge client hooks DirectInput too, and rebinds the game's devices to the window
// its D3D9 device presents to. That is the game window in the game proper, but lookdev
// presents Remix in a window of its own -- after which DirectInput only lets the keyboard
// acquire while that window is in front, never while the player is in the game window.
struct DeviceLevel {
    void* device;
    DWORD flags;
};
const int kMaxDeviceLevels = 8;
DeviceLevel g_deviceLevels[kMaxDeviceLevels];
int g_deviceLevelCount = 0;
long g_rebinds = 0;
long g_unacquirable = 0;

void RememberLevel(void* device, DWORD flags) {
    for (int i = 0; i < g_deviceLevelCount; ++i) {
        if (g_deviceLevels[i].device == device) {
            g_deviceLevels[i].flags = flags;
            return;
        }
    }
    if (g_deviceLevelCount < kMaxDeviceLevels) {
        g_deviceLevels[g_deviceLevelCount].device = device;
        g_deviceLevels[g_deviceLevelCount].flags = flags;
        ++g_deviceLevelCount;
    }
}

void ForgetDevice(void* device) {
    for (int i = 0; i < g_deviceLevelCount; ++i) {
        if (g_deviceLevels[i].device == device) {
            g_deviceLevels[i] = g_deviceLevels[--g_deviceLevelCount];
            return;
        }
    }
}

bool RecallLevel(void* device, DWORD* flags) {
    for (int i = 0; i < g_deviceLevelCount; ++i) {
        if (g_deviceLevels[i].device == device) {
            *flags = g_deviceLevels[i].flags;
            return true;
        }
    }
    return false;
}

// True when the window the game handed to SetCooperativeLevel is the one in front -- the
// only state in which a foreground device can be acquired for the game at all.
bool GameWindowInFront() {
    if (!g_inputWindow) {
        return false;
    }
    const HWND fg = GetForegroundWindow();
    return fg && GetAncestor(fg, GA_ROOT) == GetAncestor(g_inputWindow, GA_ROOT);
}

// --------------------------------------------------------------------- Remix menu state
//
// The Remix runtime tells the bridge client whether its menu is open through a registered
// window message posted to the game window, and the client handles it in its own window
// procedure. When the menu closes -- which the runtime also reports once at startup -- that
// handler drains the mouse it saw the game acquire, calling GetDeviceData with the DirectInput 8
// record size. The game's DirectInput 5 device accepts only the older size and answers any
// other with DI_OK and the count untouched, so the drain never ends and the game window stops
// responding for good. The client forgets a device it sees unacquired, so the game's devices
// are released as the message comes off the queue, before it is dispatched, and Reacquire
// takes them back on the game's next read. This holds for any bridge build that drains this
// way and costs nothing with one that does not.
//
// The same message is the one reliable record of whether the menu is open, whatever opened
// or closed it, so it also decides when the game stops reading input for the menu. The South
// Park Remix proxy (d3d9.dll, F4 overlay) posts its own UWM_SOUTHPARK_OVERLAY_ACTIVE the same
// way, and is treated like the Remix menu.

UINT g_remixMenuMessage = 0;
UINT g_overlayMessage = 0;
HHOOK g_menuHook = 0;

void UnacquireGameDevices() {
    for (int i = 0; i < g_deviceLevelCount; ++i) {
        void* device = g_deviceLevels[i].device;
        UnacquireFn unacquire = (UnacquireFn)(*(void***)device)[kSlotUnacquire];
        unacquire(device);
    }
}

LRESULT CALLBACK RemixMenuMessageHook(int code, WPARAM removal, LPARAM param) {
    const MSG* msg = (const MSG*)param;
    if (code == HC_ACTION && removal == PM_REMOVE && msg->message == g_remixMenuMessage) {
        const bool open = (msg->wParam & 1) != 0;
        UnacquireGameDevices();
        g_inputSuspendedByMenu = open && g_menuSuspendsInput;
        Log("Remix menu %s: %d game devices released before the bridge handles it",
            open ? "opened" : "closed", g_deviceLevelCount);
    } else if (code == HC_ACTION && removal == PM_REMOVE && msg->message == g_overlayMessage) {
        const bool open = (msg->wParam & 1) != 0;
        g_inputSuspendedByOverlay = open && g_menuSuspendsInput;
        Log("proxy overlay %s", open ? "opened" : "closed");
    }
    return CallNextHookEx(g_menuHook, code, removal, param);
}

// Watches the message queue of the thread that owns the game's input window. Thread-local, so
// it sees the game's own messages only.
void WatchRemixMenu(HWND window) {
    if (g_menuHook) {
        return;
    }
    g_remixMenuMessage = RegisterWindowMessageA("UWM_REMIX_UIACTIVE_MSG");
    g_overlayMessage = RegisterWindowMessageA("UWM_SOUTHPARK_OVERLAY_ACTIVE");
    const DWORD thread = GetWindowThreadProcessId(window, 0);
    if (g_remixMenuMessage && thread) {
        g_menuHook = SetWindowsHookExA(WH_GETMESSAGE, &RemixMenuMessageHook, 0, thread);
    }
    Log("watching for Remix menu messages (0x%04x) on thread %lu: %s", g_remixMenuMessage,
        (unsigned long)thread, g_menuHook ? "ok" : "FAILED");
}

// ------------------------------------------------------------------------ device hooks

HRESULT STDMETHODCALLTYPE HookedSetCooperativeLevel(void* self, HWND window, DWORD flags) {
    Vtable* v = FindVtable(self);
    if (!v) {
        return E_FAIL;
    }
    SetCooperativeLevelFn original = (SetCooperativeLevelFn)v->original[kSlotSetCooperativeLevel];
    if (Pad::IsOwnDevice(self)) {
        return original(self, window, flags);
    }

    DWORD effective = flags;
    if (g_forceNonExclusive) {
        // Exclusive is the whole problem. Foreground-only is what stops the game reading its
        // devices behind another window's back, and DirectInput enforces that for us by
        // unacquiring on focus loss.
        effective &= ~(DISCL_EXCLUSIVE | DISCL_BACKGROUND);
        effective |= DISCL_NONEXCLUSIVE | DISCL_FOREGROUND;
    }
    if (effective != flags) {
        ++g_coopCallsRewritten;
        Log("SetCooperativeLevel(hwnd=%p) 0x%08lx -> 0x%08lx", window,
            (unsigned long)flags, (unsigned long)effective);
    }

    DWORD applied = effective;
    HRESULT hr = original(self, window, effective);
    if (FAILED(hr) && effective != flags) {
        // Some device classes reject combinations the game never asked for. Better the
        // game's own input than none at all.
        Log("  rewritten level rejected (hr=0x%08lx); falling back to the original",
            (unsigned long)hr);
        applied = flags;
        hr = original(self, window, flags);
    }

    if (window) {
        g_inputWindow = window;
        WatchRemixMenu(window);
    }
    if (SUCCEEDED(hr)) {
        RememberLevel(self, applied);
    }
    return hr;
}

// The game asks for background access to its keyboard, which can always be acquired. Made
// foreground above, an Acquire fails while another window is in front -- and if that happens
// while the game sets up input, it gives up on the rest: no mouse, no joystick. A refusal for
// that reason alone is reported as success; the reads below reacquire on demand.
const HRESULT DIERR_OTHERAPPHASPRIO = (HRESULT)0x80070005L;

HRESULT STDMETHODCALLTYPE HookedAcquire(void* self) {
    Vtable* v = FindVtable(self);
    if (!v) {
        return E_FAIL;
    }
    AcquireFn original = (AcquireFn)v->original[kSlotAcquire];
    const HRESULT hr = original(self);
    if (hr == DIERR_OTHERAPPHASPRIO && g_forceNonExclusive && !Pad::IsOwnDevice(self)) {
        InterlockedIncrement(&g_deferredAcquires);
        return S_OK;
    }
    return hr;
}

bool IsLost(HRESULT hr) {
    return hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED;
}

// Foreground cooperativity means DirectInput unacquires the device whenever the window loses
// focus, and the game is on its own to notice. If it does not, its input never comes back
// after the first Alt+Tab -- so reacquire here rather than depending on the engine. A device
// that still refuses while the game window is in front has been bound to another window of
// this process (see DeviceLevel); it is bound back to the game's window and acquired again.
// Returns true when the device is acquired.
bool Reacquire(Vtable* v, void* self) {
    AcquireFn acquire = (AcquireFn)v->original[kSlotAcquire];
    if (!acquire || !GameHasFocus()) {
        return false;
    }
    if (SUCCEEDED(acquire(self))) {
        InterlockedIncrement(&g_reacquires);
        return true;
    }
    SetCooperativeLevelFn setLevel = (SetCooperativeLevelFn)v->original[kSlotSetCooperativeLevel];
    DWORD level = 0;
    if (!setLevel || !GameWindowInFront() || !RecallLevel(self, &level)) {
        return false;
    }
    if (FAILED(setLevel(self, g_inputWindow, level)) || FAILED(acquire(self))) {
        return false;
    }
    if (InterlockedIncrement(&g_rebinds) <= 5) {
        Log("device %p was bound to another window; rebound to the game window %p", self,
            (void*)g_inputWindow);
    }
    return true;
}

// A device the game cannot have reads as one with nothing pressed and no movement. The loss
// itself is never reported: the engine answers it by reacquiring in a loop that pumps no
// window messages, so it would never see the focus change that lets the acquire succeed.
HRESULT ReportNoState(void* data, DWORD size) {
    if (data && size && !IsBadWritePtr(data, size)) {
        memset(data, 0, size);
    }
    return S_OK;
}

HRESULT ReportNoData(DWORD* inOut) {
    if (inOut) {
        *inOut = 0;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HookedGetDeviceState(void* self, DWORD size, void* data) {
    Vtable* v = FindVtable(self);
    if (!v) {
        return E_FAIL;
    }
    GetDeviceStateFn original = (GetDeviceStateFn)v->original[kSlotGetDeviceState];
    if (Pad::IsOwnDevice(self)) {
        return original(self, size, data);
    }
    if (InputBlocked()) {
        InterlockedIncrement(&g_statesBlocked);
        return ReportNoState(data, size);
    }
    HRESULT hr = original(self, size, data);
    if (IsLost(hr) && Reacquire(v, self)) {
        hr = original(self, size, data);
    }
    if (IsLost(hr)) {
        InterlockedIncrement(&g_unacquirable);
        return ReportNoState(data, size);
    }
    // DIMOUSESTATE starts with lX, lY.
    if (SUCCEEDED(hr) && self == g_mouse && data && size >= 2 * sizeof(LONG) && Pad::Installed() &&
        (((const LONG*)data)[0] != 0 || ((const LONG*)data)[1] != 0)) {
        Pad::OnMouseMoved();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedGetDeviceData(void* self, DWORD objectSize, void* buffer,
                                              DWORD* inOut, DWORD flags) {
    Vtable* v = FindVtable(self);
    if (!v) {
        return E_FAIL;
    }
    GetDeviceDataFn original = (GetDeviceDataFn)v->original[kSlotGetDeviceData];
    if (Pad::IsOwnDevice(self)) {
        return original(self, objectSize, buffer, inOut, flags);
    }
    if (InputBlocked()) {
        // Buffered events keep queueing while the game is not looking and would replay as a
        // burst on the way back, so flush the queue: passing a null buffer with an unlimited
        // count is dinput's own way to discard it.
        DWORD discard = 0xffffffffu;
        original(self, objectSize, 0, &discard, 0);
        InterlockedIncrement(&g_statesBlocked);
        return ReportNoData(inOut);
    }
    const DWORD capacity = inOut ? *inOut : 0;
    HRESULT hr = original(self, objectSize, buffer, inOut, flags);
    if (IsLost(hr) && Reacquire(v, self)) {
        if (inOut) {
            *inOut = capacity;
        }
        hr = original(self, objectSize, buffer, inOut, flags);
    }
    if (IsLost(hr)) {
        InterlockedIncrement(&g_unacquirable);
        return ReportNoData(inOut);
    }
    if (SUCCEEDED(hr) && self == g_keyboard && buffer && Pad::Installed()) {
        Pad::AddKeyEvents(buffer, objectSize, inOut, capacity);
    }
    return hr;
}

// A later interface version is a different object with its own vtable, so whatever
// QueryInterface hands back has to be patched too or the game simply uses the unhooked one.
HRESULT STDMETHODCALLTYPE HookedDeviceQueryInterface(void* self, const IID& iid, void** out) {
    Vtable* v = FindVtable(self);
    if (!v) {
        return E_FAIL;
    }
    QueryInterfaceFn original = (QueryInterfaceFn)v->original[kSlotQueryInterface];
    HRESULT hr = original(self, iid, out);
    if (SUCCEEDED(hr) && out && *out && !IsUnknownIid(iid)) {
        PatchDevice(*out);
    }
    return hr;
}

// The device list feeds UnacquireGameDevices, so a device leaves it as it is destroyed.
ULONG STDMETHODCALLTYPE HookedDeviceRelease(void* self) {
    Vtable* v = FindVtable(self);
    if (!v) {
        return 0;
    }
    ReleaseFn original = (ReleaseFn)v->original[kSlotRelease];
    const ULONG remaining = original(self);
    if (remaining == 0) {
        ForgetDevice(self);
    }
    return remaining;
}

void PatchDevice(void* device) {
    if (!device) {
        return;
    }
    const bool fresh = (FindVtable(device) == 0);
    Vtable* v = EnsureVtable(device);
    if (!v) {
        return;
    }
    PatchMethod(v, kSlotAcquire, (void*)&HookedAcquire);
    PatchMethod(v, kSlotSetCooperativeLevel, (void*)&HookedSetCooperativeLevel);
    PatchMethod(v, kSlotGetDeviceState, (void*)&HookedGetDeviceState);
    PatchMethod(v, kSlotGetDeviceData, (void*)&HookedGetDeviceData);
    PatchMethod(v, kSlotQueryInterface, (void*)&HookedDeviceQueryInterface);
    PatchMethod(v, kSlotRelease, (void*)&HookedDeviceRelease);
    if (fresh) {
        ++g_devicesPatched;
        Log("device vtable %p patched", (void*)v->table);
    }
}

// ----------------------------------------------------------------------- factory hooks

// The game's keyboard, which the pad layer adds Esc to (Start). Spelled out rather than taken
// from dxguid.lib for the same reason as kIidUnknown.
const GUID kGuidSysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF,
                                { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
// The game's mouse, whose movement hands the view back from the pad (Pad::OnMouseMoved).
const GUID kGuidSysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF,
                             { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

HRESULT STDMETHODCALLTYPE HookedCreateDevice(void* self, const GUID* guid, void** out,
                                             void* outer) {
    Vtable* v = FindVtable(self);
    if (!v) {
        return E_FAIL;
    }
    if (Pad::Installed() && Pad::IsVirtualJoystick(guid)) {
        return Pad::CreateVirtualJoystick(out);
    }
    CreateDeviceFn original = (CreateDeviceFn)v->original[kSlotCreateDevice];
    HRESULT hr = original(self, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        PatchDevice(*out);
        if (guid && IsEqualGUID(*guid, kGuidSysKeyboard)) {
            g_keyboard = *out;
        }
        if (guid && IsEqualGUID(*guid, kGuidSysMouse)) {
            g_mouse = *out;
        }
    }
    return hr;
}

// With the pad layer on, the game is offered the one virtual joystick (pad.cpp) instead of the
// real ones, which the layer reads itself.
const DWORD kDevTypeJoystick = 4;   // DIDEVTYPE_JOYSTICK

HRESULT STDMETHODCALLTYPE HookedEnumDevices(void* self, DWORD type, void* callback, void* context,
                                            DWORD flags) {
    Vtable* v = FindVtable(self);
    if (!v) {
        return E_FAIL;
    }
    if (Pad::Installed() && !Pad::IsOwnFactory(self) && (type & 0xff) == kDevTypeJoystick && callback) {
        Pad::EnumerateJoysticks(callback, context);
        return S_OK;
    }
    EnumDevicesFn original = (EnumDevicesFn)v->original[kSlotEnumDevices];
    return original(self, type, callback, context, flags);
}

HRESULT STDMETHODCALLTYPE HookedFactoryQueryInterface(void* self, const IID& iid, void** out) {
    Vtable* v = FindVtable(self);
    if (!v) {
        return E_FAIL;
    }
    QueryInterfaceFn original = (QueryInterfaceFn)v->original[kSlotQueryInterface];
    HRESULT hr = original(self, iid, out);
    if (SUCCEEDED(hr) && out && *out && !IsUnknownIid(iid)) {
        PatchFactory(*out);
    }
    return hr;
}

void PatchFactory(void* factory) {
    if (!factory) {
        return;
    }
    const bool fresh = (FindVtable(factory) == 0);
    Vtable* v = EnsureVtable(factory);
    if (!v) {
        return;
    }
    PatchMethod(v, kSlotCreateDevice, (void*)&HookedCreateDevice);
    PatchMethod(v, kSlotEnumDevices, (void*)&HookedEnumDevices);
    PatchMethod(v, kSlotQueryInterface, (void*)&HookedFactoryQueryInterface);
    if (fresh) {
        Log("IDirectInput vtable %p patched", (void*)v->table);
    }
}

// ---------------------------------------------------------------- executable IAT fixes

// The executable imports ClipCursor, SetCursor and mouse_event and manages the pointer with
// all three. Those calls do nothing visible while exclusive mode has the cursor suppressed;
// once it does not, they are exactly what drags the pointer back under the game.
typedef HCURSOR (WINAPI *SetCursorFn)(HCURSOR);
typedef BOOL (WINAPI *ClipCursorFn)(const RECT*);
typedef void (WINAPI *MouseEventFn)(DWORD, DWORD, DWORD, DWORD, ULONG_PTR);

SetCursorFn g_realSetCursor = 0;
ClipCursorFn g_realClipCursor = 0;
MouseEventFn g_realMouseEvent = 0;

HCURSOR WINAPI ShimSetCursor(HCURSOR cursor) {
    g_gameCursor = cursor;
    // The game hides the pointer by setting it to nothing, which is right while it owns the
    // mouse. Only refuse that while the pointer has been handed to the desktop, or the game
    // would take it back on its next frame.
    if (!cursor && g_keepCursorVisible && g_cursorReleased) {
        return g_realSetCursor(LoadCursorA(0, IDC_ARROW));
    }
    return g_realSetCursor(cursor);
}

BOOL WINAPI ShimClipCursor(const RECT* rect) {
    // Confining the pointer to the game window is precisely what stops it reaching anything
    // else. Let the game believe it worked.
    (void)rect;
    return TRUE;
}

void WINAPI ShimMouseEvent(DWORD flags, DWORD dx, DWORD dy, DWORD data, ULONG_PTR extra) {
    if (MenuOwnsInput() || (g_suspendWhenUnfocused && !GameHasFocus())) {
        return;
    }
    g_realMouseEvent(flags, dx, dy, data, extra);
}

// Rewrites one import thunk in the executable, confined to the thunk arrays the import
// descriptors name rather than searching memory for a pointer value -- a blind
// search-and-replace across a live heap eventually finds, and destroys, something that
// merely happens to hold that number.
int RedirectImport(void* original, void* replacement) {
    HMODULE exe = GetModuleHandleA(0);
    if (!exe || !original) {
        return 0;
    }
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const char*)exe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return 0;
    }
    int patched = 0;
    const IMAGE_IMPORT_DESCRIPTOR* desc =
        (const IMAGE_IMPORT_DESCRIPTOR*)((char*)exe + dir.VirtualAddress);
    for (; !IsBadReadPtr(desc, sizeof(*desc)) && desc->FirstThunk != 0; ++desc) {
        void** thunk = (void**)((char*)exe + desc->FirstThunk);
        for (; !IsBadReadPtr(thunk, sizeof(void*)) && *thunk != 0; ++thunk) {
            if (*thunk != original) {
                continue;
            }
            DWORD previous = 0;
            if (VirtualProtect(thunk, sizeof(void*), PAGE_READWRITE, &previous)) {
                *thunk = replacement;
                DWORD ignored = 0;
                VirtualProtect(thunk, sizeof(void*), previous, &ignored);
                ++patched;
            }
        }
    }
    return patched;
}

// The executable's imports are already snapped by the time a statically linked DLL's DllMain
// runs, so its thunks can be rewritten from here. The loader lock is held, so nothing in
// this path may load a module -- every function it touches is already resident.
void PatchExecutableImports() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) {
        Log("user32 not resident at load time; cursor thunks left alone");
        return;
    }
    g_realSetCursor = (SetCursorFn)GetProcAddress(user32, "SetCursor");
    g_realClipCursor = (ClipCursorFn)GetProcAddress(user32, "ClipCursor");
    g_realMouseEvent = (MouseEventFn)GetProcAddress(user32, "mouse_event");

    int cursor = 0, clip = 0, mouse = 0;
    if (g_realSetCursor && g_keepCursorVisible) {
        cursor = RedirectImport((void*)g_realSetCursor, (void*)&ShimSetCursor);
    }
    if (g_realClipCursor && g_keepCursorVisible) {
        clip = RedirectImport((void*)g_realClipCursor, (void*)&ShimClipCursor);
    }
    if (g_realMouseEvent) {
        mouse = RedirectImport((void*)g_realMouseEvent, (void*)&ShimMouseEvent);
    }
    Log("executable thunks redirected: SetCursor %d, ClipCursor %d, mouse_event %d",
        cursor, clip, mouse);
}

void LoadConfig(HMODULE self) {
    char modulePath[MAX_PATH];
    const DWORD len = GetModuleFileNameA(self, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    char* slash = strrchr(modulePath, '\\');
    if (slash) {
        *(slash + 1) = 0;
    } else {
        modulePath[0] = 0;
    }
    _snprintf(g_iniPath, MAX_PATH, "%sVideo_D3D9.ini", modulePath);
    _snprintf(g_logPath, MAX_PATH, "%sDInputShim.log", modulePath);
    g_iniPath[MAX_PATH - 1] = 0;
    g_logPath[MAX_PATH - 1] = 0;

    g_forceNonExclusive    = ReadBool("Input", "ForceNonExclusiveInput", true);
    g_suspendWhenUnfocused = ReadBool("Input", "SuspendInputWhenUnfocused", true);
    g_menuSuspendsInput    = ReadBool("Input", "RemixMenuSuspendsInput", true);
    g_keepCursorVisible    = ReadBool("Input", "KeepCursorVisible", true);
    g_controller           = ReadBool("Input", "Controller", true);
}

} // namespace

// ------------------------------------------------------------------------- exports

extern "C" HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD version, void** out,
                                             void* outer) {
    if (!g_realCreateA) {
        g_realCreateA = (DirectInputCreateAFn)Real("DirectInputCreateA");
    }
    if (!g_realCreateA) {
        return E_FAIL;
    }
    HRESULT hr = g_realCreateA(inst, version, out, outer);
    Log("DirectInputCreateA(version=0x%lx) -> 0x%08lx", (unsigned long)version,
        (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {
        PatchFactory(*out);
        // The game's code is decrypted by now, and it is about to enumerate joysticks.
        if (g_controller) {
            Pad::Install(g_realCreateA, &InputBlocked, &Log, g_iniPath);
        }
    }
    return hr;
}

extern "C" HRESULT WINAPI DirectInputCreateW(HINSTANCE inst, DWORD version, void** out,
                                             void* outer) {
    if (!g_realCreateW) {
        g_realCreateW = (DirectInputCreateWFn)Real("DirectInputCreateW");
    }
    if (!g_realCreateW) {
        return E_FAIL;
    }
    HRESULT hr = g_realCreateW(inst, version, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        PatchFactory(*out);
    }
    return hr;
}

extern "C" HRESULT WINAPI DirectInputCreateEx(HINSTANCE inst, DWORD version, const IID& iid,
                                              void** out, void* outer) {
    if (!g_realCreateEx) {
        g_realCreateEx = (DirectInputCreateExFn)Real("DirectInputCreateEx");
    }
    if (!g_realCreateEx) {
        return E_FAIL;
    }
    HRESULT hr = g_realCreateEx(inst, version, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        PatchFactory(*out);
    }
    return hr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void) {
    NoArgHrFn fn = (NoArgHrFn)Real("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

// Straight passthrough, deliberately: a factory reached this way is not patched. South Park
// calls DirectInputCreateA, so this exists to keep the module a complete dinput.dll rather
// than to cover a path the game uses -- anything arriving through CoCreateInstance would
// keep exclusive mode.
extern "C" HRESULT WINAPI DllGetClassObject(const CLSID& clsid, const IID& iid, void** out) {
    DllGetClassObjectFn fn = (DllGetClassObjectFn)Real("DllGetClassObject");
    return fn ? fn(clsid, iid, out) : E_FAIL;
}

extern "C" HRESULT WINAPI DllRegisterServer(void) {
    NoArgHrFn fn = (NoArgHrFn)Real("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

extern "C" HRESULT WINAPI DllUnregisterServer(void) {
    NoArgHrFn fn = (NoArgHrFn)Real("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

// ----------------------------------------------------------------------- DPI awareness
//
// The engine sizes its window from the screen metrics before any renderer loads. A process
// that is not DPI aware gets virtualised metrics -- 1920x1080 on a 4K monitor at 200% -- and a
// window Windows then scales up, blurred. The Remix bridge declares the process DPI aware later,
// when the device comes up, which leaves a window created under one set of coordinates being
// managed under another. This module is the only code that runs before the engine creates its
// window, so the process becomes per-monitor aware here, once, for everything after it.
typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(HANDLE);
typedef HANDLE (WINAPI *GetThreadDpiAwarenessContextFn)(void);
typedef int (WINAPI *GetAwarenessFromDpiAwarenessContextFn)(HANDLE);
const HANDLE kPerMonitorAwareV2 = (HANDLE)-4;    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

void BecomeDpiAware() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) {
        Log("user32 not resident at load time; DPI awareness left to the system");
        return;
    }
    SetProcessDpiAwarenessContextFn set =
        (SetProcessDpiAwarenessContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    GetThreadDpiAwarenessContextFn current =
        (GetThreadDpiAwarenessContextFn)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    GetAwarenessFromDpiAwarenessContextFn awareness =
        (GetAwarenessFromDpiAwarenessContextFn)GetProcAddress(user32,
                                                              "GetAwarenessFromDpiAwarenessContext");
    // Fails harmlessly when a manifest or a compatibility setting has already decided.
    const BOOL set_ok = set ? set(kPerMonitorAwareV2) : FALSE;
    const int now = (current && awareness) ? awareness(current()) : -1;
    static const char* const kNames[] = {"unaware", "system aware", "per-monitor aware"};
    Log("DPI awareness: %s (%s)", (now >= 0 && now <= 2) ? kNames[now] : "unknown",
        set_ok ? "set here" : "already decided before this module");
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        InitializeCriticalSection(&g_logLock);
        LoadConfig(self);
        DeleteFileA(g_logPath);
        g_logReady = true;
        Log("=== DInputShim loaded ===");
        Log("ForceNonExclusiveInput=%d SuspendInputWhenUnfocused=%d "
            "RemixMenuSuspendsInput=%d KeepCursorVisible=%d",
            g_forceNonExclusive, g_suspendWhenUnfocused, g_menuSuspendsInput,
            g_keepCursorVisible);
        BecomeDpiAware();
        PatchExecutableImports();
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("=== DInputShim unloading: %d device vtables patched, %d cooperative levels "
            "rewritten, %ld reads blocked, %ld acquires deferred to the first read, %ld "
            "reacquires, %ld rebinds, %ld reads of an unacquirable device ===",
            g_devicesPatched, g_coopCallsRewritten, g_statesBlocked, g_deferredAcquires,
            g_reacquires, g_rebinds, g_unacquirable);
        if (g_menuHook) {
            UnhookWindowsHookEx(g_menuHook);
        }
        g_logReady = false;
        DeleteCriticalSection(&g_logLock);
    }
    return TRUE;
}
