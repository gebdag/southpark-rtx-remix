#include "lookdev.h"

#include "config.h"
#include "device.h"
#include "log.h"
#include "texture.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <vector>

// Capture file (.spcap), little-endian, every field 4 bytes, read by the lookdev host.
//
//   header   magic 'SPCP', version, back buffer width, height, frame count
//   startup  InitializeDriver arguments 2..8; StartTextureEnumeration count and argument 3;
//            texture file name: length, bytes padded to 4 (length 0 = never set)
//   textures count, then per set: EnumerateTexture argument 2, size in bytes (0 = unreadable
//            at enumeration), the bytes padded to 4
//   state    SetZPlanes near and far (floats; both 0 when it was never called), the render flags (0xb6/0xb7 accumulated), then
//            a command count and per command: word0, word1, data size (16 for the 0x03 blocks
//            the original reads through word1, else 0), the data padded to 4. One command per
//            state opcode and sub-command: the last the engine sent before the capture began
//   records  type, payload size, payload -- until the end of the file
//
//   REC_FRAME_BEGIN / REC_FRAME_END   no payload
//   REC_ZPLANES                       near, far (floats)
//   REC_FLUSH_TEXTURES                no payload
//   REC_LIST                          engine address of the list, region count, then per
//                                     region: address, size, the bytes padded to 4
//
// A REC_LIST is one top-level GFXDLL_ProcessDisplayList call. Its regions are every byte of
// engine memory the interpreter read during that call, merged where they touch, as they were
// when the call returned. The engine reuses its buffers between calls, so memory is stored
// per call, not per frame.

namespace Lookdev {
bool g_recording = false;
bool g_trackState = false;
}

