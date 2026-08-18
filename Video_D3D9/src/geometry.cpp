#include "geometry.h"
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

// Texture set selected by opcode 0xa6. -1 means untextured.
int g_currentTexture = -1;

// Per-triangle skip bitmask. Opcode 0xa8 installs it and resets the counter, 0xa7 clears it.
// Each emitted triangle consumes one counter step; a set bit means the engine wants that
// triangle dropped. Ignoring this draws geometry the original culls.
const unsigned int* g_skipMask = 0;
int g_skipCounter = 0;
const int kSkipMaskLimit = 0x80;
int g_textureSelects = 0;
int g_clampLogged = 0;


int g_texturedFlushes = 0;
int g_untexturedFlushes = 0;

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
};
GeoWorst g_worst;
unsigned char g_slotWritten[kMaxVertices];   // per display list, not per frame

// Which vertex-upload batch last wrote each slot, and which model matrix that batch used.
// A triangle whose three slots come from different batches is stitching together geometry
// that was baked under different transforms.
short g_slotSrc[kMaxVertices][3];   // source object-space coords, for blame reporting
unsigned char g_slotRaw[kMaxVertices][20];  // the whole source vertex, to judge if it is real
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

// One-shot opcode census for the first display list, so the stream's real content is known
// rather than inferred from the original's dispatch table.
bool g_opcodeCensusDone = false;
int  g_opcodeSeen[256];
int  g_listDepth = 0;

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
    const float a = (fabsf(rz[best]) > 1e-6f) ? (m[2 + best * 4] / rz[best]) : 1.0f;
    const float b = m[14] - v[14] * a;

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
    LogLine("    flush: %d tris, %d verts", g_indexCount / 3, vertexCount);

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
    dev->SetRenderState(D3DRS_CULLMODE,
                        (g_renderFlags & kFlagBackfaceCull) ? D3DCULL_CCW : D3DCULL_NONE);

    IDirect3DTexture9* tex = (g_currentTexture >= 0) ? Texture::GetTexture(g_currentTexture) : 0;
    if (tex) { ++g_texturedFlushes; } else { ++g_untexturedFlushes; }
    if (g_renderFlags & kFlagBackfaceCull) { ++g_audit.culledBatches; }
    else { ++g_audit.unculledBatches; }
    dev->SetTexture(0, tex);
    // Modulating against diffuse keeps pre-lit vertex colours meaningful; lit geometry
    // carries white so the texture passes through untouched.
    dev->SetTextureStageState(0, D3DTSS_COLOROP, tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, tex ? D3DTOP_SELECTARG1 : D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, tex ? D3DTA_TEXTURE : D3DTA_DIFFUSE);

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
            d.diffuse = 0xffffffffu;
        } else {
            d.nx = 0.0f;
            d.ny = 1.0f;
            d.nz = 0.0f;
            d.diffuse = s.bgra | 0xff000000u;
        }
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
    if (g_skipMask) {
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

void ApplyRenderStates() {
    IDirect3DDevice9* dev = Device::Get();
    if (!dev) {
        return;
    }
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    // Cull mode is per-batch state, applied at draw time in Flush().
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    // The palette's low bit is genuine per-texel opacity, so alpha testing gives the game
    // its cut-out foliage and signage.
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHAREF, 0x80);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    dev->SetFVF(kWorldFVF);
    // Untextured: colour comes from the vertex diffuse channel.
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
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

void Geometry::BeginFrame() {
    g_indexCount = 0;
    g_highWater = 0;
    g_stackDepth = 0;
    g_renderFlags = 0;
    g_currentTexture = -1;
    g_skipMask = 0;
    g_skipCounter = 0;
    Identity(g_modelStack[0]);
    if (g_viewDirty) {
        g_viewDirty = false;
    }
    ApplyRenderStates();
    BuildProjection();
}

void Geometry::EndFrame() {
    Flush();
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
        if (!g_opcodeCensusDone) {
            ++g_opcodeSeen[op];
        }

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
            if (g_textureSelects < 12) {
                ++g_textureSelects;
                LogLine("    tex select: word0=0x%08x -> index %d", word0, tex);
                char path[128];
                _snprintf(path, sizeof(path), "texdump_%03d.bin", tex);
                path[sizeof(path) - 1] = 0;
                Texture::DumpSet(tex, path);
            }
            if (tex != g_currentTexture) {
                Flush();
                g_currentTexture = tex;
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
            }
            g_worst.haveBB = true;
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
        }
    }

    // Only report frames carrying real scene geometry; the menu backdrop is two triangles
    // and says nothing about whether the interpreter is coherent.
    if (g_auditFramesLogged >= 4 || g_audit.tris < 300) {
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
    LogLine("      edge length histogram (log2 buckets, world units):");
    for (int i = 0; i < 12; ++i) {
        if (g_audit.edgeHist[i]) {
            LogLine("        <%-8d %d", 1 << (i + 1), g_audit.edgeHist[i]);
        }
    }
    memset(&g_audit, 0, sizeof(g_audit));
}

void Geometry::LogTextureStats() {
    LogLine("    texture stats: %d selects, %d textured flushes, %d untextured",
            g_textureSelects, g_texturedFlushes, g_untexturedFlushes);
}

void Geometry::LogOpcodeCensus() {
    if (g_opcodeCensusDone) {
        return;
    }
    // Nothing to report until a frame has actually carried a display list.
    int total = 0;
    for (int i = 0; i < 256; ++i) {
        total += g_opcodeSeen[i];
    }
    if (total == 0) {
        return;
    }
    g_opcodeCensusDone = true;
    LogLine("    display-list opcode census:");
    for (int i = 0; i < 256; ++i) {
        if (g_opcodeSeen[i]) {
            LogLine("      op 0x%02x  x%d", i, g_opcodeSeen[i]);
        }
    }
}

void Geometry::ReleaseResources() {
    g_indexCount = 0;
    g_highWater = 0;
}
