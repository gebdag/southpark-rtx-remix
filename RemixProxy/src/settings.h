#pragma once

#include <windows.h>
#include <d3d9.h>

// Game settings on the F4 panel: the pad's turn and look sensitivity, the field of view, the
// culling radius, the draw distance, the HUD layout and the starry sky.
// They belong to other modules -- the input shim (dinput.dll) reads [Input] PadTurnSensitivity
// and PadLookSensitivity, the renderer (Video_D3D.dll) [Display] FieldOfView and HudAtSides,
// [FullMap] KeepAroundCamera and DrawDistancePercent, and [Sky] StarrySky. A change on the panel
// is handed to its module at once through the module's SouthPark_SetSetting export; the ini is
// written only by Save.
namespace Settings {

void Init(const char* iniPath);
// The renderer's projection, to show the field of view in degrees.
void OnSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
// Hands this frame's changes to their modules; call once a frame.
void Update();
void DrawPanel();

// Writes the panel's values to Video_D3D9.ini; returns how many keys changed in the file.
int Save();
// Puts every value back to its module's default, applied at once and saved only by Save.
void ResetToDefaults();

// Writes one key of Video_D3D9.ini when its text differs from what the file holds, so
// comments and untouched keys stay as they are. True when the file changed.
bool WriteKey(const char* section, const char* key, const char* text);

} // namespace Settings
