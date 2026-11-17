#include "lights.h"
#include "game_build.h"
#include "log.h"
#include "overlay.h"
#include "remix.h"
#include "settings.h"
#include "zones.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace {

// ------------------------------------------------------------------------------ game data
//
// SouthPark.exe, unpacked in place at its preferred base. Layouts:
//   pool  +4 active head, nodes from +0xc (10 x 0x18)
//   node  +4 next, +8 definition*, +0xc float intensity, +0x10/12/14 s16 position, +0x16 s16 enabled
//   table +0 stride, +4 count, +8 records: s16 reach, s16 falloff, u8 colour[4]
const unsigned int kWorld = 0x00522a68;
const unsigned int kWorldLightTable = 0x84;
const unsigned int kPool = 0x00524208;
const unsigned int kPoolNodes = kPool + 0xc;
const int kNodeCount = 10;
const int kNodeSize = 0x18;

// The light upload FUN_00426970 and the table lookup FUN_0043f470 as built. Any other executable
// would put something else at these addresses, so nothing is read unless both match.
const unsigned char kUploadPrologue[] = {0x83, 0xec, 0x20, 0x56, 0x57, 0x68, 0x08, 0x42, 0x52, 0x00};
const unsigned char kLookupPrologue[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x80, 0x84, 0x00, 0x00, 0x00};

// `address` and the addresses inside `bytes` are the US build's (game_build.h); they are
// compared as the running build's.
bool Matches(unsigned int address, const unsigned char* bytes, size_t size) {
    unsigned char placed[32];
    if (size > sizeof(placed)) {
        return false;
    }
    memcpy(placed, bytes, size);
    GameBuild::PlaceEmbedded(placed, size);
    address = GameBuild::Place(address);
    return !IsBadReadPtr((const void*)address, size) && memcmp((const void*)address, placed, size) == 0;
}

template <typename T> T Read(unsigned int address) {
    return *(const T*)address;
}

// The pool's first node in the running build; nodes link to each other by their real addresses.
unsigned int PoolNodes() {
    return GameBuild::Place(kPool) + (kPoolNodes - kPool);
}

bool IsNode(unsigned int node) {
    const unsigned int first = PoolNodes();
    return node >= first && node < first + kNodeCount * kNodeSize && (node - first) % kNodeSize == 0;
}

struct Definition {
    int reach = 0;
    int falloff = 0;
    unsigned char rgba[4] = {};
};

struct LiveLight {
    int slot = 0;            // pool node, 0..9; stable for the light's life
    int definition = -1;
    float position[3] = {};
    float intensity = 0.0f;
    Definition def;
};

bool g_gameSupported = false;
std::vector<Definition> g_definitions;
std::vector<LiveLight> g_live;
std::vector<Zones::Zone> g_zones;
std::vector<remixapi_LightHandle> g_handles;
int g_peakLive = 0;
unsigned int g_frames = 0;
unsigned int g_framesWithLights = 0;

// ------------------------------------------------------------------------------ settings

struct LightSettings {
    bool enabled = true;
    float radianceScale = 1500.0f;
    float emitterRadius = 6.0f;
    bool scaleByReach = true;
    bool swapRedBlue = false;
    bool markers = false;
    bool keepFallbackLight = true;
    bool zoneLights = true;
    float zoneRadianceScale = 1500.0f;
    // A zone's brightness grows with the square of its reach, so a large zone lights its far
    // side the way the game's does. The level-wide fills, up to 12,000 units across, would be
    // suns that way, so the reach counted for brightness stops here.
    float zoneReachCap = 2048.0f;
    // Outdoors the sun and the sky light the scene; the zones over open ground were fills for
    // the original's vertex lighting, and as lights they add a second, coloured sun.
    bool zonesInTheOpen = false;
    // A soft light above the camera, coloured by the light the engine gives an object standing
    // there. It keeps rooms that have only the level's large fills -- no lamp-sized zone to send --
    // from going black, at the cost of a light that travels with the player.
    bool playerLight = false;
    // Only for dark interiors: while the player is playing, under level geometry and out of
    // reach of every light sent -- caves, and rooms with nothing else to light them.
    bool playerLightOnlyInTheDark = true;
    float playerLightScale = 1500.0f;
    float playerLightReach = 512.0f;
    float playerLightHeight = 40.0f;
} g_settings;

// The test light: one light of the same kind, placed by hand, to see the Remix side work without
// waiting for the game to spawn one.
struct TestLight {
    bool enabled = false;
    float position[3] = {};
    float colour[3] = {1.0f, 0.8f, 0.4f};
    float intensity = 1.0f;
    float reach = 1023.0f;
    float distance = 200.0f;
} g_test;

// The [Lights] keys of Video_D3D9.ini, each tied to one setting: a number or a switch.
struct Key {
    const char* name;
    float* number;
    bool* flag;
};

