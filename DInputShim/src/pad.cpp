#define DIRECTINPUT_VERSION 0x0500
#include "pad.h"
#include "game_patch.h"

#include <dinput.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

Pad::BlockedFn g_inputBlocked = 0;
Pad::LogFn g_log = 0;
Pad::DirectInputCreateAFn g_realCreate = 0;
bool g_installed = false;

using GamePatch::Is;
using GamePatch::Matches;
using GamePatch::Write;

#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

// ------------------------------------------------------------------ the uniform pad

struct PadState {
    float lx = 0, ly = 0, rx = 0, ry = 0;    // -1..1, up and right positive
    float lt = 0, rt = 0;                    // 0..1
    bool a = false, b = false, x = false, y = false;
    bool lb = false, rb = false, back = false, start = false, l3 = false, r3 = false;
    bool up = false, down = false, left = false, right = false;
};

const float kStickDeadZone = 0.24f;     // XInput's own recommendation, 7849 / 32767
const float kTriggerPressed = 0.12f;

// Radial dead zone, rescaled so the stick still reaches full deflection.
void ApplyDeadZone(float& x, float& y) {
    const float length = sqrtf(x * x + y * y);
    if (length < kStickDeadZone) {
        x = y = 0.0f;
        return;
    }
    const float scaled = (length > 1.0f ? 1.0f : (length - kStickDeadZone) / (1.0f - kStickDeadZone));
    x = x / length * scaled;
    y = y / length * scaled;
}

// ------------------------------------------------------------------ XInput source

struct XInputGamepad {
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT thumbLX, thumbLY, thumbRX, thumbRY;
};
struct XInputState {
    DWORD packet;
    XInputGamepad pad;
};
typedef DWORD (WINAPI* XInputGetStateFn)(DWORD, XInputState*);

XInputGetStateFn g_xinputGetState = 0;
bool g_xinputLoaded = false;
int g_xinputSlot = -1;            // the connected pad in use

// ---- finding pads, off the game thread
//
// Looking for a pad is slow: DirectInput's EnumDevices walks every HID device on the machine,
// which takes about 50 ms, and an empty XInput slot is slow to answer. On the game thread each
// search would stall a frame. So the search runs on a thread of its own, once a second while
// the game thread wants a pad, and the game thread only opens what it found.
const DWORD kScanIntervalMs = 1000;
volatile LONG g_wantXInput = 0;        // set by the game thread while it has no XInput pad
volatile LONG g_xinputFound = -1;      // a slot that answered, for the game thread to take
volatile LONG g_wantDirectInput = 0;   // likewise for DirectInput
volatile LONG g_diFoundReady = 0;      // g_diFound* hold a pad for the game thread to open
GUID g_diFoundInstance;
WORD g_diFoundVendor = 0;
char g_diFoundName[MAX_PATH];
void* volatile g_scanFactory = 0;      // the search thread's own DirectInput

void LoadXInput() {
    g_xinputLoaded = true;
    const char* names[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char* name : names) {
        if (HMODULE module = LoadLibraryA(name)) {
            g_xinputGetState = (XInputGetStateFn)GetProcAddress(module, "XInputGetState");
            if (g_xinputGetState) {
                LOG("pad: XInput from %s", name);
                return;
            }
        }
    }
    LOG("pad: no XInput");
}

float Thumb(SHORT v) {
    return v < 0 ? v / 32768.0f : v / 32767.0f;
}

bool ReadXInput(PadState& s) {
    if (!g_xinputGetState) {
        return false;
    }
    XInputState state;
    if (g_xinputSlot >= 0 && g_xinputGetState((DWORD)g_xinputSlot, &state) != ERROR_SUCCESS) {
        LOG("pad: XInput pad %d disconnected", g_xinputSlot);
        g_xinputSlot = -1;
    }
    if (g_xinputSlot < 0) {
        InterlockedExchange(&g_wantXInput, 1);
        const LONG found = InterlockedExchange(&g_xinputFound, -1);
        if (found < 0 || g_xinputGetState((DWORD)found, &state) != ERROR_SUCCESS) {
            return false;
        }
        g_xinputSlot = (int)found;
        InterlockedExchange(&g_wantXInput, 0);
        LOG("pad: XInput pad %d connected", g_xinputSlot);
    }
    const XInputGamepad& p = state.pad;
    s.lx = Thumb(p.thumbLX); s.ly = Thumb(p.thumbLY);
    s.rx = Thumb(p.thumbRX); s.ry = Thumb(p.thumbRY);
    s.lt = p.leftTrigger / 255.0f; s.rt = p.rightTrigger / 255.0f;
    s.up = (p.buttons & 0x0001) != 0;    s.down = (p.buttons & 0x0002) != 0;
    s.left = (p.buttons & 0x0004) != 0;  s.right = (p.buttons & 0x0008) != 0;
    s.start = (p.buttons & 0x0010) != 0; s.back = (p.buttons & 0x0020) != 0;
    s.l3 = (p.buttons & 0x0040) != 0;    s.r3 = (p.buttons & 0x0080) != 0;
    s.lb = (p.buttons & 0x0100) != 0;    s.rb = (p.buttons & 0x0200) != 0;
    s.a = (p.buttons & 0x1000) != 0;     s.b = (p.buttons & 0x2000) != 0;
    s.x = (p.buttons & 0x4000) != 0;     s.y = (p.buttons & 0x8000) != 0;
    return true;
}

