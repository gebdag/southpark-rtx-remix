#include "engine.h"
#include "config.h"
#include "log.h"
#include "lookdev.h"

#include <windows.h>
#include <string.h>

namespace {

// SouthPark.exe is Petite-packed, but it unpacks in place at its preferred base, so the
// addresses recovered from the dumped image are also the addresses in the running process.
// Every site below is verified against the bytes it expects before anything is written; a
// mismatch logs and skips rather than corrupting a build these addresses do not belong to.

// ------------------------------------------------------------------- patch helpers

bool Verify(unsigned int address, const unsigned char* expected, size_t size) {
    return !IsBadReadPtr((const void*)address, size) &&
           memcmp((const void*)address, expected, size) == 0;
}

bool Overwrite(unsigned int address, const void* bytes, size_t size) {
    DWORD previous = 0;
    if (!VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &previous)) {
        return false;
    }
    memcpy((void*)address, bytes, size);
    DWORD ignored = 0;
    VirtualProtect((void*)address, size, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)address, size);
    return true;
}

// Rewrites one 4-byte operand embedded in an instruction, after checking that the operand
// currently holds the value this was built against.
bool PatchOperand(unsigned int address, unsigned int expected, unsigned int replacement) {
    unsigned char bytes[4];
    memcpy(bytes, &expected, 4);
    if (!Verify(address, bytes, 4)) {
        LogLine("    engine: 0x%08X does not hold 0x%08X -- not patching", address, expected);
        return false;
    }
    return Overwrite(address, &replacement, 4);
}

// ------------------------------------------------------------------- frustum test
//
// FUN_004468d0(planeSet, aabb): the six-plane frustum test, __cdecl, 1 when the box is inside.
// It is the last of the engine's visibility tests, after the room test and the 2D X/Z box test
// below; DisableFrustumTest bypasses it.
//
// It has to be bypassed AT THE CALL SITE rather than by rewriting the function, because only
// six of its seven callers are drawing:
//
//   0x0043c26a  FUN_0043c080  <- 0x0043d677                     display-list emit
//   0x00440d38  FUN_00440910  <- 0x0043d4fc                     display-list emit
//   0x00440e63  FUN_00440910
//   0x004411bc  FUN_00440f50  <- 0x0042243d                     frame build
//   0x00441313  FUN_00440f50
//   0x004432b7  FUN_00443230  <- 0x00422489                     frame build
//   0x00450724  FUN_00450630  <- 0x004146e8, 0x0041473c, ...    GAMEPLAY
//
// That last one is not a renderer function. FUN_00450630 takes an entity, tests the camera's
// room mask against the entity's own (0x004506c3), compares the camera's bounds box against
// the entity's (0x004506ca onwards), and only then reaches the frustum test -- and along the
// way it writes a visibility flag back onto the entity and registers it for submission,
// abandoning the entity when that registration returns nothing (0x0045072e). Four of its seven
// callers sit in the gameplay code around 0x00414700.
//
// Rewriting the shared function would run every entity in the level down that path at once,
// which produces not more scenery but enemies and NPCs that stop existing and a player's
// weapon that stops working. Redirecting only the six drawing call sites leaves entity logic
// on the engine's own answer.
const unsigned int kFrustumPlaneTest = 0x004468D0;
const unsigned int kFrustumDrawCallSites[] = {
    0x0043C26A, 0x00440D38, 0x00440E63, 0x004411BC, 0x00441313, 0x004432B7,
};
const unsigned char kAlwaysVisible[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };  // mov eax,1; ret

// The seventh site, inside the entity path. South Park's town is built out of entities, so
// leaving this one alone leaves the town culled -- the six drawing sites above are not where
// the houses are decided. It has to be bypassed too, but not without a bound: the visible
// side of FUN_00450630 registers the entity through a resource cache (FUN_0044bcc0, a hash
// table of 0x2000 slots at 0x00599054), and reporting every object in the level visible every
// frame starves it (see the resource cache below).
//
// So this site gets a budget instead of a stub. Objects the engine's own test accepts always
// pass; objects it rejects are let through until the budget for the frame is used up. The
// peak is logged, so the budget can be set to what the town actually needs.
const unsigned int kFrustumEntityCallSite = 0x00450724;

// ------------------------------------------------------------------- room visibility
//
// THIS is how the game brings level geometry in, and it runs BEFORE the frustum test.
//
// Every object carries a room bitmask at +0x1c and the camera one at +0x4e; an object sharing
// no bit with the camera is dropped where it stands and never reaches the frustum test, so
// widening the frustum without widening this does nothing at all.
//
// It is widened at the point of use rather than by forcing the camera's mask to all ones. The
// mask is a live field the engine recomputes (FUN_00420e20) and reads from several places;
// nopping the branch changes the decision at this one site and leaves the field alone.
//
// What remains after it is the engine's own distance box (0x004506ca onwards) and then the
// frustum test, so this does not admit the whole level -- it admits everything the camera can
// actually see, which is the point.
struct Branch {
    const char* name;
    unsigned int address;
    unsigned char expected[6];
    unsigned int length;
    bool entityPath;
};

const Branch kRoomTests[] = {
    // Drawing paths: safe, these only decide what is emitted.
    { "display-list emit", 0x0043C13C, { 0x0F, 0x84, 0xEA, 0x01, 0x00, 0x00 }, 6, false },
    { "frame build 1",     0x0044113A, { 0x0F, 0x84, 0xCC, 0x00, 0x00, 0x00 }, 6, false },
    { "frame build 2",     0x00441250, { 0x0F, 0x84, 0x05, 0x01, 0x00, 0x00 }, 6, false },
    // The entity path. South Park's town is entities, so this is the one that matters -- and
    // the one that has to stay behind the frustum budget.
    { "entity visibility", 0x004506C8, { 0x74, 0x66, 0x00, 0x00, 0x00, 0x00 },  2, true  },
};

int g_roomTestsWidened = 0;


int g_frustumSitesPatched = 0;
bool g_entitySitePatched = false;
int g_extraVisible = 0;         // overrides granted this frame
int g_extraVisiblePeak = 0;
int g_extraVisibleDenied = 0;   // rejections the budget could not cover

// The resource cache the visible side of the entity path registers through. It is a hash
// table whose slots are only ever released by the NOT-visible side, so every object the
// budget forces visible is an entry that is never given back. A few dozen overrides a frame
// fill its few thousand slots within seconds -- which is why geometry stops appearing a little
// way into a scene rather than immediately, and why it is objects that arrive LATE, after a
// cut, that go missing: by then there is no slot left to register them.
const unsigned int kResourceCacheMask  = 0x00599050;
const unsigned int kResourceCacheTable = 0x00599054;
unsigned int g_cachePeak = 0;
unsigned int g_cacheSlots = 0;