const Key kKeys[] = {
    {"Enabled", nullptr, &g_settings.enabled},
    {"RadianceScale", &g_settings.radianceScale, nullptr},
    {"EmitterRadius", &g_settings.emitterRadius, nullptr},
    {"ScaleByReach", nullptr, &g_settings.scaleByReach},
    {"SwapRedBlue", nullptr, &g_settings.swapRedBlue},
    {"Markers", nullptr, &g_settings.markers},
    {"KeepFallbackLight", nullptr, &g_settings.keepFallbackLight},
    {"ZoneLights", nullptr, &g_settings.zoneLights},
    {"ZoneRadianceScale", &g_settings.zoneRadianceScale, nullptr},
    {"ZoneReachCap", &g_settings.zoneReachCap, nullptr},
    {"ZonesInTheOpen", nullptr, &g_settings.zonesInTheOpen},
    {"PlayerLight", nullptr, &g_settings.playerLight},
    {"PlayerLightOnlyInTheDark", nullptr, &g_settings.playerLightOnlyInTheDark},
    {"PlayerLightScale", &g_settings.playerLightScale, nullptr},
    {"PlayerLightReach", &g_settings.playerLightReach, nullptr},
    {"PlayerLightHeight", &g_settings.playerLightHeight, nullptr},
};

void ReadKey(const char* ini, const Key& k) {
    char text[32];
    GetPrivateProfileStringA("Lights", k.name, "", text, sizeof(text), ini);
    if (!text[0]) {
        return;
    }
    if (k.number) {
        *k.number = (float)atof(text);
    } else {
        *k.flag = atoi(text) != 0;
    }
}

// ------------------------------------------------------------------------------ camera
//
// The renderer's view is a rigid transform under the row-vector convention,
// p_view = p_world * R + t, looking down view-space -z.

float g_view[16];
float g_projection[16];
bool g_haveView = false;
bool g_haveProjection = false;

bool Camera(float* position, float* forward) {
    if (!g_haveView) {
        return false;
    }
    const float* v = g_view;
    position[0] = -(v[12] * v[0] + v[13] * v[1] + v[14] * v[2]);
    position[1] = -(v[12] * v[4] + v[13] * v[5] + v[14] * v[6]);
    position[2] = -(v[12] * v[8] + v[13] * v[9] + v[14] * v[10]);
    forward[0] = -v[2];
    forward[1] = -v[6];
    forward[2] = -v[10];
    return true;
}

bool ToScreen(const float* p, int width, int height, float* sx, float* sy) {
    if (!g_haveView || !g_haveProjection) {
        return false;
    }
    float eye[4], clip[4];
    for (int c = 0; c < 4; ++c) {
        eye[c] = p[0] * g_view[c] + p[1] * g_view[4 + c] + p[2] * g_view[8 + c] + g_view[12 + c];
    }
    for (int c = 0; c < 4; ++c) {
        clip[c] = eye[0] * g_projection[c] + eye[1] * g_projection[4 + c] +
                  eye[2] * g_projection[8 + c] + eye[3] * g_projection[12 + c];
    }
    if (clip[3] <= 1e-3f) {
        return false;
    }
    *sx = (clip[0] / clip[3] * 0.5f + 0.5f) * width;
    *sy = (0.5f - clip[1] / clip[3] * 0.5f) * height;
    return true;
}

// ------------------------------------------------------------------------------ pool

void ReadDefinitions(unsigned int table) {
    g_definitions.clear();
    if (!table || IsBadReadPtr((const void*)table, 8)) {
        return;
    }
    const unsigned int stride = Read<unsigned int>(table);
    const unsigned int count = Read<unsigned int>(table + 4);
    if (stride < 8 || stride > 64 || count > 256 || IsBadReadPtr((const void*)(table + 8), stride * count)) {
        return;
    }
    for (unsigned int i = 0; i < count; ++i) {
        const unsigned int record = table + 8 + i * stride;
        Definition d;
        d.reach = Read<short>(record);
        d.falloff = Read<short>(record + 2);
        memcpy(d.rgba, (const void*)(record + 4), 4);
        g_definitions.push_back(d);
    }
}

void ReadPool() {
    g_live.clear();
    const unsigned int table = Read<unsigned int>(GameBuild::Place(kWorld) + kWorldLightTable);
    ReadDefinitions(table);
    const unsigned int stride = table ? Read<unsigned int>(table) : 0;

    unsigned int node = Read<unsigned int>(GameBuild::Place(kPool) + 4);
    for (int steps = 0; node && steps < kNodeCount && IsNode(node); ++steps) {
        if (Read<short>(node + 0x16)) {
            LiveLight l;
            l.slot = (int)((node - PoolNodes()) / kNodeSize);
            l.intensity = Read<float>(node + 0xc);
            l.position[0] = Read<short>(node + 0x10);
            l.position[1] = Read<short>(node + 0x12);
            l.position[2] = Read<short>(node + 0x14);
            const unsigned int record = Read<unsigned int>(node + 0x8);
            if (stride && record >= table + 8 && (record - table - 8) % stride == 0) {
                l.definition = (int)((record - table - 8) / stride);
                if (l.definition < (int)g_definitions.size()) {
                    l.def = g_definitions[l.definition];
                }
            }
            g_live.push_back(l);
        }
        node = Read<unsigned int>(node + 0x4);
    }
    ++g_frames;
    if (!g_live.empty()) {
        ++g_framesWithLights;
    }
    if ((int)g_live.size() > g_peakLive) {
        g_peakLive = (int)g_live.size();
        Log("lights: new peak of %d live game lights", g_peakLive);
    }
}