namespace {

enum Mode {
    ModeOff,
    ModeGame,
    ModeHost,
};

Mode g_mode = ModeOff;
bool g_plain = false;

const unsigned int kMagic = 0x50435053;   // "SPCP"
const unsigned int kVersion = 2;

enum RecordType {
    REC_FRAME_BEGIN = 1,
    REC_FRAME_END = 2,
    REC_ZPLANES = 3,
    REC_FLUSH_TEXTURES = 4,
    REC_LIST = 5,
};

bool EnvFlag(const char* name) {
    char value[8];
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    return n > 0 && n < sizeof(value) && value[0] == '1';
}

typedef std::vector<unsigned char> Bytes;

void PutU32(Bytes& out, unsigned int v) {
    const unsigned char* p = (const unsigned char*)&v;
    out.insert(out.end(), p, p + 4);
}

void PutF32(Bytes& out, float v) {
    unsigned int bits;
    memcpy(&bits, &v, 4);
    PutU32(out, bits);
}

void PutPadded(Bytes& out, const void* data, unsigned int bytes) {
    const unsigned char* p = (const unsigned char*)data;
    out.insert(out.end(), p, p + bytes);
    out.resize(out.size() + ((4 - (bytes & 3)) & 3), 0);
}

void CaptureDir(char* out, size_t size) {
    _snprintf(out, size, "%slookdev\\", Config::BaseDir());
    out[size - 1] = 0;
}

// ------------------------------------------------------------------- start-up exports

unsigned int g_initArgs[7];
unsigned int g_enumerationCount = 0;
unsigned int g_enumerationArg3 = 0;
char g_textureFileName[MAX_PATH];
std::vector<unsigned int> g_setArg2;

// ------------------------------------------------------------------- carried-over state

// Render flags as the original keeps them: 0xb7 sets and 0xb6 clears, both masked.
const unsigned int kRenderFlagMask = 0x000f3205;

struct StateCommand {
    unsigned int word0;
    unsigned int word1;
    unsigned int dataBytes;
    unsigned char data[16];
};

std::map<unsigned int, StateCommand> g_state;
unsigned int g_renderFlags = 0;
float g_zNear = 0.0f;
float g_zFar = 0.0f;

// Which commands are state, and under which key the last one is kept. Zero for everything
// else: geometry, matrices, sub-lists and the per-frame skip mask. Opcode 0x03 carries a raw
// pointer to the 16 bytes the original reads for sub-commands 0x80..0x88.
unsigned int StateKey(unsigned int word0) {
    const unsigned int op = word0 & 0xff;
    switch (op) {
    case 0x03: {
        const unsigned int sub = (word0 >> 8) & 0xff;
        return (sub >= 0x80 && sub <= 0x88) ? (op << 24) | sub : 0;
    }
    case 0xa6:
    case 0xa9:
        return 0xa9u << 24;                          // either one selects the texture set
    case 0xae:
        return (op << 24) | (word0 >> 16);           // sub-command in the high half
    case 0xb9:
    case 0xba:
        return (op << 24) | ((word0 >> 8) & 0xffff); // shift and length of the field written
    case 0xbc:
        return (op << 24) | ((word0 >> 24) << 8) | ((word0 >> 8) & 0xff);  // sub, index
    case 0xaf:
    case 0xf7:
    case 0xf8:
    case 0xf9:
    case 0xfa:
    case 0xfb:
    case 0xfd:
        return op << 24;
    default:
        return 0;
    }
}

void PutState(Bytes& out) {
    PutF32(out, g_zNear);
    PutF32(out, g_zFar);
    PutU32(out, g_renderFlags);
    PutU32(out, (unsigned int)g_state.size());
    for (std::map<unsigned int, StateCommand>::const_iterator it = g_state.begin();
         it != g_state.end(); ++it) {
        PutU32(out, it->second.word0);
        PutU32(out, it->second.word1);
        PutU32(out, it->second.dataBytes);
        PutPadded(out, it->second.data, it->second.dataBytes);
    }
}

// ------------------------------------------------------------------- capture

struct Range {
    unsigned int address;
    unsigned int bytes;
};

bool g_announced = false;
HWND g_gameWindow = 0;
HWND g_remixWindow = 0;
char g_pendingShots[64];

// The window's client area as it is on screen, written as a 24-bit BMP. Copied off the
// desktop because that works for both windows whatever presents into them.
bool SaveWindow(HWND hwnd, const char* path) {
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) {
        return false;
    }
    POINT origin = {0, 0};
    ClientToScreen(hwnd, &origin);
    const int w = rc.right, h = rc.bottom;
    HDC screen = GetDC(0);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bitmap);
    BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);
    SelectObject(mem, old);
    BITMAPINFOHEADER bi = {sizeof(bi)};
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    const int stride = (w * 3 + 3) & ~3;
    std::vector<unsigned char> pixels((size_t)stride * h);
    GetDIBits(mem, bitmap, 0, h, &pixels[0], (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(0, screen);
    BITMAPFILEHEADER bf = {};
    bf.bfType = 0x4d42;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    bf.bfSize = bf.bfOffBits + (DWORD)pixels.size();
    FILE* f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(&pixels[0], 1, pixels.size(), f);
    return fclose(f) == 0;
}

// <name>.bmp is the game window -- the original renderer, the ground truth -- and
// <name>_remix.bmp the Remix window.
void SaveWindows(const char* name) {
    char dir[MAX_PATH];
    char path[MAX_PATH];
    CaptureDir(dir, sizeof(dir));
    _snprintf(path, sizeof(path), "%s%s.bmp", dir, name);
    path[sizeof(path) - 1] = 0;
    const bool game = SaveWindow(g_gameWindow, path);
    _snprintf(path, sizeof(path), "%s%s_remix.bmp", dir, name);
    path[sizeof(path) - 1] = 0;
    const bool remix = SaveWindow(g_remixWindow, path);
    LogLine("    lookdev: pictures of \"%s\": game window %s, Remix window %s", name,
            game ? "saved" : "FAILED", remix ? "saved" : "FAILED");
}
bool g_capturing = false;
int g_framesLeft = 0;
int g_framesCaptured = 0;
char g_captureName[64];
Bytes g_header;       // magic .. state, built when the capture starts
Bytes g_records;
std::vector<Range> g_reads;
unsigned int g_listAddress = 0;
bool g_inList = false;

void PutRecord(RecordType type, const Bytes* payload) {
    PutU32(g_records, (unsigned int)type);
    PutU32(g_records, payload ? (unsigned int)payload->size() : 0u);
    if (payload) {
        g_records.insert(g_records.end(), payload->begin(), payload->end());
    }
}

// Names become file names; anything but [A-Za-z0-9_-] is replaced.
void SanitiseName(const char* in, char* out, size_t size) {
    size_t n = 0;
    for (; *in && n + 1 < size; ++in) {
        const char c = *in;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (c == '\r' || c == '\n') {
            break;
        }
        out[n++] = ok ? c : '_';
    }
    out[n] = 0;
    if (n == 0) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        _snprintf(out, size, "sp_%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond);
        out[size - 1] = 0;
    }
}

// The capture key, or a capture request file in <game>\lookdev. Returns true and fills the
// capture name when either asks for a capture.
bool CaptureRequested(char* name, size_t size) {
    char path[MAX_PATH];
    CaptureDir(path, sizeof(path));
    strncat(path, "capture.request", sizeof(path) - strlen(path) - 1);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        char requested[64] = {0};
        FILE* f = fopen(path, "rb");
        if (f) {
            fread(requested, 1, sizeof(requested) - 1, f);
            fclose(f);
        }
        DeleteFileA(path);
        SanitiseName(requested, name, size);
        return true;
    }
    if (GetAsyncKeyState(Config::LookdevCaptureKey()) & 1) {
        SanitiseName("", name, size);
        return true;
    }
    return false;
}