// Counts occupied slots. Walked at intervals rather than per frame: the table is thousands of
// entries and this is a diagnostic, not a hot path.
void SampleResourceCache() {
    if (IsBadReadPtr((const void*)kResourceCacheMask, 4) ||
        IsBadReadPtr((const void*)kResourceCacheTable, 4)) {
        return;
    }
    const unsigned int mask = *(const unsigned int*)kResourceCacheMask;
    void** table = *(void***)kResourceCacheTable;
    if (!table || mask == 0 || mask > 0xFFFF) {
        return;
    }
    const unsigned int slots = mask + 1;
    if (IsBadReadPtr(table, slots * sizeof(void*))) {
        return;
    }
    unsigned int used = 0;
    for (unsigned int i = 0; i < slots; ++i) {
        if (table[i]) {
            ++used;
        }
    }
    g_cacheSlots = slots;
    if (used > g_cachePeak) {
        g_cachePeak = used;
    }
    if (used * 10 >= slots * 9) {
        LogLine("    engine: entity resource cache %u/%u slots used -- it is nearly full, and "
                "objects that arrive after it fills cannot register, so they are never drawn. "
                "Lower UncullBudget or set UncullEntities=0.", used, slots);
    }
}

// ------------------------------------------------------- the level around the camera
//
// Every drawing path compares grid sections, objects and entities against a 2D box in X and
// Z around the camera's view volume (camera+0x332: min X, min Z, max X, max Z). The volume's
// apex is the eye, so the box never reaches behind it: what the player has just walked past
// is not drawn, and the path tracer lights the room through the gap. Loading is not the
// cause -- sections stream by a box centred on the eye (camera+0x342), so they are there,
// only not drawn.
//
// The tests stay; the box they read grows. SP_BuildCullBounds also writes the union of the
// cameras' view boxes to 0x005005f0, a copy nothing else in the engine reads. After it runs,
// that copy is widened to a square of KeepAroundCamera around each eye, and the drawing
// sites read it in place of the camera field: each is the same fld/fcomp with an absolute
// address instead of [reg+disp32], the same length, so nothing else in the code moves.
// Gameplay decides from the published cull box at 0x00500640 (activation, despawning,
// particle lifetime), which is left alone.
const unsigned int kBuildCullBounds         = 0x00423020;  // SP_BuildCullBounds(set), __cdecl
const unsigned int kBuildCullBoundsCallSite = 0x0044477C;  // once a frame, SP_RenderFrame
const unsigned int kViewBoxCopy  = 0x005005F0;   // min X, min Z, max X, max Z
const unsigned int kCameraList   = 0x20;         // in the camera set: first drawn camera
const unsigned int kCameraNext   = 0x04;
const unsigned int kCameraEyeX   = 0x266;        // the eye, world space
const unsigned int kCameraEyeZ   = 0x26E;
const unsigned int kCameraPlanes = 0x2D2;        // what every frustum-test caller passes
const unsigned int kCameraViewBox = 0x332;

// The four reads of the camera's view box at one test, in the order the code makes them.
struct ViewBoxTest {
    const char* name;
    unsigned char opcode;          // D8 fcomp, D9 fld
    unsigned char modrm;           // [reg+disp32]
    unsigned int sites[4];
    unsigned int fields[4];
};

const ViewBoxTest kViewBoxTests[] = {
    // FUN_00440f50: a whole grid section is skipped when its cell misses the box.
    { "level sections",   0xD8, 0x9F, { 0x004410B2, 0x004410C5, 0x004410D9, 0x004410ED },
      { 0x332, 0x33A, 0x336, 0x33E } },
    { "level geometry",   0xD9, 0x87, { 0x0044116D, 0x00441184, 0x00441194, 0x004411A4 },
      { 0x33A, 0x332, 0x33E, 0x336 } },
    { "placed instances", 0xD9, 0x87, { 0x004412C4, 0x004412D7, 0x004412E6, 0x004412F6 },
      { 0x33A, 0x332, 0x33E, 0x336 } },
    // FUN_00443230, the level's object list.
    { "level objects",    0xD9, 0x87, { 0x0044324D, 0x0044326A, 0x00443281, 0x00443298 },
      { 0x33A, 0x332, 0x33E, 0x336 } },
    // FUN_0043c080. The hidden side skips only the emitter's draw matrix; its light is
    // registered either way.
    { "particle emitters", 0xD9, 0x85, { 0x0043C20A, 0x0043C21F, 0x0043C234, 0x0043C249 },
      { 0x33A, 0x332, 0x33E, 0x336 } },
    // FUN_00450630, the entity path (the town's houses among them). The frustum test that
    // follows it is answered by EntityFrustumTest.
    { "entities",         0xD9, 0x83, { 0x004506CA, 0x004506E3, 0x004506F6, 0x00450709 },
      { 0x33A, 0x332, 0x33E, 0x336 } },
};

float g_keepAroundCamera = 0.0f;   // 0 until the sites are patched
int g_viewBoxTestsPatched = 0;
int g_keptEntities = 0;            // entities drawn this frame only because they are near
int g_keptEntitiesPeak = 0;

typedef void (__cdecl *BuildCullBoundsFn)(unsigned char*);

void __cdecl BuildCullBoundsAroundCamera(unsigned char* set) {
    ((BuildCullBoundsFn)kBuildCullBounds)(set);
    float* box = (float*)kViewBoxCopy;
    const float r = g_keepAroundCamera;
    for (const unsigned char* camera = *(unsigned char**)(set + kCameraList); camera;
         camera = *(unsigned char**)(camera + kCameraNext)) {
        const float x = *(const float*)(camera + kCameraEyeX);
        const float z = *(const float*)(camera + kCameraEyeZ);
        if (x - r < box[0]) box[0] = x - r;
        if (z - r < box[1]) box[1] = z - r;
        if (x + r > box[2]) box[2] = x + r;
        if (z + r > box[3]) box[3] = z + r;
    }
}

// `box` is the entity's bounds: min X, min Y, min Z, max X, max Y, max Z.
bool NearCamera(const void* planes, const float* box) {
    const unsigned char* camera = (const unsigned char*)planes - kCameraPlanes;
    const float x = *(const float*)(camera + kCameraEyeX);
    const float z = *(const float*)(camera + kCameraEyeZ);
    const float r = g_keepAroundCamera;
    return box[0] < x + r && box[3] > x - r && box[2] < z + r && box[5] > z - r;
}

