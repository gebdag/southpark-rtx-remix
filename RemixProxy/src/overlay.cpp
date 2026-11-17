#include "overlay.h"
#include "lights.h"
#include "log.h"
#include "settings.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vector>

namespace {

struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR colour;
    float u, v;
};
const DWORD kVertexFormat = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// ------------------------------------------------------------------------ window and input

HWND g_window = 0;
WNDPROC g_previousProc = 0;
UINT g_overlayMessage = 0;
bool g_open = false;

int g_backBufferWidth = 0;
int g_backBufferHeight = 0;
RECT g_destination = {};
bool g_haveDestination = false;

float g_mouseX = -1.0f;
float g_mouseY = -1.0f;
bool g_mouseDown = false;
bool g_mousePressed = false;      // went down since the last frame; consumed by one widget
unsigned int g_dragging = 0;      // the slider being dragged
int g_wheel = 0;                  // wheel movement since the last frame, in WHEEL_DELTA units

void SetOpen(bool open) {
    g_open = open;
    g_dragging = 0;
    g_wheel = 0;
    g_mouseDown = false;
    g_mousePressed = false;
    // The input shim stops the game reading its devices while this says the overlay is open.
    PostMessageA(g_window, g_overlayMessage, open ? 1 : 0, 0);
    Log("overlay %s", open ? "opened" : "closed");
}

// Client coordinates to back buffer pixels: Present may place the back buffer inside the window
// with bars around it.
void TrackMouse(LPARAM position) {
    const float cx = (float)(short)LOWORD(position);
    const float cy = (float)(short)HIWORD(position);
    RECT target = g_destination;
    if (!g_haveDestination) {
        GetClientRect(g_window, &target);
    }
    const float width = (float)(target.right - target.left);
    const float height = (float)(target.bottom - target.top);
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    g_mouseX = (cx - target.left) * g_backBufferWidth / width;
    g_mouseY = (cy - target.top) * g_backBufferHeight / height;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const bool repeat = (lp & (1 << 30)) != 0;
    if (message == WM_KEYDOWN && (wp == VK_F4 || (wp == VK_ESCAPE && g_open))) {
        if (!repeat) {
            SetOpen(wp == VK_F4 ? !g_open : false);
        }
        return 0;
    }
    if (message == WM_KEYUP && (wp == VK_F4 || (wp == VK_ESCAPE && g_open))) {
        return 0;
    }
    if (g_open) {
        switch (message) {
        case WM_MOUSEMOVE:
            TrackMouse(lp);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            TrackMouse(lp);
            g_mouseDown = true;
            g_mousePressed = true;
            SetCapture(window);
            return 0;
        case WM_LBUTTONUP:
            TrackMouse(lp);
            g_mouseDown = false;
            ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL:
            g_wheel += GET_WHEEL_DELTA_WPARAM(wp);
            return 0;
        // The rest of the mouse and the keyboard belong to the overlay too. System keys still
        // pass, so Alt+Enter and the Remix menu's Alt+X keep working.
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            return 0;
        }
    }
    return CallWindowProcA(g_previousProc, window, message, wp, lp);
}

// ------------------------------------------------------------------------------ font atlas
//
// A monospaced font rendered once through GDI into a texture: 16 columns of character cells,
// printable ASCII from 32, and cell 127 filled solid so boxes and text share one texture and
// one draw.

const int kAtlasColumns = 16;
const int kFirstChar = 32;
const int kSolidChar = 127;

IDirect3DTexture9* g_font = 0;
int g_fontPixels = 0;
int g_cellWidth = 0;
int g_cellHeight = 0;
int g_atlasWidth = 0;
int g_atlasHeight = 0;