// ------------------------------------------------------------- DirectInput source
//
// Any other pad, read through a DirectInput instance of our own. Pads XInput already has
// (Microsoft's vendor id) are skipped. Sony pads (DualSense, DualShock 4) report the left
// stick on X/Y, the right stick on Z/Rz, L2/R2 on Rx/Ry and buttons Square, Cross, Circle,
// Triangle, L1, R1, L2, R2, Create, Options, L3, R3; other pads are taken to follow the same
// HID gamepad convention with A, B, X, Y first.

const WORD kVendorMicrosoft = 0x045e;
const WORD kVendorSony = 0x054c;

IDirectInputA* g_di = 0;
IDirectInputDevice2A* g_diPad = 0;
WORD g_diVendor = 0;
HWND g_window = 0;
bool g_diLogged = false;

struct FoundPad {
    GUID instance;
    WORD vendor;
    char name[MAX_PATH];
    bool found;
};

BOOL CALLBACK FindPad(LPCDIDEVICEINSTANCEA instance, LPVOID context) {
    FoundPad* found = (FoundPad*)context;
    const WORD vendor = LOWORD(instance->guidProduct.Data1);
    if (vendor == kVendorMicrosoft) {
        return DIENUM_CONTINUE;
    }
    found->instance = instance->guidInstance;
    found->vendor = vendor;
    lstrcpynA(found->name, instance->tszProductName, sizeof(found->name));
    found->found = true;
    return DIENUM_STOP;
}

DWORD WINAPI ScanForPads(LPVOID) {
    IDirectInputA* di = 0;   // its own, so the game thread's is never used from two threads
    for (;;) {
        if (g_xinputGetState && InterlockedCompareExchange(&g_wantXInput, 0, 0) &&
            InterlockedCompareExchange(&g_xinputFound, -1, -1) < 0) {
            XInputState state;
            for (DWORD i = 0; i < 4; ++i) {
                if (g_xinputGetState(i, &state) == ERROR_SUCCESS) {
                    InterlockedExchange(&g_xinputFound, (LONG)i);
                    break;
                }
            }
        }
        if (InterlockedCompareExchange(&g_wantDirectInput, 0, 0) &&
            !InterlockedCompareExchange(&g_diFoundReady, 0, 0)) {
            if (!di && g_realCreate) {
                // Published before it enumerates: the shim's enumeration hook offers the
                // virtual joystick to every factory but ours.
                if (FAILED(g_realCreate(GetModuleHandleA(0), 0x0500, (void**)&di, 0))) {
                    di = 0;
                }
                g_scanFactory = di;
            }
            FoundPad found = {};
            if (di) {
                di->EnumDevices(DIDEVTYPE_JOYSTICK, &FindPad, &found, DIEDFL_ATTACHEDONLY);
            }
            if (found.found) {
                g_diFoundInstance = found.instance;
                g_diFoundVendor = found.vendor;
                lstrcpynA(g_diFoundName, found.name, sizeof(g_diFoundName));
                InterlockedExchange(&g_diFoundReady, 1);
            }
        }
        Sleep(kScanIntervalMs);
    }
}

void CloseDirectInputPad() {
    if (g_diPad) {
        g_diPad->Unacquire();
        g_diPad->Release();
        g_diPad = 0;
    }
}