bool PatchViewBoxTest(const ViewBoxTest& t) {
    for (int i = 0; i < 4; ++i) {
        unsigned char expected[6] = { t.opcode, t.modrm };
        memcpy(expected + 2, &t.fields[i], 4);
        if (!Verify(t.sites[i], expected, sizeof(expected))) {
            LogLine("    engine: 0x%08X is not the expected view-box read (%s) -- that test "
                    "keeps the engine's box", t.sites[i], t.name);
            return false;
        }
    }
    for (int i = 0; i < 4; ++i) {
        unsigned char patch[6] = { t.opcode, (unsigned char)((t.modrm & 0x38) | 0x05) };
        const unsigned int address = kViewBoxCopy + (t.fields[i] - kCameraViewBox);
        memcpy(patch + 2, &address, 4);
        Overwrite(t.sites[i], patch, sizeof(patch));
    }
    return true;
}

typedef int (__cdecl *FrustumTestFn)(const void*, const void*);

// The frustum test at the entity site. The engine's answer stands; an entity it rejects is
// still drawn when it lies within KeepAroundCamera of the eye, and, with UncullEntities on,
// while the frame's budget lasts.
int __cdecl EntityFrustumTest(const void* planes, const void* box) {
    if (((FrustumTestFn)kFrustumPlaneTest)(planes, box)) {
        return 1;
    }
    if (g_keepAroundCamera > 0.0f && NearCamera(planes, (const float*)box)) {
        ++g_keptEntities;
        return 1;
    }
    if (!Config::UncullEntities()) {
        return 0;
    }
    const int budget = Config::UncullBudget();
    if (budget > 0 && g_extraVisible >= budget) {
        ++g_extraVisibleDenied;
        return 0;
    }
    ++g_extraVisible;
    return 1;
}

// ------------------------------------------------------- the camera's own draw distance
//
// This is what still removes the town once the room and frustum tests are widened, and it is
// neither of them. Between those two tests SP_DrawObjectIfVisible rejects an object whose
// bounds do not overlap the camera's frustum AABB in X and Z, and that box -- along with the
// frustum corner points, the side planes and the published cull box -- is sized entirely by
// one float: the camera's draw distance at camera+0x4e4, 1024 units by default. An object
// beyond it never reaches the frustum test at all, so almost nothing gets as far as the
// budgeted override.
//
// So the distance is raised at its source and the engine rebuilds everything spatial from it,
// rather than the boxes being rewritten after the fact. That matters twice over: the derived
// volumes stay consistent with each other, and -- because camera+0x4e4 is the field the
// game's own "Fog Distance" video option moves -- a larger value is a state the engine is
// built to run in. Gameplay keeps making its decisions from a camera that is merely set to
// see further, which is why this does not repeat the entity starvation that forcing the room
// mask caused.
//
// It raises the field to a floor rather than scaling it, because the engine does not rewrite
// it every frame: on two of the three paths below the field simply keeps what it already
// holds, so a multiply would compound frame over frame until it saturated whatever ceiling it
// was given. A floor lands on the same value however many times it runs, and steps aside if
// the fog option ever asks for more.
//
// SP_UpdateCamera reaches the value by three paths and they converge at 0x00421807:
//
//   004217e9  je  0x421807          no fog option -- the field keeps the constructor's 1024
//   004217fb  jle 0x421807          option <= 0 -- likewise
//   00421801  fstp [esi+0x4e4]      option applied
//   00421807  fld dword [esp+0x10]  <- all three meet here, before anything is derived
//
// esi holds the camera across all of it. The two instructions at the meeting point are
// relocated into the detour; nothing branches into the ten bytes they occupy, verified by a
// scan of every rel8 and rel32 branch in the image. The scale is done in SSE because the very
// next instruction loads the x87 stack, and esp is moved with lea rather than add/sub so the
// detour leaves the flags alone as well.
const unsigned int kCameraDistanceSite   = 0x00421807;
const unsigned int kCameraDistanceResume = 0x00421811;
const unsigned char kCameraDistanceCode[10] = {
    0xD9, 0x44, 0x24, 0x10,                      // fld  dword ptr [esp+0x10]
    0xDC, 0x0D, 0x40, 0x94, 0x4C, 0x00,          // fmul qword ptr [0x4c9440]
};

// Read by the patched code itself, so they are globals rather than captured values.
float g_drawDistanceFloor   = 0.0f;
float g_drawDistanceApplied = 0.0f;
bool  g_drawDistancePatched = false;

// The field of view is the value loaded at the same site, in degrees: the camera eases it
// toward its target each frame (camera+0x4ec) or takes a cutscene's, then derives the vertical
// angle, the projection and the frustum corners from it. Scaling it there widens the picture
// and the culling together. The engine's own value is kept for the log.
float g_fovScale  = 1.0f;
float g_fovEngine = 0.0f;
bool  g_cameraDetourInstalled = false;

// FUN_0049bf00, "is a cutscene camera running": return [[0x4e1250] + 0x28].
const unsigned int kCutsceneTest = 0x0049bf00;
const unsigned char kCutsceneTestCode[9] = {0xA1, 0x50, 0x12, 0x4E, 0x00, 0x8B, 0x40, 0x28, 0xC3};
const unsigned int kCutsceneState = 0x004e1250;
bool g_cutsceneTestKnown = false;

// ------------------------------------------------------------------- camera aspect ratio
//
// SP_UpdateCamera holds a horizontal field of view and derives everything else from the
// camera's viewport shape (camera+0x2a2 / +0x2a6, 640x480): the vertical field of view, and
// from it an aspect ratio that reaches three consumers -- the stored camera aspect, the
// projection builder (FUN_004aea90) and the far corners of the frustum that culls entities and
// geometry. Scaling that one value after the vertical field of view is fixed widens all three
// together: the picture keeps its height and gains at the sides (Hor+), and the culling sees
// exactly what the wider picture shows.
//
//   00421880  fld  [esp+0x1c]       tan(fov/2), over the breathing scale
//   00421884  fdiv [esp+0x18]       / tan(vertical fov/2)     -> the aspect, in st0
//   00421888  fstp [esp+0x20]       <- patched: multiplied by g_cameraAspectScale first
//   0042188c  mov  eax, [esp+0x20]
//   00421890  fstp [esi+0x54]       vertical fov
//   0042189d  mov  [esi+0x58], eax  camera aspect; eax is also the projection's argument
//   00421a47  fld  [esp+0x1c]       the same slot, esp back at its earlier depth: frustum width
//
// Nothing branches into the eight relocated bytes. The engine's own picture is 640x480, so
// the scale is the display's aspect over 4:3.
const unsigned int kCameraAspectSite   = 0x00421888;
const unsigned int kCameraAspectResume = 0x00421890;
const unsigned char kCameraAspectCode[8] = {
    0xD9, 0x5C, 0x24, 0x20,                      // fstp dword ptr [esp+0x20]
    0x8B, 0x44, 0x24, 0x20,                      // mov  eax, dword ptr [esp+0x20]
};

