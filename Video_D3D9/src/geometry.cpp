#include "geometry.h"
#include "config.h"
#include "device.h"
#include "log.h"
#include "texture.h"

#include <windows.h>
#include <d3d9.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

namespace {

// ---------------------------------------------------------------- display list

// Commands are 8 bytes: word0 carries the opcode in its low byte, word1 the operand.
// Opcodes are read as signed bytes, matching the original's dispatch.
enum Opcode {
    OP_MATRIX     = 0x01,
    OP_STATE      = 0x03,
    OP_VERTICES   = 0x04,
    OP_SUBLIST    = 0x06,
    OP_TRIANGLES  = 0xad,
    OP_TRI_PAIR   = 0xb1,
    OP_END        = 0xb8,
    OP_FLAGS_CLR  = 0xb6,
    OP_FLAGS_SET  = 0xb7,
    OP_TRI_ONE    = 0xbf,
    OP_TEXTURE     = 0xa9,   // carries the texture-set index directly
    OP_TEXTURE_ALT = 0xa6,   // same index, biased by one
    OP_SKIPMASK_CLR = 0xa7,
    OP_SKIPMASK_SET = 0xa8,
    OP_MATRIX_POP   = 0xbd,
    OP_SEGMENT    = 0xbc,
    // The frame's background colour, packed 0xAABBGGRR in word1. The original unpacks it
    // into the viewport clear; without it the background is whatever this renderer picked.
    OP_BACKGROUND = 0xf8,
    // The N64 combiner's two constant colours. The original multiplies every vertex colour
    // by one of them, chosen by the mode below. Ignoring them draws South Park's
    // flat-coloured house walls white.
    OP_PRIM_COLOUR = 0xfa,
    OP_ENV_COLOUR  = 0xfb,
    // Selects one of 41 colour-combiner modes, and with it whether the constant colours
    // above participate at all. Without this the tint is applied to everything.
    OP_COMBINER_MODE = 0xaf,
    // Variable-length commands. Every other opcode is exactly 8 bytes; these three consume
    // extra words, and treating them as fixed-length desynchronises the whole walk -- after
    // which operand bytes get decoded as opcodes.
    OP_2D_PRIM      = 0xab,   // 8 bytes
    OP_2D_PRIM_LONG = 0xac,   // 8 + 8
    OP_SKIP24       = 0xe4,   // 8 + 16
};

// Render-state bitmask maintained by opcodes 0xb7 (set) and 0xb6 (clear). Only the bits in
// kFlagMask are writable, matching the original's masking.
const unsigned int kFlagMask     = 0x000f3205;
const unsigned int kFlagLighting = 0x00020000;
// The original drives CULLMODE from this bit: set means D3DCULL_CCW, clear means no culling.
// Forcing D3DCULL_NONE draws the back faces of everything, including the far side of ground
// skirts and horizon geometry the engine expects to be invisible.
const unsigned int kFlagBackfaceCull = 0x00002000;
unsigned int g_renderFlags = 0;

// The display list's two constant colours, as opcodes 0xfa (primitive) and 0xfb
// (environment) deliver them: bytes r, g, b, a from the low byte of word1 upwards.
//
// These are not decoration. The original's transform stage multiplies each vertex colour by
// a constant colour taken from one of them -- South Park's houses are flat-coloured boxes
// whose walls carry no texture and white vertex colours, so this is the whole of their
// colour. Confirmed against the original's own packing at 0x10008486, which builds
// `(alpha << 24) | (vertex[+4] << 16) | (vertex[+5] << 8) | vertex[+6]`: the source bytes
// are r, g, b, a in memory order, so a D3DCOLOR needs red and blue swapped.
unsigned int g_primColour = 0xffffffffu;
unsigned int g_envColour  = 0xffffffffu;

// Which constant colour a batch actually uses -- or none at all.
//
// The tint is *gated*, and applying it ungated tints the whole world: snow turns red, people
// turn blue. The gate is the colour-combiner mode, delivered by opcode 0xaf into
// DAT_10087ef8, and resolved by FUN_10005850 through a 41-entry jump table at 0x10005edc.
// Each case raises a set of flags, and FUN_1000ba50 then picks the colour in a fixed order:
//
//     envRGB   -> environment colour     (0x10087ee0)
//     primRGB2 -> primitive colour       (0x10087ee4)
//     scalar   -> a computed grey        (0x100876b4, when non-negative)
//     primRGB  -> primitive colour       (0x10087a54)
//     primRGB3 -> primitive colour       (0x10087a50)
//     otherwise: no constant colour, and the vertex colour stands as authored
//
// The table below is that jump table read back, one entry per mode. The three modes that
// resolve a computed grey are treated as untinted: their value is derived from the
// environment colour through per-mode arithmetic, and guessing at it is a good way to
// discolour something else.
enum ConstantSource { CONST_NONE, CONST_PRIM, CONST_ENV };

const int kCombinerModeCount = 0x29;
const unsigned char kConstantSource[kCombinerModeCount] = {
    CONST_PRIM,  // 0x00
    CONST_PRIM,  // 0x01
    CONST_PRIM,  // 0x02
    CONST_NONE,  // 0x03  alpha only
    CONST_PRIM,  // 0x04
    CONST_PRIM,  // 0x05
    CONST_PRIM,  // 0x06
    CONST_PRIM,  // 0x07
    CONST_NONE,  // 0x08  computed grey
    CONST_PRIM,  // 0x09
    CONST_ENV,   // 0x0a
    CONST_ENV,   // 0x0b
    CONST_NONE,  // 0x0c
    CONST_NONE,  // 0x0d  alpha only
    CONST_NONE,  // 0x0e  alpha only
    CONST_NONE,  // 0x0f  computed grey
    CONST_ENV,   // 0x10
    CONST_NONE,  // 0x11  computed grey
    CONST_PRIM,  // 0x12
    CONST_PRIM,  // 0x13
    CONST_PRIM,  // 0x14
    CONST_PRIM,  // 0x15
    CONST_PRIM,  // 0x16
    CONST_ENV,   // 0x17
    CONST_PRIM,  // 0x18
    CONST_PRIM,  // 0x19
    CONST_NONE,  // 0x1a  alpha only
    CONST_ENV,   // 0x1b
    CONST_NONE,  // 0x1c
    CONST_PRIM,  // 0x1d
    CONST_PRIM,  // 0x1e
    CONST_NONE,  // 0x1f
    CONST_PRIM,  // 0x20
    CONST_NONE,  // 0x21
    CONST_NONE,  // 0x22
    CONST_NONE,  // 0x23
    CONST_NONE,  // 0x24
    CONST_NONE,  // 0x25
    CONST_PRIM,  // 0x26
    CONST_PRIM,  // 0x27
    CONST_NONE,  // 0x28
};

// No mode until the list names one, and no mode means no tint -- which is what the engine's
// own flags read as before anything sets them.
int g_combinerMode = -1;

unsigned int ConstantColourForBatch() {
    if (g_combinerMode < 0 || g_combinerMode >= kCombinerModeCount) {
        return 0xffffffffu;
    }
    switch (kConstantSource[g_combinerMode]) {
    case CONST_PRIM: return g_primColour;
    case CONST_ENV:  return g_envColour;
    default:         return 0xffffffffu;
    }
}

// Texture set selected by opcode 0xa9. -1 means untextured.
int g_currentTexture = -1;
// The two 16-bit fields of that opcode's second word. They choose which frame of an animated
// set and which of its palettes to draw with, so a change to either has to break the batch
// exactly as a change of set does.
int g_currentSubA = 0;
int g_currentSubB = 0;

// Per-triangle skip bitmask. Opcode 0xa8 installs it and resets the counter, 0xa7 clears it.
// Each emitted triangle consumes one counter step; a set bit means the engine wants that
// triangle dropped. Ignoring this draws geometry the original culls.
const unsigned int* g_skipMask = 0;
int g_skipCounter = 0;
const int kSkipMaskLimit = 0x80;
int g_textureSelects = 0;
// Selects of a mouth set (MTH*). Character mouths animate by sub-chunk, and only the raw
// operand separates "the engine stopped asking for new frames" from "the renderer stopped
// honouring the request", so every mouth select is recorded (RecordMouthOperand).
unsigned int g_mouthSelects = 0;
int g_clampLogged = 0;


int g_texturedFlushes = 0;
int g_untexturedFlushes = 0;
int g_skyBatches = 0;

// Per-texture-set draw census. "White house" and "the texture failed to decode" look
// identical on screen, and so does "this batch never selected a texture at all" -- the only
// way to tell them apart is to count, per set, how much geometry was drawn with it and
// whether a texture was actually bound at the time.
const int kCensusSets = 4096;
struct SetDraws {
    unsigned int flushes;
    unsigned int tris;
    unsigned int flushesWithoutTexture;
    // The tint the batch carried, kept so the census can separate a texture that decoded to
    // the wrong colour from one that decoded correctly and was then tinted wrongly.
    unsigned int lastConstant;
    int lastMode;
};
SetDraws g_setDraws[kCensusSets];
// House walls carry no texture and white vertices, so they never appear in the per-set
// census -- every one of them lands in the untextured bucket below. To see what actually
// decides their colour, each distinct (combiner mode, constant colour, blend) triple an
// untextured batch is drawn with is counted here. A wall that changes colour as the camera
// turns has to be drawn under more than one of these.
struct UntexturedKind {
    int mode;
    unsigned int constant;
    unsigned char blend;      // 0 opaque, 1 additive ONE/ONE, 2 SRCALPHA
    unsigned int flushes;
    unsigned int tris;
};
const int kMaxUntexturedKinds = 64;
UntexturedKind g_untexturedKinds[kMaxUntexturedKinds];
int g_untexturedKindCount = 0;

void RecordUntexturedBatch(int mode, unsigned int constant, unsigned char blend,
                           unsigned int tris) {
    for (int i = 0; i < g_untexturedKindCount; ++i) {
        UntexturedKind& k = g_untexturedKinds[i];
        if (k.mode == mode && k.constant == constant && k.blend == blend) {
            ++k.flushes;
            k.tris += tris;
            return;
        }
    }
    if (g_untexturedKindCount >= kMaxUntexturedKinds) {
        return;
    }
    UntexturedKind& k = g_untexturedKinds[g_untexturedKindCount++];
    k.mode = mode; k.constant = constant; k.blend = blend; k.flushes = 1; k.tris = tris;
}

unsigned int g_noSelectFlushes = 0;   // batches drawn before any 0xa9 named a set
unsigned int g_noSelectTris = 0;
unsigned int g_litTris = 0;
unsigned int g_unlitTris = 0;
unsigned int g_constantColourChanges = 0;
unsigned int g_tintedUploads = 0;
unsigned int g_untintedUploads = 0;
unsigned int g_modeSelects[0x29];
unsigned int g_skyFramesYieldedTo2D = 0;
unsigned int g_opaqueFlushes = 0;
unsigned int g_blendedFlushes = 0;

// Frame dump. Armed from a key, spent over one frame, so the log stays readable.
bool g_dumpArmed = false;
bool g_dumpingFrame = false;
int  g_dumpBatch = 0;

// Operands above 0x00ffffff are segment-relative. The engine installs the bases through
// opcode 0xbc, sub-command 6, before any list references them.
const int kSegmentCount = 16;
unsigned int g_segmentBase[kSegmentCount];

// Matrix opcode flag bits, from the original's stack handler.
enum MatrixFlags {
    MAT_SET_VIEW     = 1,
    MAT_SET_ABSOLUTE = 2,
    MAT_PUSH         = 4,
};

// Source vertex as the engine lays it out: 20 bytes, integer object-space position.
#pragma pack(push, 1)
struct SourceVertex {
    short x, y, z;
    short unused;
    float u, v;
    union {
        unsigned int bgra;                 // pre-lit colour
        struct { signed char nx, ny, nz, pad; };  // normal, when lighting is enabled
    };
};
#pragma pack(pop)

struct WorldVertex {
    float x, y, z;
    float nx, ny, nz;
    D3DCOLOR diffuse;
    float u, v;
};

const DWORD kWorldFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// Triangle indices are single bytes, so only 256 slots are ever addressable.
const int kMaxVertices = 256;
const int kMaxIndices  = 8192;

WorldVertex g_vertices[kMaxVertices];
unsigned short g_indices[kMaxIndices];
int g_indexCount = 0;
int g_highWater  = 0;   // highest slot written this frame; the window outlives a flush

// ------------------------------------------------------------- geometry audit
//
// These counters check the display-list interpretation directly: a triangle that references a
// slot nothing has written this list is reading a stale vertex from earlier geometry, which is
// exactly what draws a long thin sliver across the scene.
struct GeoAudit {
    int lists;
    int tris;
    int staleRefs;      // triangle referenced a slot never written in this list
    int degenerate;     // two or more indices equal
    int badFloats;      // non-finite position
    int edgeHist[12];   // log2 buckets of longest edge, world units
    float maxEdge;
    int maxEdgeTri[3];
    // Aspect ratio = longest edge / shortest edge. A stretched sliver has a huge ratio; a
    // well-formed triangle is near 1.
    int culledBatches, unculledBatches;
    int sublists;       // G_DL recursions: one per object or map chunk the engine chose to draw
    int aspect2, aspect10, aspect100, aspect1000;
    float maxAspect;
    int maxAspectTri[3];
    float bbMin[3], bbMax[3];
    int verts;
};
GeoAudit g_audit;

// Worst case across the entire run, so a bad frame cannot hide behind good ones.
struct GeoWorst {
    int frames;
    int tris;
    int staleRefs;
    int degenerate;
    int badFloats;
    int aspect10, aspect100, aspect1000;
    float maxAspect;
    int worstFrameTris;
    float maxEdge;
    float bbMin[3], bbMax[3];
    bool haveBB;
    int minFrameTris, maxFrameTris;
    float widestFrame[3];   // largest per-frame world extent seen
};
GeoWorst g_worst;
unsigned char g_slotWritten[kMaxVertices];   // per display list, not per frame

// Which vertex-upload batch last wrote each slot, and which model matrix that batch used.
// A triangle whose three slots come from different batches is stitching together geometry
// that was baked under different transforms.
short g_slotSrc[kMaxVertices][3];   // source object-space coords, for blame reporting
unsigned char g_slotRaw[kMaxVertices][20];  // the whole source vertex, to judge if it is real
// The vertex colour as authored, before the combiner constant is folded in.
//
// The constant cannot be applied when a vertex block is loaded: the engine loads a block of
// vertices once and then emits triangles from it,
// and it is free to change the primitive or environment colour in between -- on the N64 the
// constant is a combiner input applied as each triangle is rasterised, not something baked
// into a vertex. Folding it at load time gives every triangle the colour that happened to be
// current when the block arrived, which for the first draw of a newly spawned object is
// whatever the previous object left behind.
unsigned int g_slotUntinted[kMaxVertices];
unsigned short g_slotBatch[kMaxVertices];
unsigned short g_slotMatrix[kMaxVertices];
unsigned short g_batchSerial = 0;
unsigned short g_matrixSerial = 0;
int g_mixedBatchTris = 0;
int g_skippedTris = 0;

// Which opcode produced the triangle currently being emitted, so blame can be assigned to a
// specific handler instead of the interpreter as a whole.
unsigned char g_emitOpcode = 0;
unsigned int g_emitWord0 = 0, g_emitWord1 = 0;
int g_badDumped = 0;
int g_vertDumped = 0;
struct OpStat { int tris; int aspect10; int aspect100; int aspect1000; float worst; };
OpStat g_opStat[256];
int g_mixedMatrixTris = 0;
// Triangles that were still pending when a vertex upload overwrote the window, and so were
// drawn before it (UploadVertices) rather than against the next object's vertices.
int g_uploadFlushTris = 0;
int g_auditFramesLogged = 0;

bool Finite(float v) {
    return v == v && v <= 3.4e38f && v >= -3.4e38f;
}

float EdgeLen(const WorldVertex& a, const WorldVertex& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

void AuditTriangle(int a, int b, int c) {
    ++g_audit.tris;
    if (g_slotBatch[a] != g_slotBatch[b] || g_slotBatch[b] != g_slotBatch[c]) {
        ++g_mixedBatchTris;
    }
    if (g_slotMatrix[a] != g_slotMatrix[b] || g_slotMatrix[b] != g_slotMatrix[c]) {
        ++g_mixedMatrixTris;
    }
    if (a == b || b == c || a == c) {
        ++g_audit.degenerate;
    }
    if (!g_slotWritten[a] || !g_slotWritten[b] || !g_slotWritten[c]) {
        ++g_audit.staleRefs;
    }
    const WorldVertex& va = g_vertices[a];
    const WorldVertex& vb = g_vertices[b];
    const WorldVertex& vc = g_vertices[c];
    if (!Finite(va.x) || !Finite(va.y) || !Finite(va.z) ||
        !Finite(vb.x) || !Finite(vb.y) || !Finite(vb.z) ||
        !Finite(vc.x) || !Finite(vc.y) || !Finite(vc.z)) {
        ++g_audit.badFloats;
        return;
    }
    float e = EdgeLen(va, vb);
    const float e2 = EdgeLen(vb, vc);
    const float e3 = EdgeLen(vc, va);
    if (e2 > e) e = e2;
    if (e3 > e) e = e3;
    if (e > g_audit.maxEdge) {
        g_audit.maxEdge = e;
        g_audit.maxEdgeTri[0] = a; g_audit.maxEdgeTri[1] = b; g_audit.maxEdgeTri[2] = c;
    }
    int bucket = 0;
    float t = e;
    while (bucket < 11 && t >= 2.0f) { t *= 0.5f; ++bucket; }
    ++g_audit.edgeHist[bucket];

    float shortest = EdgeLen(va, vb);
    if (e2 < shortest) shortest = e2;
    if (e3 < shortest) shortest = e3;
    const float aspect = (shortest > 1e-4f) ? (e / shortest) : 1e6f;
    if (aspect > 1000.0f)     ++g_audit.aspect1000;
    else if (aspect > 100.0f) ++g_audit.aspect100;
    else if (aspect > 10.0f)  ++g_audit.aspect10;
    else if (aspect > 2.0f)   ++g_audit.aspect2;

    // Dump the worst offenders in full: the command that produced them, the slots they
    // reference, and where those slots actually are in world space.
    if (aspect > 50.0f && e > 200.0f && g_badDumped < 10) {
        ++g_badDumped;
        LogLine("    BAD TRI #%d op=0x%02x word0=%08x word1=%08x aspect=%.0f longest=%.1f",
                g_badDumped, g_emitOpcode, g_emitWord0, g_emitWord1, aspect, e);
        const int ii[3] = { a, b, c };
        const WorldVertex* vv[3] = { &va, &vb, &vc };
        for (int k = 0; k < 3; ++k) {
            const unsigned char* rw = g_slotRaw[ii[k]];
            LogLine("        slot %3d src(%6d %6d %6d) uv(%.3f,%.3f) argb=%08x",
                    ii[k], (int)g_slotSrc[ii[k]][0], (int)g_slotSrc[ii[k]][1],
                    (int)g_slotSrc[ii[k]][2], vv[k]->u, vv[k]->v, vv[k]->diffuse);
            LogLine("                 raw %02x%02x %02x%02x %02x%02x %02x%02x | %02x%02x%02x%02x %02x%02x%02x%02x | %02x%02x%02x%02x",
                    rw[0],rw[1],rw[2],rw[3],rw[4],rw[5],rw[6],rw[7],
                    rw[8],rw[9],rw[10],rw[11],rw[12],rw[13],rw[14],rw[15],
                    rw[16],rw[17],rw[18],rw[19]);
        }
    }

    OpStat& os = g_opStat[g_emitOpcode];
    ++os.tris;
    if (aspect > 1000.0f)     ++os.aspect1000;
    else if (aspect > 100.0f) ++os.aspect100;
    else if (aspect > 10.0f)  ++os.aspect10;
    if (aspect > os.worst) os.worst = aspect;
    if (aspect > g_audit.maxAspect) {
        g_audit.maxAspect = aspect;
        g_audit.maxAspectTri[0] = a;
        g_audit.maxAspectTri[1] = b;
        g_audit.maxAspectTri[2] = c;
    }
}

void AuditVertex(const WorldVertex& v) {
    if (!Finite(v.x) || !Finite(v.y) || !Finite(v.z)) {
        return;
    }
    if (g_audit.verts == 0) {
        g_audit.bbMin[0] = g_audit.bbMax[0] = v.x;
        g_audit.bbMin[1] = g_audit.bbMax[1] = v.y;
        g_audit.bbMin[2] = g_audit.bbMax[2] = v.z;
    } else {
        const float p[3] = { v.x, v.y, v.z };
        for (int i = 0; i < 3; ++i) {
            if (p[i] < g_audit.bbMin[i]) g_audit.bbMin[i] = p[i];
            if (p[i] > g_audit.bbMax[i]) g_audit.bbMax[i] = p[i];
        }
    }
    ++g_audit.verts;
}

// ------------------------------------------------------------- matrix handling

const int kMatrixStackDepth = 32;

D3DMATRIX g_view;                          // rigid camera, factored out of the fused matrix
D3DMATRIX g_proj;                          // projection, factored out of the same
bool g_haveProjection = false;
D3DMATRIX g_modelStack[kMatrixStackDepth];
int  g_stackDepth = 0;
bool g_viewDirty = true;

float g_zNear = 1.0f;
float g_zFar  = 32768.0f;
int   g_screenWidth  = 640;
int   g_screenHeight = 480;

// Diagnostics: the first frames' matrices are logged once so the camera convention can be
// confirmed against real data rather than assumed.
int g_matricesLogged = 0;
int g_factorLogged = 0;

// Running opcode census. Which opcodes the engine actually emits is the only direct evidence
// of what it culls before the plugin ever sees the stream, so this is cumulative rather than
// sampled from the first list -- the first list is a menu backdrop and carries no world.
int  g_opcodeSeen[256];
int  g_listDepth = 0;

void LogOpcodeCensus() {
    LogLine("      display-list opcode census (cumulative):");
    for (int i = 0; i < 256; ++i) {
        if (g_opcodeSeen[i]) {
            LogLine("        op 0x%02x  x%d", i, g_opcodeSeen[i]);
        }
    }
}

void Identity(D3DMATRIX& m) {
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
}

// Row-vector, row-major, translation in the last row -- the same convention as D3DMATRIX,
// so the engine's 16 floats map across without transposing.
void Multiply(D3DMATRIX& out, const D3DMATRIX& a, const D3DMATRIX& b) {
    const float* A = &a._11;
    const float* B = &b._11;
    float* O = &out._11;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            O[r * 4 + c] = A[r * 4 + 0] * B[0 * 4 + c] +
                           A[r * 4 + 1] * B[1 * 4 + c] +
                           A[r * 4 + 2] * B[2 * 4 + c] +
                           A[r * 4 + 3] * B[3 * 4 + c];
        }
    }
}