bool BuildFont(IDirect3DDevice9* device, int pixels) {
    if (g_font) {
        g_font->Release();
        g_font = 0;
    }
    HDC dc = CreateCompatibleDC(0);
    HFONT font = CreateFontA(-pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    TEXTMETRICA metrics;
    GetTextMetricsA(dc, &metrics);
    SIZE m;
    GetTextExtentPoint32A(dc, "M", 1, &m);
    g_cellWidth = m.cx;
    g_cellHeight = metrics.tmHeight;
    const int rows = (kSolidChar - kFirstChar + kAtlasColumns) / kAtlasColumns;
    g_atlasWidth = g_cellWidth * kAtlasColumns;
    g_atlasHeight = g_cellHeight * rows;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = g_atlasWidth;
    info.bmiHeader.biHeight = -g_atlasHeight;     // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = 0;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, 0, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    memset(bits, 0, (size_t)g_atlasWidth * g_atlasHeight * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    for (int c = kFirstChar; c < kSolidChar; ++c) {
        const char ch = (char)c;
        TextOutA(dc, ((c - kFirstChar) % kAtlasColumns) * g_cellWidth,
                 ((c - kFirstChar) / kAtlasColumns) * g_cellHeight, &ch, 1);
    }
    GdiFlush();
    const unsigned int* pixelsIn = (const unsigned int*)bits;
    const int solidX = ((kSolidChar - kFirstChar) % kAtlasColumns) * g_cellWidth;
    const int solidY = ((kSolidChar - kFirstChar) / kAtlasColumns) * g_cellHeight;

    bool ok = SUCCEEDED(device->CreateTexture(g_atlasWidth, g_atlasHeight, 1, 0, D3DFMT_A8R8G8B8,
                                              D3DPOOL_MANAGED, &g_font, 0));
    D3DLOCKED_RECT locked;
    if (ok && SUCCEEDED(g_font->LockRect(0, &locked, 0, 0))) {
        for (int y = 0; y < g_atlasHeight; ++y) {
            unsigned int* row = (unsigned int*)((unsigned char*)locked.pBits + y * locked.Pitch);
            for (int x = 0; x < g_atlasWidth; ++x) {
                const unsigned int p = pixelsIn[y * g_atlasWidth + x];
                unsigned int coverage = p & 0xff;
                if (((p >> 8) & 0xff) > coverage) coverage = (p >> 8) & 0xff;
                if (((p >> 16) & 0xff) > coverage) coverage = (p >> 16) & 0xff;
                const bool solid = x >= solidX && x < solidX + g_cellWidth &&
                                   y >= solidY && y < solidY + g_cellHeight;
                row[x] = ((solid ? 255u : coverage) << 24) | 0x00ffffffu;
            }
        }
        g_font->UnlockRect(0);
    } else {
        ok = false;
    }

    SelectObject(dc, oldBitmap);
    SelectObject(dc, oldFont);
    DeleteObject(bitmap);
    DeleteObject(font);
    DeleteDC(dc);
    g_fontPixels = ok ? pixels : 0;
    Log("overlay font: %d px, cells %dx%d%s", pixels, g_cellWidth, g_cellHeight, ok ? "" : " -- FAILED");
    return ok;
}

// ------------------------------------------------------------------------------ geometry

// Three layers, drawn in order: markers in the scene, then the panel's background, then its
// content -- the background's size is only known once the content has been laid out.
std::vector<Vertex> g_markers;
std::vector<Vertex> g_panelBack;
std::vector<Vertex> g_panelFront;
std::vector<Vertex>* g_layer = &g_markers;

// The panel's scrolled content is clipped to its visible area: quads are cut on the CPU, and a
// widget outside it does not see the mouse.
bool g_clipping = false;
float g_clip[4] = {};    // x0, y0, x1, y1

void CellUv(int c, float* u0, float* v0, float* u1, float* v1) {
    const int index = c - kFirstChar;
    const float x = (float)((index % kAtlasColumns) * g_cellWidth);
    const float y = (float)((index / kAtlasColumns) * g_cellHeight);
    *u0 = x / g_atlasWidth;
    *v0 = y / g_atlasHeight;
    *u1 = (x + g_cellWidth) / g_atlasWidth;
    *v1 = (y + g_cellHeight) / g_atlasHeight;
}

