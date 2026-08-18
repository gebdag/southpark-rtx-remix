#include "config.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {

char g_iniPath[MAX_PATH];
char g_skyNames[512];

bool g_disableBackfaceCulling  = true;
bool g_disableTriangleSkipMask = false;
bool g_disableOffscreenQuadReject = false;
bool g_useEngineBackgroundColour = true;
bool g_depthlessSky = true;
bool g_logEveryCall = false;

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
    "[Sky]\r\n"
    "; Clear to the background colour the engine sends through display-list opcode 0xf8,\r\n"
    "; the way the original renderer does, instead of to a fixed colour.\r\n"
    "UseEngineBackgroundColour=1\r\n"
    "\r\n"
    "; Draw sky-textured batches with no depth test and no depth write. A skydome should be\r\n"
    "; drawn that way regardless, and it is what Remix's sky auto-detection looks for.\r\n"
    "DepthlessSky=1\r\n"
    "\r\n"
    "; Texture-set names treated as sky, matched against the name stored in chunk 3 of the\r\n"
    "; set. Matching is by prefix, so 'Day' covers Day1 and Day2.\r\n"
    "SkyTextureNames=Dawn,Day,Nite\r\n"
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
    GetPrivateProfileStringA("Sky", "SkyTextureNames", "Dawn,Day,Nite",
                             g_skyNames, sizeof(g_skyNames), g_iniPath);

    LogLine("=== config: %s", g_iniPath);
    LogLine("    DisableBackfaceCulling=%d DisableTriangleSkipMask=%d DisableOffscreenQuadReject=%d",
            g_disableBackfaceCulling, g_disableTriangleSkipMask, g_disableOffscreenQuadReject);
    LogLine("    UseEngineBackgroundColour=%d DepthlessSky=%d SkyTextureNames=%s",
            g_useEngineBackgroundColour, g_depthlessSky, g_skyNames);
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