// Opens the pad the search thread found.
bool OpenDirectInputPad() {
    if (!InterlockedCompareExchange(&g_diFoundReady, 0, 0)) {
        return false;
    }
    FoundPad found = {};
    found.instance = g_diFoundInstance;
    found.vendor = g_diFoundVendor;
    lstrcpynA(found.name, g_diFoundName, sizeof(found.name));
    InterlockedExchange(&g_diFoundReady, 0);
    if (!g_di) {
        if (!g_realCreate || FAILED(g_realCreate(GetModuleHandleA(0), 0x0500, (void**)&g_di, 0)) || !g_di) {
            g_di = 0;
            return false;
        }
    }
    IDirectInputDeviceA* device = 0;
    if (FAILED(g_di->CreateDevice(found.instance, &device, 0)) || !device) {
        return false;
    }
    HRESULT hr = device->QueryInterface(IID_IDirectInputDevice2A, (void**)&g_diPad);
    device->Release();
    if (FAILED(hr) || !g_diPad) {
        g_diPad = 0;
        return false;
    }
    g_diPad->SetDataFormat(&c_dfDIJoystick);
    g_diPad->SetCooperativeLevel(g_window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    DIPROPRANGE range = {};
    range.diph.dwSize = sizeof(range);
    range.diph.dwHeaderSize = sizeof(range.diph);
    range.diph.dwHow = DIPH_DEVICE;
    range.lMin = -32768;
    range.lMax = 32767;
    g_diPad->SetProperty(DIPROP_RANGE, &range.diph);
    g_diVendor = found.vendor;
    LOG("pad: DirectInput pad '%s' (vendor %04x)", found.name, found.vendor);
    return true;
}

float Axis(LONG v) {
    return v < 0 ? v / 32768.0f : v / 32767.0f;
}

bool ReadDirectInput(PadState& s) {
    if (!g_diPad) {
        InterlockedExchange(&g_wantDirectInput, 1);
        if (!OpenDirectInputPad()) {
            return false;
        }
        InterlockedExchange(&g_wantDirectInput, 0);
    }
    DIJOYSTATE j;
    HRESULT hr = g_diPad->Poll();
    if (FAILED(hr)) {
        hr = g_diPad->Acquire();
        if (SUCCEEDED(hr)) {
            hr = g_diPad->Poll();
        }
    }
    if (SUCCEEDED(hr)) {
        hr = g_diPad->GetDeviceState(sizeof(j), &j);
    }
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        return false;
    }
    if (FAILED(hr)) {
        LOG("pad: DirectInput pad lost (0x%08lx)", (unsigned long)hr);
        CloseDirectInputPad();
        return false;
    }
    if (!g_diLogged) {
        // The first values read: all zero until the pad sends its first report (a sleeping pad
        // never does), else its resting position.
        g_diLogged = true;
        LOG("pad: first read X %ld Y %ld Z %ld Rx %ld Ry %ld Rz %ld hat %ld buttons %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            j.lX, j.lY, j.lZ, j.lRx, j.lRy, j.lRz, (long)j.rgdwPOV[0], j.rgbButtons[0], j.rgbButtons[1],
            j.rgbButtons[2], j.rgbButtons[3], j.rgbButtons[4], j.rgbButtons[5], j.rgbButtons[6], j.rgbButtons[7],
            j.rgbButtons[8], j.rgbButtons[9], j.rgbButtons[10], j.rgbButtons[11], j.rgbButtons[12], j.rgbButtons[13]);
    }
    s.lx = Axis(j.lX); s.ly = -Axis(j.lY);
    s.rx = Axis(j.lZ); s.ry = -Axis(j.lRz);
    // The triggers come from their digital buttons, not the Rx/Ry axes: until a pad's first
    // report arrives -- and a sleeping pad never sends one -- DirectInput reads every axis as
    // 0, which in this range is a trigger half pressed. The sticks rest at 0 anyway.
    const BYTE* b = j.rgbButtons;
    if (g_diVendor == kVendorSony) {
        s.x = b[0] != 0; s.a = b[1] != 0; s.b = b[2] != 0; s.y = b[3] != 0;
    } else {
        s.a = b[0] != 0; s.b = b[1] != 0; s.x = b[2] != 0; s.y = b[3] != 0;
    }
    s.lb = b[4] != 0; s.rb = b[5] != 0;
    s.lt = b[6] ? 1.0f : 0.0f; s.rt = b[7] ? 1.0f : 0.0f;
    s.back = b[8] != 0; s.start = b[9] != 0; s.l3 = b[10] != 0; s.r3 = b[11] != 0;
    const DWORD pov = j.rgdwPOV[0];
    if (LOWORD(pov) != 0xffff) {
        s.up = pov > 27000 || pov < 9000;
        s.right = pov > 0 && pov < 18000;
        s.down = pov > 9000 && pov < 27000;
        s.left = pov > 18000;
    }
    return true;
}

PadState g_state;
bool g_haveState = false;
// Who looks: the pad's right stick or the mouse, whichever moved last (see MouseLookPitch).
bool g_padLooks = false;
bool g_escapeDown = false;      // Start, as last sent to the window
bool g_escapeEvent = false;     // Start, as last appended to the keyboard buffer
DWORD g_keySequence = 0x40000000;
DWORD g_lastRead = 0;

// Start is Esc: the window half of it (see Pad::AddKeyEvents for the DirectInput half).
void SendEscape(bool down) {
    if (down == g_escapeDown || !g_window) {
        return;
    }
    g_escapeDown = down;
    const UINT scan = MapVirtualKeyA(VK_ESCAPE, MAPVK_VK_TO_VSC);
    const LPARAM flags = down ? 1 : (1 | (1u << 30) | (1u << 31));
    PostMessageA(g_window, down ? WM_KEYDOWN : WM_KEYUP, VK_ESCAPE, flags | (LPARAM)(scan << 16));
}

// Turn and look go to the N64 game's own stick handling (FUN_00425d50, FUN_00425410 scheme 0,
// FUN_00417130), which squares the stick for a gentle centre: turning is a rate, 576 degrees a
// second at the N64's full +-80; looking is a position, -90..90 degrees, that springs back to
// level when the stick is let go. The sensitivities scale the stick before the game sees it, so
// full deflection reaches that fraction of the N64's range: turning at 0.6 tops out near 210
// degrees a second, the game squaring the rest.
const char* g_iniPath = 0;
float g_turnSensitivity = 0.6f;
float g_lookSensitivity = 1.0f;
FILETIME g_iniWritten = {};
DWORD g_iniNextCheck = 0;