// Pre-transformed pixel centres sit on integer coordinates in D3D9; the half-pixel shift lands
// each texel exactly on a pixel.
void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, D3DCOLOR c) {
    if (g_clipping) {
        const float cx0 = x0 > g_clip[0] ? x0 : g_clip[0];
        const float cy0 = y0 > g_clip[1] ? y0 : g_clip[1];
        const float cx1 = x1 < g_clip[2] ? x1 : g_clip[2];
        const float cy1 = y1 < g_clip[3] ? y1 : g_clip[3];
        if (cx0 >= cx1 || cy0 >= cy1) {
            return;
        }
        // Texture coordinates follow the cut. A solid rectangle has u0 == u1, which stays put.
        const float du = x1 > x0 ? (u1 - u0) / (x1 - x0) : 0.0f;
        const float dv = y1 > y0 ? (v1 - v0) / (y1 - y0) : 0.0f;
        u1 = u0 + (cx1 - x0) * du; u0 = u0 + (cx0 - x0) * du;
        v1 = v0 + (cy1 - y0) * dv; v0 = v0 + (cy0 - y0) * dv;
        x0 = cx0; y0 = cy0; x1 = cx1; y1 = cy1;
    }
    const Vertex a = {x0 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, c, u0, v0};
    const Vertex b = {x1 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, c, u1, v0};
    const Vertex d = {x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, c, u1, v1};
    const Vertex e = {x0 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, c, u0, v1};
    g_layer->push_back(a); g_layer->push_back(b); g_layer->push_back(d);
    g_layer->push_back(a); g_layer->push_back(d); g_layer->push_back(e);
}

void SolidUv(float* u, float* v) {
    float u0, v0, u1, v1;
    CellUv(kSolidChar, &u0, &v0, &u1, &v1);
    *u = (u0 + u1) * 0.5f;
    *v = (v0 + v1) * 0.5f;
}

// ------------------------------------------------------------------------------ panel layout

const D3DCOLOR kPanelColour   = D3DCOLOR_ARGB(225, 16, 20, 30);
const D3DCOLOR kTitleColour   = D3DCOLOR_ARGB(255, 32, 54, 92);
const D3DCOLOR kTextColour    = D3DCOLOR_ARGB(255, 235, 238, 245);
const D3DCOLOR kDimColour     = D3DCOLOR_ARGB(255, 140, 150, 170);
const D3DCOLOR kAccentColour  = D3DCOLOR_ARGB(255, 120, 180, 255);
const D3DCOLOR kFrameColour   = D3DCOLOR_ARGB(255, 60, 80, 115);
const D3DCOLOR kControlColour = D3DCOLOR_ARGB(255, 28, 36, 54);
const D3DCOLOR kHoverColour   = D3DCOLOR_ARGB(255, 44, 62, 96);
const D3DCOLOR kFillColour    = D3DCOLOR_ARGB(255, 70, 120, 200);

const int kPanelChars = 74;      // the panel's content width, in characters

// The panel stays where it was dragged, and is never taller than the picture: what does not fit
// scrolls, with the wheel or the bar on its right.
float g_panelX = -1.0f;   // unplaced until the first frame open
float g_panelY = 0.0f;
bool g_movingPanel = false;
float g_grabX = 0.0f, g_grabY = 0.0f;
float g_scroll = 0.0f;
float g_contentHeight = 0.0f;   // laid out last frame
bool g_draggingScrollbar = false;
float g_viewTop = 0.0f, g_viewHeight = 0.0f;
float g_contentX = 0.0f;
float g_contentRight = 0.0f;
float g_rowTop = 0.0f;
float g_rowBottom = 0.0f;
float g_cursorX = 0.0f;
bool g_sameLine = false;

float Pad() { return g_cellHeight * 0.4f; }

// Reserves a w x h box for the next widget, on the current row or a new one.
void Place(float w, float h, float* x, float* y) {
    if (g_sameLine) {
        *x = g_cursorX + g_cellWidth;
        *y = g_rowTop;
        if (g_rowTop + h > g_rowBottom) g_rowBottom = g_rowTop + h;
    } else {
        *x = g_contentX;
        *y = g_rowBottom + Pad() * 0.6f;
        g_rowTop = *y;
        g_rowBottom = *y + h;
    }
    g_sameLine = false;
    g_cursorX = *x + w;
}

bool Hovered(float x0, float y0, float x1, float y1) {
    if (g_clipping && !(g_mouseX >= g_clip[0] && g_mouseX < g_clip[2] && g_mouseY >= g_clip[1] &&
                        g_mouseY < g_clip[3])) {
        return false;
    }
    return g_mouseX >= x0 && g_mouseX < x1 && g_mouseY >= y0 && g_mouseY < y1;
}

