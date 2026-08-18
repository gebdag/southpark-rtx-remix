#include "pcx.h"
#include "device.h"
#include "log.h"

#include <windows.h>
#include <d3d9.h>
#include <string.h>
#include <stdio.h>

namespace {

// The engine's image object, as consumed by the original renderer. Only the fields the
// plugin actually reads are named; the object is allocated and owned by the engine.
struct EngineImageFile {
    void** vtable;      // +0x00  vtable[1] is Load(const char*), __thiscall
    int    width;       // +0x04
    int    height;      // +0x08
    int    unknown0c;   // +0x0c
    void*  indexed;     // +0x10  8-bit indices
    unsigned char* rgba;// +0x14  32-bit pixels, byte order R,G,B,A
};

// vtable[1] loads a named file from the engine's archive; vtable[3] makes a blank image.
typedef void (__thiscall *ImageLoadFn)(void* self, const char* name);
typedef void (__thiscall *ImageCreateFn)(void* self, int width, int height);

const int kMaxImages = 2048;

struct Entry {
    char* name;
    int   refcount;
    EngineImageFile* image;
    IDirect3DTexture9* texture;
    int   texWidth;
    int   texHeight;
    // Dirty rect in pixels, half-open. Empty when x0 >= x1.
    int   dx0, dy0, dx1, dy1;
    bool  synthetic;   // made by Create(); never matched by name in Load()
};

Entry g_entries[kMaxImages];
Pcx::AllocFn g_alloc = 0;
Pcx::DeallocFn g_dealloc = 0;
unsigned int g_syntheticCounter = 0;

int FindFreeSlot() {
    for (int i = 0; i < kMaxImages; ++i) {
        if (g_entries[i].refcount == 0) {
            return i;
        }
    }
    return -1;
}

bool Valid(int h) {
    return h >= 0 && h < kMaxImages && g_entries[h].refcount > 0 && g_entries[h].image != 0;
}

int NextPow2(int v) {
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

// The original derives per-pixel alpha from luminance and writes it back into the engine's
// buffer, so anything the engine later reads sees the same values it always has.
void FillAlphaFromLuminance(EngineImageFile* img) {
    const int count = img->width * img->height;
    unsigned char* p = img->rgba;
    for (int i = 0; i < count; ++i, p += 4) {
        p[3] = (unsigned char)(((int)p[0] + (int)p[1] + (int)p[2]) / 3);
    }
}

// Engine pixels are R,G,B,A byte order; D3DFMT_A8R8G8B8 is B,G,R,A in memory.
void UploadRect(Entry& e, int x0, int y0, int x1, int y1) {
    if (!e.texture || !e.image || x0 >= x1 || y0 >= y1) {
        return;
    }
    RECT r;
    r.left = x0; r.top = y0; r.right = x1; r.bottom = y1;

    D3DLOCKED_RECT lr;
    if (FAILED(e.texture->LockRect(0, &lr, &r, 0))) {
        return;
    }
    const int srcW = e.image->width;
    for (int y = y0; y < y1; ++y) {
        const unsigned char* src = e.image->rgba + ((size_t)y * srcW + x0) * 4;
        unsigned int* dst = (unsigned int*)((unsigned char*)lr.pBits + (size_t)(y - y0) * lr.Pitch);
        for (int x = x0; x < x1; ++x, src += 4) {
            *dst++ = ((unsigned int)src[3] << 24) | ((unsigned int)src[0] << 16) |
                     ((unsigned int)src[1] << 8)  |  (unsigned int)src[2];
        }
    }
    e.texture->UnlockRect(0);
}

bool CreateTexture(Entry& e) {
    IDirect3DDevice9* dev = Device::Get();
    if (!dev) {
        LogLine("    PCX: no device, cannot create texture");
        return false;
    }
    const int w = e.image->width;
    const int h = e.image->height;

    HRESULT hr = dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &e.texture, 0);
    if (SUCCEEDED(hr)) {
        e.texWidth = w;
        e.texHeight = h;
        return true;
    }
    // Fall back to power-of-two when the driver refuses NPOT; UVs get scaled to compensate.
    const int pw = NextPow2(w);
    const int ph = NextPow2(h);
    hr = dev->CreateTexture(pw, ph, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &e.texture, 0);
    if (FAILED(hr)) {
        LogLine("    PCX: CreateTexture %dx%d failed hr=0x%08lx", w, h, (unsigned long)hr);
        e.texture = 0;
        return false;
    }
    e.texWidth = pw;
    e.texHeight = ph;
    LogLine("    PCX: %dx%d padded to %dx%d", w, h, pw, ph);
    return true;
}

void FreeEntry(Entry& e) {
    if (e.texture) {
        e.texture->Release();
        e.texture = 0;
    }
    if (e.image && g_dealloc) {
        g_dealloc(e.image);
    }
    e.image = 0;
    if (e.name) {
        free(e.name);
        e.name = 0;
    }
    e.refcount = 0;
    e.synthetic = false;
    e.dx0 = e.dy0 = e.dx1 = e.dy1 = 0;
}

} // namespace