// The engine's camera slot holds view x projection already multiplied together. Remix needs
// them apart, and applying the fused matrix as a view while also setting a projection scales
// x and y twice and stretches the picture.
//
// With a rigid view V (rotation R, translation t) and a standard projection P, the product is
//   row i<3: [ R[i][0]*p0, R[i][1]*p1, R[i][2]*a, -R[i][2] ]
//   row 3:   [ t[0]*p0,    t[1]*p1,    t[2]*a + b, -t[2]   ]
// so the fourth column recovers R's third column, and the norms of the first two columns
// recover the projection scales. Everything else follows.
void LogMatrix(const char* label, const D3DMATRIX& m);

void FactorViewProjection(const D3DMATRIX& vp) {
    const float* m = &vp._11;

    float p0 = sqrtf(m[0]*m[0] + m[4]*m[4] + m[8]*m[8]);
    const float p1 = sqrtf(m[1]*m[1] + m[5]*m[5] + m[9]*m[9]);
    if (p0 < 1e-6f || p1 < 1e-6f) {
        // Not a decomposable camera matrix; fall back to using it whole.
        g_view = vp;
        g_haveProjection = false;
        return;
    }

    float rz[3] = { -m[3], -m[7], -m[11] };
    const float rzLen = sqrtf(rz[0]*rz[0] + rz[1]*rz[1] + rz[2]*rz[2]);
    if (rzLen < 1e-6f) {
        g_view = vp;
        g_haveProjection = false;
        return;
    }

    Identity(g_view);
    float* v = &g_view._11;
    v[0] = m[0] / p0;  v[1] = m[1] / p1;  v[2] = rz[0];
    v[4] = m[4] / p0;  v[5] = m[5] / p1;  v[6] = rz[1];
    v[8] = m[8] / p0;  v[9] = m[9] / p1;  v[10] = rz[2];
    v[12] = m[12] / p0; v[13] = m[13] / p1; v[14] = -m[15];

    // The engine's projection flips X, and taking the column norms as positive scales pushes
    // that flip into the view, leaving a mirrored (determinant -1) rotation. A mirrored view
    // reverses triangle winding, so backface culling then removes exactly the wrong faces.
    // Move the flip into the projection instead, keeping the view a proper rotation.
    const float det = v[0] * (v[5]*v[10] - v[6]*v[9])
                    - v[1] * (v[4]*v[10] - v[6]*v[8])
                    + v[2] * (v[4]*v[9]  - v[5]*v[8]);
    if (det < 0.0f) {
        v[0] = -v[0];  v[4] = -v[4];  v[8] = -v[8];  v[12] = -v[12];
        p0 = -p0;
    }

    // Depth row: VP[i][2] = R[i][2]*a, so a comes from whichever component is largest.
    int best = 0;
    for (int i = 1; i < 3; ++i) {
        if (fabsf(rz[i]) > fabsf(rz[best])) {
            best = i;
        }
    }
    float a = (fabsf(rz[best]) > 1e-6f) ? (m[2 + best * 4] / rz[best]) : 1.0f;
    float b = m[14] - v[14] * a;

    // The depth row is a = f/(n-f), b = n*f/(n-f), so the engine's own near and far read
    // straight back out of it. Re-deriving the row against a further far plane keeps the
    // near plane exactly where the engine put it and moves nothing else -- which matters
    // once the executable stops culling, because everything it then submits beyond the
    // engine's own far plane would otherwise be clipped away again here.
    if (Config::FarPlane() > 0.0f && a < -1.0f) {
        const float zNear = b / a;
        const float zFar  = Config::FarPlane();
        if (zNear > 0.0f && zFar > zNear) {
            a = zFar / (zNear - zFar);
            b = a * zNear;
        }
    }

    memset(&g_proj, 0, sizeof(g_proj));
    g_proj._11 = p0;
    g_proj._22 = p1;
    g_proj._33 = a;
    g_proj._34 = -1.0f;
    g_proj._43 = b;
    g_haveProjection = true;

    if (g_factorLogged < 2) {
        ++g_factorLogged;
        LogLine("    factored camera: p0=%.4f p1=%.4f a=%.4f b=%.4f det=%.3f", p0, p1, a, b, det);
        LogMatrix("  fused view*proj", vp);
        LogMatrix("  rigid view", g_view);
        LogMatrix("  projection", g_proj);
    }
}