bool Clicked(float x0, float y0, float x1, float y1) {
    if (g_mousePressed && !g_dragging && Hovered(x0, y0, x1, y1)) {
        g_mousePressed = false;
        return true;
    }
    return false;
}

unsigned int Id(const char* label) {
    unsigned int h = 2166136261u;
    for (const char* p = label; *p; ++p) {
        h = (h ^ (unsigned char)*p) * 16777619u;
    }
    return h ? h : 1;
}

// What a label shows: anything after "##" only keeps two widgets with the same text apart.
int VisibleLength(const char* label) {
    const char* hidden = strstr(label, "##");
    return hidden ? (int)(hidden - label) : (int)strlen(label);
}

void TextN(float x, float y, D3DCOLOR colour, const char* text, int length) {
    for (int i = 0; i < length; ++i) {
        int c = (unsigned char)text[i];
        if (c < kFirstChar || c >= kSolidChar) {
            c = '?';
        }
        float u0, v0, u1, v1;
        CellUv(c, &u0, &v0, &u1, &v1);
        const float cx = x + i * g_cellWidth;
        Quad(cx, y, cx + g_cellWidth, y + g_cellHeight, u0, v0, u1, v1, colour);
    }
}

float PanelWidth() { return kPanelChars * g_cellWidth + Pad() * 4.0f; }
float TitleHeight() { return g_cellHeight + Pad() * 2.0f; }
float ScrollRange() { return g_contentHeight > g_viewHeight ? g_contentHeight - g_viewHeight : 0.0f; }

// The scrollbar's track: in the panel's right margin, beside the content.
void ScrollTrack(float* x0, float* x1) {
    *x1 = g_panelX + PanelWidth() - Pad() * 0.5f;
    *x0 = *x1 - Pad();
}

void ClampPanel() {
    const float w = PanelWidth();
    g_panelX = g_panelX > g_backBufferWidth - w ? g_backBufferWidth - w : g_panelX;
    g_panelY = g_panelY > g_backBufferHeight - TitleHeight() ? g_backBufferHeight - TitleHeight() : g_panelY;
    g_panelX = g_panelX < 0.0f ? 0.0f : g_panelX;
    g_panelY = g_panelY < 0.0f ? 0.0f : g_panelY;
}

void BeginPanel() {
    if (g_panelX < 0.0f) {
        g_panelX = g_panelY = g_cellHeight * 1.5f;
    }
    const float titleHeight = TitleHeight();
    // Dragged by its title bar.
    if (g_mousePressed && !g_dragging &&
        g_mouseX >= g_panelX && g_mouseX < g_panelX + PanelWidth() &&
        g_mouseY >= g_panelY && g_mouseY < g_panelY + titleHeight) {
        g_movingPanel = true;
        g_grabX = g_mouseX - g_panelX;
        g_grabY = g_mouseY - g_panelY;
        g_mousePressed = false;
    }
    if (g_movingPanel) {
        if (g_mouseDown) {
            g_panelX = g_mouseX - g_grabX;
            g_panelY = g_mouseY - g_grabY;
        } else {
            g_movingPanel = false;
        }
    }
    ClampPanel();

    // What fits below the title, down to the bottom of the picture.
    const float margin = (float)g_cellHeight * 0.5f;
    g_viewTop = g_panelY + titleHeight;
    const float room = g_backBufferHeight - margin - g_viewTop;
    g_viewHeight = g_contentHeight < room ? g_contentHeight : room;
    g_viewHeight = g_viewHeight > (float)g_cellHeight ? g_viewHeight : (float)g_cellHeight;

    // The wheel scrolls three lines a notch; the bar is dragged.
    g_scroll -= g_wheel / (float)WHEEL_DELTA * 3.0f * (g_cellHeight + Pad() * 0.6f);
    g_wheel = 0;
    float trackX0, trackX1;
    ScrollTrack(&trackX0, &trackX1);
    if (ScrollRange() > 0.0f && g_mousePressed && !g_dragging && g_mouseX >= trackX0 &&
        g_mouseX < trackX1 && g_mouseY >= g_viewTop && g_mouseY < g_viewTop + g_viewHeight) {
        g_draggingScrollbar = true;
        g_mousePressed = false;
    }
    if (g_draggingScrollbar) {
        if (g_mouseDown) {
            const float thumb = g_viewHeight * g_viewHeight / g_contentHeight;
            const float t = (g_mouseY - g_viewTop - thumb * 0.5f) / (g_viewHeight - thumb);
            g_scroll = t * ScrollRange();
        } else {
            g_draggingScrollbar = false;
        }
    }
    g_scroll = g_scroll > ScrollRange() ? ScrollRange() : g_scroll;
    g_scroll = g_scroll < 0.0f ? 0.0f : g_scroll;

    g_layer = &g_panelFront;
    g_contentX = g_panelX + Pad() * 2.0f;
    g_contentRight = g_contentX + kPanelChars * g_cellWidth;
    Ui::Text(g_contentX, g_panelY + Pad(), kTextColour, "South Park");
    const char* hint = "drag to move   F4 / Esc closes";
    Ui::Text(g_contentRight - (float)strlen(hint) * g_cellWidth, g_panelY + Pad(), kDimColour, hint);

    g_clipping = true;
    g_clip[0] = g_panelX;
    g_clip[1] = g_viewTop;
    g_clip[2] = g_panelX + PanelWidth();
    g_clip[3] = g_viewTop + g_viewHeight;
    g_rowTop = g_rowBottom = g_viewTop - g_scroll;
    g_sameLine = false;
}