void Pcx::SetAlloc(AllocFn fn)     { g_alloc = fn; }
void Pcx::SetDealloc(DeallocFn fn) { g_dealloc = fn; }

int Pcx::Load(const char* name) {
    if (!name) {
        return -1;
    }
    for (int i = 0; i < kMaxImages; ++i) {
        if (g_entries[i].refcount > 0 && !g_entries[i].synthetic &&
            g_entries[i].name && _stricmp(g_entries[i].name, name) == 0) {
            ++g_entries[i].refcount;
            return i;
        }
    }
    if (!g_alloc) {
        LogLine("    PCX: no allocate callback registered, cannot load \"%s\"", name);
        return -1;
    }

    const int slot = FindFreeSlot();
    if (slot < 0) {
        LogLine("    PCX: table full");
        return -1;
    }

    Entry& e = g_entries[slot];
    memset(&e, 0, sizeof(e));

    e.image = (EngineImageFile*)g_alloc();
    if (!e.image) {
        LogLine("    PCX: allocate callback returned NULL for \"%s\"", name);
        return -1;
    }
    // vtable[1] loads the named file out of the engine's archive.
    ((ImageLoadFn)e.image->vtable[1])(e.image, name);

    if (e.image->width <= 0 || e.image->height <= 0 || !e.image->rgba) {
        LogLine("    PCX: \"%s\" loaded empty (w=%d h=%d rgba=%p)",
                name, e.image->width, e.image->height, (void*)e.image->rgba);
        if (g_dealloc) {
            g_dealloc(e.image);
        }
        e.image = 0;
        return -1;
    }

    FillAlphaFromLuminance(e.image);

    e.refcount = 1;
    e.name = _strdup(name);
    e.dx0 = 0; e.dy0 = 0; e.dx1 = e.image->width; e.dy1 = e.image->height;

    if (CreateTexture(e)) {
        UploadRect(e, 0, 0, e.image->width, e.image->height);
    }

    LogLine("    PCX[%d] \"%s\" %dx%d", slot, name, e.image->width, e.image->height);
    return slot;
}

