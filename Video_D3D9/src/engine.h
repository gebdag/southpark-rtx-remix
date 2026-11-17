#pragma once

// Runtime patches applied to SouthPark.exe from inside the video plugin.
//
// Everything the plugin can do about culling is downstream of a decision the executable has
// already made: geometry it culls is never written into the display list, so no renderer
// option can bring it back. The one lever that reaches upstream is patching the engine's own
// code, which the plugin can do because it is loaded into that process.
namespace Engine {

// Applies whatever the ini asks for. Safe to call once the executable has unpacked itself,
// which is true by the time any GFXDLL_ export runs. Every patch verifies the bytes it is
// about to overwrite and refuses if they do not match, so a different build of the game
// leaves the process untouched rather than corrupted.
void Install();

// What the engine asked to see, and how full its fixed-size draw arrays got. Both are needed
// to tell "the whole map is drawn" from "the patch is not running" and from "the map does not
// fit the arrays".
void LogState();

// True once the engine's camera has been patched to take its aspect ratio from the display
// ([Display] AspectRatio other than 4:3). Without it the engine renders 4:3 whatever the back
// buffer is, so the display has to stay 4:3 too.
bool CameraFollowsDisplayAspect();

// The back buffer's width over height. The camera widens its view to it (Hor+) from the next
// frame on; ignored while CameraFollowsDisplayAspect() is false.
void SetDisplayAspect(float aspect);

// [Display] HudAtSides: where the 2D quad being drawn goes on a picture wider than 4:3. -1 the
// picture's left edge, 1 its right edge, 0 the centred 4:3 frame -- everything but the in-game
// HUD's side elements. `caller` is the engine's return address from GFXDLL_DrawQuadPCX.
int HudSide(unsigned int caller);

// Once a frame: brings the camera's field of view to [Display] FieldOfView, except in
// cutscenes, which keep the framing they were made with, and the radius drawn around the
// camera to [FullMap] KeepAroundCamera.
void OnFrame();

// Sends one of the engine's API calls to `replacement` instead. The unpacked executable calls
// every API through the copy protection's dispatcher (`call [0x0156D218]`), which picks the
// real function from the return address, so its import table cannot be redirected; the call
// site itself is rewritten. `replacement` must take what the API that site reaches takes, in
// the same calling convention. Returns false, leaving the code alone, when `site` holds
// neither that call nor this redirect already.
bool RedirectApiCall(unsigned int site, const void* replacement);

} // namespace Engine