// ------------------------------------------------------------------------------ Remix

// South Park gives Remix no lights of its own, so every scene is lit by Remix's fallback light
// -- which by default only shines while no other light exists, so the first game light would
// black out everything else. Kept on, game lights add to the scene.
void ApplyFallbackLight() {
    const auto& api = Remix::Api();
    if (api.SetConfigVariable) {
        api.SetConfigVariable("rtx.fallbackLightMode", g_settings.keepFallbackLight ? "2" : "1");
    }
}

unsigned long long Hash(const char* text) {
    unsigned long long h = 14695981039346656037ull;
    for (const char* p = text; *p; ++p) {
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    }
    return h;
}

// With a cone direction, the sphere is shaped to the 45-degree half angle of the game's cone
// zones.
void SubmitLight(unsigned long long hash, const float* position, const float* rgb, float scale,
                 const float* coneDirection = nullptr) {
    const auto& api = Remix::Api();
    remixapi_LightInfoSphereEXT sphere = {};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {position[0], position[1], position[2]};
    sphere.radius = g_settings.emitterRadius;
    sphere.shaping_hasvalue = coneDirection ? TRUE : FALSE;
    if (coneDirection) {
        sphere.shaping_value.direction = {coneDirection[0], coneDirection[1], coneDirection[2]};
        sphere.shaping_value.coneAngleDegrees = 45.0f;
        sphere.shaping_value.coneSoftness = 0.1f;
        sphere.shaping_value.focusExponent = 0.0f;
    }
    sphere.volumetricRadianceScale = 1.0f;

    remixapi_LightInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = hash;
    info.radiance = {rgb[0] * scale, rgb[1] * scale, rgb[2] * scale};

    remixapi_LightHandle handle = nullptr;
    if (api.CreateLight(&info, &handle) == REMIXAPI_ERROR_CODE_SUCCESS && handle) {
        api.DrawLightInstance(handle);
        g_handles.push_back(handle);
    }
}

// Brightness: colour x intensity x scale x (reach / 1024)^2 -- the game's long-reach lights are
// meant to carry further.
float Brightness(float intensity, float reach, float scale) {
    const float r = g_settings.scaleByReach ? reach / 1024.0f : 1.0f;
    return intensity * scale * r * r;
}

// Zones Remix can show: it has no light that takes light away, and a box zone is a directional
// light confined to a box, which no Remix light is.
bool ZoneIsLight(const Zones::Zone& z) {
    return !z.subtractive && z.shape != Zones::Box;
}

// ------------------------------------------------------------------ zones in the open
//
// A zone is in the open when no level geometry is above it. The renderer (Video_D3D.dll) keeps
// the level's recent triangles for its normals and answers that for a few points
// (SouthPark_CoverAbove: how many have geometry above them, or -1 when it holds nothing around
// any). Five points are asked -- the centre and four around it -- so a beam or a sign over one
// point does not make a street a room. A roof does not go away, so a zone once found covered
// stays covered; an open one is asked again now and then, in case its roof had not been drawn
// yet. Until a zone has an answer it is not sent, so open fills do not flash on as a level
// loads. Without the renderer's answer every zone is sent.
typedef int (__cdecl *CoverAboveFn)(const float* points, int count);
CoverAboveFn g_coverAbove = nullptr;
bool g_coverLooked = false;

enum Cover { CoverUnknown, CoverOpen, CoverRoofed };
struct ZoneCover {
    Cover state = CoverUnknown;
    unsigned int nextCheck = 0;
};
std::unordered_map<unsigned long long, ZoneCover> g_zoneCover;
const int kCoverChecksPerFrame = 4;      // each is one pass over the renderer's triangles
const unsigned int kRecheckFrames = 120;
const int kCoverPoints = 5;
const int kCoverNeeded = 3;              // points with geometry above that make a zone covered

CoverAboveFn CoverAbove() {
    if (!g_coverLooked) {
        g_coverLooked = true;
        if (HMODULE renderer = GetModuleHandleA("Video_D3D.dll")) {
            g_coverAbove = (CoverAboveFn)GetProcAddress(renderer, "SouthPark_CoverAbove");
        }
        Log("lights: %s", g_coverAbove ? "the renderer tells which zones are in the open"
                                       : "no cover answers from the renderer -- every zone is sent");
    }
    return g_coverAbove;
}

Cover ZoneCoverOf(unsigned long long hash, const Zones::Zone& z, int& checksLeft) {
    ZoneCover& c = g_zoneCover[hash];
    CoverAboveFn ask = CoverAbove();
    if (!ask || c.state == CoverRoofed || g_frames < c.nextCheck || checksLeft == 0) {
        return ask ? c.state : CoverRoofed;
    }
    --checksLeft;
    float spread = z.reach * 0.25f;
    spread = spread < 16.0f ? 16.0f : (spread > 128.0f ? 128.0f : spread);
    const float offsets[kCoverPoints][2] = {{0, 0}, {spread, 0}, {-spread, 0}, {0, spread}, {0, -spread}};
    float points[kCoverPoints][3];
    for (int i = 0; i < kCoverPoints; ++i) {
        points[i][0] = z.centre[0] + offsets[i][0];
        points[i][1] = z.centre[1];
        points[i][2] = z.centre[2] + offsets[i][1];
    }
    const int covered = ask(&points[0][0], kCoverPoints);
    if (covered >= 0) {
        c.state = covered >= kCoverNeeded ? CoverRoofed : CoverOpen;
    }
    c.nextCheck = g_frames + kRecheckFrames;
    return c.state;
}

