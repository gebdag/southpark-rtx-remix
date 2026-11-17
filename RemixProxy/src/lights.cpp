#include "lights.h"
#include "log.h"
#include "overlay.h"
#include "remix.h"
#include "zones.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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

bool Matches(unsigned int address, const unsigned char* bytes, size_t size) {
    return !IsBadReadPtr((const void*)address, size) && memcmp((const void*)address, bytes, size) == 0;
}

template <typename T> T Read(unsigned int address) {
    return *(const T*)address;
}

bool IsNode(unsigned int node) {
    return node >= kPoolNodes && node < kPoolNodes + kNodeCount * kNodeSize &&
           (node - kPoolNodes) % kNodeSize == 0;
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

struct Settings {
    bool enabled = true;
    float radianceScale = 1500.0f;
    float emitterRadius = 6.0f;
    bool scaleByReach = true;
    bool swapRedBlue = false;
    bool markers = false;
    bool keepFallbackLight = true;
    bool zoneLights = true;
    float zoneRadianceScale = 1500.0f;
    // Most zones are not lamps but fills hundreds to thousands of units across, hung over the
    // town to tint whole streets. As lights they would be suns.
    float zoneMaxRadius = 512.0f;
    // A soft light above the camera, coloured by the light the engine gives an object standing
    // there. It keeps rooms that have only the level's large fills -- no lamp-sized zone to send --
    // from going black, at the cost of a light that travels with the player.
    bool playerLight = false;
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

float ReadFloat(const char* ini, const char* key, float fallback) {
    char text[32];
    GetPrivateProfileStringA("Lights", key, "", text, sizeof(text), ini);
    return text[0] ? (float)atof(text) : fallback;
}

bool ReadBool(const char* ini, const char* key, bool fallback) {
    return GetPrivateProfileIntA("Lights", key, fallback ? 1 : 0, ini) != 0;
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
    const unsigned int table = Read<unsigned int>(kWorld + kWorldLightTable);
    ReadDefinitions(table);
    const unsigned int stride = table ? Read<unsigned int>(table) : 0;

    unsigned int node = Read<unsigned int>(kPool + 4);
    for (int steps = 0; node && steps < kNodeCount && IsNode(node); ++steps) {
        if (Read<short>(node + 0x16)) {
            LiveLight l;
            l.slot = (int)((node - kPoolNodes) / kNodeSize);
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
    return !z.subtractive && z.shape != Zones::Box && z.radius <= g_settings.zoneMaxRadius;
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
    g_settings.enabled = ReadBool(ini, "Enabled", g_settings.enabled);
    g_settings.radianceScale = ReadFloat(ini, "RadianceScale", g_settings.radianceScale);
    g_settings.emitterRadius = ReadFloat(ini, "EmitterRadius", g_settings.emitterRadius);
    g_settings.scaleByReach = ReadBool(ini, "ScaleByReach", g_settings.scaleByReach);
    g_settings.swapRedBlue = ReadBool(ini, "SwapRedBlue", g_settings.swapRedBlue);
    g_settings.markers = ReadBool(ini, "Markers", g_settings.markers);
    g_settings.keepFallbackLight = ReadBool(ini, "KeepFallbackLight", g_settings.keepFallbackLight);
    g_settings.zoneLights = ReadBool(ini, "ZoneLights", g_settings.zoneLights);
    g_settings.zoneRadianceScale = ReadFloat(ini, "ZoneRadianceScale", g_settings.zoneRadianceScale);
    g_settings.zoneMaxRadius = ReadFloat(ini, "ZoneMaxRadius", g_settings.zoneMaxRadius);
    g_settings.playerLight = ReadBool(ini, "PlayerLight", g_settings.playerLight);
    g_settings.playerLightScale = ReadFloat(ini, "PlayerLightScale", g_settings.playerLightScale);
    g_settings.playerLightReach = ReadFloat(ini, "PlayerLightReach", g_settings.playerLightReach);
    g_settings.playerLightHeight = ReadFloat(ini, "PlayerLightHeight", g_settings.playerLightHeight);

    g_gameSupported = Matches(0x00426970, kUploadPrologue, sizeof(kUploadPrologue)) &&
                      Matches(0x0043f470, kLookupPrologue, sizeof(kLookupPrologue));
    Log("lights: %s; radiance scale %.0f, emitter radius %.1f, fallback light %s",
        g_gameSupported ? "South Park light pool found" : "not the expected SouthPark.exe -- game lights off",
        g_settings.radianceScale, g_settings.emitterRadius, g_settings.keepFallbackLight ? "kept on" : "Remix default");
    Log("lights: %s", Zones::Install() ? "level light zones hooked" : "not the expected SouthPark.exe -- zone lights off");
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

void Lights::SubmitFrame() {
    const auto& api = Remix::Api();
    for (auto handle : g_handles) {
        api.DestroyLight(handle);
    }
    g_handles.clear();

    if (g_settings.enabled) {
        for (const auto& l : g_live) {
            char name[32];
            _snprintf(name, sizeof(name), "sp_light_%d_%d", l.slot, l.definition);
            float rgb[3];
            Rgb(l.def.rgba, rgb);
            SubmitLight(Hash(name), l.position, rgb,
                        Brightness(l.intensity, (float)l.def.reach, g_settings.radianceScale));
        }
    }
    if (g_settings.zoneLights) {
        for (const auto& z : g_zones) {
            if (!ZoneIsLight(z)) {
                continue;
            }
            // Named by where it is: which section a zone came from changes as the camera moves.
            char name[64];
            _snprintf(name, sizeof(name), "sp_zone_%d_%d_%d_%d", (int)z.centre[0], (int)z.centre[1],
                      (int)z.centre[2], (int)z.radius);
            float rgb[3];
            ZoneRgb(z, rgb);
            SubmitLight(Hash(name), z.centre, rgb, Brightness(1.0f, z.radius, g_settings.zoneRadianceScale),
                        z.shape == Zones::Cone ? z.axis : nullptr);
        }
    }
    if (g_settings.playerLight) {
        const Zones::PointLight engine = Zones::LightAtProbe();
        float camera[3], forward[3];
        if (engine.valid && Camera(camera, forward)) {
            camera[1] += g_settings.playerLightHeight;
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = (engine.ambient[c] + engine.directional[c]) / 255.0f;
            }
            SubmitLight(Hash("sp_player_light"), camera, rgb,
                        Brightness(1.0f, g_settings.playerLightReach, g_settings.playerLightScale));
        }
    }
    if (g_test.enabled) {
        SubmitLight(Hash("sp_test_light"), g_test.position, g_test.colour,
                    Brightness(g_test.intensity, g_test.reach, g_settings.radianceScale));
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
    Ui::Slider("Largest zone sent", &g_settings.zoneMaxRadius, 50.0f, 8000.0f, "%.0f units", true);
    if (g_zones.empty()) {
        Ui::LabelDim("None in this level, or the game has zone lighting off.");
    } else {
        // Only the zones that become lights: the fills not sent can number dozens.
        int notSent = 0;
        Ui::LabelDim("      zone shape   colour          ambient  radius   centre                  distance");
        for (size_t i = 0; i < g_zones.size(); ++i) {
            const Zones::Zone& z = g_zones[i];
            if (!ZoneIsLight(z)) {
                ++notSent;
                continue;
            }
            Ui::Swatch(ZoneColour(z));
            Ui::SameLine();
            float distance = 0.0f;
            if (haveCamera) {
                const float dx = z.centre[0] - camera[0], dy = z.centre[1] - camera[1], dz = z.centre[2] - camera[2];
                distance = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            Ui::Label("%4d %-6s %4.0f %4.0f %4.0f  %8.2f %7.0f   %6.0f %6.0f %6.0f   %8.0f%s", (int)i,
                      ShapeName(z), z.colour[0], z.colour[1], z.colour[2], z.ambientShare, z.radius, z.centre[0],
                      z.centre[1], z.centre[2], distance, z.flickers ? "  flicker" : "");
        }
        Ui::LabelDim("%d more not sent: larger than the limit, boxes, or subtractive.", notSent);
    }

    Ui::Heading("Player light");
    Ui::Checkbox("Light above the camera, coloured by the game's light there", &g_settings.playerLight);
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