void LogMatrix(const char* label, const D3DMATRIX& m) {
    const float* f = &m._11;
    LogLine("    %s:", label);
    for (int r = 0; r < 4; ++r) {
        LogLine("      [%9.4f %9.4f %9.4f %9.4f]", f[r*4+0], f[r*4+1], f[r*4+2], f[r*4+3]);
    }
}

// ------------------------------------------------------------------ submission

void BuildProjection();
void Flush();
void ApplyRenderStates();
bool DrawSkybox();
void DumpBatch(int vertexCount, bool textured, bool culled, bool sky);

// ------------------------------------------------------------------ synthesised sky
//
// South Park has no skydome. Where the world does not cover the screen the engine simply
// leaves the frame's background colour showing, which is fine for a rasteriser and useless
// to a path tracer: cleared pixels are not a surface, so there is nothing to bound the
// scene, nothing for rays that miss the world to hit, and no sky to light anything with.
//
// So the renderer builds one: a large box centred on the camera, painted with the very
// colour the engine asked the background to be (display-list opcode 0xf8), drawn before any
// world geometry with the depth buffer switched off entirely. Depth-off is both physically
// right for a backdrop -- everything else simply draws over it -- and the shape RTX Remix's
// sky detection looks for.

bool g_skyboxDrawn = false;      // once per frame
// g_view and g_proj are written only when a view matrix arrives and deliberately persist
// across frames, so this tracks whether one has *ever* arrived. Clearing it per frame would
// drop the sky on any frame that reuses the previous camera without resending it.
bool g_viewValid = false;
int  g_skyboxLogged = 0;