void EndPanel() {
    g_contentHeight = g_rowBottom + Pad() * 2.0f - (g_viewTop - g_scroll);
    g_clipping = false;
    const float right = g_panelX + PanelWidth();
    const float bottom = g_viewTop + g_viewHeight;
    g_layer = &g_panelBack;
    Ui::Rect(g_panelX, g_panelY, right, bottom, kPanelColour);
    Ui::Rect(g_panelX, g_panelY, right, g_viewTop, kTitleColour);
    if (ScrollRange() > 0.0f) {
        float x0, x1;
        ScrollTrack(&x0, &x1);
        const float thumb = g_viewHeight * g_viewHeight / g_contentHeight;
        const float top = g_viewTop + (g_viewHeight - thumb) * (g_scroll / ScrollRange());
        g_layer = &g_panelFront;
        Ui::Rect(x0, g_viewTop, x1, bottom, kControlColour);
        Ui::Rect(x0, top, x1, top + thumb, g_draggingScrollbar ? kAccentColour : kFrameColour);
    }
    g_layer = &g_markers;
    // The panel keeps the mouse to itself: a press on it is not a press anywhere else.
    if (g_mouseX >= g_panelX && g_mouseX < right && g_mouseY >= g_panelY && g_mouseY < bottom) {
        g_mousePressed = false;
    }
}

// ------------------------------------------------------------------------------ drawing

void Draw(IDirect3DDevice9* device) {
    IDirect3DStateBlock9* saved = 0;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved))) {
        return;
    }
    D3DVIEWPORT9 viewport = {0, 0, (DWORD)g_backBufferWidth, (DWORD)g_backBufferHeight, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetVertexShader(0);
    device->SetPixelShader(0);
    device->SetFVF(kVertexFormat);
    device->SetTexture(0, g_font);
    device->SetTexture(1, 0);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(0, D3DTSS_CONSTANT, 0);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const std::vector<Vertex>* layers[] = {&g_markers, &g_panelBack, &g_panelFront};
    for (const auto* layer : layers) {
        if (!layer->empty()) {
            device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(layer->size() / 3), layer->data(), sizeof(Vertex));
        }
    }
    saved->Apply();
    saved->Release();
}

} // namespace

// =============================================================================== Overlay

void Overlay::Attach(HWND window) {
    if (!window || window == g_window) {
        return;
    }
    g_window = window;
    g_overlayMessage = RegisterWindowMessageA("UWM_SOUTHPARK_OVERLAY_ACTIVE");
    g_previousProc = (WNDPROC)SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)&WindowProc);
    Log("overlay attached to window %p (F4)", (void*)window);
}