float ValidSensitivity(float value, float fallback) {
    return (value >= 0.05f && value <= 1.0f) ? value : fallback;
}

float ReadSensitivity(const char* key, float fallback) {
    char text[32];
    GetPrivateProfileStringA("Input", key, "", text, sizeof(text), g_iniPath);
    return ValidSensitivity(text[0] ? (float)atof(text) : fallback, fallback);
}

void RefreshSettings() {
    const DWORD now = GetTickCount();
    if (!g_iniPath || (int)(now - g_iniNextCheck) < 0) {
        return;
    }
    g_iniNextCheck = now + 500;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &info) ||
        CompareFileTime(&info.ftLastWriteTime, &g_iniWritten) == 0) {
        return;
    }
    g_iniWritten = info.ftLastWriteTime;
    g_turnSensitivity = ReadSensitivity("PadTurnSensitivity", 0.6f);
    g_lookSensitivity = ReadSensitivity("PadLookSensitivity", 1.0f);
    LOG("pad: turn sensitivity %.2f, look sensitivity %.2f", g_turnSensitivity, g_lookSensitivity);
}

// XInput first, then DirectInput. The game polls its joystick only in play and some menus, and
// Start reaches it through the keyboard, so the keyboard read refreshes the pad too.
void ReadPad() {
    g_lastRead = GetTickCount();
    RefreshSettings();
    if (!g_xinputLoaded) {
        // Here rather than at install: loaded inside the game's DirectInputCreateA, XInput
        // makes the game's mouse fail to set up right after.
        LoadXInput();
        if (HANDLE thread = CreateThread(0, 0, &ScanForPads, 0, 0, 0)) {
            CloseHandle(thread);
        } else {
            LOG("pad: no search thread (error %lu) -- pads connected later are not found",
                (unsigned long)GetLastError());
        }
    }
    PadState s;
    if (ReadXInput(s)) {
        // An XInput pad is in use, so the search stops looking for DirectInput ones.
        InterlockedExchange(&g_wantDirectInput, 0);
        g_haveState = true;
    } else {
        g_haveState = ReadDirectInput(s);
    }
    if (g_haveState) {
        ApplyDeadZone(s.lx, s.ly);
        ApplyDeadZone(s.rx, s.ry);
        if (s.rx != 0.0f || s.ry != 0.0f) {
            g_padLooks = true;
        }
    }
    g_state = s;
    SendEscape(g_haveState && s.start && !(g_inputBlocked && g_inputBlocked()));
}

// ------------------------------------------------------------------ what the game sees

// The game maps 0..65535 to about +-80 with a dead gap at 0x8000..0x8064; rest sits in it.
const LONG kCentre = 0x8032;

LONG ToGameAxis(float v) {
    const float clamped = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return clamped < 0 ? (LONG)(kCentre + clamped * kCentre) : (LONG)(kCentre + clamped * (65535 - kCentre));
}

enum VirtualButton {
    kButtonA, kButtonB, kButtonX, kButtonY, kButtonLB, kButtonRB, kButtonLT, kButtonRT,
    kButtonBack, kButtonStart, kButtonL3, kButtonR3, kButtonCount
};

DWORD g_lastPov = 0xffffffff;

// One hat value: the game fires only on exact cardinals and never releases one cardinal when
// the next follows directly, so diagonals read centred and a change of direction passes
// through centre for a frame.
DWORD HatValue(const PadState& s) {
    const int directions = s.up + s.down + s.left + s.right;
    DWORD pov = 0xffffffff;
    if (directions == 1) {
        pov = s.up ? 0 : s.right ? 9000 : s.down ? 18000 : 27000;
    }
    if (pov != 0xffffffff && g_lastPov != 0xffffffff && pov != g_lastPov) {
        pov = 0xffffffff;
    }
    g_lastPov = pov;
    return pov;
}

void FillGameState(DIJOYSTATE& j) {
    memset(&j, 0, sizeof(j));
    j.lX = j.lY = j.lZ = j.lRx = j.lRy = j.lRz = kCentre;
    j.rgdwPOV[0] = j.rgdwPOV[1] = j.rgdwPOV[2] = j.rgdwPOV[3] = 0xffffffff;
    if (!g_haveState || (g_inputBlocked && g_inputBlocked())) {
        g_lastPov = 0xffffffff;
        return;
    }
    const PadState& s = g_state;
    // Axis slots once patched (see PatchGame): walk Y, turn 1 -> Z, look 8 -> Rx, strafe 32 -> Rz.
    j.lY = ToGameAxis(-s.ly);              // walk: up is the low end, forward
    j.lZ = ToGameAxis(s.rx * g_turnSensitivity);    // turn: the N64 stick's X
    j.lRx = ToGameAxis(-s.ry * g_lookSensitivity);  // look: the N64 stick's Y
    j.lRz = ToGameAxis(s.lx);              // strafe
    j.rgdwPOV[0] = HatValue(s);
    const bool pressed[kButtonCount] = {s.a, s.b, s.x, s.y, s.lb, s.rb, s.lt > kTriggerPressed,
                                        s.rt > kTriggerPressed, s.back, s.start, s.l3, s.r3};
    for (int i = 0; i < kButtonCount; ++i) {
        j.rgbButtons[i] = pressed[i] ? 0x80 : 0;
    }
}