// Camera position in world space. The factored view is a rigid transform under the engine's
// row-vector convention, p_view = p_world * R + t, so the eye is at -t * R^T.
void CameraPosition(float* out) {
    const float* v = &g_view._11;
    out[0] = -(v[12] * v[0] + v[13] * v[1] + v[14] * v[2]);
    out[1] = -(v[12] * v[4] + v[13] * v[5] + v[14] * v[6]);
    out[2] = -(v[12] * v[8] + v[13] * v[9] + v[14] * v[10]);
}

// The box has to sit inside the frustum's far plane or it is clipped away, and depth being
// off does not change that -- w-clipping still happens. The projection's depth row is
// a = f/(n-f), b = n*f/(n-f), so the far plane reads straight back out of it.
float SkyboxHalfSize() {
    const float configured = Config::SkyboxSize();
    if (configured > 0.0f) {
        return configured;
    }
    float zFar = g_zFar;
    if (g_haveProjection && g_proj._33 < -1.0f) {
        const float derived = g_proj._43 / (1.0f + g_proj._33);
        if (Finite(derived) && derived > 1.0f) {
            zFar = derived;
        }
    }
    // A cube's corners are sqrt(3) further out than its faces, so keep the whole box well
    // inside the far plane rather than just its sides.
    float half = zFar * 0.4f;
    if (half < 64.0f)      half = 64.0f;
    if (half > 1.0e6f)     half = 1.0e6f;
    return half;
}

// False when the camera is not usable yet, so the caller can try again on the next batch
// rather than losing the backdrop for the whole frame.
bool DrawSkybox() {
    IDirect3DDevice9* dev = Device::Get();
    if (!dev || !g_viewValid || !g_haveProjection) {
        return false;
    }

    float eye[3];
    CameraPosition(eye);
    if (!Finite(eye[0]) || !Finite(eye[1]) || !Finite(eye[2])) {
        return false;
    }
    const float h = SkyboxHalfSize();

    unsigned int rgb = Config::SkyboxColour() >= 0
                     ? (unsigned int)Config::SkyboxColour()
                     : (Device::ClearColour() & 0x00ffffffu);
    // The engine's backdrop is a pale wash picked to sit behind a rasterised scene. As real
    // geometry lighting a path-traced one it wants to read as sky, so it is pulled towards
    // one -- keeping the engine's day/night changes rather than replacing them outright.
    const int shift = Config::SkyboxBlueShift();
    if (shift > 0) {
        const unsigned int target = (unsigned int)Config::SkyboxBlueTarget();
        unsigned int mixed = 0;
        for (int c = 16; c >= 0; c -= 8) {
            const int from = (int)((rgb >> c) & 0xff);
            const int to   = (int)((target >> c) & 0xff);
            mixed |= (unsigned int)(from + (to - from) * shift / 100) << c;
        }
        rgb = mixed;
    }
    const D3DCOLOR colour = (D3DCOLOR)(0xff000000u | rgb);

    WorldVertex v[8];
    for (int i = 0; i < 8; ++i) {
        v[i].x = eye[0] + ((i & 1) ? h : -h);
        v[i].y = eye[1] + ((i & 2) ? h : -h);
        v[i].z = eye[2] + ((i & 4) ? h : -h);
        // Normals point back at the camera: the viewer is inside the box, so the faces that
        // matter are the inward ones.
        const float nx = (i & 1) ? -1.0f : 1.0f;
        const float ny = (i & 2) ? -1.0f : 1.0f;
        const float nz = (i & 4) ? -1.0f : 1.0f;
        const float inv = 0.57735027f;
        v[i].nx = nx * inv; v[i].ny = ny * inv; v[i].nz = nz * inv;
        v[i].diffuse = colour;
        v[i].u = 0.0f;
        v[i].v = 0.0f;
    }
    static const unsigned short kBoxIndices[36] = {
        0,1,3, 0,3,2,   4,6,7, 4,7,5,     // -z, +z
        0,4,5, 0,5,1,   2,3,7, 2,7,6,     // -y, +y
        0,2,6, 0,6,4,   1,5,7, 1,7,3,     // -x, +x
    };

    D3DMATRIX identity;
    Identity(identity);
    dev->SetTransform(D3DTS_WORLD, &identity);
    dev->SetTransform(D3DTS_VIEW, &g_view);
    dev->SetTransform(D3DTS_PROJECTION, &g_proj);

    dev->SetTexture(0, 0);
    dev->SetFVF(kWorldFVF);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    // Seen from the inside, so winding is not worth reasoning about.
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    const HRESULT hr = dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 8, 12,
                                                   kBoxIndices, D3DFMT_INDEX16,
                                                   v, sizeof(WorldVertex));
    if (FAILED(hr) && g_skyboxLogged < 4) {
        ++g_skyboxLogged;
        LogLine("    skybox draw failed hr=0x%08lx", (unsigned long)hr);
    } else if (g_skyboxLogged < 2) {
        ++g_skyboxLogged;
        LogLine("    skybox: half-size %.0f at eye (%.0f, %.0f, %.0f), colour 0x%06x",
                h, eye[0], eye[1], eye[2], colour & 0x00ffffffu);
    }

    // The world pipeline's own state was just overwritten wholesale, so hand it back the way
    // a pipeline switch would.
    ApplyRenderStates();
    return true;
}

