#include "config.h"
#include "device.h"
#include "log.h"
#include "lookdev.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {

// The log lands beside the DLL rather than in the working directory, which the engine
// changes during startup.
void BuildLogPath(HMODULE self, char* out, size_t size) {
    char modulePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(self, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        lstrcpynA(out, "Video_D3D9.log", (int)size);
        return;
    }
    char* slash = strrchr(modulePath, '\\');
    if (slash) {
        *(slash + 1) = 0;
    } else {
        modulePath[0] = 0;
    }
    _snprintf(out, size, "%s%s", modulePath, Lookdev::LogFileName());
    out[size - 1] = 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Lookdev::DetectMode();
        char logPath[MAX_PATH];
        BuildLogPath(module, logPath, sizeof(logPath));
        LogOpen(logPath);
        Config::Load(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        Device::Destroy();
        LogClose();
    }
    return TRUE;
}
