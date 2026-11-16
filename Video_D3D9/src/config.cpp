#include "config.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

char g_iniPath[MAX_PATH];
char g_baseDir[MAX_PATH];
char g_skyNames[512];
int g_screenshotFrame = 0;
int g_screenshotInterval = 0;

bool g_disableBackfaceCulling  = true;
bool g_disableTriangleSkipMask = false;
bool g_disableOffscreenQuadReject = false;
bool g_useEngineBackgroundColour = true;
bool g_depthlessSky = true;
bool g_logEveryCall = false;
bool g_loadEntireMap = true;
bool g_disableFrustumTest = true;
bool g_uncullRooms = true;
bool g_uncullEntities = false;
int g_uncullBudget = 512;
unsigned int g_batchLimitScale = 8;
float g_drawDistance = 8192.0f;
unsigned int g_cameraDrawListScale = 1;
int g_effectPaletteMode = 1;
float g_farPlane = 100000.0f;
bool g_applyConstantColour = true;
bool g_resetStatePerFrame = false;
bool g_synthesiseSkybox = true;
float g_skyboxSize = 0.0f;
int g_skyboxColour = -1;
int g_skyboxBlueShift = 45;
int g_skyboxBlueTarget = 0x4A82D6;

// Written out when no ini exists, so the options are discoverable without the source.
const char* kDefaultIni =
    "; Video_D3D9.ini -- options for the D3D9 replacement renderer.\r\n"
    "; Delete this file to regenerate it with the defaults.\r\n"
    ";\r\n"
    "; The defaults are tuned for RTX Remix. Set the culling options to 0 to get the\r\n"
    "; engine's own rasteriser behaviour back.\r\n"
    "\r\n"
    "[Culling]\r\n"
    "; Force D3DCULL_NONE. The engine asks for backface culling on essentially every batch\r\n"
    "; through render-flag bit 0x2000; with it on, a path tracer never sees those faces and\r\n"
    "; thin geometry lights and shadows incorrectly. 0 follows the engine.\r\n"
    "DisableBackfaceCulling=1\r\n"
    "\r\n"
    "; Ignore the per-triangle skip bitmask (display-list opcodes 0xa8 / 0xa7): 1 draws the\r\n"
    "; triangles the engine marks to be skipped.\r\n"
    "DisableTriangleSkipMask=0\r\n"
    "\r\n"
    "; The engine drops a whole 2D quad when any corner leaves the screen rather than\r\n"
    "; clipping it. 1 draws it anyway and lets the GPU clip.\r\n"
    "DisableOffscreenQuadReject=0\r\n"
    "\r\n"
    "[FullMap]\r\n"
    "; South Park culls the world in the executable, not in the renderer: geometry outside\r\n"
    "; the camera's frustum is never written into the display list at all, and a path tracer\r\n"
    "; then lights the scene through the holes that leaves. 1 patches SouthPark.exe in\r\n"
    "; memory to skip that test; the file on disk is not touched.\r\n"
    "LoadEntireMap=1\r\n"
    "\r\n"
    "; Bypass the engine's six-plane frustum test at its six DRAWING call sites. Turn this\r\n"
    "; off first if the map misbehaves.\r\n"
    "DisableFrustumTest=1\r\n"
    "\r\n"
    "; Widen the engine's ROOM VISIBILITY test, which is what actually brings level geometry\r\n"
    "; in and runs before the frustum test entirely. Every object carries a room bitmask and\r\n"
    "; is dropped outright when it shares no bit with the camera's, so widening the frustum\r\n"
    "; without this does nothing -- rejected objects never reach the frustum test. It is\r\n"
    "; widened at its four test sites rather than by forcing the camera's mask, so the field\r\n"
    "; the engine recomputes and reads elsewhere is left alone. The distance box and the\r\n"
    "; frustum still apply afterwards, so this admits what the camera can see, not the level.\r\n"
    "UncullRooms=1\r\n"
    "\r\n"
    "; The test has a seventh caller, inside the engine's entity path -- and South Park's\r\n"
    "; town is built out of entities, so without this the town stays culled no matter what\r\n"
    "; the six drawing sites do. That path also registers each entity through a resource\r\n"
    "; cache, and reporting the whole level visible every frame stops enemies spawning and\r\n"
    "; breaks the player's weapon. Hence the budget below.\r\n"
    "UncullEntities=1\r\n"
    "\r\n"
    "; How many objects the engine's own test rejects may be shown anyway, per frame. 0 is\r\n"
    "; unlimited, which has the problems above. Raise it until the town looks right and\r\n"
    "; lower it if entities misbehave; the log reports the peak actually used, so if the\r\n"
    "; town needs only a few hundred there is headroom to spare.\r\n"
    "UncullBudget=512\r\n"
    "\r\n"
    "; The engine records each frame's draw work into three fixed arrays -- 512 models, 1024\r\n"
    "; mesh instances, 512 material batches -- and stops recording, without any diagnostic,\r\n"
    "; once one fills. An entry it cannot record is geometry that is not drawn that frame:\r\n"
    "; that is what makes particular house walls flicker in and out even with the engine's\r\n"
    "; own culling left alone, and what drops the first-person weapon once the map is opened\r\n"
    "; up. This multiplies all three and relocates them, and it applies whether or not\r\n"
    "; LoadEntireMap is on. 1 keeps the engine's own sizes. The log says when one fills.\r\n"
    "BatchLimitScale=8\r\n"
    "\r\n"
    "; What still culls the town after the tests above are widened is the camera's own\r\n"
    "; draw distance -- 1024 units, the field the game's 'Fog Distance' video option\r\n"
    "; moves. The engine sizes its frustum corners, side planes, frustum box and cull\r\n"
    "; box from it, and the 2D X/Z box test that runs between the room and frustum\r\n"
    "; tests rejects everything outside that box before the frustum override is ever\r\n"
    "; reached. Raising it lets the engine rebuild all of them itself, so nothing is\r\n"
    "; hand-forged and gameplay keeps reading a camera merely set to see further.\r\n"
    ";\r\n"
    "; This is a floor in world units, not a multiplier: the engine only rewrites the\r\n"
    "; field when the fog option is set, so scaling it in place would compound every\r\n"
    "; frame. 0 keeps the engine's own 1024.\r\n"
    "DrawDistance=8192\r\n"
    "\r\n"
    "; The engine records each frame's visible objects into a fixed 128-entry list and stops\r\n"
    "; recording when it fills -- an object past it is simply not drawn, with no diagnostic.\r\n"
    "; That is what bounds DrawDistance: at 8192 units the list peaks around 93 of 128, so a\r\n"
    "; busier scene crosses it and geometry goes missing. This multiplies it and relocates it.\r\n"
    ";\r\n"
    "; CAUTION: relocating makes it one shared buffer instead of a field inside each camera.\r\n"
    "; Equivalent for a single viewport, WRONG for split screen -- set to 1 for multiplayer.\r\n"
    "CameraDrawListScale=1\r\n"
    "\r\n"
    "; Far plane for the renderer's projection, in world units. The engine builds its own\r\n"
    "; at a little over a thousand units, which would clip away most of what the wider\r\n"
    "; volume submits. 0 keeps the engine's value.\r\n"
    "FarPlane=100000\r\n"
    "\r\n"
    "[Input]\r\n"
    "; Every key here is read by the dinput.dll shim, not by this renderer. The game acquires\r\n"
    "; the mouse and keyboard through DirectInput in exclusive mode before it loads any\r\n"
    "; renderer at all, which suppresses the OS cursor for the whole process, swallows\r\n"
    "; Alt+Tab, and leaves the game reading input while another window has focus. None of\r\n"
    "; that is reachable from here, so the renderer does not touch input at all.\r\n"
    "\r\n"
    "; Rewrite every SetCooperativeLevel to non-exclusive and foreground-only.\r\n"
    "ForceNonExclusiveInput=1\r\n"
    "\r\n"
    "; Report empty devices while the game window is not the foreground window, so alt-\r\n"
    "; tabbing away really does stop the game rather than leaving it running blind.\r\n"
    "SuspendInputWhenUnfocused=1\r\n"
    "\r\n"
    "; Mirror the Alt+X Remix overlay toggle: while the overlay is up the game stops reading\r\n"
    "; its devices, so the pointer belongs to the overlay. Alt+X again hands it back. Set to\r\n"
    "; 0 if the overlay is bound to some other key.\r\n"
    "RemixMenuHotkeySuspendsInput=1\r\n"
    "\r\n"
    "; Keep an OS cursor on screen: refuse the game's SetCursor(NULL), give its window class\r\n"
    "; an arrow, and drop its ClipCursor and its recentring mouse_event calls while it is not\r\n"
    "; entitled to the pointer.\r\n"
    "KeepCursorVisible=1\r\n"
    "\r\n"
    "[Textures]\r\n"
    "; How to read a format-3 palette entry. Format 3 is the effect atlas -- blood, smoke,\r\n"
    "; explosions, splats, the sky -- and its entries are not packed colour: the shape is\r\n"
    "; greyscale and the colour arrives from the combiner constant, which is how one 16-entry\r\n"
    "; palette serves blood, smoke and snow alike.\r\n"
    ";\r\n"
    ";   1  intensity in the high byte, alpha in the low byte (the N64 IA16 TLUT layout)\r\n"
    ";   2  the two swapped\r\n"
    ";   3  ARGB4444, for comparison only: it turns brown splats blue and gives the blood\r\n"
    ";      spray the wrong alpha\r\n"
    ";\r\n"
    "; Which byte carries alpha is the one thing the palette data does not settle on its own,\r\n"
    "; so if an effect is absent rather than merely miscoloured, try 2 before going back to 3.\r\n"
    "EffectPaletteMode=1\r\n"
    "\r\n"
    "[Colour]\r\n"
    "; Multiply world geometry by the display list's constant colour, as opcodes 0xfa\r\n"
    "; (primitive) and 0xfb (environment) deliver it. The original folds that colour into\r\n"
    "; every vertex before transforming, and South Park's house walls carry no texture and\r\n"
    "; white vertex colours -- it is the only thing that gives them their colour. 0 draws\r\n"
    "; them as the vertices alone describe them, which is white.\r\n"
    "ApplyConstantColour=1\r\n"
    "\r\n"
    "[Sky]\r\n"
    "; Clear to the background colour the engine sends through display-list opcode 0xf8,\r\n"
    "; the way the original renderer does, instead of to a fixed colour.\r\n"
    "UseEngineBackgroundColour=1\r\n"
    "\r\n"
    "; Draw sky-textured batches with no depth test and no depth write. A skydome should be\r\n"
    "; drawn that way regardless, and it is what Remix's sky auto-detection looks for.\r\n"
    "DepthlessSky=1\r\n"
    "\r\n"
    "; Draw a large box around the camera in the background colour, before any world\r\n"
    "; geometry and with depth off. The game has no skydome -- where the world does not\r\n"
    "; cover the screen it simply leaves the cleared frame showing -- and a path tracer\r\n"
    "; needs a real surface there: something to bound the scene, to catch the rays that\r\n"
    "; miss the world, and to light it.\r\n"
    "SynthesiseSkybox=1\r\n"
    "\r\n"
    "; Half-extent of that box in world units. 0 derives it from the projection's far plane,\r\n"
    "; which is the right answer unless the box is visibly being clipped.\r\n"
    "SkyboxSize=0\r\n"
    "\r\n"
    "; Colour of the box as 0xRRGGBB. -1 follows the background colour the engine sends\r\n"
    "; through display-list opcode 0xf8, which is what the game intends the sky to be, and\r\n"
    "; keeps its day/night changes.\r\n"
    "SkyboxColour=-1\r\n"
    "\r\n"
    "; How far to pull that colour towards SkyboxBlueTarget, 0-100. The engine's backdrop is\r\n"
    "; a pale wash chosen to sit behind a rasterised scene; as a real surface lighting a\r\n"
    "; path-traced one it wants to read as sky. 0 leaves the colour exactly as chosen.\r\n"
    "SkyboxBlueShift=45\r\n"
    "SkyboxBlueTarget=0x4A82D6\r\n"
    "\r\n"
    "; Texture-set names treated as sky, matched against the name stored in chunk 3 of the\r\n"
    "; set. Matching is by prefix, so 'Day' covers Day1 and Day2.\r\n"
    "SkyTextureNames=Dawn,Day,Nite\r\n"
    "\r\n"
    "[Debug]\r\n"
    "; Two keys are always live and need no setting here:\r\n"
    ";   F10  log the NEXT frame draw by draw -- what texture each one bound and how much of\r\n"
    ";        it is transparent, the render flags, the combiner mode and constant colour, the\r\n"
    ";        world bounding box, and whether the normals are zero or not unit length. Stand\r\n"
    ";        in front of something that looks wrong and press it once.\r\n"
    ";   F11  screenshot, written beside this file as shot_NNNNN.bmp.\r\n"
    "\r\n"
    "; Frame to start writing screenshots at, and how many frames apart to repeat. 0\r\n"
    "; disables; F11 always takes one on demand. They land beside this file as\r\n"
    "; shot_NNNNN.bmp, because PrintWindow returns black for a D3D9 swapchain and the\r\n"
    "; renderer therefore has to take its own.\r\n"
    "ScreenshotFrame=0\r\n"
    "ScreenshotInterval=0\r\n"
    "\r\n"
    "[Log]\r\n"
    "; Log every engine call. The 2D layer alone makes about 1.6 million calls per run, so\r\n"
    "; this produces gigabytes and costs frame rate. Leave off unless tracing a problem.\r\n"
    "LogEveryCall=0\r\n";

