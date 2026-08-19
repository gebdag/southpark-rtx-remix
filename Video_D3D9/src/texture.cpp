#include "texture.h"
#include "config.h"
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

// A set is not one image. Animated sets hold a run of frames in chunk 1 and may hold
// several palettes in chunk 2, and the display list picks a pair per draw. Each decoded
// (image, palette) pair is cached separately; almost every set only ever uses one.
struct Variant {
    unsigned short pix, pal;
    unsigned short transparentPercent;   // texels the alpha test would discard
    unsigned int meanColour;             // mean of the opaque texels, 0x00RRGGBB
    IDirect3DTexture9* tex;
};
// Deep enough for the widest sets (SMOKE01 has 30 frames, BOOM05 27 palettes). A
// set that still overflows recycles its oldest entry rather than decoding afresh every draw
// -- at 20,000 flushes a second, a decode that is not cached is also a texture that is
// never released.
const int kMaxVariants = 64;
struct SetCache {
    Variant v[kMaxVariants];
    int n;
    int next;   // round-robin victim once the cache is full
};
SetCache g_cache[kMaxSets];
bool  g_decodeFailed[kMaxSets];
int   g_decodeFailuresLogged = 0;
// Filled by the decoder and consumed by the caller that cached the variant.
unsigned short g_lastDecodeTransparentPercent = 0;
unsigned int g_lastDecodeMeanColour = 0;
int   g_decodeFailures = 0;
bool  g_isSky[kMaxSets];
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