void Flush() {
    IDirect3DDevice9* dev = Device::Get();
    if (!dev || g_indexCount < 3 || g_highWater == 0) {
        g_indexCount = 0;
        return;
    }
    // Indices are bytes and may point past what this batch actually uploaded; clamping the
    // declared vertex count keeps DrawIndexedPrimitiveUP inside the array either way.
    int maxIndex = 0;
    for (int i = 0; i < g_indexCount; ++i) {
        if (g_indices[i] > maxIndex) {
            maxIndex = g_indices[i];
        }
    }
    const int vertexCount = (maxIndex + 1 > g_highWater) ? maxIndex + 1 : g_highWater;
    if (Config::LogEveryCall()) {
        LogLine("    flush: %d tris, %d verts", g_indexCount / 3, vertexCount);
    }

    // The 2D layer leaves the device set up for screen-space quads, so reclaim it before
    // drawing world geometry -- most importantly the vertex format, which would otherwise
    // reinterpret these vertices as the 2D layout.
    if (Device::ClaimPipeline(Device::PipelineWorld)) {
        ApplyRenderStates();
    }

    // The backdrop goes down before anything else in the frame, which is only possible once
    // a camera has arrived -- the view matrix reaches the plugin mid-list, not at frame
    // start. This is the first draw that follows it.
    //
    // It is drawn with depth off, so it would erase anything already on screen. The 2D layer
    // is depth-free too, so a frame that opens with a full-screen 2D backdrop and only then
    // draws world geometry gets no sky rather than a painted-over menu.
    if (!g_skyboxDrawn && Config::SynthesiseSkybox()) {
        if (!Device::FrameHasDrawn()) {
            // Claimed only on success: a camera that is not usable yet must leave the frame
            // still empty, so the next batch can try again.
            if (DrawSkybox()) {
                Device::ClaimFirstDraw();
                g_skyboxDrawn = true;
            }
        } else {
            ++g_skyFramesYieldedTo2D;
            g_skyboxDrawn = true;
        }
    }

    // Positions are already world space because the model matrix is baked at upload time,
    // so WORLD is identity and VIEW carries the camera alone.
    D3DMATRIX identity;
    Identity(identity);
    dev->SetTransform(D3DTS_WORLD, &identity);
    dev->SetTransform(D3DTS_VIEW, &g_view);
    // The camera arrives mid-list, so the projection is applied here rather than at frame
    // start, where it would still be the previous frame's.
    BuildProjection();

    // CCW matches the original directly: the engine's negated Y scale cancels out through the
    // projection and viewport, so the winding is not reversed.
    const bool cull = (g_renderFlags & kFlagBackfaceCull) != 0
                   && !Config::DisableBackfaceCulling();
    dev->SetRenderState(D3DRS_CULLMODE, cull ? D3DCULL_CCW : D3DCULL_NONE);

    // Blending, per batch, and opaque unless the batch genuinely needs otherwise.
    //
    // World draws are not blended by default, because the original does not blend them: a
    // census of every SetRenderState it makes
    // shows SRCBLEND set to ONE at three sites and SRCALPHA at only two, and no combiner mode
    // requests a translucent blend at all -- the two that touch the blend factors set
    // ONE / ZERO, which is opaque and is also the DX6 device default the rest inherit.
    //
    // It is not a cosmetic difference under a path tracer. RTX Remix classifies an
    // alpha-blended draw as a translucent material, so blending everything would turn solid
    // house walls into red glass you could see the furniture through.
    //
    // Two cases genuinely blend: format 3 is the only palette carrying graduated alpha (the
    // effects and sky atlas), and combiner mode 0x16 is the one the original sets
    // D3DTBLEND_ADD for.
    const int textureFormat = (g_currentTexture >= 0) ? Texture::Format(g_currentTexture) : -1;
    const bool additive = (g_combinerMode == 0x16);
    const bool graduated = (textureFormat == 3);
    if (additive) {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    } else if (graduated) {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    } else {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (additive || graduated) { ++g_blendedFlushes; } else { ++g_opaqueFlushes; }
    if (g_currentTexture < 0) {
        RecordUntexturedBatch(g_combinerMode,
                              Config::ApplyConstantColour() ? ConstantColourForBatch()
                                                            : 0xffffffffu,
                              additive ? 1 : (graduated ? 2 : 0),
                              (unsigned int)(g_indexCount / 3));
    }

    // A skydome has no business in the depth buffer, and drawing it depth-free is also the
    // shape Remix's sky auto-detection looks for.
    const bool sky = Config::DepthlessSky() && Texture::IsSkySet(g_currentTexture);
    dev->SetRenderState(D3DRS_ZENABLE, sky ? D3DZB_FALSE : D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, sky ? FALSE : TRUE);
    if (sky) { ++g_skyBatches; }

    IDirect3DTexture9* tex = (g_currentTexture >= 0)
                           ? Texture::GetTexture(g_currentTexture, g_currentSubA, g_currentSubB)
                           : 0;
    if (tex) { ++g_texturedFlushes; } else { ++g_untexturedFlushes; }
    if (g_currentTexture >= 0 && g_currentTexture < kCensusSets) {
        SetDraws& d = g_setDraws[g_currentTexture];
        d.lastConstant = Config::ApplyConstantColour() ? ConstantColourForBatch() : 0xffffffffu;
        d.lastMode = g_combinerMode;
        ++d.flushes;
        d.tris += (unsigned int)(g_indexCount / 3);
        if (!tex) {
            ++d.flushesWithoutTexture;
        }
    } else {
        ++g_noSelectFlushes;
        g_noSelectTris += (unsigned int)(g_indexCount / 3);
    }
    if (g_renderFlags & kFlagLighting) { g_litTris += (unsigned int)(g_indexCount / 3); }
    else { g_unlitTris += (unsigned int)(g_indexCount / 3); }
    if (g_renderFlags & kFlagBackfaceCull) { ++g_audit.culledBatches; }
    else { ++g_audit.unculledBatches; }
    dev->SetTexture(0, tex);
    // Modulating against diffuse keeps pre-lit vertex colours meaningful; lit geometry
    // carries white so the texture passes through untouched.
    //
    // With no texture bound, D3DTA_TEXTURE is undefined by the D3D9 specification -- it was
    // selected here regardless, which is what drew every untextured batch flat white. An
    // untextured batch has to fall back to the vertex colour, which is where its colour
    // actually is.
    dev->SetTextureStageState(0, D3DTSS_COLOROP, tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, tex ? D3DTA_TEXTURE : D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, tex ? D3DTA_TEXTURE : D3DTA_DIFFUSE);

    // The combiner constant, applied here rather than when the vertices were loaded, so a
    // colour the engine sets after loading a block still reaches the triangles drawn from it.
    // Kept in the vertex colour rather than a texture-stage factor deliberately: a vertex
    // colour survives Remix's material conversion, where a fixed-function stage factor need
    // not, and it is the vertex colour Remix writes out as primvars:displayColor.
    {
        const unsigned int k = Config::ApplyConstantColour() ? ConstantColourForBatch()
                                                            : 0xffffffffu;
        const unsigned int kr = k & 0xff, kg = (k >> 8) & 0xff, kb = (k >> 16) & 0xff;
        if (k == 0xffffffffu) { ++g_untintedUploads; } else { ++g_tintedUploads; }
        for (int i = 0; i < vertexCount; ++i) {
            const unsigned int u = g_slotUntinted[i];
            const unsigned int r = (((u >> 16) & 0xffu) * kr) / 255u;
            const unsigned int g = (((u >> 8) & 0xffu) * kg) / 255u;
            const unsigned int b = ((u & 0xffu) * kb) / 255u;
            g_vertices[i].diffuse = (D3DCOLOR)(0xff000000u | (r << 16) | (g << 8) | b);
        }
    }

    if (g_dumpingFrame) {
        DumpBatch(vertexCount, tex != 0, cull, sky);
    }

    HRESULT hr = dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vertexCount,
                                             g_indexCount / 3,
                                             g_indices, D3DFMT_INDEX16,
                                             g_vertices, sizeof(WorldVertex));
    if (FAILED(hr)) {
        LogLine("    DrawIndexedPrimitiveUP failed hr=0x%08lx", (unsigned long)hr);
    }
    g_indexCount = 0;
}

void SetMatrixFromList(const float* src, unsigned char flags) {
    D3DMATRIX m;
    memcpy(&m, src, sizeof(D3DMATRIX));

    if (g_matricesLogged < 6) {
        ++g_matricesLogged;
        LogMatrix(flags & MAT_SET_VIEW ? "VIEW matrix" : "model matrix", m);
    }

    if (flags & MAT_SET_VIEW) {
        FactorViewProjection(m);
        g_viewDirty = true;
        g_viewValid = true;
        return;
    }

    bool pushed = false;
    if (flags & MAT_PUSH) {
        if (g_stackDepth + 1 < kMatrixStackDepth) {
            ++g_stackDepth;
            pushed = true;
        }
    }
    const D3DMATRIX before = g_modelStack[g_stackDepth];
    if (flags & MAT_SET_ABSOLUTE) {
        g_modelStack[g_stackDepth] = m;
    } else if (pushed) {
        // A push concatenates against the level below.
        Multiply(g_modelStack[g_stackDepth], m, g_modelStack[g_stackDepth - 1]);
    } else {
        // Without a push the original accumulates in place: dest and parent are the same
        // level. Using the parent instead re-parents every concatenation and misplaces the
        // geometry it produces.
        const D3DMATRIX current = g_modelStack[g_stackDepth];
        Multiply(g_modelStack[g_stackDepth], m, current);
    }
    // Only a genuine change to the effective model transform matters; view-matrix opcodes and
    // no-op reassignments do not rebake anything.
    if (memcmp(&before, &g_modelStack[g_stackDepth], sizeof(D3DMATRIX)) != 0) {
        ++g_matrixSerial;
    }
}

// The original bakes vertices at upload time, so one draw legitimately mixes vertices from
// several matrices; deferring a single matrix to draw time skews polygons. Baking
// here reproduces that exactly, and means positions reach D3D already in world space.
void TransformToWorld(const D3DMATRIX& m, float x, float y, float z,
                      float& ox, float& oy, float& oz) {
    const float* f = &m._11;
    ox = x * f[0] + y * f[4] + z * f[8]  + f[12];
    oy = x * f[1] + y * f[5] + z * f[9]  + f[13];
    oz = x * f[2] + y * f[6] + z * f[10] + f[14];
}

void RotateToWorld(const D3DMATRIX& m, float x, float y, float z,
                   float& ox, float& oy, float& oz) {
    const float* f = &m._11;
    ox = x * f[0] + y * f[4] + z * f[8];
    oy = x * f[1] + y * f[5] + z * f[9];
    oz = x * f[2] + y * f[6] + z * f[10];
}

void UploadVertices(const SourceVertex* src, int count, int destSlot) {
    if (!src || count <= 0 || destSlot < 0) {
        return;
    }
    // The vertex window is a fixed set of slots that uploads overwrite in place, and indices
    // name slots rather than carrying vertex data with them. Anything already indexed must be
    // drawn before those slots change, or it silently re-binds to the next object's geometry
    // and comes out as a sliver stretched between the two. The original opens its upload
    // handler with exactly this flush (FUN_10006830's first instruction calls FUN_100092d0).
    if (g_indexCount >= 3) {
        g_uploadFlushTris += g_indexCount / 3;
    }
    Flush();
    if (count > 64 && g_clampLogged < 8) {
        ++g_clampLogged;
        LogLine("    large vertex batch: slot=%d count=%d", destSlot, count);
    }
    if (destSlot + count > kMaxVertices) {
        if (g_clampLogged < 8) {
            ++g_clampLogged;
            LogLine("    vertex upload clamped: slot=%d count=%d (limit %d)",
                    destSlot, count, kMaxVertices);
        }
        count = kMaxVertices - destSlot;
        if (count <= 0) {
            return;
        }
    }
    // Raw source bytes against the decoded fields, to confirm the 20-byte layout rather than
    // assume it.
    if (g_vertDumped < 2) {
        ++g_vertDumped;
        LogLine("    VERT DUMP batch slot=%d count=%d  (stride 20)", destSlot, count);
        const int n = count < 6 ? count : 6;
        for (int i = 0; i < n; ++i) {
            const unsigned char* raw = (const unsigned char*)&src[i];
            LogLine("      [%d] %02x%02x %02x%02x %02x%02x %02x%02x | %02x%02x%02x%02x %02x%02x%02x%02x | %02x%02x%02x%02x"
                    "   xyz=(%d,%d,%d) uv=(%.3f,%.3f)",
                    i, raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],raw[6],raw[7],
                    raw[8],raw[9],raw[10],raw[11], raw[12],raw[13],raw[14],raw[15],
                    raw[16],raw[17],raw[18],raw[19],
                    (int)src[i].x, (int)src[i].y, (int)src[i].z, src[i].u, src[i].v);
        }
        const float* mm = &g_modelStack[g_stackDepth]._11;
        LogLine("      model row0 %.3f %.3f %.3f  row3 %.1f %.1f %.1f",
                mm[0], mm[1], mm[2], mm[12], mm[13], mm[14]);
    }

    const D3DMATRIX& model = g_modelStack[g_stackDepth];
    // Bit 0x20000 of the render flags decides what the four bytes at +16 mean: a signed
    // normal when lighting is on, a pre-lit colour when it is off. Never both.
    const bool lit = (g_renderFlags & kFlagLighting) != 0;

    // The original tints in its upload handler, in the same pass as the transform; here the
    // slot keeps its untinted colour and the constant is applied when the batch is drawn
    // (Flush), since the engine may change it between upload and draw.

    for (int i = 0; i < count; ++i) {
        WorldVertex& d = g_vertices[destSlot + i];
        const SourceVertex& s = src[i];

        TransformToWorld(model, (float)s.x, (float)s.y, (float)s.z, d.x, d.y, d.z);

        if (lit) {
            RotateToWorld(model,
                          (float)s.nx * (1.0f / 127.0f),
                          (float)s.ny * (1.0f / 127.0f),
                          (float)s.nz * (1.0f / 127.0f),
                          d.nx, d.ny, d.nz);
            // Model matrices carry scale, so rotating a unit normal by one does not leave it
            // unit length (it is not zero, though). The engine's own rasteriser never noticed, because
            // it lit in software and D3D lighting is off here either way. A path tracer does
            // notice: it shades with whatever normal it is handed, and a normal whose length
            // is wrong shades a flat wall as though it were curved.
            const float len2 = d.nx * d.nx + d.ny * d.ny + d.nz * d.nz;
            if (len2 > 1e-12f) {
                const float inv = 1.0f / sqrtf(len2);
                d.nx *= inv;
                d.ny *= inv;
                d.nz *= inv;
            }
            // Lit vertices take their colour from the constant alone, so white is what is
            // left once the constant is factored back out.
            g_slotUntinted[destSlot + i] = 0xffffffffu;
        } else {
            d.nx = 0.0f;
            d.ny = 1.0f;
            d.nz = 0.0f;
            // The four source bytes sit in memory as r, g, b, a -- the original pairs byte
            // +0 with the constant colour's red channel. D3DCOLOR is 0xAARRGGBB, so red and
            // blue swap on the way in; taking the dword as-is tinted the whole world
            // backwards.
            const unsigned int c = s.bgra;
            g_slotUntinted[destSlot + i] = 0xff000000u | ((c & 0x000000ffu) << 16) |
                                           (((c >> 8) & 0xffu) << 8) | ((c >> 16) & 0xffu);
        }
        // Filled properly at draw time, when the constant that actually applies is known.
        d.diffuse = g_slotUntinted[destSlot + i];
        d.u = s.u;
        d.v = s.v;
    }
    ++g_batchSerial;
    for (int i = 0; i < count; ++i) {
        g_slotWritten[destSlot + i] = 1;
        memcpy(g_slotRaw[destSlot + i], &src[i], 20);
        g_slotSrc[destSlot + i][0] = src[i].x;
        g_slotSrc[destSlot + i][1] = src[i].y;
        g_slotSrc[destSlot + i][2] = src[i].z;
        g_slotBatch[destSlot + i] = g_batchSerial;
        g_slotMatrix[destSlot + i] = g_matrixSerial;
        AuditVertex(g_vertices[destSlot + i]);
    }
    if (destSlot + count > g_highWater) {
        g_highWater = destSlot + count;
    }
}