void PutStartup(Bytes& out) {
    for (int i = 0; i < 7; ++i) {
        PutU32(out, g_initArgs[i]);
    }
    PutU32(out, g_enumerationCount);
    PutU32(out, g_enumerationArg3);
    const unsigned int length = (unsigned int)strlen(g_textureFileName);
    PutU32(out, length);
    PutPadded(out, g_textureFileName, length);
}

void PutTextures(Bytes& out) {
    const int count = Texture::Count();
    PutU32(out, (unsigned int)count);
    for (int i = 0; i < count; ++i) {
        unsigned int bytes = 0;
        const void* blob = Texture::Blob(i, &bytes);
        PutU32(out, i < (int)g_setArg2.size() ? g_setArg2[i] : 0u);
        PutU32(out, bytes);
        if (blob) {
            PutPadded(out, blob, bytes);
        }
    }
}

void StartCapture(const char* name) {
    lstrcpynA(g_captureName, name, sizeof(g_captureName));
    g_header.clear();
    g_records.clear();
    g_framesLeft = Config::LookdevCaptureFrames();
    g_framesCaptured = 0;

    PutU32(g_header, kMagic);
    PutU32(g_header, kVersion);
    PutU32(g_header, (unsigned int)Device::Width());
    PutU32(g_header, (unsigned int)Device::Height());
    PutU32(g_header, 0);   // frame count, patched when the capture is written
    PutStartup(g_header);
    PutTextures(g_header);
    PutState(g_header);

    g_capturing = true;
    LogLine("    lookdev: capturing %d frame(s) as \"%s\" (%d texture sets)",
            g_framesLeft, g_captureName, Texture::Count());
}

void WriteCapture() {
    char dir[MAX_PATH];
    CaptureDir(dir, sizeof(dir));
    CreateDirectoryA(dir, 0);

    char path[MAX_PATH];
    char temp[MAX_PATH];
    _snprintf(path, sizeof(path), "%s%s.spcap", dir, g_captureName);
    path[sizeof(path) - 1] = 0;
    _snprintf(temp, sizeof(temp), "%s.partial", path);
    temp[sizeof(temp) - 1] = 0;

    const unsigned int frames = (unsigned int)g_framesCaptured;
    memcpy(&g_header[16], &frames, 4);

    // Written aside and renamed, so anything waiting for the file never reads half of it.
    FILE* f = fopen(temp, "wb");
    bool ok = f != 0;
    if (f) {
        ok = fwrite(&g_header[0], 1, g_header.size(), f) == g_header.size();
        if (ok && !g_records.empty()) {
            ok = fwrite(&g_records[0], 1, g_records.size(), f) == g_records.size();
        }
        ok = (fclose(f) == 0) && ok;
    }
    if (ok) {
        ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    }
    LogLine("    lookdev: %s %s (%u frames, %u KB)", ok ? "wrote" : "FAILED to write", path,
            frames, (unsigned int)((g_header.size() + g_records.size()) / 1024));
    g_header.clear();
    g_header.shrink_to_fit();
    g_records.clear();
    g_records.shrink_to_fit();
}

} // namespace