// How many sub-chunks a nested blob holds. Zero when it is not a nested blob at all --
// which is a real case: the palette-less formats store a nested blob with a count of 0.
unsigned int SubChunkCount(const unsigned char* nested, unsigned int nestedBytes) {
    if (!nested || nestedBytes < 8) {
        return 0;
    }
    const unsigned int count = *(const unsigned int*)nested;
    if (count > 32 || 4 + (count + 1) * 4 > nestedBytes) {
        return 0;
    }
    return count;
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

// Palette entries are always stored byte-swapped, and after the swap two different layouts
// are in use. The original tells them apart by the gate it stashes at texState+0x9c
// (FUN_1000cf90): 0x8000 for formats 0 and 1, 0xc000 for format 3. That gate is carried into
// the texture builder, which at 0x1000609b raises a flag when the tile is colour-indexed and
// the gate is 0xc000 -- format 3 and nothing else.
//
//   RGBA5551 -- one bit of alpha. World surfaces: opaque, or cut out along a hard edge.
//               R = bits 15..11, G = 10..6, B = 5..1, A = bit 0.
//
//   IA16     -- intensity in the high byte, alpha in the low byte. Format 3 only, which is
//               the effect atlas: BSPURT01, BSPLSH01, EXPLOD01, SMOKE01, PUFF01, the poo
//               splats, the sky. These carry no colour of their own at all -- they are a
//               greyscale shape and a soft edge, and the colour arrives from the combiner's
//               constant colour, which is why one 16-entry palette serves blood, smoke and
//               snow alike.
//
// Read as 4444 instead, the two bytes fall across the wrong channels and produce colour out
// of nothing -- blue where brown belongs. The ramps below decompose into two clean linear runs
// of step 0x11, one per byte, and 4444 shreds them --
//
//   Ringo01  high byte 0x11,0x22,0x33 ... 0xff exactly, an intensity ramp
//            as ARGB4444: 0x660c -> r102 g0 b204, a blue-purple that is in no palette
//   SNOWPF01 both bytes ramp by 0x11, offset from each other: a puff fading out
//
// The alpha goes wrong the same way: the blood spray turns invisible and the pool vanishes
// once it spreads.
inline unsigned int ExpandPaletteEntry(unsigned short stored, int effectMode) {
    const unsigned short c = (unsigned short)(((stored & 0x00ff) << 8) | (stored >> 8));
    if (effectMode == 1 || effectMode == 2) {
        // Which byte is which is the one thing the data does not settle on its own. Most
        // format-3 sets put a clean 0x11-step 0..255 ramp in the low byte, which reads as a
        // textbook alpha ramp; Ringo01 and the soft puffs put their clean ramp in the HIGH
        // byte instead. Mode 1 is intensity-high/alpha-low, which is what the N64 IA16 TLUT
        // format specifies; mode 2 is the other way round, kept switchable because a wrong
        // guess here does not look wrong so much as absent.
        const unsigned int hi = (c >> 8) & 0xff;
        const unsigned int lo = c & 0xff;
        const unsigned int i = (effectMode == 1) ? hi : lo;
        const unsigned int a = (effectMode == 1) ? lo : hi;
        return (a << 24) | (i << 16) | (i << 8) | i;
    }
    if (effectMode == 3) {
        unsigned int a = (c >> 12) & 0x0f;
        unsigned int r = (c >> 8) & 0x0f;
        unsigned int g = (c >> 4) & 0x0f;
        unsigned int b = c & 0x0f;
        a = (a << 4) | a;
        r = (r << 4) | r;
        g = (g << 4) | g;
        b = (b << 4) | b;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    unsigned int r = (c >> 11) & 0x1f;
    unsigned int g = (c >> 6) & 0x1f;
    unsigned int b = (c >> 1) & 0x1f;
    const unsigned int a = (c & 1) ? 0xffu : 0x00u;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Format 3 is the only one whose palette carries a full alpha channel.
// Format 3 is the effect atlas and the only format whose palette is not packed colour.
// Returns the mode ExpandPaletteEntry should use: 0 leaves RGBA5551 for everything else.
inline int EffectPaletteMode(int format) {
    return (format == 3) ? Config::EffectPaletteMode() : 0;
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

static const char* SetName(const void* blob);

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

// The sky list is a comma-separated set of name prefixes, so "Day" covers Day1 and Day2.
static bool NameIsSky(const char* name) {
    if (!name || !*name) {
        return false;
    }
    const char* list = Config::SkyTextureNames();
    while (*list) {
        while (*list == ',' || *list == ' ') {
            ++list;
        }
        const char* end = list;
        while (*end && *end != ',') {
            ++end;
        }
        int len = (int)(end - list);
        while (len > 0 && list[len - 1] == ' ') {
            --len;
        }
        if (len > 0 && _strnicmp(name, list, len) == 0) {
            return true;
        }
        list = end;
    }
    return false;
}

void Texture::FinishEnumeration() {
    int bytes = 0;
    int valid = 0;
    int skies = 0;
    for (int i = 0; i < g_count; ++i) {
        if (g_sets[i] && NameIsSky(SetName(g_sets[i]))) {
            g_isSky[i] = true;
            ++skies;
            LogLine("    sky texture set %d: %s", i, SetName(g_sets[i]));
        }
    }
    LogLine("    %d sky sets matched \"%s\"", skies, Config::SkyTextureNames());
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

namespace {

// Which sub-chunk of the image, and of the palette, a draw selects.
//
// Opcode 0xa9 carries two 16-bit fields. The original (FUN_1000cf90) stores both, then uses
// the larger of the two as the image sub-chunk -- except for format 3, the animated format,
// where the first field alone drives it. The second field always selects the palette.
void ResolveSubChunks(int fmt, int subA, int subB, int* outPix, int* outPal) {
    const unsigned int a = (unsigned int)subA;
    const unsigned int b = (unsigned int)subB;
    *outPix = (int)((fmt == 3) ? a : (a > b ? a : b));
    *outPal = (int)b;
}

// Which image sub-chunk to actually decode, once the set's real frame count is known.
//
// The rule above reproduces SP_SelectTextureSet exactly, unsigned max and all, but the
// original never clamps -- and this renderer has to, or it indexes past the blob. The engine
// can ask a set for an image sub-chunk beyond its frame count together with a palette that is
// in range -- the 15-frame mouth sets are asked for palettes 12, 13 and 14 that way -- and the
// unsigned max then picks the out-of-range half. A set's variants otherwise pair frame N with
// palette N, so when the two disagree it is the in-range one that is meaningful.
//
// So prefer a candidate that actually exists: the resolved value when it fits, then the
// palette index, then the other half. Frame 0 is the last resort, not the first answer:
// falling back to it leaves a mouth wearing a palette from a frame it is not showing.
int ResolveImageSubChunk(int resolved, int subA, int subB, unsigned int frames) {
    if (frames == 0) {
        return 0;
    }
    if (resolved >= 0 && (unsigned int)resolved < frames) {
        return resolved;
    }
    if (subB >= 0 && (unsigned int)subB < frames) {
        return subB;
    }
    if (subA >= 0 && (unsigned int)subA < frames) {
        return subA;
    }
    return 0;
}

int ClampIndex(int wanted, unsigned int available) {
    if (available == 0) {
        return 0;
    }
    if (wanted < 0 || (unsigned int)wanted >= available) {
        return 0;
    }
    return wanted;
}

IDirect3DTexture9* Decode(int index, int pixSub, int palSub) {
    IDirect3DDevice9* dev = Device::Get();
    const unsigned char* base = (const unsigned char*)g_sets[index];
    const unsigned int size = BlobSize(base);
    if (!dev || size == 0) {
        return 0;
    }

    unsigned int hdrSize = 0, pixChunkSize = 0, palChunkSize = 0;
    const unsigned char* hdr = ChunkAt(base, size, 0, &hdrSize);
    const unsigned char* pixChunk = ChunkAt(base, size, 1, &pixChunkSize);
    const unsigned char* palChunk = ChunkAt(base, size, 2, &palChunkSize);
    if (!hdr || hdrSize < 4 || !pixChunk) {
        return 0;
    }

    const int w = hdr[2] ? hdr[2] : 256;
    const int h = hdr[3] ? hdr[3] : 256;

    unsigned int pixSize = 0, palSize = 0;
    const unsigned char* pixelData =
        SubChunk(pixChunk, (unsigned int)pixSub, pixChunkSize, &pixSize);
    if (!pixelData) {
        return 0;
    }
    // Formats 2, 4, 5 and 6 carry no palette at all -- their chunk 2 is a nested blob with a
    // count of zero. The original reflects this: FUN_1000cf90 only fetches a palette when its
    // per-format gate (texState+0x9c) is non-zero, which holds for formats 0, 1 and 3 alone.
    const unsigned char* palette = 0;
    if (palChunk && SubChunkCount(palChunk, palChunkSize) > 0) {
        palette = SubChunk(palChunk, (unsigned int)palSub, palChunkSize, &palSize);
    }

    const unsigned int pixels = (unsigned int)(w * h);
    int bpp;
    if (pixSize >= pixels) {
        bpp = 8;
    } else if (pixSize * 2 >= pixels) {
        bpp = 4;
    } else {
        LogLine("    texture %d: payload too small (%u for %ux%u)", index, pixSize, w, h);
        return 0;
    }

    IDirect3DTexture9* tex = 0;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, 0))) {
        return 0;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, 0, 0))) {
        tex->Release();
        return 0;
    }
    unsigned int clearTexels = 0;
    unsigned int sumR = 0, sumG = 0, sumB = 0, opaqueTexels = 0;

    const unsigned short* pal = (const unsigned short*)palette;
    const unsigned int palEntries = palette ? palSize / 2 : 0;
    const int effectMode = EffectPaletteMode(hdr[0]);
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
            if (palEntries) {
                dst[x] = (idx < palEntries) ? ExpandPaletteEntry(pal[idx], effectMode) : 0u;
                if ((dst[x] >> 24) == 0) {
                    ++clearTexels;
                } else {
                    // Mean over the visible texels only: averaging in the transparent ones
                    // drags every cut-out texture towards whatever the clear colour is and
                    // hides the very difference this is here to show.
                    sumR += (dst[x] >> 16) & 0xff;
                    sumG += (dst[x] >> 8) & 0xff;
                    sumB += dst[x] & 0xff;
                    ++opaqueTexels;
                }
            } else {
                // Palette-less sets are single-channel coverage: white modulated by the
                // vertex colour, with the sample as opacity.
                dst[x] = (idx << 24) | 0x00ffffffu;
            }
        }
    }
    tex->UnlockRect(0);
    g_lastDecodeTransparentPercent =
        pixels ? (unsigned short)((clearTexels * 100) / pixels) : 0;
    g_lastDecodeMeanColour = opaqueTexels
        ? (((sumR / opaqueTexels) << 16) | ((sumG / opaqueTexels) << 8) | (sumB / opaqueTexels))
        : 0u;
    return tex;
}

} // namespace