bool ReadBool(const char* section, const char* key, bool fallback) {
    return GetPrivateProfileIntA(section, key, fallback ? 1 : 0, g_iniPath) != 0;
}

} // namespace

void Config::Load(void* self) {
    char modulePath[MAX_PATH];
    const DWORD len = GetModuleFileNameA((HMODULE)self, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        lstrcpynA(g_iniPath, "Video_D3D9.ini", MAX_PATH);
    } else {
        char* slash = strrchr(modulePath, '\\');
        if (slash) {
            *(slash + 1) = 0;
        } else {
            modulePath[0] = 0;
        }
        _snprintf(g_iniPath, MAX_PATH, "%sVideo_D3D9.ini", modulePath);
        g_iniPath[MAX_PATH - 1] = 0;
        lstrcpynA(g_baseDir, modulePath, MAX_PATH);
    }

    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileA(g_iniPath, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, 0);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, kDefaultIni, (DWORD)strlen(kDefaultIni), &written, 0);
            CloseHandle(f);
        }
    }

    g_disableBackfaceCulling     = ReadBool("Culling", "DisableBackfaceCulling", true);
    g_disableTriangleSkipMask    = ReadBool("Culling", "DisableTriangleSkipMask", false);
    g_disableOffscreenQuadReject = ReadBool("Culling", "DisableOffscreenQuadReject", false);
    g_useEngineBackgroundColour  = ReadBool("Sky", "UseEngineBackgroundColour", true);
    g_depthlessSky               = ReadBool("Sky", "DepthlessSky", true);
    g_logEveryCall               = ReadBool("Log", "LogEveryCall", false);
    g_loadEntireMap              = ReadBool("FullMap", "LoadEntireMap", true);
    g_disableFrustumTest         = ReadBool("FullMap", "DisableFrustumTest", true);
    g_uncullRooms                = ReadBool("FullMap", "UncullRooms", true);
    g_uncullEntities             = ReadBool("FullMap", "UncullEntities", false);
    g_uncullBudget = GetPrivateProfileIntA("FullMap", "UncullBudget", 512, g_iniPath);
    if (g_uncullBudget < 0) g_uncullBudget = 0;
    g_batchLimitScale = (unsigned int)GetPrivateProfileIntA("FullMap", "BatchLimitScale", 8,
                                                            g_iniPath);
    if (g_batchLimitScale < 1)  g_batchLimitScale = 1;
    if (g_batchLimitScale > 64) g_batchLimitScale = 64;
    g_cameraDrawListScale = (unsigned int)GetPrivateProfileIntA(
        "FullMap", "CameraDrawListScale", 1, g_iniPath);
    if (g_cameraDrawListScale < 1)  g_cameraDrawListScale = 1;
    if (g_cameraDrawListScale > 16) g_cameraDrawListScale = 16;
    g_drawDistance = (float)GetPrivateProfileIntA("FullMap", "DrawDistance", 8192, g_iniPath);
    if (g_drawDistance < 0.0f)     g_drawDistance = 0.0f;
    // These boxes also feed spatial lookups and a near-infinite extent crashes the game.
    if (g_drawDistance > 65536.0f) g_drawDistance = 65536.0f;
    g_farPlane = (float)GetPrivateProfileIntA("FullMap", "FarPlane", 100000, g_iniPath);
    g_effectPaletteMode = GetPrivateProfileIntA("Textures", "EffectPaletteMode", 1, g_iniPath);
    if (g_effectPaletteMode < 1 || g_effectPaletteMode > 3) g_effectPaletteMode = 1;
    g_applyConstantColour        = ReadBool("Colour", "ApplyConstantColour", true);
    g_resetStatePerFrame         = ReadBool("Colour", "ResetStatePerFrame", false);
    g_synthesiseSkybox           = ReadBool("Sky", "SynthesiseSkybox", true);
    g_skyboxSize = (float)GetPrivateProfileIntA("Sky", "SkyboxSize", 0, g_iniPath);
    if (g_skyboxSize < 0.0f) g_skyboxSize = 0.0f;
    {
        // Written as six hex digits, which GetPrivateProfileInt cannot parse.
        char colour[32];
        GetPrivateProfileStringA("Sky", "SkyboxColour", "-1", colour, sizeof(colour), g_iniPath);
        g_skyboxColour = (int)strtol(colour, 0, 0);
        if (g_skyboxColour >= 0) {
            g_skyboxColour &= 0x00ffffff;
        } else {
            g_skyboxColour = -1;
        }
        GetPrivateProfileStringA("Sky", "SkyboxBlueTarget", "0x4A82D6", colour, sizeof(colour),
                                 g_iniPath);
        g_skyboxBlueTarget = (int)strtol(colour, 0, 0) & 0x00ffffff;
    }
    g_skyboxBlueShift = GetPrivateProfileIntA("Sky", "SkyboxBlueShift", 45, g_iniPath);
    if (g_skyboxBlueShift < 0)   g_skyboxBlueShift = 0;
    if (g_skyboxBlueShift > 100) g_skyboxBlueShift = 100;
    g_screenshotFrame = GetPrivateProfileIntA("Debug", "ScreenshotFrame", 0, g_iniPath);
    g_screenshotInterval = GetPrivateProfileIntA("Debug", "ScreenshotInterval", 0, g_iniPath);
    GetPrivateProfileStringA("Sky", "SkyTextureNames", "Dawn,Day,Nite",
                             g_skyNames, sizeof(g_skyNames), g_iniPath);

    LogLine("=== config: %s", g_iniPath);
    LogLine("    DisableBackfaceCulling=%d DisableTriangleSkipMask=%d DisableOffscreenQuadReject=%d",
            g_disableBackfaceCulling, g_disableTriangleSkipMask, g_disableOffscreenQuadReject);
    LogLine("    UseEngineBackgroundColour=%d DepthlessSky=%d SkyTextureNames=%s",
            g_useEngineBackgroundColour, g_depthlessSky, g_skyNames);
    LogLine("    LoadEntireMap=%d DisableFrustumTest=%d UncullRooms=%d UncullEntities=%d "
            "UncullBudget=%d BatchLimitScale=%u FarPlane=%.0f",
            g_loadEntireMap, g_disableFrustumTest, g_uncullRooms, g_uncullEntities,
            g_uncullBudget, g_batchLimitScale, g_farPlane);
    LogLine("    DrawDistance=%.0f CameraDrawListScale=%u",
            g_drawDistance, g_cameraDrawListScale);
    LogLine("    EffectPaletteMode=%d", g_effectPaletteMode);
    LogLine("    ApplyConstantColour=%d ResetStatePerFrame=%d",
            g_applyConstantColour, g_resetStatePerFrame);
    LogLine("    SynthesiseSkybox=%d SkyboxSize=%.0f SkyboxColour=%d "
            "SkyboxBlueShift=%d SkyboxBlueTarget=0x%06X",
            g_synthesiseSkybox, g_skyboxSize, g_skyboxColour,
            g_skyboxBlueShift, g_skyboxBlueTarget);
    LogLine("    ScreenshotFrame=%d ScreenshotInterval=%d",
            g_screenshotFrame, g_screenshotInterval);
    LogLine("    LogEveryCall=%d", g_logEveryCall);
    LogSetCallLogging(g_logEveryCall);
}

