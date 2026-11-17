#pragma once

#include <windows.h>
#include <d3d9.h>

// A 32-bit client reaches the 64-bit runtime through the bridge, which is what the header's
// x86 switch is for (the bridge's own header sets it the same way).
#define REMIX_ALLOW_X86
#include <remix/remix_c.h>

// The Remix API, as the 32-bit bridge client exposes it. Requires exposeRemixApi = True in
// .trex\bridge.conf; the bridge refuses the API otherwise.
namespace Remix {

typedef void(__cdecl* SceneCallback)(void);

// Connects through the bridge client's remixapi_InitializeLibrary. The bridge accepts this only
// once its server is running, which is guaranteed after the first CreateDevice. beginScene runs
// inside the bridge's BeginScene, which is where Remix takes this frame's lights.
bool Connect(HMODULE bridge, SceneCallback beginScene);

bool Connected();
const remixapi_Interface& Api();

} // namespace Remix