IDirect3DTexture9* Texture::GetTexture(int index, int subA, int subB) {
    if (index < 0 || index >= g_count || !g_sets[index] || g_decodeFailed[index]) {
        return 0;
    }

    const unsigned char* base = (const unsigned char*)g_sets[index];
    const unsigned int size = BlobSize(base);
    unsigned int hdrSize = 0, pixChunkSize = 0, palChunkSize = 0;
    const unsigned char* hdr = ChunkAt(base, size, 0, &hdrSize);
    const unsigned char* pixChunk = ChunkAt(base, size, 1, &pixChunkSize);
    const unsigned char* palChunk = ChunkAt(base, size, 2, &palChunkSize);
    if (!hdr || hdrSize < 1 || !pixChunk) {
        g_decodeFailed[index] = true;
        return 0;
    }

    int pixSub = 0, palSub = 0;
    ResolveSubChunks(hdr[0], subA, subB, &pixSub, &palSub);
    pixSub = ResolveImageSubChunk(pixSub, subA, subB, SubChunkCount(pixChunk, pixChunkSize));
    palSub = ClampIndex(palSub, SubChunkCount(palChunk, palChunkSize));

    SetCache& cache = g_cache[index];
    for (int i = 0; i < cache.n; ++i) {
        if (cache.v[i].pix == (unsigned short)pixSub && cache.v[i].pal == (unsigned short)palSub) {
            return cache.v[i].tex;
        }
    }

    IDirect3DTexture9* tex = Decode(index, pixSub, palSub);
    if (!tex) {
        ++g_decodeFailures;
        if (g_decodeFailuresLogged < 40) {
            ++g_decodeFailuresLogged;
            LogLine("    texture decode FAILED: set %d frame %d palette %d  \"%s\"",
                    index, pixSub, palSub, Name(index));
        }
        if (cache.n == 0) {
            // Frame 0 failing means the set as a whole is undecodable; a single missing
            // frame is not grounds for giving up on the rest.
            g_decodeFailed[index] = true;
        }
        return 0;
    }
    if (cache.n < kMaxVariants) {
        cache.v[cache.n].pix = (unsigned short)pixSub;
        cache.v[cache.n].pal = (unsigned short)palSub;
        cache.v[cache.n].transparentPercent = g_lastDecodeTransparentPercent;
        cache.v[cache.n].meanColour = g_lastDecodeMeanColour;
        cache.v[cache.n].tex = tex;
        ++cache.n;
    } else {
        // Full: recycle a slot. Dropping the texture on the floor instead leaks one per
        // draw call, which is how a long run runs the device out of memory and starts
        // failing every later decode.
        Variant& victim = cache.v[cache.next];
        cache.next = (cache.next + 1) % kMaxVariants;
        if (victim.tex) {
            victim.tex->Release();
        }
        victim.pix = (unsigned short)pixSub;
        victim.pal = (unsigned short)palSub;
        victim.transparentPercent = g_lastDecodeTransparentPercent;
        victim.meanColour = g_lastDecodeMeanColour;
        victim.tex = tex;
    }
    return tex;
}