// --------------------------------------------------------- the virtual DirectInput joystick

// {53504144-0001-4d6f-6450-61644a6f7900}: "SPAD", then "ModPadJoy".
const GUID kVirtualInstance = {0x53504144, 0x0001, 0x4d6f, {0x64, 0x50, 0x61, 0x64, 0x4a, 0x6f, 0x79, 0x00}};
const GUID kVirtualProduct = {0x53504144, 0x0002, 0x4d6f, {0x64, 0x50, 0x61, 0x64, 0x4a, 0x6f, 0x79, 0x00}};
const char kVirtualName[] = "Gamepad";

void FillInstance(DIDEVICEINSTANCEA& d) {
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.guidInstance = kVirtualInstance;
    d.guidProduct = kVirtualProduct;
    d.dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8);
    lstrcpynA(d.tszInstanceName, kVirtualName, sizeof(d.tszInstanceName));
    lstrcpynA(d.tszProductName, kVirtualName, sizeof(d.tszProductName));
    d.wUsagePage = 1;
    d.wUsage = 5;
}

// The axes the game asks about by offset, and the only ones it reads; X, Y and Z must exist
// or the game reads a missing axis as full deflection.
bool HasAxis(DWORD offset) {
    return offset == DIJOFS_X || offset == DIJOFS_Y || offset == DIJOFS_Z || offset == DIJOFS_RZ;
}

class VirtualJoystick : public IDirectInputDevice2A {
public:
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, LPVOID* out) override {
        if (!out) {
            return E_POINTER;
        }
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirectInputDeviceA) ||
            IsEqualIID(iid, IID_IDirectInputDevice2A)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = 0;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_refs); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0) {
            delete this;
        }
        return (ULONG)refs;
    }

    // IDirectInputDeviceA
    STDMETHODIMP GetCapabilities(LPDIDEVCAPS caps) override {
        if (!caps || caps->dwSize < sizeof(DIDEVCAPS_DX3)) {
            return DIERR_INVALIDPARAM;
        }
        const DWORD size = caps->dwSize;
        memset(caps, 0, size);
        caps->dwSize = size;
        caps->dwFlags = DIDC_ATTACHED;
        caps->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8);
        caps->dwAxes = 4;
        caps->dwButtons = kButtonCount;
        caps->dwPOVs = 1;
        return DI_OK;
    }
    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) override { return DI_OK; }
    STDMETHODIMP GetProperty(REFGUID property, LPDIPROPHEADER header) override {
        if (&property == &DIPROP_RANGE && header && header->dwSize >= sizeof(DIPROPRANGE)) {
            if (header->dwHow == DIPH_BYOFFSET && !HasAxis(header->dwObj)) {
                return DIERR_OBJECTNOTFOUND;
            }
            DIPROPRANGE* range = (DIPROPRANGE*)header;
            range->lMin = 0;
            range->lMax = 65535;
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }
    // Range and dead zone are the game's to ask for and ours to honour: values always arrive
    // in 0..65535 with the dead zone already applied.
    STDMETHODIMP SetProperty(REFGUID, LPCDIPROPHEADER) override { return DI_OK; }
    STDMETHODIMP Acquire() override { return DI_OK; }
    STDMETHODIMP Unacquire() override { return DI_OK; }
    STDMETHODIMP GetDeviceState(DWORD size, LPVOID data) override {
        if (!data || size < sizeof(DIJOYSTATE)) {
            return DIERR_INVALIDPARAM;
        }
        FillGameState(*(DIJOYSTATE*)data);
        return DI_OK;
    }
    STDMETHODIMP GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD count, DWORD) override {
        if (count) {
            *count = 0;
        }
        return DIERR_NOTBUFFERED;
    }
    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT format) override {
        return (format && format->dwDataSize == sizeof(DIJOYSTATE)) ? DI_OK : DIERR_INVALIDPARAM;
    }
    STDMETHODIMP SetEventNotification(HANDLE) override { return DI_OK; }
    STDMETHODIMP SetCooperativeLevel(HWND window, DWORD) override {
        if (window) {
            g_window = window;
        }
        return DI_OK;
    }
    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) override { return DIERR_OBJECTNOTFOUND; }
    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEA info) override {
        if (!info || info->dwSize < sizeof(DIDEVICEINSTANCE_DX3A)) {
            return DIERR_INVALIDPARAM;
        }
        const DWORD size = info->dwSize;
        DIDEVICEINSTANCEA full;
        FillInstance(full);
        memcpy(info, &full, size < sizeof(full) ? size : sizeof(full));
        info->dwSize = size;
        return DI_OK;
    }
    STDMETHODIMP RunControlPanel(HWND, DWORD) override { return DI_OK; }
    STDMETHODIMP Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

    // IDirectInputDevice2A
    STDMETHODIMP CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT* out, LPUNKNOWN) override {
        if (out) {
            *out = 0;
        }
        return DIERR_UNSUPPORTED;
    }
    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override { return DI_OK; }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override {
        return DI_OK;
    }
    STDMETHODIMP Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    // The game treats anything but exactly 0 as a lost joystick.
    STDMETHODIMP Poll() override {
        ReadPad();
        return DI_OK;
    }
    STDMETHODIMP SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }

private:
    LONG m_refs = 1;
};

// ------------------------------------------------------------------ executable patches
//
// SouthPark.exe, unpacked in place; each site is checked before it is written. Applied when
// the game creates DirectInput: before that, the copy protection has not yet decrypted the code.

// The axis reader FUN_00473660 dispatches on the slot's axis bit through a byte table
// (indexed by slot - 1) and a jump table: entry 0 X scaled by Xspeed, 1 Y, 2 Z, 3 and 4 zero.
// Only Y and Z are unscaled, but the N64 stick handling wants four full +-80 axes -- walk,
// strafe, turn, look -- with no mouse-style accumulator in the way. So:
//   slot 1 (turn's default)    -> entry 2, Z unscaled;
//   slot 8 (look, see below)   -> entry 3, patched to read Rx;
//   slot 32 (strafe's default) -> entry 3, which reads Rz for it;
//   slots 16, 64, 128          -> entry 4, zero.
const unsigned int kAxisJumpEntry3 = 0x004736f0;
const unsigned int kAxisJumpEntry3Stock = 0x004736df;
const unsigned int kAxisByteTable = 0x004736f8;
const int kZeroSlots[] = {16, 64, 128};

// Entry 3, for slots 8 and 32: ESI holds slot - 1, ECX the DIJOYSTATE. Same epilogue as the
// stock entries.
__declspec(naked) void ReadExtraAxis() {
    __asm {
        cmp esi, 7
        jne rz
        mov eax, [ecx + 0x0c]      // DIJOFS_RX
        pop esi
        ret
    rz:
        mov eax, [ecx + 0x14]      // DIJOFS_RZ
        pop esi
        ret
    }
}

// AlwaysLook writes one value (MOV AL,2) to both the look slot (+0xa90) and the analog mask
// (+0xa91), in FUN_00489830 and in the menu's toggle (FUN_00489340). The look slot becomes 8
// (Rx), and the mask store `88 81 91 0a 00 00` (mov [ecx+0xa91], al) becomes `88 b1 ...`
// (mov [ecx+0xa91], dh) -- DH is 0, the high byte of the joystick index just loaded into EDX
// -- so look reaches the game through the N64 stick byte and not the accumulator, which has
// no spring and no dead zone and would let the view slide.
const unsigned int kAlwaysLookImmediates[] = {0x004898bb, 0x0048953b};
const unsigned int kAlwaysLookMaskStores[] = {0x004898cf, 0x00489556};
// FUN_00488430("AlwaysLook", 0): the default when the registry has no value (PUSH 0).
const unsigned int kAlwaysLookDefault = 0x0048989a;
// Button_0..15 defaults, as indexes into the action table at 0x4ca52c:
// 0 fire, 1 secondary fire, 2 jump, 5 next weapon, 6 previous weapon, 9 taunt, 10 center view,
// 11 none. Laid out in VirtualButton order.
const unsigned int kButtonDefaults = 0x004ca55c;
const DWORD kStockButtonDefaults[16] = {0, 2, 1, 6, 5, 9, 7, 8, 3, 4, 10, 10, 10, 10, 10, 10};
const DWORD kPadButtonDefaults[16] = {
    2,      // A       jump
    1,      // B       secondary fire
    0,      // X       fire
    9,      // Y       taunt
    6,      // LB      previous weapon
    5,      // RB      next weapon
    1,      // LT      secondary fire
    0,      // RT      fire
    11,     // Back
    11,     // Start   (Esc, through the keyboard)
    11,     // L3
    10,     // R3      center view
    11, 11, 11, 11,
};

// ---------------------------------------------------------------------------- menus
//
// The menus read a joystick through two functions, called once a frame and only while a menu
// owns input (FUN_0048e890, gated by FUN_0048e760): FUN_00476100 returns direction bits from
// the stick (1 up, 2 down, 4 left, 8 right) and FUN_004760c0 whether button 0 is down. Each
// fires every third frame while held, with no initial delay -- and the frame rate is uncapped,
// so a held stick races through a menu. The menus never read the hat and have no "back".
// Both are replaced: one step per press, then a timed repeat, from the D-pad or the left stick;
// A confirms and B goes back, each once per press.
const unsigned int kMenuDirections = 0x00476100;
const unsigned int kMenuConfirm = 0x004760c0;
const unsigned char kMenuDirectionsStock[6] = {0x51, 0xa1, 0x40, 0x0e, 0x7e, 0x00};   // push ecx; mov eax, [..]
const unsigned char kMenuConfirmStock[5] = {0xa1, 0x40, 0x0e, 0x7e, 0x00};           // mov eax, [0x007e0e40]
// FUN_004871d0(vk, type): the menus' own key event; type 1 is a key release, which is when
// Esc becomes "back".
const unsigned int kMenuKeyEvent = 0x004871d0;
typedef void(__cdecl* MenuKeyEventFn)(int vk, int type);