int Pcx::Create(int width, int height) {
    if (!g_alloc || width <= 0 || height <= 0) {
        LogLine("    PCX: cannot create %dx%d", width, height);
        return -1;
    }
    const int slot = FindFreeSlot();
    if (slot < 0) {
        LogLine("    PCX: table full");
        return -1;
    }

    Entry& e = g_entries[slot];
    memset(&e, 0, sizeof(e));

    e.image = (EngineImageFile*)g_alloc();
    if (!e.image) {
        LogLine("    PCX: allocate callback returned NULL for Create");
        return -1;
    }
    ((ImageCreateFn)e.image->vtable[3])(e.image, width, height);

    if (e.image->width <= 0 || e.image->height <= 0 || !e.image->rgba) {
        LogLine("    PCX: Create(%d,%d) produced an empty image", width, height);
        if (g_dealloc) {
            g_dealloc(e.image);
        }
        e.image = 0;
        return -1;
    }

    // The original names created images with a running counter, keeping them distinct from
    // any real filename so the by-name lookup in Load can never collide with one.
    char name[16];
    _snprintf(name, sizeof(name), "%08u", ++g_syntheticCounter);
    name[sizeof(name) - 1] = 0;

    e.refcount = 1;
    e.name = _strdup(name);
    e.synthetic = true;
    e.dx0 = 0; e.dy0 = 0; e.dx1 = e.image->width; e.dy1 = e.image->height;

    if (CreateTexture(e)) {
        UploadRect(e, 0, 0, e.image->width, e.image->height);
    }

    LogLine("    PCX[%d] created %dx%d", slot, e.image->width, e.image->height);
    return slot;
}

void Pcx::Unload(int handle) {
    if (handle < 0 || handle >= kMaxImages || g_entries[handle].refcount <= 0) {
        return;
    }
    if (--g_entries[handle].refcount == 0) {
        FreeEntry(g_entries[handle]);
    }
}

int Pcx::Width(int handle)  { return Valid(handle) ? g_entries[handle].image->width  : 0; }
int Pcx::Height(int handle) { return Valid(handle) ? g_entries[handle].image->height : 0; }

void* Pcx::RgbaPixels(int handle)    { return Valid(handle) ? g_entries[handle].image->rgba    : 0; }
void* Pcx::IndexedPixels(int handle) { return Valid(handle) ? g_entries[handle].image->indexed : 0; }

void Pcx::MarkDirty(int handle, int x0, int y0, int x1, int y1) {
    if (!Valid(handle)) {
        return;
    }
    Entry& e = g_entries[handle];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > e.image->width)  x1 = e.image->width;
    if (y1 > e.image->height) y1 = e.image->height;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (e.dx0 >= e.dx1) {
        e.dx0 = x0; e.dy0 = y0; e.dx1 = x1; e.dy1 = y1;
        return;
    }
    if (x0 < e.dx0) e.dx0 = x0;
    if (y0 < e.dy0) e.dy0 = y0;
    if (x1 > e.dx1) e.dx1 = x1;
    if (y1 > e.dy1) e.dy1 = y1;
}

void Pcx::Update(int handle) {
    if (!Valid(handle)) {
        return;
    }
    Entry& e = g_entries[handle];
    if (!e.texture && !CreateTexture(e)) {
        return;
    }
    // An Update with no recorded dirty rect means the engine rewrote the whole image.
    int x0 = e.dx0, y0 = e.dy0, x1 = e.dx1, y1 = e.dy1;
    if (x0 >= x1) {
        x0 = 0; y0 = 0; x1 = e.image->width; y1 = e.image->height;
    }
    UploadRect(e, x0, y0, x1, y1);
    e.dx0 = e.dy0 = e.dx1 = e.dy1 = 0;
}

IDirect3DTexture9* Pcx::Texture(int handle) {
    return Valid(handle) ? g_entries[handle].texture : 0;
}

float Pcx::UScale(int handle) {
    if (!Valid(handle) || g_entries[handle].texWidth == 0) {
        return 1.0f;
    }
    return (float)g_entries[handle].image->width / (float)g_entries[handle].texWidth;
}

float Pcx::VScale(int handle) {
    if (!Valid(handle) || g_entries[handle].texHeight == 0) {
        return 1.0f;
    }
    return (float)g_entries[handle].image->height / (float)g_entries[handle].texHeight;
}

void Pcx::ReleaseAll() {
    for (int i = 0; i < kMaxImages; ++i) {
        if (g_entries[i].refcount > 0) {
            FreeEntry(g_entries[i]);
        }
    }
}