// Every frame/palette pair actually decoded for the mouth sets, with how much of each is
// transparent: a pair that decodes to nothing draws no mouth at all, which looks like a mouth
// that stopped animating.
void Texture::LogMouthVariants() {
    LogLine("    MOUTH/EYE variants actually decoded:");
    for (int i = 0; i < g_count; ++i) {
        if (!g_sets[i] || g_cache[i].n == 0) {
            continue;
        }
        const char* n = SetName(g_sets[i]);
        if (!(n[0] == 'M' && n[1] == 'T' && n[2] == 'H')) {
            continue;
        }
        for (int v = 0; v < g_cache[i].n; ++v) {
            const Variant& var = g_cache[i].v[v];
            LogLine("      set %4d %-14s frame %2u palette %2u   clear=%u%%%s",
                    i, n, var.pix, var.pal, var.transparentPercent,
                    var.transparentPercent >= 99 ? "   <-- decodes to nothing" : "");
        }
    }
}

void Texture::LogSetCensus() {
    int byFormat[8];
    memset(byFormat, 0, sizeof(byFormat));
    LogLine("    TEXTURE CENSUS: %d sets", g_count);
    for (int i = 0; i < g_count; ++i) {
        if (!g_sets[i]) {
            continue;
        }
        const unsigned char* base = (const unsigned char*)g_sets[i];
        const unsigned int size = BlobSize(base);
        unsigned int hdrSize = 0, pixChunkSize = 0, palChunkSize = 0;
        const unsigned char* hdr = ChunkAt(base, size, 0, &hdrSize);
        const unsigned char* pixChunk = ChunkAt(base, size, 1, &pixChunkSize);
        const unsigned char* palChunk = ChunkAt(base, size, 2, &palChunkSize);
        if (!hdr || hdrSize < 4) {
            continue;
        }
        const int fmt = hdr[0];
        if (fmt >= 0 && fmt < 8) {
            ++byFormat[fmt];
        }
        const int w = hdr[2] ? hdr[2] : 256;
        const int h = hdr[3] ? hdr[3] : 256;
        // Alpha actually decoded for palette 0, which is the number that says whether an
        // effect can be seen at all: a set whose alpha is zero everywhere draws nothing.
        unsigned int aMin = 255, aMax = 0, aSum = 0, aN = 0;
        {
            unsigned int palSize0 = 0;
            const unsigned char* p0 =
                (palChunk && SubChunkCount(palChunk, palChunkSize) > 0)
                    ? SubChunk(palChunk, 0, palChunkSize, &palSize0) : 0;
            const int ia = EffectPaletteMode(hdr[0]);
            for (unsigned int e = 0; p0 && e + 1 < palSize0; e += 2) {
                const unsigned short stored =
                    (unsigned short)(p0[e] | ((unsigned short)p0[e + 1] << 8));
                const unsigned int a = (ExpandPaletteEntry(stored, ia) >> 24) & 0xff;
                if (a < aMin) aMin = a;
                if (a > aMax) aMax = a;
                aSum += a;
                ++aN;
            }
        }
        const unsigned int frames = SubChunkCount(pixChunk, pixChunkSize);
        const unsigned int pals = SubChunkCount(palChunk, palChunkSize);
        unsigned int frame0 = 0, pal0 = 0;
        SubChunk(pixChunk, 0, pixChunkSize, &frame0);
        SubChunk(palChunk, 0, palChunkSize, &pal0);
        const unsigned int px = (unsigned int)(w * h);
        LogLine("      set %4d fmt=%d %3dx%-3d frames=%-2u pal=%-2u frame0=%-6u (%.2f bpp) "
                "pal0=%-4u  %s",
                i, fmt, w, h, frames, pals, frame0,
                px ? (double)frame0 * 8.0 / (double)px : 0.0, pal0, SetName(g_sets[i]));
        if (aN && fmt == 3) {
            LogLine("            palette alpha min=%u max=%u mean=%u over %u entries%s",
                    aMin, aMax, aSum / aN, aN, (aMax < 16) ? "   <-- effectively invisible" : "");
        }
    }
    LogLine("    decode failures so far: %d", g_decodeFailures);
    LogLine("    format totals: 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d",
            byFormat[0], byFormat[1], byFormat[2], byFormat[3],
            byFormat[4], byFormat[5], byFormat[6], byFormat[7]);
}