void EmitTriangle(int a, int b, int c) {
    if (g_skipMask && !Config::DisableTriangleSkipMask()) {
        const int n = g_skipCounter++;
        if (n < kSkipMaskLimit && (g_skipMask[n >> 5] & (1u << (n & 31)))) {
            ++g_skippedTris;
            return;
        }
    }
    // Audit only what survives culling -- measuring before the skip test counts triangles
    // that never reach the GPU and overstates the damage.
    if (a < kMaxVertices && b < kMaxVertices && c < kMaxVertices) {
        AuditTriangle(a, b, c);
    }
    if (g_indexCount + 3 > kMaxIndices) {
        Flush();
    }
    if (a >= kMaxVertices || b >= kMaxVertices || c >= kMaxVertices) {
        return;
    }
    g_indices[g_indexCount++] = (unsigned short)a;
    g_indices[g_indexCount++] = (unsigned short)b;
    g_indices[g_indexCount++] = (unsigned short)c;
}

// Each dword packs three vertex indices in bytes 3, 2 and 1.
void EmitPackedTriangle(unsigned int word) {
    EmitTriangle((word >> 24) & 0xff, (word >> 16) & 0xff, (word >> 8) & 0xff);
}

const void* ResolvePointer(unsigned int packed) {
    if (packed & 0x0f000000) {
        const unsigned int seg = (packed >> 24) & 0x0f;
        const unsigned int base = g_segmentBase[seg];
        if (!base) {
            return 0;
        }
        return (const void*)(base + (packed & 0x00ffffff));
    }
    return (const void*)packed;
}

void BuildProjection() {
    IDirect3DDevice9* dev = Device::Get();
    if (!dev) {
        return;
    }
    if (g_haveProjection) {
        dev->SetTransform(D3DTS_PROJECTION, &g_proj);
        return;
    }
    // No camera factored yet: a plain 90-degree perspective frustum matched to the viewport
    // aspect, until the first view matrix brings the engine's own projection.
    const float aspect = (g_screenHeight > 0)
                       ? (float)g_screenWidth / (float)g_screenHeight : 1.3333f;
    const float zn = g_zNear > 0.0f ? g_zNear : 1.0f;
    const float zf = g_zFar > zn ? g_zFar : zn + 1.0f;

    D3DMATRIX p;
    memset(&p, 0, sizeof(p));
    p._11 = 1.0f / aspect;
    p._22 = 1.0f;
    p._33 = zf / (zf - zn);
    p._34 = 1.0f;
    p._43 = -zn * zf / (zf - zn);
    dev->SetTransform(D3DTS_PROJECTION, &p);
}

// One line per draw, plus a second when the normals are not unit length. This is what says
// whether a wall that looks see-through bound a texture that is mostly transparent, whether
// it is lit by normals that do not point anywhere sensible, and whether the engine asked for
// environment-map texture coordinates -- which this renderer does not generate, so such a
// surface samples its texture somewhere other than where the engine intended.
void DumpBatch(int vertexCount, bool textured, bool culled, bool sky) {
    ++g_dumpBatch;

    char texture[160];
    if (g_currentTexture >= 0) {
        Texture::Describe(g_currentTexture, g_currentSubA, g_currentSubB, texture,
                          sizeof(texture));
    } else {
        lstrcpynA(texture, "untextured", sizeof(texture));
    }
    if (!textured && g_currentTexture >= 0) {
        lstrcatA(texture, "  [NOT BOUND]");
    }

    float lo[3] = { 1e30f, 1e30f, 1e30f };
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    int zeroNormals = 0;
    int badLengthNormals = 0;
    for (int i = 0; i < vertexCount && i < kMaxVertices; ++i) {
        const WorldVertex& v = g_vertices[i];
        const float p[3] = { v.x, v.y, v.z };
        for (int c = 0; c < 3; ++c) {
            if (p[c] < lo[c]) lo[c] = p[c];
            if (p[c] > hi[c]) hi[c] = p[c];
        }
        const float len2 = v.nx * v.nx + v.ny * v.ny + v.nz * v.nz;
        if (len2 < 1e-6f) {
            ++zeroNormals;
        } else if (len2 < 0.81f || len2 > 1.21f) {
            ++badLengthNormals;
        }
    }

    const unsigned int constant = ConstantColourForBatch();
    LogLine("      #%-4d %s", g_dumpBatch, texture);
    LogLine("            %3d tris %3d verts  flags=0x%06x%s%s%s%s  mode=0x%02x const=%06x"
            "  %s%s",
            g_indexCount / 3, vertexCount, g_renderFlags,
            (g_renderFlags & kFlagLighting) ? " lit" : " unlit",
            (g_renderFlags & 0x40000) ? " TEXGEN-ENV" : "",
            culled ? " cull" : "",
            sky ? " sky" : "",
            g_combinerMode < 0 ? 0xff : g_combinerMode,
            constant & 0x00ffffffu,
            zeroNormals ? "ZERO-NORMALS " : "",
            badLengthNormals ? "NON-UNIT-NORMALS" : "");
    if (zeroNormals || badLengthNormals) {
        LogLine("            normals: %d zero, %d non-unit of %d", zeroNormals,
                badLengthNormals, vertexCount);
    }
    LogLine("            bbox x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]",
            lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
}

void ApplyRenderStates() {
    IDirect3DDevice9* dev = Device::Get();
    if (!dev) {
        return;
    }
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    // Cull mode is per-batch state, applied at draw time in Flush().
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    // The alpha test is what cuts world surfaces out; blending is applied per batch in
    // Flush(). The original sets ALPHAREF 0 with D3DCMP_GREATER, which discards exactly the
    // fully transparent texels -- the same set as >= 1.
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHAREF, 1);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    dev->SetFVF(kWorldFVF);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

} // namespace

void Geometry::SetZPlanes(float zNear, float zFar) {
    g_zNear = zNear;
    g_zFar = zFar;
}

void Geometry::SetViewportSize(int width, int height) {
    g_screenWidth = width;
    g_screenHeight = height;
}

void Geometry::DumpNextFrame() {
    g_dumpArmed = true;
}

void Geometry::BeginFrame() {
    if (g_dumpArmed) {
        g_dumpArmed = false;
        g_dumpingFrame = true;
        g_dumpBatch = 0;
        LogLine("    ==== FRAME DUMP ====");
        const float* v = &g_view._11;
        LogLine("      camera fwd(%.3f, %.3f, %.3f) t(%.1f, %.1f, %.1f)",
                v[2], v[6], v[10], v[12], v[13], v[14]);
    }
    g_indexCount = 0;
    g_highWater = 0;
    g_stackDepth = 0;
    g_renderFlags = 0;
    g_currentTexture = -1;
    g_currentSubA = 0;
    g_currentSubB = 0;
    g_skipMask = 0;
    g_skipCounter = 0;
    // A frame inherits no constant colour: white is the identity for the multiply, so a
    // batch that never sets one is left exactly as its vertices describe it.
    // The combiner mode and the two constant colours are deliberately not reset here: they
    // are plain globals in the original, written only by their opcodes and never cleared per
    // frame. Clearing them would draw everything ahead of the frame's first 0xaf/0xfa/0xfb
    // untinted -- white walls, one frame in every few.
    g_skyboxDrawn = false;
    Identity(g_modelStack[0]);
    if (g_viewDirty) {
        g_viewDirty = false;
    }
    ApplyRenderStates();
    BuildProjection();
}