const DWORD kMenuRepeatDelayMs = 400;
const DWORD kMenuRepeatMs = 130;
const float kMenuStickPress = 0.5f;
const float kMenuStickRelease = 0.35f;

int g_menuDirection = 0;
DWORD g_menuNextRepeat = 0;
bool g_menuConfirmHeld = false;
bool g_menuBackHeld = false;

void RefreshPad() {
    if (GetTickCount() - g_lastRead > 15) {
        ReadPad();
    }
}

// The direction held: the D-pad first, else the left stick past half way (held until it
// drops back below a third, so it does not flicker at the threshold); one axis, the larger.
int HeldMenuDirection(const PadState& s) {
    if (s.up || s.down || s.left || s.right) {
        return s.up ? 1 : s.down ? 2 : s.left ? 4 : 8;
    }
    const float threshold = g_menuDirection ? kMenuStickRelease : kMenuStickPress;
    const float x = s.lx, y = s.ly;
    if (fabsf(x) < threshold && fabsf(y) < threshold) {
        return 0;
    }
    if (fabsf(y) >= fabsf(x)) {
        return y > 0 ? 1 : 2;
    }
    return x < 0 ? 4 : 8;
}

unsigned int __cdecl MenuDirections() {
    RefreshPad();
    const bool usable = g_haveState && !(g_inputBlocked && g_inputBlocked());
    const PadState none;
    const PadState& s = usable ? g_state : none;

    if (s.b && !g_menuBackHeld) {
        ((MenuKeyEventFn)GameBuild::Place(kMenuKeyEvent))(VK_ESCAPE, 1);
    }
    g_menuBackHeld = s.b;

    const int direction = HeldMenuDirection(s);
    const DWORD now = GetTickCount();
    unsigned int step = 0;
    if (direction != g_menuDirection) {
        step = direction;
        g_menuNextRepeat = now + kMenuRepeatDelayMs;
    } else if (direction && (int)(now - g_menuNextRepeat) >= 0) {
        step = direction;
        g_menuNextRepeat = now + kMenuRepeatMs;
    }
    g_menuDirection = direction;
    return step;
}

unsigned int __cdecl MenuConfirm() {
    RefreshPad();
    const bool held = g_haveState && g_state.a && !(g_inputBlocked && g_inputBlocked());
    const bool pressed = held && !g_menuConfirmHeld;
    g_menuConfirmHeld = held;
    return pressed ? 1 : 0;
}

// A jump at the start of the function: the caller's call lands in ours instead.
void Redirect(unsigned int function, void* replacement) {
    Write<unsigned char>(function, 0xe9);
    Write<int>(function + 1, (int)((unsigned int)replacement - (GameBuild::Place(function) + 5)));
}

// ------------------------------------------------------------------ look: pad or mouse
//
// The pad looks through the N64 stick byte, a position that springs back to level; mouse look
// through an accumulator that keeps the pitch where the mouse left it. The player code takes
// one or the other each frame -- the accumulator whenever its flag is set -- and with mouse
// look on, the mouse poller sets that flag every frame, moving or not (FUN_004731d0 calling
// FUN_00473170 at 0x4733c9). So the call goes through only while the mouse has the view: from
// the moment the right stick moves until the mouse moves again, the stick does.
const unsigned int kMouseLookCall = 0x004733c9;
const unsigned int kMouseLookPitch = 0x00473170;
typedef void(__cdecl* MouseLookPitchFn)(int delta, int player, int flag);

void __cdecl MouseLookPitch(int delta, int player, int flag) {
    if (!g_padLooks) {
        ((MouseLookPitchFn)GameBuild::Place(kMouseLookPitch))(delta, player, flag);
    }
}

bool GameMatches() {
    bool ok = Is<unsigned int>(kAxisJumpEntry3, kAxisJumpEntry3Stock) && Is<unsigned char>(kAxisByteTable + 31, 3) &&
              Is<unsigned char>(kAxisByteTable + 0, 0) && Is<unsigned char>(kAxisByteTable + 7, 3);
    ok = ok && Matches(kMenuDirections, kMenuDirectionsStock, sizeof(kMenuDirectionsStock)) &&
         Matches(kMenuConfirm, kMenuConfirmStock, sizeof(kMenuConfirmStock)) &&
         Is<unsigned char>(kMouseLookCall, 0xe8) &&
         Is<int>(kMouseLookCall + 1,
                 (int)(GameBuild::Place(kMouseLookPitch) - (GameBuild::Place(kMouseLookCall) + 5)));
    for (int slot : kZeroSlots) {
        ok = ok && Is<unsigned char>(kAxisByteTable + slot - 1, 3);
    }
    for (unsigned int immediate : kAlwaysLookImmediates) {
        ok = ok && Is<unsigned short>(immediate - 1, 0x02b0);    // b0 02
    }
    for (unsigned int store : kAlwaysLookMaskStores) {
        ok = ok && Is<unsigned short>(store, 0x8188) && Is<unsigned int>(store + 2, 0x00000a91);
    }
    ok = ok && Is<unsigned short>(kAlwaysLookDefault - 1, 0x006a);   // 6a 00
    const unsigned int buttons = GameBuild::Place(kButtonDefaults);
    ok = ok && !IsBadReadPtr((const void*)buttons, sizeof(kStockButtonDefaults)) &&
         memcmp((const void*)buttons, kStockButtonDefaults, sizeof(kStockButtonDefaults)) == 0;
    return ok;
}