int Texture::Count() {
    return g_count;
}

int Texture::MeanColour(int index) {
    if (index < 0 || index >= g_count || g_cache[index].n == 0) {
        return -1;
    }
    return (int)g_cache[index].v[0].meanColour;
}

const char* Texture::Name(int index) {
    if (index < 0 || index >= g_count || !g_sets[index]) {
        return "";
    }
    return SetName(g_sets[index]);
}

void Texture::Describe(int index, int subA, int subB, char* out, int capacity) {
    if (!out || capacity <= 0) {
        return;
    }
    out[0] = 0;
    if (index < 0 || index >= g_count || !g_sets[index]) {
        _snprintf(out, capacity, "no texture (index %d)", index);
        out[capacity - 1] = 0;
        return;
    }
    const unsigned char* base = (const unsigned char*)g_sets[index];
    const unsigned int size = BlobSize(base);
    unsigned int hdrSize = 0, pixChunkSize = 0, palChunkSize = 0;
    const unsigned char* hdr = ChunkAt(base, size, 0, &hdrSize);
    const unsigned char* pixChunk = ChunkAt(base, size, 1, &pixChunkSize);
    const unsigned char* palChunk = ChunkAt(base, size, 2, &palChunkSize);
    if (!hdr || hdrSize < 4) {
        _snprintf(out, capacity, "set %d unreadable", index);
        out[capacity - 1] = 0;
        return;
    }
    int pixSub = 0, palSub = 0;
    ResolveSubChunks(hdr[0], subA, subB, &pixSub, &palSub);
    pixSub = ResolveImageSubChunk(pixSub, subA, subB, SubChunkCount(pixChunk, pixChunkSize));
    palSub = ClampIndex(palSub, SubChunkCount(palChunk, palChunkSize));

    int transparent = -1;
    const SetCache& cache = g_cache[index];
    for (int i = 0; i < cache.n; ++i) {
        if (cache.v[i].pix == (unsigned short)pixSub && cache.v[i].pal == (unsigned short)palSub) {
            transparent = (int)cache.v[i].transparentPercent;
            break;
        }
    }
    // Both the resolved sub-chunks and the raw operands the display list asked for. An
    // animation that has stopped moving looks identical to one the engine stopped driving,
    // and only the raw values tell the two apart: a frame that reads "asked 7 -> 0" was
    // clamped here, one that reads "asked 0" was never requested.
    int rawPix = 0, rawPal = 0;
    ResolveSubChunks(hdr[0], subA, subB, &rawPix, &rawPal);
    _snprintf(out, capacity,
              "set %4d fmt=%d %3dx%-3d frame %d/%u pal %d/%u (asked %d,%d -> %d,%d) "
              "clear=%d%% %s%s",
              index, hdr[0], hdr[2] ? hdr[2] : 256, hdr[3] ? hdr[3] : 256,
              pixSub, SubChunkCount(pixChunk, pixChunkSize),
              palSub, SubChunkCount(palChunk, palChunkSize),
              subA, subB, rawPix, rawPal, transparent,
              SetName(g_sets[index]), g_decodeFailed[index] ? "  DECODE FAILED" : "");
    out[capacity - 1] = 0;
}