float g_cameraAspectScale   = 1.0f;
bool  g_cameraAspectPatched = false;

// ------------------------------------------------------- the camera's visible-object lists
//
// This is where the culling actually ends, and it is a hard cap rather than a test.
//
// FUN_0043ce30(object, camera) classifies one object and appends it to per-camera lists, each
// of which simply stops accepting entries when it is full:
//
//   camera + 0x17c   list, count at + 0x97c, cap 0x200   (512 entries)
//   camera + 0x980   list, count at + 0xb80, cap 0x80    (128)
//   camera + 0xc88   list, count at + 0xe88, cap 0x80    (128)   <- what FUN_0043d690 draws
//   camera + 0xf0c   count only,             cap 0x20    (32)
//
//   0043cea2  cmp eax, 0x80          ; the cap
//   0043cea7  jge 0043cebd           ; full -- silently drop the object
//   0043cea9  mov [esi+eax*4+0xc88], edi
//
// Widening the room and frustum tests lets more objects be *considered* and changes nothing
// once the list is full.
//
// The lists are packed against each other inside the camera -- 0x17c..0x97c, then the count,
// then 0x980..0xb80, then its count -- so there is no room to raise a cap in place. Raising
// one means relocating it and rewriting every instruction that reaches it. The counters below
// show which cap is hit; the drawn list, the one that bounds the draw distance, is relocated
// by RaiseDrawnList ([FullMap] CameraDrawListScale).
const unsigned int kVisibleListConsumer = 0x0043D690;
const unsigned int kVisibleListConsumerCallSite = 0x00422475;

struct CameraList {
    const char* name;
    unsigned int countOffset;
    int cap;
    int peak;
    unsigned int framesFull;
    unsigned int framesNearFull;
};

CameraList g_cameraLists[] = {
    { "classified", 0x97C, 0x200, 0, 0, 0 },
    { "secondary",  0xB80, 0x080, 0, 0, 0 },
    { "drawn",      0xE88, 0x080, 0, 0, 0 },
    { "tertiary",   0xF0C, 0x020, 0, 0, 0 },
};

void __cdecl OnCameraListsBuilt(unsigned char* camera) {
    if (!camera || IsBadReadPtr(camera, 0xF10)) {
        return;
    }
    for (int i = 0; i < (int)(sizeof(g_cameraLists) / sizeof(g_cameraLists[0])); ++i) {
        CameraList& l = g_cameraLists[i];
        const int count = *(const int*)(camera + l.countOffset);
        if (count < 0 || count > l.cap) {
            continue;
        }
        if (count > l.peak) {
            l.peak = count;
        }
        if (count >= l.cap) {
            ++l.framesFull;
            if (l.framesFull == 1 || (l.framesFull % 600) == 0) {
                LogLine("    engine: camera %s list FULL at %d entries -- objects are being "
                        "dropped before any visibility test (%u frames so far). Lower "
                        "DrawDistance.", l.name, l.cap, l.framesFull);
            }
        } else if (count * 4 >= l.cap * 3) {
            // These caps are the one thing DrawDistance can overrun, and the engine gives no
            // sign when it does -- an object past the cap is simply not drawn. Saying so at
            // three quarters gives a chance to lower the distance before geometry starts
            // going missing, rather than after.
            ++l.framesNearFull;
            if (l.framesNearFull == 1 || (l.framesNearFull % 600) == 0) {
                LogLine("    engine: camera %s list at %d/%d -- close to the cap, and past it "
                        "objects are dropped silently. Lower DrawDistance if geometry starts "
                        "vanishing (%u frames so far)",
                        l.name, count, l.cap, l.framesNearFull);
            }
        }
    }
}

// ------------------------------------------- raising the camera's drawn-object list
//
// The list at camera+0xc88 is the one FUN_0043d690 actually draws, and it is a fixed 128
// entries: FUN_0043ce30 compares the count at camera+0xe88 against 0x80 and, when it is
// full, simply does not record the object. Nothing is logged and nothing is drawn -- which is
// why raising DrawDistance made particular objects vanish rather than making the map deeper.
//
// The list cannot grow where it is. It runs 0xc88..0xe88 and its own count sits immediately
// after it, with the next list starting at 0xe8c, so every extra entry would overwrite the
// count and then the list behind it. It has to move.
//
// Moving it is practical because every instruction that reaches it is enumerable, and each
// one has an equal-length replacement:
//
//   0043cea2  cmp eax, 0x80                    the cap          -- imm32, rewritten in place
//   0043cea9  mov [esi+eax*4+0xc88], edi        the only store   -- 7 bytes, becomes
//                                                                   mov [eax*4+abs32], edi
//   0043d6d2  lea edi, [esi+0xc88]  \           the readers -- 6 bytes each, become
//   00441f67  lea ecx, [eax+0xc88]   |                        mov reg, imm32 plus a nop
//   00442001  lea edi, [eax+0xc88]   | each paired with a read of the count at +0xe88,
//   00442064  lea ecx, [eax+0xc88]   | which stays where it is and needs no patch
//   004422d6  lea ecx, [edx+0xc88]  /
//   0043d7af  add eax, 0xc88                    the sixth reader -- 5 bytes, becomes
//                                                                   mov eax, imm32
//
// That last one forms the address with an add rather than a displacement, so a search for
// `[reg + 0xc88]` misses it; left unpatched, it reads whatever remains at camera+0xc88 and
// dereferences it as an object pointer. The list above covers every instruction in .text that
// mentions the constant in any operand form. The count at +0xe88 is left alone, so the
// engine's own bookkeeping is untouched.
//
// THE ONE CAVEAT: the relocated list is a single buffer, where the original was a field
// inside each camera. With one viewport that is exactly equivalent. With split screen two
// cameras would share it and the second would overwrite the first, so this is off unless
// asked for, and should stay off for multiplayer.
struct ListPatch {
    unsigned int address;
    unsigned char expected[8];
    unsigned int length;
    unsigned char movOpcode;   // mov r32, imm32 for the readers; 0 for the store
};