void Geometry::EndFrame() {
    Flush();
    if (g_dumpingFrame) {
        g_dumpingFrame = false;
        LogLine("    ==== END FRAME DUMP: %d batches ====", g_dumpBatch);
    }
}

void Geometry::ProcessDisplayList(const unsigned int* list) {
    if (!list) {
        return;
    }
    if (g_listDepth == 0) {
        // Slot ownership is scoped to one top-level list, matching how the engine fills and
        // then references the vertex window.
        memset(g_slotWritten, 0, sizeof(g_slotWritten));
        ++g_audit.lists;
    }
    if (g_listDepth > 32) {
        LogLine("    display list nested too deep -- bailing");
        return;
    }
    ++g_listDepth;
    for (int guard = 0; guard < 1 << 20; ++guard) {
        const unsigned int word0 = list[0];
        const unsigned int word1 = list[1];
        const unsigned char op = (unsigned char)(word0 & 0xff);
        ++g_opcodeSeen[op];

        if (op == OP_END) {
            Flush();
            --g_listDepth;
            return;
        }

        switch (op) {
        case OP_MATRIX: {
            const float* m = (const float*)ResolvePointer(word1);
            if (m) {
                SetMatrixFromList(m, (unsigned char)((word0 >> 8) & 0xff));
            }
            break;
        }
        case OP_VERTICES: {
            const SourceVertex* v = (const SourceVertex*)ResolvePointer(word1);
            if (v) {
                UploadVertices(v, (int)(word0 >> 16), (int)((word0 >> 8) & 0xff));
            }
            break;
        }
        case OP_SUBLIST:
            ++g_audit.sublists;
            ProcessDisplayList((const unsigned int*)ResolvePointer(word1));
            break;

        case OP_TRIANGLES: {
            // word1's low byte counts the triangles; they are packed two per 8-byte command
            // in the words that follow.
            int count = (int)(word1 & 0xff);
            const unsigned int* p = list;
            g_emitOpcode = OP_TRIANGLES;
            if (count > 1) {
                const int pairs = count >> 1;
                count -= pairs * 2;
                for (int i = 0; i < pairs; ++i) {
                    EmitPackedTriangle(p[0]);
                    EmitPackedTriangle(p[1]);
                    p += 2;
                }
            }
            // An odd tail comes from word1 of the command after the pairs.
            g_emitOpcode = OP_TRIANGLES;
            if (count == 1) {
                EmitPackedTriangle(p[1]);
            } else if (p != list) {
                p -= 2;
            }
            list = p;
            break;
        }
        case OP_2D_PRIM:
            // Screen-space primitive; not drawn here, but standard length.
            break;

        case OP_2D_PRIM_LONG:
            list += 2;      // one extra command, on top of the loop's own advance
            break;

        case OP_SKIP24:
            list += 4;      // two extra commands
            break;

        case OP_SEGMENT: {
            // Only sub-command 6 installs a base; the rest are unrelated state.
            const int sub = (int)((signed char)((word0 >> 24) & 0xff));
            if (sub == 6) {
                const int index = (int)((signed char)((word0 >> 8) & 0xff)) / 4;
                if (index >= 0 && index < kSegmentCount) {
                    g_segmentBase[index] = word1;
                }
            }
            break;
        }
        case OP_TRI_PAIR:
            g_emitOpcode = OP_TRI_PAIR;
            g_emitWord0 = word0; g_emitWord1 = word1;
            EmitPackedTriangle(word1);
            EmitPackedTriangle(word0);
            break;

        case OP_TEXTURE_ALT:
        case OP_TEXTURE: {
            // The index is a sign-extended 16-bit field in word0 bits 8..23. Opcode 0xa6
            // biases it by one; 0xa9 does not.
            int tex = (int)((signed short)((word0 >> 8) & 0xffff));
            if (op == OP_TEXTURE_ALT) {
                --tex;
            }
            if (g_textureSelects < 60) {
                ++g_textureSelects;
                LogLine("    tex select: word0=0x%08x word1=0x%08x -> index %d  %s",
                        word0, word1, tex, Texture::Name(tex));
                if (g_textureSelects <= 12) {
                    char path[128];
                    _snprintf(path, sizeof(path), "texdump_%03d.bin", tex);
                    path[sizeof(path) - 1] = 0;
                    Texture::DumpSet(tex, path);
                }
            }
            // word1 holds the two sub-chunk selectors: the low half sign-extended, the
            // high half as-is. Texture::GetTexture turns them into a frame and a palette.
            const int subA = (int)(short)(word1 & 0xffff);
            const int subB = (int)(word1 >> 16);
            {
                const char* setName = Texture::Name(tex);
                if (setName[0] == 'M' && setName[1] == 'T' && setName[2] == 'H') {
                    ++g_mouthSelects;
                    if (g_mouthSelects <= 40 || (g_mouthSelects % 250) == 0) {
                        LogLine("    mouth select #%u: set %d word1=0x%08x subA=%d subB=%d  %s",
                                g_mouthSelects, tex, word1, subA, subB, setName);
                    }
                }
            }
            if (tex != g_currentTexture || subA != g_currentSubA || subB != g_currentSubB) {
                Flush();
                g_currentTexture = tex;
                g_currentSubA = subA;
                g_currentSubB = subB;
            }
            break;
        }

        case OP_BACKGROUND:
            Device::SetClearColour(word1);
            break;

        case OP_COMBINER_MODE:
            // FUN_1000c951 stores word1 and marks the state dirty; FUN_10005850 then
            // resolves it through the mode table the next time it runs.
            //
            // Break the batch first. The mode decides both which constant colour applies --
            // primitive, environment or none -- and whether the batch blends, and both of
            // those are read when the batch is drawn. Triangles already queued were queued
            // under the old mode and have to be drawn under it.
            if ((int)word1 != g_combinerMode) {
                Flush();
            }
            g_combinerMode = (int)word1;
            if (g_combinerMode >= 0 && g_combinerMode < kCombinerModeCount) {
                ++g_modeSelects[g_combinerMode];
            }
            break;

        case OP_PRIM_COLOUR:
        case OP_ENV_COLOUR: {
            // This has to break the batch.
            //
            // The constant is applied when the triangles are drawn -- as the hardware does it,
            // so a newly spawned object never inherits the previous object's colour -- so
            // anything still queued would be drawn with the NEW colour instead of the one it
            // was emitted under. A batch that spans a change would come out one flat colour,
            // and which geometry shares a batch shifts with the camera: house walls would
            // change colour as you turn.
            //
            // Only when the change is one the batch would actually see: a write to the
            // environment register while the mode is taking the primitive colour changes
            // nothing on screen and does not deserve a draw call.
            unsigned int& slot = (op == OP_PRIM_COLOUR) ? g_primColour : g_envColour;
            if (slot != word1) {
                const unsigned int effectiveBefore = ConstantColourForBatch();
                const unsigned int previous = slot;
                slot = word1;
                const unsigned int effectiveAfter = ConstantColourForBatch();
                slot = previous;
                if (effectiveAfter != effectiveBefore) {
                    Flush();
                }
                slot = word1;
                ++g_constantColourChanges;
            }
            break;
        }

        case OP_MATRIX_POP:
            // Without this the hierarchy stack only ever grows, and every later object
            // inherits a wrongly accumulated transform.
            if (g_stackDepth > 0) {
                --g_stackDepth;
                ++g_matrixSerial;
            }
            break;

        case OP_SKIPMASK_SET:
            g_skipMask = (const unsigned int*)ResolvePointer(word1);
            g_skipCounter = 0;
            break;

        case OP_SKIPMASK_CLR:
            g_skipMask = 0;
            break;

        case OP_TRI_ONE:
            g_emitOpcode = OP_TRI_ONE;
            g_emitWord0 = word0; g_emitWord1 = word1;
            EmitPackedTriangle(word1);
            break;

        case OP_FLAGS_SET:
            g_renderFlags |= (word1 & kFlagMask);
            break;

        case OP_FLAGS_CLR:
            g_renderFlags &= ~(word1 & kFlagMask);
            break;

        default:
            break;
        }
        list += 2;
    }
    LogLine("    display list guard tripped -- malformed stream?");
    --g_listDepth;
}