unsigned long long ZoneHash(const Zones::Zone& z) {
    // Named by where it is: which section a zone came from changes as the camera moves.
    char name[64];
    _snprintf(name, sizeof(name), "sp_zone_%d_%d_%d_%d", (int)z.centre[0], (int)z.centre[1],
              (int)z.centre[2], (int)z.radius);
    return Hash(name);
}

// Whether a zone light is sent this frame, asking about its cover while checks are left.
bool ZoneSent(const Zones::Zone& z, int& checksLeft) {
    if (!ZoneIsLight(z)) {
        return false;
    }
    const Cover cover = ZoneCoverOf(ZoneHash(z), z, checksLeft);
    return g_settings.zonesInTheOpen ? true : cover == CoverRoofed;
}

float ZoneBrightness(const Zones::Zone& z) {
    const float reach = z.reach < g_settings.zoneReachCap ? z.reach : g_settings.zoneReachCap;
    return Brightness(1.0f, reach, g_settings.zoneRadianceScale);
}

void ZoneRgb(const Zones::Zone& z, float* out) {
    for (int c = 0; c < 3; ++c) {
        out[c] = z.colour[c] > 0.0f ? z.colour[c] / 255.0f : 0.0f;
    }
}

D3DCOLOR ZoneColour(const Zones::Zone& z) {
    float rgb[3];
    ZoneRgb(z, rgb);
    for (float& c : rgb) {
        c = c > 1.0f ? 1.0f : c;
    }
    return D3DCOLOR_ARGB(255, (int)(rgb[0] * 255), (int)(rgb[1] * 255), (int)(rgb[2] * 255));
}

const char* ShapeName(const Zones::Zone& z) {
    switch (z.shape) {
    case Zones::Sphere: return "sphere";
    case Zones::Box: return "box";
    case Zones::Cone: return "cone";
    default: return "?";
    }
}

// ------------------------------------------------------------------------ player light
//
// With PlayerLightOnlyInTheDark the light shines only when three things hold: the player is
// playing (in a level, not paused, no cutscene camera), the renderer finds level geometry above
// the camera, and the camera is outside the reach of every light this proxy sends. Each test
// switches on and off at different thresholds, and the light fades in and out, so a doorway or
// the edge of a lamp's reach does not make it flicker.

// The engine's "in a level" (FUN_00443650: [0x53e3f2] == 0x0d) and "paused" (FUN_00443660:
// [0x53e40c] != 0) getters, and its cutscene test (FUN_0049bf00: [[0x4e1250] + 0x28]) as built.
// The fields are read only when all three match.
const unsigned char kInLevelCode[] = {0x8b, 0x0d, 0xf2, 0xe3, 0x53, 0x00, 0x33, 0xc0, 0x83, 0xf9, 0x0d};
const unsigned char kPausedCode[] = {0x8b, 0x0d, 0x0c, 0xe4, 0x53, 0x00, 0x33, 0xc0, 0x85, 0xc9};
const unsigned char kCutsceneCode[] = {0xa1, 0x50, 0x12, 0x4e, 0x00, 0x8b, 0x40, 0x28, 0xc3};
const unsigned int kGameMode = 0x0053e3f2;
const unsigned int kGameModeLevel = 0x0d;
const unsigned int kPaused = 0x0053e40c;
const unsigned int kCutsceneState = 0x004e1250;
bool g_gameStateKnown = false;

// Around the camera, like a zone's five points: a roof over 4 of them makes it enclosed, and it
// stays enclosed until no more than 2 are covered.
const float kPlayerCoverSpread = 64.0f;
const int kRoofedAt = 4;
const int kOpenAt = 2;
const DWORD kPlayerCoverIntervalMs = 250;
// Near a light: inside its reach; no longer near: past this much of it.
const float kLeaveReach = 1.25f;
const float kPlayerFadeSeconds = 1.0f;

struct PlayerLight {
    bool playing = true;
    bool roofed = false;
    int covered = -1;               // points with a roof at the last answer; -1 before one
    DWORD nextCoverCheck = 0;
    bool nearLight = false;
    float nearest = -1.0f;          // nearest sent light's distance over its reach; -1 for none
    float fade = 0.0f;              // 0 dark .. 1 full brightness
    float target = 0.0f;            // what it fades toward in play
    DWORD lastTick = 0;
} g_player;

