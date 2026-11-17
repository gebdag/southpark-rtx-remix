#include "remix.h"
#include "log.h"

namespace {

remixapi_Interface g_api = {};
bool g_connected = false;

// Exported by the bridge client; declared here rather than taken from the bridge's own header,
// which is not part of the public API.
typedef remixapi_ErrorCode(__stdcall* RegisterCallbacksFn)(Remix::SceneCallback beginScene,
                                                           Remix::SceneCallback endScene,
                                                           Remix::SceneCallback present);

} // namespace

bool Remix::Connect(HMODULE bridge, SceneCallback beginScene) {
    if (g_connected) {
        return true;
    }
    const auto initialize = (PFN_remixapi_InitializeLibrary)GetProcAddress(bridge, "remixapi_InitializeLibrary");
    const auto registerCallbacks = (RegisterCallbacksFn)GetProcAddress(bridge, "remixapi_RegisterCallbacks");
    if (!initialize || !registerCallbacks) {
        Log("Remix API: the bridge client exports no remixapi_InitializeLibrary -- not a Remix bridge?");
        return false;
    }

    // For API major version 0 the bridge requires the same minor version as its own; it adapts the
    // interface table to any patch level of it.
    remixapi_InitializeLibraryInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    const remixapi_ErrorCode status = initialize(&info, &g_api);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        Log("Remix API: initialisation failed, code %d%s", (int)status,
            status == REMIXAPI_ERROR_CODE_NOT_INITIALIZED ? " (is exposeRemixApi = True in .trex\\bridge.conf?)" : "");
        return false;
    }
    registerCallbacks(beginScene, nullptr, nullptr);
    g_connected = true;
    Log("Remix API %d.%d.%d connected", REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    return true;
}

bool Remix::Connected() {
    return g_connected;
}

const remixapi_Interface& Remix::Api() {
    return g_api;
}