const unsigned int kDrawnListCapSite = 0x0043CEA2;   // cmp eax, 0x80  (imm32 at +1)
const unsigned char kDrawnListCapCode[5] = { 0x3D, 0x80, 0x00, 0x00, 0x00 };
const unsigned int kDrawnListStore = 0x0043CEA9;
const unsigned char kDrawnListStoreCode[7] = { 0x89, 0xBC, 0x86, 0x88, 0x0C, 0x00, 0x00 };

const ListPatch kDrawnListReaders[] = {
    { 0x0043D6D2, { 0x8D, 0xBE, 0x88, 0x0C, 0x00, 0x00 }, 6, 0xBF },   // lea edi, [esi+0xc88]
    { 0x00441F67, { 0x8D, 0x88, 0x88, 0x0C, 0x00, 0x00 }, 6, 0xB9 },   // lea ecx, [eax+0xc88]
    { 0x00442001, { 0x8D, 0xB8, 0x88, 0x0C, 0x00, 0x00 }, 6, 0xBF },   // lea edi, [eax+0xc88]
    { 0x00442064, { 0x8D, 0x88, 0x88, 0x0C, 0x00, 0x00 }, 6, 0xB9 },   // lea ecx, [eax+0xc88]
    { 0x004422D6, { 0x8D, 0x8A, 0x88, 0x0C, 0x00, 0x00 }, 6, 0xB9 },   // lea ecx, [edx+0xc88]
    { 0x0043D7AF, { 0x05, 0x88, 0x0C, 0x00, 0x00, 0x00 }, 5, 0xB8 },   // add eax, 0xc88
};

unsigned int g_drawnListCap = 0x80;

// Verifies every site before writing any of them: a half-relocated list draws from one
// address and records into another, which is a crash rather than a glitch.
bool RaiseDrawnList(unsigned int factor) {
    const unsigned int newCap = 0x80 * factor;
    const int readers = (int)(sizeof(kDrawnListReaders) / sizeof(kDrawnListReaders[0]));

    if (!Verify(kDrawnListCapSite, kDrawnListCapCode, sizeof(kDrawnListCapCode)) ||
        !Verify(kDrawnListStore, kDrawnListStoreCode, sizeof(kDrawnListStoreCode))) {
        LogLine("    engine: camera drawn-list code is not what was expected -- left alone");
        return false;
    }
    for (int i = 0; i < readers; ++i) {
        if (!Verify(kDrawnListReaders[i].address, kDrawnListReaders[i].expected,
                    kDrawnListReaders[i].length)) {
            LogLine("    engine: camera drawn-list reader at 0x%08X is not what was expected "
                    "-- list left alone", kDrawnListReaders[i].address);
            return false;
        }
    }

    void* buffer = VirtualAlloc(0, newCap * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) {
        LogLine("    engine: could not allocate %u bytes for the camera drawn list",
                newCap * 4);
        return false;
    }
    const unsigned int base = (unsigned int)buffer;

    unsigned char store[7];
    store[0] = 0x89; store[1] = 0x3C; store[2] = 0x85;    // mov [eax*4 + abs32], edi
    memcpy(store + 3, &base, 4);
    if (!Overwrite(kDrawnListStore, store, sizeof(store))) {
        VirtualFree(buffer, 0, MEM_RELEASE);
        return false;
    }
    for (int i = 0; i < readers; ++i) {
        unsigned char code[6];
        code[0] = kDrawnListReaders[i].movOpcode;          // mov reg, imm32
        memcpy(code + 1, &base, 4);
        code[5] = 0x90;                                    // padding, only the 6-byte forms
        Overwrite(kDrawnListReaders[i].address, code, kDrawnListReaders[i].length);
    }
    PatchOperand(kDrawnListCapSite + 1, 0x80, newCap);

    g_drawnListCap = newCap;
    LogLine("    engine: camera drawn-object list %u -> %u entries, relocated to 0x%08X "
            "(single viewport only)", 0x80u, newCap, base);
    return true;
}

// ------------------------------------------------------------------- draw arrays
//
// The engine records each frame's draw work into three fixed arrays and stops recording, with
// no diagnostic, once a counter reaches its bound. With culling removed a whole level is
// offered to them at once, so whatever is early in the walk fills them and the rest of the map
// is silently dropped -- which looks exactly like only the starting area being loaded.
//
// The counters are cleared at the end of every frame's build, so they read as zero anywhere
// outside it. SP_EmitDrawLists is the one point where the queue is complete and still intact.
const unsigned int kEmitDrawLists = 0x0042B230;
const unsigned int kEmitDrawListsCallSite = 0x00429FDF;

struct DrawArray {
    const char* name;
    const int* counter;
    unsigned int limitSite;     // the 4-byte bound embedded in the cmp
    unsigned int limit;
    unsigned int baseSites[2];  // instructions holding the array's base address
    unsigned int base;
    unsigned int stride;
    int peak;
    unsigned int raisedLimit;
    unsigned int saturatedFrames;
};

DrawArray g_arrays[] = {
    { "models",          (const int*)0x00502748, 0x0042A441, 0x200,
      { 0x0042A45E, 0x00429FF4 }, 0x00502754, 4,    0, 0, 0 },
    { "mesh instances",  (const int*)0x0050274C, 0x0042B18D, 0x400,
      { 0x0042B1A0, 0 },          0x00502F58, 0x24, 0, 0, 0 },
    { "material batches",(const int*)0x00502738, 0x0042B114, 0x200,
      { 0x0042B126, 0 },          0x0050BF58, 0x20, 0, 0, 0 },
};

unsigned int g_frameTick = 0;

void __cdecl OnDrawListsReady() {
    if (g_extraVisible > g_extraVisiblePeak) {
        g_extraVisiblePeak = g_extraVisible;
    }
    g_extraVisible = 0;
    if (g_keptEntities > g_keptEntitiesPeak) {
        g_keptEntitiesPeak = g_keptEntities;
    }
    g_keptEntities = 0;
    if ((++g_frameTick % 120) == 0) {
        SampleResourceCache();
    }
    for (int i = 0; i < (int)(sizeof(g_arrays) / sizeof(g_arrays[0])); ++i) {
        DrawArray& a = g_arrays[i];
        const int value = *a.counter;
        if (value > a.peak) {
            a.peak = value;
        }
        // A saturated array drops the rest of the frame's geometry silently, so say so the
        // first time and then at intervals -- a shutdown-only report cannot be correlated
        // with what was on screen when something vanished.
        const unsigned int limit = a.raisedLimit ? a.raisedLimit : a.limit;
        if ((unsigned int)value >= limit) {
            ++a.saturatedFrames;
            if (a.saturatedFrames == 1 || (a.saturatedFrames % 600) == 0) {
                LogLine("    engine: %s array FULL at %u entries -- geometry is being "
                        "dropped (%u frames so far). Raise BatchLimitScale.",
                        a.name, limit, a.saturatedFrames);
            }
        }
    }
}