void PatchGame() {
    LOG("pad: SouthPark.exe build %s", GameBuild::Name());
    if (!GameMatches()) {
        LOG("pad: not the expected SouthPark.exe -- the game keeps its own stick layout");
        return;
    }
    Write<unsigned int>(kAxisJumpEntry3, (unsigned int)&ReadExtraAxis);
    Write<unsigned char>(kAxisByteTable + 0, 2);
    for (int slot : kZeroSlots) {
        Write<unsigned char>(kAxisByteTable + slot - 1, 4);
    }
    for (unsigned int immediate : kAlwaysLookImmediates) {
        Write<unsigned char>(immediate, 8);
    }
    for (unsigned int store : kAlwaysLookMaskStores) {
        Write<unsigned char>(store + 1, 0xb1);
    }
    Write<unsigned char>(kAlwaysLookDefault, 1);
    for (int i = 0; i < 16; ++i) {
        Write<DWORD>(kButtonDefaults + 4 * i, kPadButtonDefaults[i]);
    }
    Redirect(kMenuDirections, (void*)&MenuDirections);
    Redirect(kMenuConfirm, (void*)&MenuConfirm);
    Write<int>(kMouseLookCall + 1,
               (int)((unsigned int)&MouseLookPitch - (GameBuild::Place(kMouseLookCall) + 5)));
    LOG("pad: game patched -- N64 stick turn and look, strafe, pad button defaults, menu navigation");
}

} // namespace

bool Pad::SetSetting(const char* section, const char* key, float value) {
    if (lstrcmpiA(section, "Input") != 0) {
        return false;
    }
    float* sensitivity = lstrcmpiA(key, "PadTurnSensitivity") == 0   ? &g_turnSensitivity
                         : lstrcmpiA(key, "PadLookSensitivity") == 0 ? &g_lookSensitivity
                                                                      : nullptr;
    if (!sensitivity) {
        return false;
    }
    *sensitivity = ValidSensitivity(value, *sensitivity);
    return true;
}

void Pad::Install(DirectInputCreateAFn realCreate, BlockedFn inputBlocked, LogFn log, const char* iniPath) {
    if (g_installed) {
        return;
    }
    g_installed = true;
    g_realCreate = realCreate;
    g_inputBlocked = inputBlocked;
    g_log = log;
    g_iniPath = iniPath;
    PatchGame();
}

bool Pad::Installed() {
    return g_installed;
}

bool Pad::IsOwnFactory(void* factory) {
    return factory && (factory == (void*)g_di || factory == g_scanFactory);
}

void Pad::OnMouseMoved() {
    g_padLooks = false;
}

bool Pad::IsOwnDevice(void* device) {
    return device && device == (void*)g_diPad;
}

void Pad::EnumerateJoysticks(void* callback, void* context) {
    DIDEVICEINSTANCEA instance;
    FillInstance(instance);
    ((LPDIENUMDEVICESCALLBACKA)callback)(&instance, context);
}

bool Pad::IsVirtualJoystick(const GUID* guid) {
    return guid && IsEqualGUID(*guid, kVirtualInstance);
}

HRESULT Pad::CreateVirtualJoystick(void** out) {
    if (!out) {
        return E_POINTER;
    }
    *out = static_cast<IDirectInputDevice2A*>(new VirtualJoystick());
    LOG("pad: the game has the virtual joystick");
    return DI_OK;
}

void Pad::AddKeyEvents(void* buffer, DWORD recordSize, DWORD* count, DWORD capacity) {
    if (GetTickCount() - g_lastRead > 30) {
        ReadPad();
    }
    if (g_escapeEvent == g_escapeDown || !buffer || !count || *count >= capacity ||
        recordSize < sizeof(DIDEVICEOBJECTDATA)) {
        return;
    }
    DIDEVICEOBJECTDATA* record = (DIDEVICEOBJECTDATA*)((BYTE*)buffer + *count * recordSize);
    memset(record, 0, recordSize);
    record->dwOfs = DIK_ESCAPE;
    record->dwData = g_escapeDown ? 0x80 : 0;
    record->dwTimeStamp = GetTickCount();
    record->dwSequence = ++g_keySequence;
    ++*count;
    g_escapeEvent = g_escapeDown;
}