// Why the player is not playing, or nullptr while they are.
const char* NotPlaying() {
    if (!g_gameStateKnown) {
        return nullptr;
    }
    if (Read<unsigned int>(GameBuild::Place(kGameMode)) != kGameModeLevel) {
        return "not in a level";
    }
    if (Read<unsigned int>(GameBuild::Place(kPaused)) != 0) {
        return "paused";
    }
    const unsigned int cutscene = Read<unsigned int>(GameBuild::Place(kCutsceneState));
    if (cutscene && !IsBadReadPtr((const void*)(cutscene + 0x28), 4) && Read<int>(cutscene + 0x28) != 0) {
        return "cutscene";
    }
    return nullptr;
}

void UpdateRoof(const float* camera) {
    const DWORD now = GetTickCount();
    if ((int)(now - g_player.nextCoverCheck) < 0) {
        return;
    }
    g_player.nextCoverCheck = now + kPlayerCoverIntervalMs;
    CoverAboveFn ask = CoverAbove();
    if (!ask) {
        g_player.roofed = true;      // as with zones: without an answer, assume a roof
        return;
    }
    const float s = kPlayerCoverSpread;
    const float offsets[kCoverPoints][2] = {{0, 0}, {s, 0}, {-s, 0}, {0, s}, {0, -s}};
    float points[kCoverPoints][3];
    for (int i = 0; i < kCoverPoints; ++i) {
        points[i][0] = camera[0] + offsets[i][0];
        points[i][1] = camera[1];
        points[i][2] = camera[2] + offsets[i][1];
    }
    const int covered = ask(&points[0][0], kCoverPoints);
    if (covered < 0) {
        return;                      // the renderer holds nothing around here yet
    }
    g_player.covered = covered;
    if (covered >= kRoofedAt) {
        g_player.roofed = true;
    } else if (covered <= kOpenAt) {
        g_player.roofed = false;
    }
}

// Tracks the nearest light sent this frame, as distance over reach.
struct NearestLight {
    const float* camera = nullptr;
    float nearest = -1.0f;
    void Consider(const float* position, float reach) {
        if (!camera || reach <= 0.0f) {
            return;
        }
        const float dx = position[0] - camera[0], dy = position[1] - camera[1], dz = position[2] - camera[2];
        const float relative = sqrtf(dx * dx + dy * dy + dz * dz) / reach;
        if (nearest < 0.0f || relative < nearest) {
            nearest = relative;
        }
    }
};

// How bright the player light is this frame, 0..1.
float PlayerLightFade(const float* camera, float nearest) {
    const DWORD now = GetTickCount();
    float step = (now - g_player.lastTick) / 1000.0f / kPlayerFadeSeconds;
    g_player.lastTick = now;
    if (!g_settings.playerLightOnlyInTheDark) {
        g_player.fade = 1.0f;
        return g_player.fade;
    }
    const char* notPlaying = NotPlaying();
    const bool playing = !notPlaying;
    if (playing != g_player.playing) {
        g_player.playing = playing;
        g_player.target = -1.0f;     // logged again once back in play
        Log("lights: player light: %s", playing ? "in play" : notPlaying);
    }
    if (!playing || !camera) {
        g_player.fade = 0.0f;        // a cutscene or a menu cuts it at once
        return g_player.fade;
    }
    UpdateRoof(camera);
    g_player.nearest = nearest;
    g_player.nearLight = nearest >= 0.0f && nearest < (g_player.nearLight ? kLeaveReach : 1.0f);
    const float target = g_player.roofed && !g_player.nearLight ? 1.0f : 0.0f;
    if (target != g_player.target) {
        g_player.target = target;
        Log("lights: player light: %s -- roof over %d of %d points, nearest light %.2f x its reach",
            target > 0.0f ? "on" : "off", g_player.covered, kCoverPoints, g_player.nearest);
    }
    step = step > 1.0f ? 1.0f : step;
    g_player.fade += g_player.fade < target ? step : -step;
    g_player.fade = g_player.fade < 0.0f ? 0.0f : (g_player.fade > 1.0f ? 1.0f : g_player.fade);
    if (fabsf(g_player.fade - target) < step) {
        g_player.fade = target;
    }
    return g_player.fade;
}

void Rgb(const unsigned char* c, float* out) {
    out[0] = (g_settings.swapRedBlue ? c[2] : c[0]) / 255.0f;
    out[1] = c[1] / 255.0f;
    out[2] = (g_settings.swapRedBlue ? c[0] : c[2]) / 255.0f;
}

D3DCOLOR Colour(const unsigned char* c) {
    float rgb[3];
    Rgb(c, rgb);
    return D3DCOLOR_ARGB(255, (int)(rgb[0] * 255), (int)(rgb[1] * 255), (int)(rgb[2] * 255));
}

} // namespace

// =============================================================================== Lights