// ======================================================================= mode

void Lookdev::DetectMode() {
    g_plain = EnvFlag("SP_LOOKDEV_PLAIN");
    if (EnvFlag("SP_LOOKDEV_HOST")) {
        g_mode = ModeHost;
    } else if (EnvFlag("SP_LOOKDEV")) {
        g_mode = ModeGame;
    }
    g_trackState = g_mode == ModeGame;
}

void Lookdev::TrackCommand(unsigned int word0, unsigned int word1) {
    const unsigned int op = word0 & 0xff;
    if (op == 0xb7) {
        g_renderFlags |= word1 & kRenderFlagMask;
        return;
    }
    if (op == 0xb6) {
        g_renderFlags &= ~(word1 & kRenderFlagMask);
        return;
    }
    const unsigned int key = StateKey(word0);
    if (!key) {
        return;
    }
    StateCommand& c = g_state[key];
    c.word0 = word0;
    c.word1 = word1;
    c.dataBytes = 0;
    if (op == 0x03) {
        c.dataBytes = sizeof(c.data);
        memcpy(c.data, (const void*)word1, sizeof(c.data));
    }
}

bool Lookdev::IsHost()       { return g_mode == ModeHost; }
bool Lookdev::IsTee()        { return g_mode == ModeGame; }
bool Lookdev::UsePlainD3D9() { return g_plain; }

const char* Lookdev::LogFileName() {
    return g_mode == ModeHost ? "Video_D3D9.lookdev.log" : "Video_D3D9.log";
}

// ======================================================================= start-up exports

void Lookdev::OnInitializeDriver(HWND window, const unsigned int args[7]) {
    g_gameWindow = window;
    memcpy(g_initArgs, args, sizeof(g_initArgs));
}

// ======================================================================= the Remix window