bool Config::DisableBackfaceCulling()     { return g_disableBackfaceCulling; }
bool Config::DisableTriangleSkipMask()    { return g_disableTriangleSkipMask; }
bool Config::DisableOffscreenQuadReject() { return g_disableOffscreenQuadReject; }
bool Config::UseEngineBackgroundColour()  { return g_useEngineBackgroundColour; }
bool Config::DepthlessSky()               { return g_depthlessSky; }
const char* Config::SkyTextureNames()     { return g_skyNames; }
bool Config::LogEveryCall()               { return g_logEveryCall; }
bool Config::LoadEntireMap()              { return g_loadEntireMap; }
bool Config::DisableFrustumTest()         { return g_disableFrustumTest; }
bool Config::UncullRooms()                { return g_uncullRooms; }
bool Config::UncullEntities()             { return g_uncullEntities; }
int Config::UncullBudget()                { return g_uncullBudget; }
unsigned int Config::BatchLimitScale()    { return g_batchLimitScale; }
unsigned int Config::CameraDrawListScale() { return g_cameraDrawListScale; }
float Config::DrawDistance()              { return g_drawDistance; }
float Config::FarPlane()                  { return g_farPlane; }
int Config::EffectPaletteMode()           { return g_effectPaletteMode; }
bool Config::ApplyConstantColour()        { return g_applyConstantColour; }
bool Config::ResetStatePerFrame()        { return g_resetStatePerFrame; }
bool Config::SynthesiseSkybox()           { return g_synthesiseSkybox; }
float Config::SkyboxSize()                { return g_skyboxSize; }
int Config::SkyboxColour()                { return g_skyboxColour; }
int Config::SkyboxBlueShift()             { return g_skyboxBlueShift; }
int Config::SkyboxBlueTarget()            { return g_skyboxBlueTarget; }
int Config::ScreenshotFrame()             { return g_screenshotFrame; }
int Config::ScreenshotInterval()           { return g_screenshotInterval; }
const char* Config::BaseDir()             { return g_baseDir; }
