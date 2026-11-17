#pragma once

#include <windows.h>
#include <d3d9.h>

// South Park's own dynamic lights, sent to RTX Remix.
//
// The engine attaches point lights to some particle effects: a 10-node pool at 0x524208, each
// node following its particle and fading with its alpha. The PC video plugins never drew them.
// Each frame this reads the pool and submits one Remix sphere light per enabled node.
namespace Lights {

// Reads [Lights] from the renderer's settings file and checks this is the expected game build.
void Init(const char* iniPath);

void OnSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
// Before the device's BeginScene: reads the game's pool for this frame.
void OnBeginScene();
// Once the Remix API is connected.
void OnConnected();
// Inside the bridge's BeginScene: replaces last frame's Remix lights with this frame's.
void SubmitFrame();

// Overlay content.
void DrawMarkers(int width, int height);
void DrawPanel();

} // namespace Lights