int Texture::Format(int index) {
    if (index < 0 || index >= g_count || !g_sets[index]) {
        return -1;
    }
    const unsigned char* base = (const unsigned char*)g_sets[index];
    unsigned int hdrSize = 0;
    const unsigned char* hdr = ChunkAt(base, BlobSize(base), 0, &hdrSize);
    return (hdr && hdrSize >= 1) ? (int)hdr[0] : -1;
}

bool Texture::DecodeFailed(int index) {
    return index >= 0 && index < kMaxSets && g_decodeFailed[index];
}

bool Texture::IsSkySet(int index) {
    return index >= 0 && index < g_count && g_isSky[index];
}

void Texture::FlushDecoded() {
    int released = 0;
    for (int i = 0; i < g_count; ++i) {
        for (int v = 0; v < g_cache[i].n; ++v) {
            if (g_cache[i].v[v].tex) {
                g_cache[i].v[v].tex->Release();
                ++released;
            }
        }
        g_cache[i].n = 0;
        g_cache[i].next = 0;
        g_decodeFailed[i] = false;
    }
    LogLine("    texture cache flushed: %d decoded frames released", released);
}

void Texture::ReleaseAll() {
    for (int i = 0; i < g_count; ++i) {
        for (int v = 0; v < g_cache[i].n; ++v) {
            if (g_cache[i].v[v].tex) {
                g_cache[i].v[v].tex->Release();
            }
        }
        g_cache[i].n = 0;
        g_cache[i].next = 0;
        if (g_sets[i]) {
            free(g_sets[i]);
            g_sets[i] = 0;
        }
        g_decodeFailed[i] = false;
        g_isSky[i] = false;
    }
    g_count = 0;
}