void Lights::Init(const char* ini) {
    Log("lights: SouthPark.exe build %s", GameBuild::Name());
    for (const Key& k : kKeys) {
        ReadKey(ini, k);
    }

    g_gameSupported = Matches(0x00426970, kUploadPrologue, sizeof(kUploadPrologue)) &&
                      Matches(0x0043f470, kLookupPrologue, sizeof(kLookupPrologue));
    Log("lights: %s; radiance scale %.0f, emitter radius %.1f, fallback light %s",
        g_gameSupported ? "South Park light pool found" : "not the expected SouthPark.exe -- game lights off",
        g_settings.radianceScale, g_settings.emitterRadius, g_settings.keepFallbackLight ? "kept on" : "Remix default");
    Log("lights: %s", Zones::Install() ? "level light zones hooked" : "not the expected SouthPark.exe -- zone lights off");
    g_gameStateKnown = Matches(0x00443650, kInLevelCode, sizeof(kInLevelCode)) &&
                       Matches(0x00443660, kPausedCode, sizeof(kPausedCode)) &&
                       Matches(0x0049bf00, kCutsceneCode, sizeof(kCutsceneCode));
    Log("lights: player light %s%s", g_settings.playerLight ? "on" : "off",
        !g_settings.playerLightOnlyInTheDark ? ", always"
        : g_gameStateKnown                   ? ", only in the dark"
                                             : ", only in the dark (gameplay state not found: assumed playing)");
}

void Lights::OnSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) {
    if (!m) {
        return;
    }
    // The 2D backdrop sets an identity view and an orthographic projection; neither is the
    // camera. The perspective projection is the one with a non-zero _34.
    static const D3DMATRIX kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (state == D3DTS_VIEW && memcmp(m, &kIdentity, sizeof(*m)) != 0) {
        memcpy(g_view, m, sizeof(g_view));
        g_haveView = true;
        float camera[3], forward[3];
        Camera(camera, forward);
        Zones::SetProbe(camera);
    } else if (state == D3DTS_PROJECTION && m->_34 != 0.0f) {
        memcpy(g_projection, m, sizeof(g_projection));
        g_haveProjection = true;
    }
}

void Lights::OnBeginScene() {
    if (g_gameSupported) {
        ReadPool();
    }
    g_zones = Zones::Latest();
}

void Lights::OnConnected() {
    ApplyFallbackLight();
}

int Lights::Save() {
    int written = 0;
    for (const Key& k : kKeys) {
        char text[32];
        if (k.number) {
            _snprintf(text, sizeof(text), "%g", *k.number);
        } else {
            _snprintf(text, sizeof(text), "%d", *k.flag ? 1 : 0);
        }
        text[sizeof(text) - 1] = 0;
        written += Settings::WriteKey("Lights", k.name, text) ? 1 : 0;
    }
    return written;
}

void Lights::ResetToDefaults() {
    const bool fallbackChanged = g_settings.keepFallbackLight != LightSettings().keepFallbackLight;
    g_settings = LightSettings();
    if (fallbackChanged && Remix::Connected()) {
        ApplyFallbackLight();
    }
}

void Lights::SubmitFrame() {
    const auto& api = Remix::Api();
    for (auto handle : g_handles) {
        api.DestroyLight(handle);
    }
    g_handles.clear();

    float camera[3], forward[3];
    const bool haveCamera = Camera(camera, forward);
    NearestLight nearest;
    nearest.camera = haveCamera ? camera : nullptr;

    if (g_settings.enabled) {
        for (const auto& l : g_live) {
            char name[32];
            _snprintf(name, sizeof(name), "sp_light_%d_%d", l.slot, l.definition);
            float rgb[3];
            Rgb(l.def.rgba, rgb);
            SubmitLight(Hash(name), l.position, rgb,
                        Brightness(l.intensity, (float)l.def.reach, g_settings.radianceScale));
            nearest.Consider(l.position, (float)l.def.reach);
        }
    }
    if (g_settings.zoneLights) {
        int checksLeft = kCoverChecksPerFrame;
        for (const auto& z : g_zones) {
            if (!ZoneSent(z, checksLeft)) {
                continue;
            }
            float rgb[3];
            ZoneRgb(z, rgb);
            SubmitLight(ZoneHash(z), z.centre, rgb, ZoneBrightness(z),
                        z.shape == Zones::Cone ? z.axis : nullptr);
            nearest.Consider(z.centre, z.reach < g_settings.zoneReachCap ? z.reach : g_settings.zoneReachCap);
        }
    }
    if (g_test.enabled) {
        SubmitLight(Hash("sp_test_light"), g_test.position, g_test.colour,
                    Brightness(g_test.intensity, g_test.reach, g_settings.radianceScale));
        nearest.Consider(g_test.position, g_test.reach);
    }
    if (g_settings.playerLight) {
        const float fade = PlayerLightFade(nearest.camera, nearest.nearest);
        const Zones::PointLight engine = Zones::LightAtProbe();
        if (fade > 0.0f && engine.valid && haveCamera) {
            float position[3] = {camera[0], camera[1] + g_settings.playerLightHeight, camera[2]};
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = (engine.ambient[c] + engine.directional[c]) / 255.0f;
            }
            SubmitLight(Hash("sp_player_light"), position, rgb,
                        fade * Brightness(1.0f, g_settings.playerLightReach, g_settings.playerLightScale));
        }
    }
}

