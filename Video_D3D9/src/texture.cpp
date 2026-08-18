#include "texture.h"
#include "log.h"

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <d3d9.h>
#include "device.h"

namespace {

const int kMaxSets = 4096;

void* g_sets[kMaxSets];
IDirect3DTexture9* g_textures[kMaxSets];
bool  g_decodeFailed[kMaxSets];
int   g_count = 0;
int   g_paletteLogged = 0;

// Payload layout:
//   chunk0[0]  format: 0 = 8 bits/pixel with a 256-entry palette, 1 = 4 bits/pixel with 16
//   chunk0[2]  width  (0 means 256)
//   chunk0[3]  height (0 means 256)
//   chunk1     nested blob; sub-chunk 0 is the palettised image
//   chunk2     nested blob; sub-chunk 0 is the palette, 16-bit entries
//   chunk3     ASCII name
const unsigned char* SubChunk(const unsigned char* nested, unsigned int index,
                              unsigned int nestedBytes, unsigned int* outSize) {
    if (nestedBytes < 12) {
        return 0;
    }
    const unsigned int count = *(const unsigned int*)nested;
    if (count == 0 || count > 32 || 4 + (count + 1) * 4 > nestedBytes || index >= count) {
        return 0;
    }
    const unsigned int off = *(const unsigned int*)(nested + 4 + index * 4);
    const unsigned int end = *(const unsigned int*)(nested + 8 + index * 4);
    if (end <= off || end > nestedBytes) {
        return 0;
    }
    if (outSize) {
        *outSize = end - off;
    }
    return nested + off;
}

const unsigned char* ChunkAt(const unsigned char* base, unsigned int size, unsigned int index,
                             unsigned int* outSize) {
    const unsigned int count = *(const unsigned int*)base;
    if (index >= count) {
        return 0;
    }
    const unsigned int off = *(const unsigned int*)(base + 4 + index * 4);
    const unsigned int end = *(const unsigned int*)(base + 8 + index * 4);
    if (end <= off || end > size) {
        return 0;
    }
    if (outSize) {
        *outSize = end - off;
    }
    return base + off;
}

// Palette entries are stored byte-swapped; once swapped the layout is RGBA5551, with opacity
// in the low bit. The original does exactly this -- it byte-swaps each entry and tests the
// stored 0x0100 bit for transparency. Guessing at ARGB1555 and RGB565 both produced wrong
// colours; this reading makes snow white and wood brown, checked against decoded samples.
inline unsigned int ExpandPaletteEntry(unsigned short stored) {
    const unsigned short c = (unsigned short)(((stored & 0x00ff) << 8) | (stored >> 8));
    unsigned int r = (c >> 11) & 0x1f;
    unsigned int g = (c >> 6) & 0x1f;
    unsigned int b = (c >> 1) & 0x1f;
    const unsigned int a = (c & 1) ? 0xffu : 0x00u;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Blob layout:
//   +0x00  chunk count N
//   +0x04  N+1 chunk offsets, relative to the blob base; the last one is the total size
//   +off0  chunk 0, whose bytes +2 and +3 are the texture width and height
//
// The engine enumerates every texture through a single reused staging buffer, so the pointer
// it passes is only valid for the duration of the call. The original copied each set into
// its win32.tex store for exactly this reason; here they are copied to the heap instead.
const unsigned int kMaxBlobSize = 8u << 20;

unsigned int BlobSize(const void* blob) {
    const unsigned char* base = (const unsigned char*)blob;
    if (IsBadReadPtr(base, 8)) {
        return 0;
    }
    const unsigned int count = *(const unsigned int*)base;
    if (count == 0 || count > 64) {
        return 0;
    }
    if (IsBadReadPtr(base, 4 + (count + 1) * 4)) {
        return 0;
    }
    const unsigned int total = *(const unsigned int*)(base + 4 + count * 4);
    if (total < 8 || total > kMaxBlobSize) {
        return 0;
    }
    return total;
}

// Chunk 0 carries the source dimensions as bytes at +2 and +3. The engine reads the
// *processed* dimensions from +6 and +7 instead -- those arrive zeroed and the original
// fills them in while building its texture store. Leaving them zero makes the engine divide
// by zero (SouthPark.exe+0x28ce2), so the copy is completed here.
void CompleteHeader(void* blob) {
    unsigned char* base = (unsigned char*)blob;
    const unsigned int size = BlobSize(blob);
    if (size == 0) {
        return;
    }
    const unsigned int offset = *(const unsigned int*)(base + 4);
    if (offset + 8 > size) {
        return;
    }
    unsigned char* chunk = base + offset;
    if (chunk[6] == 0) {
        chunk[6] = chunk[2];
    }
    if (chunk[7] == 0) {
        chunk[7] = chunk[3];
    }
}

// Dimensions are single bytes at chunk 0 +2 and +3, matching the byte read the original
// performs during enumeration.
bool ReadDimensions(const void* blob, int* outWidth, int* outHeight) {
    const unsigned char* base = (const unsigned char*)blob;
    const unsigned int size = BlobSize(blob);
    if (size == 0) {
        return false;
    }
    const unsigned int offset = *(const unsigned int*)(base + 4);
    if (offset + 4 > size) {
        return false;
    }
    const unsigned char* chunk = base + offset;
    if (outWidth)  *outWidth  = chunk[2];
    if (outHeight) *outHeight = chunk[3];
    return true;
}

} // namespace

void Texture::StartEnumeration() {
    ReleaseAll();
    LogLine("    texture enumeration started");
}

void Texture::Enumerate(void* blob) {
    if (!blob || g_count >= kMaxSets) {
        return;
    }
    const unsigned int size = BlobSize(blob);
    if (size == 0) {
        LogLine("    texture set %d has an unreadable header -- skipped", g_count);
        g_sets[g_count++] = 0;
        return;
    }
    void* copy = malloc(size);
    if (!copy) {
        LogLine("    out of memory copying texture set %d (%u bytes)", g_count, size);
        g_sets[g_count++] = 0;
        return;
    }
    memcpy(copy, blob, size);
    CompleteHeader(copy);
    g_sets[g_count++] = copy;
}

// Chunks 1 and 2 are themselves blobs with the same count+offsets layout. Their sizes against
// the set's dimensions give the bits per pixel.
static void DescribeSet(int index, const void* blob) {
    const unsigned char* base = (const unsigned char*)blob;
    const unsigned int size = BlobSize(blob);
    if (size == 0) {
        return;
    }
    const unsigned int count = *(const unsigned int*)base;
    const unsigned char* c0 = base + *(const unsigned int*)(base + 4);
    int w = c0[2] ? c0[2] : 256;
    int h = c0[3] ? c0[3] : 256;
    LogLine("    set[%d] %p size=%u chunks=%u  hdr=%02x %02x %02x %02x %02x %02x %02x %02x  %dx%d",
            index, blob, size, count,
            c0[0], c0[1], c0[2], c0[3], c0[4], c0[5], c0[6], c0[7], w, h);

    for (unsigned int c = 1; c < count && c < 4; ++c) {
        const unsigned int off = *(const unsigned int*)(base + 4 + c * 4);
        const unsigned int end = *(const unsigned int*)(base + 8 + c * 4);
        if (off >= size || end > size || end <= off) {
            continue;
        }
        const unsigned char* sub = base + off;
        const unsigned int subBytes = end - off;
        const unsigned int subCount = *(const unsigned int*)sub;
        LogLine("      chunk%u: %u bytes, nested count=%u", c, subBytes, subCount);
        if (subCount == 0 || subCount > 32 || 4 + (subCount + 1) * 4 > subBytes) {
            continue;
        }
        for (unsigned int k = 0; k < subCount && k < 6; ++k) {
            const unsigned int so = *(const unsigned int*)(sub + 4 + k * 4);
            const unsigned int se = *(const unsigned int*)(sub + 8 + k * 4);
            if (se <= so) {
                continue;
            }
            const unsigned int px = (unsigned int)(w * h);
            LogLine("        sub[%u] %u bytes  (%u px -> %.2f bits/px)",
                    k, se - so, px, px ? (double)(se - so) * 8.0 / (double)px : 0.0);
        }
    }
}

// Writes a whole texture-set blob to disk, for decoding offline.
void Texture::DumpSet(int index, const char* path) {
    if (index < 0 || index >= g_count || !g_sets[index]) {
        return;
    }
    const unsigned int size = BlobSize(g_sets[index]);
    if (size == 0) {
        return;
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fwrite(g_sets[index], 1, size, f);
    fclose(f);
    LogLine("    dumped set %d (%u bytes) -> %s", index, size, path);
}

// The last chunk holds an ASCII name, the only stable way to tell which set is which.
static const char* SetName(const void* blob) {
    const unsigned char* base = (const unsigned char*)blob;
    const unsigned int size = BlobSize(blob);
    if (size == 0) {
        return "";
    }
    const unsigned int count = *(const unsigned int*)base;
    if (count < 2) {
        return "";
    }
    const unsigned int off = *(const unsigned int*)(base + 4 + (count - 1) * 4);
    const unsigned int end = *(const unsigned int*)(base + 4 + count * 4);
    if (off >= size || end > size || end <= off) {
        return "";
    }
    const char* p = (const char*)(base + off);
    for (unsigned int i = 0; i < end - off; ++i) {
        if (p[i] == 0) {
            return p;
        }
    }
    return "";
}

void Texture::FinishEnumeration() {
    int bytes = 0;
    int valid = 0;
    for (int i = 0; i < g_count; ++i) {
        if (g_sets[i]) {
            ++valid;
            bytes += (int)BlobSize(g_sets[i]);
        }
    }
    LogLine("    texture enumeration finished: %d sets (%d copied, %d KB)",
            g_count, valid, bytes / 1024);
    // A few fixed sets, written as probe_NNN.bin on every run as samples for checking the
    // decoder offline.
    static const int kProbe[] = { 281, 227, 17, 0, 1, 189 };
    for (unsigned int k = 0; k < sizeof(kProbe) / sizeof(kProbe[0]); ++k) {
        const int i = kProbe[k];
        if (i >= g_count || !g_sets[i]) {
            continue;
        }
        char path[64];
        _snprintf(path, sizeof(path), "probe_%03d.bin", i);
        path[sizeof(path) - 1] = 0;
        DumpSet(i, path);
    }
}

void* Texture::GetSet(int index, int* outWidth, int* outHeight) {
    if (index < 0 || index >= g_count || !g_sets[index]) {
        if (outWidth)  *outWidth = 0;
        if (outHeight) *outHeight = 0;
        return 0;
    }
    void* blob = g_sets[index];
    if (outWidth)  *outWidth = 0;
    if (outHeight) *outHeight = 0;
    ReadDimensions(blob, outWidth, outHeight);
    return blob;
}

IDirect3DTexture9* Texture::GetTexture(int index) {
    if (index < 0 || index >= g_count || !g_sets[index] || g_decodeFailed[index]) {
        return 0;
    }
    if (g_textures[index]) {
        return g_textures[index];
    }

    IDirect3DDevice9* dev = Device::Get();
    const unsigned char* base = (const unsigned char*)g_sets[index];
    const unsigned int size = BlobSize(base);
    if (!dev || size == 0) {
        g_decodeFailed[index] = true;
        return 0;
    }

    unsigned int hdrSize = 0, pixChunkSize = 0, palChunkSize = 0;
    const unsigned char* hdr = ChunkAt(base, size, 0, &hdrSize);
    const unsigned char* pixChunk = ChunkAt(base, size, 1, &pixChunkSize);
    const unsigned char* palChunk = ChunkAt(base, size, 2, &palChunkSize);
    if (!hdr || hdrSize < 4 || !pixChunk || !palChunk) {
        g_decodeFailed[index] = true;
        return 0;
    }

    const int w = hdr[2] ? hdr[2] : 256;
    const int h = hdr[3] ? hdr[3] : 256;

    unsigned int pixSize = 0, palSize = 0;
    const unsigned char* pixelData = SubChunk(pixChunk, 0, pixChunkSize, &pixSize);
    const unsigned char* palette = SubChunk(palChunk, 0, palChunkSize, &palSize);
    if (!pixelData || !palette) {
        g_decodeFailed[index] = true;
        return 0;
    }

    // Bit depth is derived from the payload rather than the format byte: formats 1 and 3 are
    // both 4bpp, and deriving it covers any format the enumeration turns up.
    const unsigned int pixels = (unsigned int)(w * h);
    int bpp;
    if (pixSize >= pixels) {
        bpp = 8;
    } else if (pixSize * 2 >= pixels) {
        bpp = 4;
    } else {
        LogLine("    texture %d: payload too small (%u for %ux%u)", index, pixSize, w, h);
        g_decodeFailed[index] = true;
        return 0;
    }
    if (palSize < 2) {
        g_decodeFailed[index] = true;
        return 0;
    }



    IDirect3DTexture9* tex = 0;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, 0))) {
        g_decodeFailed[index] = true;
        return 0;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, 0, 0))) {
        tex->Release();
        g_decodeFailed[index] = true;
        return 0;
    }

    const unsigned short* pal = (const unsigned short*)palette;
    const unsigned int palEntries = palSize / 2;
    for (int y = 0; y < h; ++y) {
        unsigned int* dst = (unsigned int*)((unsigned char*)lr.pBits + (size_t)y * lr.Pitch);
        for (int x = 0; x < w; ++x) {
            unsigned int idx;
            if (bpp == 4) {
                // The high nibble is the even pixel, matching the original's inner loop.
                const unsigned int n = (unsigned int)y * w + x;
                const unsigned char byte = pixelData[n >> 1];
                idx = (n & 1) ? (byte & 0x0f) : (byte >> 4);
            } else {
                idx = pixelData[(size_t)y * w + x];
            }
            dst[x] = (idx < palEntries) ? ExpandPaletteEntry(pal[idx]) : 0u;
        }
    }
    tex->UnlockRect(0);

    g_textures[index] = tex;
    return tex;
}

int Texture::Count() {
    return g_count;
}

void Texture::ReleaseAll() {
    for (int i = 0; i < g_count; ++i) {
        if (g_textures[i]) {
            g_textures[i]->Release();
            g_textures[i] = 0;
        }
        if (g_sets[i]) {
            free(g_sets[i]);
            g_sets[i] = 0;
        }
        g_decodeFailed[i] = false;
    }
    g_count = 0;
}