void Overlay::SetBackBufferSize(int width, int height) {
    g_backBufferWidth = width;
    g_backBufferHeight = height;
}

void Overlay::OnPresent(const RECT* destination) {
    g_haveDestination = destination != 0;
    if (destination) {
        g_destination = *destination;
    }
}

bool Overlay::IsOpen() {
    return g_open;
}

void Overlay::Render(IDirect3DDevice9* device) {
    if (g_backBufferWidth <= 0 || g_backBufferHeight <= 0) {
        return;
    }
    // 15 px at 1080 lines, scaled with the picture, rebuilt when the back buffer changes.
    int pixels = (int)(15.0f * g_backBufferHeight / 1080.0f + 0.5f);
    if (pixels < 13) {
        pixels = 13;
    }
    if (pixels != g_fontPixels && !BuildFont(device, pixels)) {
        return;
    }

    g_markers.clear();
    g_panelBack.clear();
    g_panelFront.clear();
    g_layer = &g_markers;
    Lights::DrawMarkers(g_backBufferWidth, g_backBufferHeight);
    if (g_open) {
        BeginPanel();
        Settings::DrawPanel();
        Lights::DrawPanel();
        EndPanel();
    }
    g_mousePressed = false;
    if (!g_mouseDown) {
        g_dragging = 0;
    }
    if (!g_markers.empty() || !g_panelFront.empty()) {
        Draw(device);
    }
}

// =============================================================================== Ui

float Ui::Scale() { return g_fontPixels / 15.0f; }
float Ui::LineHeight() { return (float)g_cellHeight; }
float Ui::CharWidth() { return (float)g_cellWidth; }

void Ui::Rect(float x0, float y0, float x1, float y1, D3DCOLOR colour) {
    float u, v;
    SolidUv(&u, &v);
    Quad(x0, y0, x1, y1, u, v, u, v, colour);
}

void Ui::Ring(float cx, float cy, float radius, float thickness, D3DCOLOR colour) {
    float u, v;
    SolidUv(&u, &v);
    const int segments = 32;
    const float inner = radius - thickness * 0.5f;
    const float outer = radius + thickness * 0.5f;
    for (int i = 0; i < segments; ++i) {
        const float a0 = 6.2831853f * i / segments;
        const float a1 = 6.2831853f * (i + 1) / segments;
        const Vertex p0 = {cx + cosf(a0) * inner, cy + sinf(a0) * inner, 0, 1, colour, u, v};
        const Vertex p1 = {cx + cosf(a0) * outer, cy + sinf(a0) * outer, 0, 1, colour, u, v};
        const Vertex p2 = {cx + cosf(a1) * outer, cy + sinf(a1) * outer, 0, 1, colour, u, v};
        const Vertex p3 = {cx + cosf(a1) * inner, cy + sinf(a1) * inner, 0, 1, colour, u, v};
        g_layer->push_back(p0); g_layer->push_back(p1); g_layer->push_back(p2);
        g_layer->push_back(p0); g_layer->push_back(p2); g_layer->push_back(p3);
    }
}

void Ui::Text(float x, float y, D3DCOLOR colour, const char* text) {
    TextN(x, y, colour, text, (int)strlen(text));
}

void Ui::Heading(const char* text) {
    float x, y;
    g_rowBottom += Pad() * 1.2f;
    Place(g_contentRight - g_contentX, (float)g_cellHeight, &x, &y);
    Text(x, y, kAccentColour, text);
    const float lineX = x + (strlen(text) + 1) * g_cellWidth;
    const float lineY = y + g_cellHeight * 0.55f;
    Rect(lineX, lineY, g_contentRight, lineY + (Scale() > 1.5f ? 2.0f : 1.0f), kFrameColour);
}

static void LabelV(D3DCOLOR colour, const char* fmt, va_list args) {
    char text[256];
    _vsnprintf(text, sizeof(text), fmt, args);
    text[sizeof(text) - 1] = 0;
    float x, y;
    Place((float)strlen(text) * g_cellWidth, (float)g_cellHeight, &x, &y);
    Ui::Text(x, y, colour, text);
}

void Ui::Label(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LabelV(kTextColour, fmt, args);
    va_end(args);
}