void Lights::DrawMarkers(int width, int height) {
    if (!g_settings.markers) {
        return;
    }
    const float ring = Ui::LineHeight();
    const float thickness = 2.0f * Ui::Scale();
    auto mark = [&](const float* p, D3DCOLOR colour, const char* label) {
        float x, y;
        if (!ToScreen(p, width, height, &x, &y)) {
            return;
        }
        Ui::Ring(x, y, ring, thickness * 2.5f, D3DCOLOR_ARGB(200, 0, 0, 0));
        Ui::Ring(x, y, ring, thickness, colour);
        Ui::Text(x + ring * 1.4f, y - ring * 0.5f, D3DCOLOR_ARGB(235, 255, 255, 255), label);
    };
    for (const auto& l : g_live) {
        char label[32];
        _snprintf(label, sizeof(label), "light %d (def %d)", l.slot, l.definition);
        mark(l.position, Colour(l.def.rgba), label);
    }
    for (size_t i = 0; i < g_zones.size(); ++i) {
        const Zones::Zone& z = g_zones[i];
        if (!ZoneIsLight(z)) {
            continue;
        }
        char label[32];
        _snprintf(label, sizeof(label), "zone %d %s", (int)i, ShapeName(z));
        mark(z.centre, ZoneColour(z), label);
    }
    if (g_test.enabled) {
        const D3DCOLOR c = D3DCOLOR_ARGB(255, (int)(g_test.colour[0] * 255), (int)(g_test.colour[1] * 255),
                                         (int)(g_test.colour[2] * 255));
        mark(g_test.position, c, "test light");
    }
}

