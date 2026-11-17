#pragma once

#include <windows.h>
#include <d3d9.h>

// Game settings on the F4 panel: the pad's turn and look sensitivity, the field of view and
// the starry sky.
// They belong to other modules -- the input shim (dinput.dll) reads [Input] PadTurnSensitivity
// and PadLookSensitivity, the renderer (Video_D3D.dll) [Display] FieldOfView and [Sky]
// StarrySky -- and each of
// them re-reads its keys when the ini changes. So the panel's only job is to write the ini,
// which also makes every change stick.
namespace Settings {

void Init(const char* iniPath);
// The renderer's projection, to show the field of view in degrees.
void OnSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
// Writes pending changes; call once a frame.
void Update();
void DrawPanel();

} // namespace Settings