void Ui::LabelDim(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LabelV(kDimColour, fmt, args);
    va_end(args);
}

bool Ui::Checkbox(const char* label, bool* value) {
    const float box = (float)g_cellHeight;
    const int length = VisibleLength(label);
    float x, y;
    Place(box + g_cellWidth + length * g_cellWidth, box, &x, &y);
    const bool hovered = Hovered(x, y, g_cursorX, y + box);
    Rect(x, y, x + box, y + box, kFrameColour);
    const float inset = box * 0.12f;
    Rect(x + inset, y + inset, x + box - inset, y + box - inset, hovered ? kHoverColour : kControlColour);
    if (*value) {
        const float mark = box * 0.28f;
        Rect(x + mark, y + mark, x + box - mark, y + box - mark, kAccentColour);
    }
    TextN(x + box + g_cellWidth, y, kTextColour, label, length);
    if (Clicked(x, y, g_cursorX, y + box)) {
        *value = !*value;
        return true;
    }
    return false;
}

bool Ui::Button(const char* label) {
    const int length = VisibleLength(label);
    const float padX = (float)g_cellWidth;
    const float w = length * g_cellWidth + padX * 2.0f;
    const float h = g_cellHeight + Pad();
    float x, y;
    Place(w, h, &x, &y);
    const bool hovered = Hovered(x, y, x + w, y + h);
    Rect(x, y, x + w, y + h, kFrameColour);
    Rect(x + 1, y + 1, x + w - 1, y + h - 1, hovered ? kHoverColour : kControlColour);
    TextN(x + padX, y + Pad() * 0.5f, kTextColour, label, length);
    return Clicked(x, y, x + w, y + h);
}

bool Ui::Slider(const char* label, float* value, float minimum, float maximum, const char* format,
                bool logarithmic) {
    const unsigned int id = Id(label);
    const int length = VisibleLength(label);
    const float trackWidth = 30.0f * g_cellWidth;
    const float h = (float)g_cellHeight + Pad() * 0.5f;
    float x, y;
    Place(trackWidth + g_cellWidth + length * g_cellWidth, h, &x, &y);

    const bool canLog = logarithmic && minimum > 0.0f;
    float t = canLog ? logf(*value / minimum) / logf(maximum / minimum)
                     : (*value - minimum) / (maximum - minimum);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

    const bool hovered = Hovered(x, y, x + trackWidth, y + h);
    if (g_mousePressed && hovered && !g_dragging) {
        g_dragging = id;
        g_mousePressed = false;
    }
    bool changed = false;
    if (g_dragging == id && g_mouseDown) {
        float nt = (g_mouseX - x) / trackWidth;
        nt = nt < 0.0f ? 0.0f : (nt > 1.0f ? 1.0f : nt);
        const float nv = canLog ? minimum * powf(maximum / minimum, nt) : minimum + nt * (maximum - minimum);
        changed = nv != *value;
        *value = nv;
        t = nt;
    }

    Rect(x, y, x + trackWidth, y + h, kFrameColour);
    Rect(x + 1, y + 1, x + trackWidth - 1, y + h - 1,
         (hovered || g_dragging == id) ? kHoverColour : kControlColour);
    Rect(x + 1, y + 1, x + 1 + (trackWidth - 2) * t, y + h - 1, kFillColour);
    char text[64];
    _snprintf(text, sizeof(text), format, *value);
    text[sizeof(text) - 1] = 0;
    const float textX = x + (trackWidth - strlen(text) * g_cellWidth) * 0.5f;
    Text(textX, y + Pad() * 0.25f, kTextColour, text);
    TextN(x + trackWidth + g_cellWidth, y + Pad() * 0.25f, kTextColour, label, length);
    return changed;
}

void Ui::Swatch(D3DCOLOR colour) {
    const float size = (float)g_cellHeight;
    float x, y;
    Place(size * 2.0f, size, &x, &y);
    Rect(x, y, x + size * 2.0f, y + size, kFrameColour);
    Rect(x + 1, y + 1, x + size * 2.0f - 1, y + size - 1, colour | 0xff000000u);
}

void Ui::SameLine() {
    g_sameLine = true;
}