void Lights::DrawPanel() {
    float camera[3] = {}, forward[3] = {};
    const bool haveCamera = Camera(camera, forward);

    Ui::Heading("Status");
    Ui::Label("Game light pool:  %s", g_gameSupported ? "found" : "not this build -- off");
    Ui::Label("Remix API:        %s", Remix::Connected() ? "connected"
                                      : "unavailable (exposeRemixApi = True in .trex\\bridge.conf?)");
    Ui::Label("Live lights: %d   peak: %d   frames with lights: %u of %u", (int)g_live.size(), g_peakLive,
              g_framesWithLights, g_frames);
    if (haveCamera) {
        Ui::LabelDim("Camera at (%.0f, %.0f, %.0f)", camera[0], camera[1], camera[2]);
    }

    Ui::Heading("Remix lights");
    Ui::Checkbox("Send game lights to Remix", &g_settings.enabled);
    Ui::Slider("Radiance scale", &g_settings.radianceScale, 10.0f, 100000.0f, "%.0f", true);
    Ui::Slider("Emitter radius", &g_settings.emitterRadius, 0.5f, 64.0f, "%.1f units", true);
    Ui::Checkbox("Brighter for the game's long-reach lights", &g_settings.scaleByReach);
    Ui::Checkbox("Swap red and blue", &g_settings.swapRedBlue);
    Ui::Checkbox("Markers on screen (also with this panel closed)", &g_settings.markers);
    if (Ui::Checkbox("Keep Remix's fallback light on (the game has no other lights)", &g_settings.keepFallbackLight) &&
        Remix::Connected()) {
        ApplyFallbackLight();
    }

    Ui::Heading("Active game lights");
    if (g_live.empty()) {
        Ui::LabelDim("None right now. The game spawns them only with certain effects.");
    } else {
        Ui::LabelDim("      slot def  intensity   position                  distance");
        for (const auto& l : g_live) {
            Ui::Swatch(Colour(l.def.rgba));
            Ui::SameLine();
            float distance = 0.0f;
            if (haveCamera) {
                const float dx = l.position[0] - camera[0], dy = l.position[1] - camera[1], dz = l.position[2] - camera[2];
                distance = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            Ui::Label("%4d %4d %8.2f   %6.0f %6.0f %6.0f   %8.0f", l.slot, l.definition, l.intensity,
                      l.position[0], l.position[1], l.position[2], distance);
        }
    }

    Ui::Heading("Level light zones");
    Ui::Checkbox("Send sphere and cone zones to Remix", &g_settings.zoneLights);
    Ui::Slider("Zone radiance scale", &g_settings.zoneRadianceScale, 10.0f, 100000.0f, "%.0f", true);
    Ui::Slider("Zone reach cap", &g_settings.zoneReachCap, 256.0f, 16384.0f, "%.0f units", true);
    Ui::Checkbox("Also zones in the open (the sun and sky light those)", &g_settings.zonesInTheOpen);
    if (g_zones.empty()) {
        Ui::LabelDim("None in this level, or the game has zone lighting off.");
    } else {
        // A level registers up to a hundred zones; the panel lists the nearest, dimming those
        // not sent.
        const size_t kListed = 24;
        std::vector<std::pair<float, size_t>> lights;
        int notLights = 0, sent = 0, open = 0, unchecked = 0;
        for (size_t i = 0; i < g_zones.size(); ++i) {
            const Zones::Zone& z = g_zones[i];
            if (!ZoneIsLight(z)) {
                ++notLights;
                continue;
            }
            const Cover cover = CoverAbove() ? g_zoneCover[ZoneHash(z)].state : CoverRoofed;
            if (g_settings.zonesInTheOpen || cover == CoverRoofed) {
                ++sent;
            } else if (cover == CoverOpen) {
                ++open;
            } else {
                ++unchecked;
            }
            float distance = 0.0f;
            if (haveCamera) {
                const float dx = z.centre[0] - camera[0], dy = z.centre[1] - camera[1], dz = z.centre[2] - camera[2];
                distance = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            lights.push_back({distance, i});
        }
        std::sort(lights.begin(), lights.end());
        Ui::LabelDim("      zone shape   colour          ambient   reach   centre                  distance  above");
        for (size_t k = 0; k < lights.size() && k < kListed; ++k) {
            const Zones::Zone& z = g_zones[lights[k].second];
            const Cover cover = CoverAbove() ? g_zoneCover[ZoneHash(z)].state : CoverRoofed;
            const char* above = cover == CoverRoofed ? "roof" : cover == CoverOpen ? "open" : "?";
            const bool isSent = g_settings.zonesInTheOpen || cover == CoverRoofed;
            Ui::Swatch(ZoneColour(z));
            Ui::SameLine();
            (isSent ? Ui::Label : Ui::LabelDim)(
                "%4d %-6s %4.0f %4.0f %4.0f  %8.2f %7.0f   %6.0f %6.0f %6.0f   %8.0f  %-4s%s",
                (int)lights[k].second, ShapeName(z), z.colour[0], z.colour[1], z.colour[2],
                z.ambientShare, z.reach, z.centre[0], z.centre[1], z.centre[2], lights[k].first, above,
                z.flickers ? "  flicker" : "");
        }
        Ui::LabelDim("%d sent; %d in the open and %d not yet checked, not sent; %d boxes or subtractive.",
                     sent, open, unchecked, notLights);
    }

    Ui::Heading("Player light");
    Ui::Checkbox("Light above the camera, coloured by the game's light there", &g_settings.playerLight);
    Ui::Checkbox("Only in the dark: in play, under a roof, out of every light's reach",
                 &g_settings.playerLightOnlyInTheDark);
    if (g_settings.playerLight && g_settings.playerLightOnlyInTheDark) {
        char roof[24], nearest[32];
        _snprintf(roof, sizeof(roof), g_player.covered < 0 ? "not asked yet" : "%d of %d points",
                  g_player.covered, kCoverPoints);
        _snprintf(nearest, sizeof(nearest), g_player.nearest >= 0.0f ? "%.2f x its reach" : "none sent",
                  g_player.nearest);
        Ui::LabelDim("Now %3.0f%%: %s; roof %s (%s); nearest light %s", g_player.fade * 100.0f,
                     g_player.playing ? "playing" : "not playing", g_player.roofed ? "yes" : "no", roof, nearest);
    }
    Ui::Slider("Player light scale", &g_settings.playerLightScale, 10.0f, 100000.0f, "%.0f", true);
    Ui::Slider("Player light reach", &g_settings.playerLightReach, 50.0f, 4096.0f, "%.0f units", true);
    Ui::Slider("Height above the camera", &g_settings.playerLightHeight, 0.0f, 200.0f, "%.0f units");
    {
        const Zones::PointLight engine = Zones::LightAtProbe();
        if (engine.valid) {
            Ui::LabelDim("Game's light here: ambient %.0f %.0f %.0f, directional %.0f %.0f %.0f", engine.ambient[0],
                         engine.ambient[1], engine.ambient[2], engine.directional[0], engine.directional[1],
                         engine.directional[2]);
        } else {
            Ui::LabelDim("Game's light here: not sampled yet.");
        }
    }

    Ui::Heading("Test light");
    Ui::Checkbox("Enabled##test", &g_test.enabled);
    Ui::SameLine();
    if (Ui::Button("Place in front of the camera") && haveCamera) {
        for (int i = 0; i < 3; ++i) {
            g_test.position[i] = camera[i] + forward[i] * g_test.distance;
        }
        g_test.enabled = true;
    }
    Ui::Slider("Distance from the camera", &g_test.distance, 0.0f, 4000.0f, "%.0f units");
    Ui::Slider("Intensity", &g_test.intensity, 0.0f, 10.0f, "%.2f");
    Ui::Slider("Reach", &g_test.reach, 0.0f, 4096.0f, "%.0f units");
    Ui::Slider("Red", &g_test.colour[0], 0.0f, 1.0f, "%.2f");
    Ui::Slider("Green", &g_test.colour[1], 0.0f, 1.0f, "%.2f");
    Ui::Slider("Blue", &g_test.colour[2], 0.0f, 1.0f, "%.2f");
    Ui::LabelDim("At (%.0f, %.0f, %.0f)", g_test.position[0], g_test.position[1], g_test.position[2]);

    Ui::Heading("Light definitions (the game's table)");
    if (g_definitions.empty()) {
        Ui::LabelDim("Not loaded.");
    } else {
        Ui::LabelDim("      id  reach  falloff  bytes +4..+7");
        for (size_t i = 0; i < g_definitions.size(); ++i) {
            const Definition& d = g_definitions[i];
            Ui::Swatch(Colour(d.rgba));
            Ui::SameLine();
            Ui::Label("%4d %6d %8d   %02x %02x %02x %02x", (int)i, d.reach, d.falloff, d.rgba[0], d.rgba[1],
                      d.rgba[2], d.rgba[3]);
        }
    }
}