void Geometry::LogGeometryAudit() {
    if (g_audit.tris > 0) {
        ++g_worst.frames;
        g_worst.tris += g_audit.tris;
        g_worst.staleRefs += g_audit.staleRefs;
        g_worst.degenerate += g_audit.degenerate;
        g_worst.badFloats += g_audit.badFloats;
        g_worst.aspect10 += g_audit.aspect10;
        g_worst.aspect100 += g_audit.aspect100;
        g_worst.aspect1000 += g_audit.aspect1000;
        if (g_audit.maxAspect > g_worst.maxAspect) {
            g_worst.maxAspect = g_audit.maxAspect;
            g_worst.worstFrameTris = g_audit.tris;
        }
        if (g_audit.maxEdge > g_worst.maxEdge) {
            g_worst.maxEdge = g_audit.maxEdge;
        }
        if (g_audit.verts > 0) {
            for (int i = 0; i < 3; ++i) {
                if (!g_worst.haveBB || g_audit.bbMin[i] < g_worst.bbMin[i]) g_worst.bbMin[i] = g_audit.bbMin[i];
                if (!g_worst.haveBB || g_audit.bbMax[i] > g_worst.bbMax[i]) g_worst.bbMax[i] = g_audit.bbMax[i];
                const float span = g_audit.bbMax[i] - g_audit.bbMin[i];
                if (span > g_worst.widestFrame[i]) g_worst.widestFrame[i] = span;
            }
            g_worst.haveBB = true;
        }
        if (g_worst.minFrameTris == 0 || g_audit.tris < g_worst.minFrameTris) {
            g_worst.minFrameTris = g_audit.tris;
        }
        if (g_audit.tris > g_worst.maxFrameTris) {
            g_worst.maxFrameTris = g_audit.tris;
        }
        // Periodic running summary, which is what actually shows whether any frame breaks.
        if ((g_worst.frames % 400) == 0) {
            LogLine("    GEO WORST after %d frames: %d tris total", g_worst.frames, g_worst.tris);
            LogLine("      stale %d  degenerate %d  non-finite %d",
                    g_worst.staleRefs, g_worst.degenerate, g_worst.badFloats);
            LogLine("      aspect >10/>100/>1000 : %d / %d / %d   worst %.1f (in a %d-tri frame)",
                    g_worst.aspect10, g_worst.aspect100, g_worst.aspect1000,
                    g_worst.maxAspect, g_worst.worstFrameTris);
            LogLine("      tris spanning >1 upload batch : %d   >1 model matrix : %d   (skipped by mask: %d)",
                    g_mixedBatchTris, g_mixedMatrixTris, g_skippedTris);
            LogLine("      tris drawn ahead of a vertex-window overwrite : %d", g_uploadFlushTris);
            LogLine("      per-opcode: op / tris / >10 / >100 / >1000 / worst");
            for (int o = 0; o < 256; ++o) {
                if (g_opStat[o].tris) {
                    LogLine("        0x%02x  %8d  %6d %6d %6d   %.1f", o, g_opStat[o].tris,
                            g_opStat[o].aspect10, g_opStat[o].aspect100,
                            g_opStat[o].aspect1000, g_opStat[o].worst);
                }
            }
            LogLine("      longest edge %.1f   world bbox x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]",
                    g_worst.maxEdge, g_worst.bbMin[0], g_worst.bbMax[0],
                    g_worst.bbMin[1], g_worst.bbMax[1], g_worst.bbMin[2], g_worst.bbMax[2]);
            LogLine("      tris/frame min %d max %d   widest single frame %.0f x %.0f x %.0f",
                    g_worst.minFrameTris, g_worst.maxFrameTris,
                    g_worst.widestFrame[0], g_worst.widestFrame[1], g_worst.widestFrame[2]);
            LogOpcodeCensus();
        }
    }

    // Only report frames carrying real scene geometry; the menu backdrop is two triangles
    // and says nothing about whether the interpreter is coherent.
    if (g_audit.tris < 300 || (g_worst.frames % 200) != 0) {
        memset(&g_audit, 0, sizeof(g_audit));
        return;
    }
    ++g_auditFramesLogged;
    LogLine("    GEO AUDIT frame %d: %d lists, %d tris", g_auditFramesLogged,
            g_audit.lists, g_audit.tris);
    LogLine("      stale-slot refs : %d  (%.1f%%)", g_audit.staleRefs,
            100.0 * g_audit.staleRefs / g_audit.tris);
    LogLine("      degenerate      : %d", g_audit.degenerate);
    LogLine("      non-finite      : %d", g_audit.badFloats);
    LogLine("      longest edge    : %.1f  (slots %d,%d,%d)", g_audit.maxEdge,
            g_audit.maxEdgeTri[0], g_audit.maxEdgeTri[1], g_audit.maxEdgeTri[2]);
    LogLine("      aspect >2/>10/>100/>1000 : %d / %d / %d / %d",
            g_audit.aspect2, g_audit.aspect10, g_audit.aspect100, g_audit.aspect1000);
    LogLine("      worst aspect    : %.1f  (slots %d,%d,%d)", g_audit.maxAspect,
            g_audit.maxAspectTri[0], g_audit.maxAspectTri[1], g_audit.maxAspectTri[2]);
    LogLine("      verts uploaded  : %d", g_audit.verts);
    LogLine("      draws with backface cull on/off : %d / %d",
            g_audit.culledBatches, g_audit.unculledBatches);
    LogLine("      world bbox      : x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]",
            g_audit.bbMin[0], g_audit.bbMax[0],
            g_audit.bbMin[1], g_audit.bbMax[1],
            g_audit.bbMin[2], g_audit.bbMax[2]);
    const float* v = &g_view._11;
    LogLine("      view fwd/pos    : fwd(%.3f, %.3f, %.3f) t(%.1f, %.1f, %.1f)   sublists: %d",
            v[2], v[6], v[10], v[12], v[13], v[14], g_audit.sublists);
    LogLine("      edge length histogram (log2 buckets, world units):");
    for (int i = 0; i < 12; ++i) {
        if (g_audit.edgeHist[i]) {
            LogLine("        <%-8d %d", 1 << (i + 1), g_audit.edgeHist[i]);
        }
    }
    memset(&g_audit, 0, sizeof(g_audit));
}

void Geometry::LogTextureStats() {
    LogLine("    texture stats: %d selects, %d textured flushes, %d untextured, %d sky",
            g_textureSelects, g_texturedFlushes, g_untexturedFlushes, g_skyBatches);
    LogLine("    lit tris %u / unlit tris %u", g_litTris, g_unlitTris);
    LogLine("    flushes opaque %u / blended %u", g_opaqueFlushes, g_blendedFlushes);
    LogLine("    UNTEXTURED batches by (mode, constant, blend) -- this is where the houses are:");
    for (int i = 0; i < g_untexturedKindCount; ++i) {
        const UntexturedKind& k = g_untexturedKinds[i];
        LogLine("      mode=%02X  constant R=%02X G=%02X B=%02X A=%02X  %-8s  %7u flushes  "
                "%9u tris", k.mode & 0xff,
                k.constant & 0xff, (k.constant >> 8) & 0xff, (k.constant >> 16) & 0xff,
                (k.constant >> 24) & 0xff,
                k.blend == 1 ? "ADDITIVE" : (k.blend == 2 ? "SRCALPHA" : "opaque"),
                k.flushes, k.tris);
    }
    LogLine("    constant-colour changes (opcodes 0xfa/0xfb): %u   uploads tinted %u / plain %u",
            g_constantColourChanges, g_tintedUploads, g_untintedUploads);
    LogLine("    combiner modes selected (opcode 0xaf), mode: times selected / constant colour:");
    for (int i = 0; i < kCombinerModeCount; ++i) {
        if (g_modeSelects[i]) {
            const char* src = kConstantSource[i] == CONST_PRIM ? "primitive"
                            : kConstantSource[i] == CONST_ENV  ? "environment" : "none";
            LogLine("      0x%02x  %8u  %s", i, g_modeSelects[i], src);
        }
    }
    LogLine("    frames where the 2D layer drew first and the sky stood down: %u",
            g_skyFramesYieldedTo2D);
    LogLine("    batches drawn with no set selected: %u flushes, %u tris",
            g_noSelectFlushes, g_noSelectTris);

    // Sorted by triangles, so the sets that actually cover the screen come first. A set
    // listed with NO TEXTURE draws white -- that is the difference between a wrong palette
    // and nothing bound at all.
    LogLine("    DRAW CENSUS (per texture set, by triangles drawn):");
    // Destructive selection sort: each set is printed once and then zeroed out. This runs at
    // shutdown, so consuming the counters costs nothing.
    for (int shown = 0; shown < 60; ++shown) {
        int best = -1;
        unsigned int bestTris = 0;
        for (int i = 0; i < kCensusSets; ++i) {
            const unsigned int t = g_setDraws[i].tris;
            if (t == 0) {
                continue;
            }
            if (best < 0 || t > bestTris) {
                best = i;
                bestTris = t;
            }
        }
        if (best < 0) {
            break;
        }
        const SetDraws& d = g_setDraws[best];
        // Texture colour as decoded, and the tint that was applied to it. If the decoded
        // mean is brown and the tint is blue, the palette is fine and the combiner constant
        // is not; if the decoded mean is itself blue, the palette reading is wrong.
        const int mean = Texture::MeanColour(best);
        const unsigned int k = d.lastConstant;
        char colour[96];
        if (mean >= 0) {
            _snprintf(colour, sizeof(colour),
                      "  texRGB=%02X%02X%02X tint=%02X%02X%02X mode=%02X",
                      (mean >> 16) & 0xff, (mean >> 8) & 0xff, mean & 0xff,
                      k & 0xff, (k >> 8) & 0xff, (k >> 16) & 0xff, d.lastMode & 0xff);
        } else {
            _snprintf(colour, sizeof(colour), "  (never decoded)");
        }
        colour[sizeof(colour) - 1] = 0;
        LogLine("      set %4d  %9u tris  %7u flushes%s%s%s  %s  fmt=%d",
                best, d.tris, d.flushes,
                d.flushesWithoutTexture ? "   NO TEXTURE BOUND" : "",
                Texture::DecodeFailed(best) ? " (decode failed)" : "",
                colour, Texture::Name(best), Texture::Format(best));
        g_setDraws[best].tris = 0;
    }
}

void Geometry::ReleaseResources() {
    g_indexCount = 0;
    g_highWater = 0;
}