// Moves one array somewhere larger and rewrites the bound. Both the bound and every base
// reference are checked against the values this was built against first, so a partial match
// leaves the array exactly as it was.
bool RaiseArray(DrawArray& a, unsigned int factor) {
    const unsigned int newLimit = a.limit * factor;
    void* buffer = VirtualAlloc(0, newLimit * a.stride, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) {
        LogLine("    engine: could not allocate %u bytes for %s", newLimit * a.stride, a.name);
        return false;
    }
    // Verify everything before writing anything: a half-relocated array is a crash.
    unsigned char expected[4];
    memcpy(expected, &a.limit, 4);
    if (!Verify(a.limitSite, expected, 4)) {
        LogLine("    engine: %s bound at 0x%08X is not %u -- array left alone",
                a.name, a.limitSite, a.limit);
        VirtualFree(buffer, 0, MEM_RELEASE);
        return false;
    }
    memcpy(expected, &a.base, 4);
    for (int i = 0; i < 2; ++i) {
        if (a.baseSites[i] && !Verify(a.baseSites[i], expected, 4)) {
            LogLine("    engine: %s base at 0x%08X is not 0x%08X -- array left alone",
                    a.name, a.baseSites[i], a.base);
            VirtualFree(buffer, 0, MEM_RELEASE);
            return false;
        }
    }

    const unsigned int newBase = (unsigned int)buffer;
    for (int i = 0; i < 2; ++i) {
        if (a.baseSites[i]) {
            PatchOperand(a.baseSites[i], a.base, newBase);
        }
    }
    PatchOperand(a.limitSite, a.limit, newLimit);
    a.raisedLimit = newLimit;
    LogLine("    engine: %-16s %u -> %u entries, relocated to 0x%08X",
            a.name, a.limit, newLimit, newBase);
    return true;
}

// ------------------------------------------------------------------- code patching

unsigned char* g_write = 0;

void Emit(unsigned char b)     { *g_write++ = b; }
void EmitDword(unsigned int v) { memcpy(g_write, &v, 4); g_write += 4; }

void EmitCall(unsigned int target) {
    Emit(0xE8);
    EmitDword(target - ((unsigned int)g_write + 4));
}

bool IsCallTo(unsigned int site, unsigned int target) {
    if (IsBadReadPtr((const void*)site, 5)) {
        return false;
    }
    const unsigned char* p = (const unsigned char*)site;
    if (p[0] != 0xE8) {
        return false;
    }
    int rel = 0;
    memcpy(&rel, p + 1, 4);
    return (unsigned int)(site + 5 + rel) == target;
}

// A __cdecl detour that runs a no-argument callback first, then tail-calls the original with
// the caller's arguments untouched.
unsigned char* BuildPreCallDetour(unsigned int original, void* callback) {
    unsigned char* cave = (unsigned char*)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE);
    if (!cave) {
        return 0;
    }
    g_write = cave;
    EmitCall((unsigned int)callback);
    Emit(0xE9);                                        // jmp original
    EmitDword(original - ((unsigned int)g_write + 4));
    return cave;
}

// Hands the first argument to a callback and then tail-jumps to the original with the
// caller's frame completely untouched.
//
// The tail jump is the point. A shim that instead calls the original after pushing a copy of
// argument one leaves that copy, and its own return address, sitting between the original and
// the arguments beyond the first -- so a two-argument function reads the shim's return address
// as its second argument. FUN_0043d690 is `push esi; push ebx; call`, and a corrupt second
// argument stops every object it draws from appearing. Jumping keeps the stack exactly as the
// caller built it, whatever the arity.
unsigned char* BuildArgumentTapDetour(unsigned int original, void* callback) {
    unsigned char* cave = (unsigned char*)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE);
    if (!cave) {
        return 0;
    }
    g_write = cave;
    Emit(0xFF); Emit(0x74); Emit(0x24); Emit(0x04);   // push dword ptr [esp+4]  (argument 1)
    EmitCall((unsigned int)callback);
    Emit(0x83); Emit(0xC4); Emit(0x04);               // add esp, 4
    Emit(0xE9);                                       // jmp original
    EmitDword(original - ((unsigned int)g_write + 4));
    return cave;
}

// The draw-distance floor (0 leaves the engine's distance alone) and the field of view.
bool InstallCameraDetour(float distance) {
    if (!Verify(kCameraDistanceSite, kCameraDistanceCode, sizeof(kCameraDistanceCode))) {
        LogLine("    engine: 0x%08X is not the camera draw-distance code -- not patching",
                kCameraDistanceSite);
        return false;
    }
    unsigned char* cave = (unsigned char*)VirtualAlloc(0, 128, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE);
    if (!cave) {
        return false;
    }
    g_drawDistanceFloor = distance;

    g_write = cave;
    Emit(0x8D); Emit(0x64); Emit(0x24); Emit(0xF0);            // lea    esp, [esp-16]
    Emit(0x0F); Emit(0x11); Emit(0x04); Emit(0x24);            // movups [esp], xmm0
    Emit(0xF3); Emit(0x0F); Emit(0x10); Emit(0x86); EmitDword(0x4E4);   // movss xmm0,[esi+4e4]
    Emit(0xF3); Emit(0x0F); Emit(0x5F); Emit(0x05);
    EmitDword((unsigned int)&g_drawDistanceFloor);                      // maxss xmm0,[floor]
    Emit(0xF3); Emit(0x0F); Emit(0x11); Emit(0x86); EmitDword(0x4E4);   // movss [esi+4e4],xmm0
    Emit(0xF3); Emit(0x0F); Emit(0x11); Emit(0x05);
    EmitDword((unsigned int)&g_drawDistanceApplied);                    // movss [applied],xmm0
    Emit(0x0F); Emit(0x10); Emit(0x04); Emit(0x24);            // movups xmm0, [esp]
    Emit(0x8D); Emit(0x64); Emit(0x24); Emit(0x10);            // lea    esp, [esp+16]
    Emit(0xD9); Emit(0x44); Emit(0x24); Emit(0x10);            // fld  dword ptr [esp+0x10]
    Emit(0xD9); Emit(0x15); EmitDword((unsigned int)&g_fovEngine);     // fst  [engine fov]
    Emit(0xD8); Emit(0x0D); EmitDword((unsigned int)&g_fovScale);      // fmul [fov scale]
    for (int i = 4; i < (int)sizeof(kCameraDistanceCode); ++i) {
        Emit(kCameraDistanceCode[i]);                          // fmul qword ptr [0x4c9440]
    }
    Emit(0xE9);
    EmitDword(kCameraDistanceResume - ((unsigned int)g_write + 4));

    unsigned char patch[sizeof(kCameraDistanceCode)];
    const int rel = (int)((unsigned int)cave - (kCameraDistanceSite + 5));
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);
    memset(patch + 5, 0x90, sizeof(patch) - 5);
    if (!Overwrite(kCameraDistanceSite, patch, sizeof(patch))) {
        return false;
    }
    g_cameraDetourInstalled = true;
    g_drawDistancePatched = distance > 0.0f;
    if (g_drawDistancePatched) {
        LogLine("    engine: camera draw distance raised to at least %.0f units "
                "(engine default 1024)", distance);
    }
    g_cutsceneTestKnown = Verify(kCutsceneTest, kCutsceneTestCode, sizeof(kCutsceneTestCode));
    LogLine("    engine: camera field of view follows [Display] FieldOfView%s",
            g_cutsceneTestKnown ? ", except in cutscenes" : "");
    return true;
}