namespace {

LRESULT CALLBACK RemixWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Closing it would leave the game drawing into a destroyed window; it goes with the game.
    if (msg == WM_CLOSE) {
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

} // namespace

HWND Lookdev::CreateRemixWindow() {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = RemixWindowProc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "SouthParkLookdevRemix";
    RegisterClassA(&wc);

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT frame = {0, 0, Config::LookdevRemixWindowWidth(), Config::LookdevRemixWindowHeight()};
    AdjustWindowRect(&frame, style, FALSE);
    int x = Config::LookdevRemixWindowX();
    int y = Config::LookdevRemixWindowY();
    RECT game;
    if ((x < 0 || y < 0) && g_gameWindow && GetWindowRect(g_gameWindow, &game)) {
        if (x < 0) x = game.right;
        if (y < 0) y = game.top;
    }
    g_remixWindow = CreateWindowExA(0, wc.lpszClassName, "South Park - RTX Remix (lookdev)", style,
                                    x < 0 ? CW_USEDEFAULT : x, y < 0 ? CW_USEDEFAULT : y,
                                    frame.right - frame.left, frame.bottom - frame.top, 0, 0,
                                    wc.hInstance, 0);
    if (g_remixWindow) {
        // Shown without taking focus: the game window keeps the keyboard.
        ShowWindow(g_remixWindow, SW_SHOWNOACTIVATE);
    }
    LogLine("    lookdev: Remix window %p, %dx%d at %d,%d", g_remixWindow,
            Config::LookdevRemixWindowWidth(), Config::LookdevRemixWindowHeight(), x, y);
    return g_remixWindow;
}

void Lookdev::DestroyRemixWindow() {
    if (g_remixWindow) {
        DestroyWindow(g_remixWindow);
        g_remixWindow = 0;
    }
}

void Lookdev::PumpRemixWindow() {
    MSG msg;
    while (g_remixWindow && PeekMessageA(&msg, g_remixWindow, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void Lookdev::OnSetTextureFileName(const char* name) {
    lstrcpynA(g_textureFileName, name ? name : "", sizeof(g_textureFileName));
}

void Lookdev::OnStartTextureEnumeration(unsigned int count, unsigned int arg3) {
    g_enumerationCount = count;
    g_enumerationArg3 = arg3;
    g_setArg2.clear();
}

void Lookdev::OnEnumerateTexture(unsigned int arg2) {
    g_setArg2.push_back(arg2);
}

// ======================================================================= frames

void Lookdev::OnBeginScene() {
    if (g_mode != ModeGame) {
        return;
    }
    if (g_pendingShots[0]) {
        // A frame after the capture, so both windows have presented it.
        SaveWindows(g_pendingShots);
        g_pendingShots[0] = 0;
    }
    if (!g_announced) {
        g_announced = true;
        char dir[MAX_PATH];
        CaptureDir(dir, sizeof(dir));
        CreateDirectoryA(dir, 0);
        LogLine("    lookdev: capture armed -- key 0x%02X or %scapture.request, %d frame(s)",
                Config::LookdevCaptureKey(), dir, Config::LookdevCaptureFrames());
    }
    if (!g_capturing) {
        char name[64];
        if (!CaptureRequested(name, sizeof(name))) {
            return;
        }
        StartCapture(name);
    }
    PutRecord(REC_FRAME_BEGIN, 0);
    g_recording = true;
}

void Lookdev::OnEndScene() {
    if (!g_capturing) {
        return;
    }
    g_recording = false;
    PutRecord(REC_FRAME_END, 0);
    if (g_framesCaptured++ == 0) {
        // Pictures of the first frame from both windows, taken once they have shown it.
        lstrcpynA(g_pendingShots, g_captureName, sizeof(g_pendingShots));
    }
    if (--g_framesLeft <= 0) {
        g_capturing = false;
        WriteCapture();
    }
}

void Lookdev::OnSetZPlanes(float zNear, float zFar) {
    g_zNear = zNear;
    g_zFar = zFar;
    if (!g_recording) {
        return;
    }
    Bytes payload;
    PutF32(payload, zNear);
    PutF32(payload, zFar);
    PutRecord(REC_ZPLANES, &payload);
}

void Lookdev::OnFlushTextureCache() {
    if (g_recording) {
        PutRecord(REC_FLUSH_TEXTURES, 0);
    }
}

void Lookdev::BeginList(const void* list) {
    if (!g_recording) {
        return;
    }
    g_inList = true;
    g_listAddress = (unsigned int)list;
    g_reads.clear();
}

void Lookdev::RecordRead(const void* address, unsigned int bytes) {
    if (!g_inList || bytes == 0) {
        return;
    }
    Range r = {(unsigned int)address, bytes};
    g_reads.push_back(r);
}

void Lookdev::EndList() {
    if (!g_inList) {
        return;
    }
    g_inList = false;
    std::sort(g_reads.begin(), g_reads.end(),
              [](const Range& a, const Range& b) { return a.address < b.address; });
    std::vector<Range> merged;
    for (size_t i = 0; i < g_reads.size(); ++i) {
        const unsigned int end = g_reads[i].address + g_reads[i].bytes;
        if (!merged.empty() && g_reads[i].address <= merged.back().address + merged.back().bytes) {
            Range& last = merged.back();
            if (end > last.address + last.bytes) {
                last.bytes = end - last.address;
            }
            continue;
        }
        merged.push_back(g_reads[i]);
    }
    Bytes payload;
    PutU32(payload, g_listAddress);
    PutU32(payload, (unsigned int)merged.size());
    for (size_t i = 0; i < merged.size(); ++i) {
        PutU32(payload, merged[i].address);
        PutU32(payload, merged[i].bytes);
        PutPadded(payload, (const void*)merged[i].address, merged[i].bytes);
    }
    PutRecord(REC_LIST, &payload);
}