bool InstallCameraAspect() {
    if (!Verify(kCameraAspectSite, kCameraAspectCode, sizeof(kCameraAspectCode))) {
        LogLine("    engine: 0x%08X is not the camera aspect code -- the picture stays 4:3",
                kCameraAspectSite);
        return false;
    }
    unsigned char* cave = (unsigned char*)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE);
    if (!cave) {
        return false;
    }
    g_write = cave;
    Emit(0xD8); Emit(0x0D);
    EmitDword((unsigned int)&g_cameraAspectScale);             // fmul dword ptr [scale]
    for (int i = 0; i < (int)sizeof(kCameraAspectCode); ++i) {
        Emit(kCameraAspectCode[i]);                            // the relocated originals
    }
    Emit(0xE9);
    EmitDword(kCameraAspectResume - ((unsigned int)g_write + 4));

    unsigned char patch[sizeof(kCameraAspectCode)];
    const int rel = (int)((unsigned int)cave - (kCameraAspectSite + 5));
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);
    memset(patch + 5, 0x90, sizeof(patch) - 5);
    if (!Overwrite(kCameraAspectSite, patch, sizeof(patch))) {
        return false;
    }
    g_cameraAspectPatched = true;
    LogLine("    engine: camera aspect follows the display");
    return true;
}

int RedirectCallSites(const unsigned int* sites, int count, unsigned int target,
                      unsigned char* detour) {
    int patched = 0;
    for (int i = 0; i < count; ++i) {
        const unsigned int site = sites[i];
        if (!IsCallTo(site, target)) {
            LogLine("    engine: 0x%08X is not a call to 0x%08X -- not patching", site, target);
            continue;
        }
        const int rel = (int)((unsigned int)detour - (site + 5));
        if (Overwrite(site + 1, &rel, 4)) {
            ++patched;
        }
    }
    return patched;
}

// Needs the frustum test bypassed at the drawing sites: it widens the box test in front of
// it, and the frustum would still turn away everything behind the eye.
void InstallKeepAroundCamera() {
    const float radius = Config::KeepAroundCamera();
    if (radius <= 0.0f) {
        return;
    }
    if (!IsCallTo(kBuildCullBoundsCallSite, kBuildCullBounds)) {
        LogLine("    engine: 0x%08X is not the call to SP_BuildCullBounds -- the level is not "
                "kept around the camera", kBuildCullBoundsCallSite);
        return;
    }
    g_keepAroundCamera = radius;
    const int total = (int)(sizeof(kViewBoxTests) / sizeof(kViewBoxTests[0]));
    for (int i = 0; i < total; ++i) {
        if (PatchViewBoxTest(kViewBoxTests[i])) {
            ++g_viewBoxTestsPatched;
        }
    }
    RedirectCallSites(&kBuildCullBoundsCallSite, 1, kBuildCullBounds,
                      (unsigned char*)&BuildCullBoundsAroundCamera);
    LogLine("    engine: level kept within %.0f units of the camera at %d/%d drawing tests",
            radius, g_viewBoxTestsPatched, total);
}

} // namespace

void Engine::OnFrame() {
    bool cutscene = false;
    if (g_cutsceneTestKnown) {
        const unsigned int state = *(const unsigned int*)kCutsceneState;
        cutscene = state && !IsBadReadPtr((const void*)(state + 0x28), 4) &&
                   *(const int*)(state + 0x28) != 0;
    }
    g_fovScale = cutscene ? 1.0f : Config::FieldOfView();
}

void Engine::Install() {
    // The draw arrays are raised regardless of LoadEntireMap. They are an engine limitation,
    // not a consequence of showing more of the map: the stock sizes saturate on their own in
    // the denser parts of town, and an entry the engine cannot record is geometry that is
    // simply not drawn that frame -- which is what makes particular house walls flicker in
    // and out, and what drops the first-person weapon once the map is opened up.
    const unsigned int factor = Config::BatchLimitScale();
    if (factor > 1) {
        for (int i = 0; i < (int)(sizeof(g_arrays) / sizeof(g_arrays[0])); ++i) {
            RaiseArray(g_arrays[i], factor);
        }
    }

    // The draw distance is the engine's own "Fog Distance" field, so it is raised on its
    // own switch rather than behind LoadEntireMap: 0 leaves the engine's 1024 units alone.
    const float distance = Config::DrawDistance();
    InstallCameraDetour(distance > 1024.0f ? distance : 0.0f);

    // The lookdev tee draws the same camera through the original renderer, whose picture is
    // 4:3 whatever ours is, so the camera keeps the engine's shape there.
    if (Config::AspectRatio() != Config::kOriginalAspect && !Lookdev::IsTee()) {
        InstallCameraAspect();
    }

    // Raised before the peak counters are installed, so the reported cap is the live one.
    const unsigned int listScale = Config::CameraDrawListScale();
    if (listScale > 1 && RaiseDrawnList(listScale)) {
        for (int i = 0; i < (int)(sizeof(g_cameraLists) / sizeof(g_cameraLists[0])); ++i) {
            if (g_cameraLists[i].countOffset == 0xE88) {
                g_cameraLists[i].cap = (int)g_drawnListCap;
            }
        }
    }

    // Measuring the peaks needs the detour whether or not anything else is patched.
    unsigned char* listDetour = BuildArgumentTapDetour(kVisibleListConsumer,
                                                      (void*)&OnCameraListsBuilt);
    if (listDetour) {
        RedirectCallSites(&kVisibleListConsumerCallSite, 1, kVisibleListConsumer, listDetour);
    }

    unsigned char* emitDetour = BuildPreCallDetour(kEmitDrawLists, (void*)&OnDrawListsReady);
    if (emitDetour) {
        RedirectCallSites(&kEmitDrawListsCallSite, 1, kEmitDrawLists, emitDetour);
    }

    if (!Config::LoadEntireMap()) {
        LogLine("    engine: LoadEntireMap=0, culling left as the engine wrote it");
        return;
    }

    if (Config::DisableFrustumTest()) {
        unsigned char* stub = (unsigned char*)VirtualAlloc(0, sizeof(kAlwaysVisible),
                                                           MEM_COMMIT | MEM_RESERVE,
                                                           PAGE_EXECUTE_READWRITE);
        if (stub) {
            memcpy(stub, kAlwaysVisible, sizeof(kAlwaysVisible));
            const int total = (int)(sizeof(kFrustumDrawCallSites) /
                                    sizeof(kFrustumDrawCallSites[0]));
            g_frustumSitesPatched = RedirectCallSites(kFrustumDrawCallSites, total,
                                                      kFrustumPlaneTest, stub);
            LogLine("    engine: frustum test bypassed at %d/%d drawing call sites",
                    g_frustumSitesPatched, total);
        }
        if (Config::UncullRooms()) {
            const unsigned char nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
            for (int i = 0; i < (int)(sizeof(kRoomTests) / sizeof(kRoomTests[0])); ++i) {
                const Branch& b = kRoomTests[i];
                if (b.entityPath && !Config::UncullEntities()) {
                    continue;
                }
                if (!Verify(b.address, b.expected, b.length)) {
                    LogLine("    engine: room test at 0x%08X (%s) is not the expected branch "
                            "-- left alone", b.address, b.name);
                    continue;
                }
                if (Overwrite(b.address, nops, b.length)) {
                    ++g_roomTestsWidened;
                }
            }
            LogLine("    engine: room visibility widened at %d/%d sites", g_roomTestsWidened,
                    (int)(sizeof(kRoomTests) / sizeof(kRoomTests[0])));
        }

        InstallKeepAroundCamera();
        if (Config::UncullEntities() || g_keepAroundCamera > 0.0f) {
            g_entitySitePatched = RedirectCallSites(&kFrustumEntityCallSite, 1,
                                                    kFrustumPlaneTest,
                                                    (unsigned char*)&EntityFrustumTest) > 0;
        }
        if (Config::UncullEntities()) {
            LogLine("    engine: entity visibility %s, budget %d objects per frame",
                    g_entitySitePatched ? "widened" : "UNCHANGED", Config::UncullBudget());
        }
    }

}

bool Engine::CameraFollowsDisplayAspect() {
    return g_cameraAspectPatched;
}

void Engine::SetDisplayAspect(float aspect) {
    if (g_cameraAspectPatched && aspect > 0.0f) {
        g_cameraAspectScale = aspect / Config::kOriginalAspect;
    }
}

void Engine::LogState() {
    LogLine("    engine: frustum bypassed at %d drawing sites; entity site %s, peak %d "
            "overrides in a frame, %d denied by the budget",
            g_frustumSitesPatched, g_entitySitePatched ? "widened" : "untouched",
            g_extraVisiblePeak, g_extraVisibleDenied);
    LogLine("    engine: room visibility widened at %d sites", g_roomTestsWidened);
    if (g_keepAroundCamera > 0.0f) {
        LogLine("    engine: level kept within %.0f units of the camera at %d drawing tests; "
                "peak %d entities in a frame drawn only for being near",
                g_keepAroundCamera, g_viewBoxTestsPatched, g_keptEntitiesPeak);
    }
    SampleResourceCache();
    if (g_cacheSlots) {
        LogLine("    engine: entity resource cache peak %u / %u slots%s",
                g_cachePeak, g_cacheSlots,
                (g_cachePeak * 10 >= g_cacheSlots * 9)
                    ? "   <-- saturated; late-arriving objects could not register"
                    : "");
    }
    if (g_cameraDetourInstalled) {
        LogLine("    engine: field of view %.1f degrees from the engine, x%.2f in the last frame",
                g_fovEngine, g_fovScale);
    }
    if (g_drawDistancePatched) {
        LogLine("    engine: camera draw distance floor %.0f -> %.0f units in the last "
                "frame (engine default 1024)", g_drawDistanceFloor, g_drawDistanceApplied);
    } else {
        LogLine("    engine: camera draw distance left at the engine's own value");
    }
    LogLine("    engine camera visible-object lists (the cap that ends the culling):");
    for (int i = 0; i < (int)(sizeof(g_cameraLists) / sizeof(g_cameraLists[0])); ++i) {
        const CameraList& l = g_cameraLists[i];
        LogLine("      %-12s peak %4d / %4d   full on %u frames, over three quarters on %u%s",
                l.name, l.peak, l.cap, l.framesFull, l.framesNearFull,
                l.framesFull ? "   <-- objects dropped here"
                             : (l.peak * 4 >= l.cap * 3 ? "   <-- close; lower DrawDistance"
                                                        : ""));
    }
    LogLine("    engine draw arrays:");
    for (int i = 0; i < (int)(sizeof(g_arrays) / sizeof(g_arrays[0])); ++i) {
        const DrawArray& a = g_arrays[i];
        const unsigned int limit = a.raisedLimit ? a.raisedLimit : a.limit;
        LogLine("      %-16s peak %5d / %5u   full on %u frames%s", a.name, a.peak, limit,
                a.saturatedFrames,
                a.saturatedFrames ? "   SATURATED -- geometry was dropped" : "");
    }
}

bool Engine::RedirectApiCall(unsigned int site, const void* replacement) {
    // call dword ptr [0x0156D218], six bytes, becomes a direct call plus a NOP.
    static const unsigned char kDispatcherCall[6] = {0xFF, 0x15, 0x18, 0xD2, 0x56, 0x01};
    unsigned char patch[6];
    patch[0] = 0xE8;
    const int rel = (int)((unsigned int)replacement - (site + 5));
    memcpy(patch + 1, &rel, 4);
    patch[5] = 0x90;
    if (Verify(site, patch, sizeof(patch))) {
        return true;
    }
    if (!Verify(site, kDispatcherCall, sizeof(kDispatcherCall))) {
        LogLine("    engine: 0x%08X is not a dispatcher call -- not patching", site);
        return false;
    }
    return Overwrite(site, patch, sizeof(patch));
}
