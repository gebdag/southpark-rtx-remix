#pragma once

// Runtime options, read once from Video_D3D9.ini beside the DLL.
//
// These exist because the right answer differs between plain rasterisation and RTX Remix.
// A path tracer wants geometry present and double-sided so light transport can find it,
// where the engine assumes a rasteriser that discards back faces. Rather than pick one, the
// behaviour is switchable and the file documents itself.
namespace Config {

// Reads the ini next to `self`, writing a commented default file if none exists.
void Load(void* self);

// --------------------------------------------------------------------------- culling


// Ignore the per-triangle skip bitmask installed by opcodes 0xa8/0xa7.
bool DisableTriangleSkipMask();

// Draw 2D quads even when a corner leaves the screen. The original drops the whole quad.
bool DisableOffscreenQuadReject();

// ------------------------------------------------------------------------ full map

// Patch the executable so it stops culling the world against its own view volume, and stop
// the renderer clipping what that then submits. The engine's culler is a 2D box test in X
// and Z: geometry outside the box is never written into the display list at all, which is
// why this cannot be answered on the renderer side alone.
bool LoadEntireMap();

// Bypass the engine's six-plane frustum test at its drawing call sites, the last of its
// visibility tests (after the room test and the 2D X/Z box test).
bool DisableFrustumTest();

// Widen the engine's room-visibility test, which runs BEFORE the frustum test and is what
// actually brings level geometry in: every object carries a room bitmask and is dropped
// outright when it shares no bit with the camera's. Widening the frustum without this does
// nothing, because rejected objects never reach the frustum test at all.
bool UncullRooms();

// Also bypass it at the seventh call site, the one inside the engine's entity path. The town
// is built out of entities, so without this it stays culled -- but that path also registers
// each entity through a resource cache, and reporting the whole level visible every frame
// stops enemies spawning and breaks the player's weapon.
bool UncullEntities();

// How many objects the engine's own test rejects may be shown anyway, per frame. 0 is
// unlimited, with the problems above. The renderer logs the peak actually used.
int UncullBudget();

// Multiplier on the engine's three fixed draw arrays, which it overruns silently. 1 keeps the
// engine's own sizes. Anything above 1 relocates each array to a larger buffer and rewrites
// its bound, which is what a whole map needs once the frustum test is off.
unsigned int BatchLimitScale();

// [FullMap] DrawDistancePercent, as a multiplier (1 to 4): scales the camera's draw distance
// (camera+0x4e4), which the engine sets from the fog distance of the area the camera is in.
// That distance sizes the frustum corner points, the side planes, the frustum AABB the
// engine's 2D X/Z test compares against, and the cull box it publishes -- so it, not the
// frustum test, is what still removes the town once the room and frustum tests are widened.
// 1 (100%) is the level as designed. Set while the game runs (the F4 overlay, 100 to 200%).
float DrawDistanceScale();

// Radius around the camera, in world units, inside which the level is drawn whichever way
// the camera faces. The engine's 2D X/Z box test compares objects against a box around the
// view cone, and the cone starts at the eye, so without this nothing behind the camera is
// drawn at all. Drawing only; loading, spawning and simulation are untouched. 0 is off.
// Re-read while the game runs (the F4 overlay's culling radius).
float KeepAroundCamera();

// Multiplier on the camera's drawn-object list, the fixed 128-entry array the engine
// records each frame's visible objects into and overruns silently -- an object past it is
// simply not drawn. This is what bounds the draw distance: a busy scene at a large draw
// distance fills it and geometry goes missing.
//
// Raising it relocates the list, which makes it a single shared buffer rather than a field
// inside each camera. That is equivalent for one viewport and WRONG for split screen, so
// leave it at 1 for multiplayer.
unsigned int CameraDrawListScale();

// Far plane the renderer builds its projection with, in world units. The engine's own is a
// little over a thousand units, which clips away most of what the wider volume submits.
// Zero keeps the engine's value.
float FarPlane();


// ------------------------------------------------------------------------ textures

// How to read a format-3 palette entry. Format 3 is the effect atlas -- blood, smoke,
// explosions, splats, sky -- and its entries are not packed colour: the shape is greyscale
// and the colour arrives from the combiner constant. Reading them as 4444 scatters the two
// bytes across the wrong channels: brown splats come out blue and the blood spray's alpha is
// wrong.
//
//   1  intensity in the high byte, alpha in the low byte  (the N64 IA16 TLUT layout)
//   2  the two swapped
//   3  the original ARGB4444 reading, for comparison
//
// Switchable because which byte carries alpha is the one thing the data does not settle: most
// sets put a clean 0..255 ramp in the low byte, a few put it in the high byte instead.
int EffectPaletteMode();

// ---------------------------------------------------------------------------- colour

// Multiply world geometry by the display list's constant colour, as opcodes 0xfa (primitive)
// and 0xfb (environment) deliver it. The original folds that colour into every vertex before
// transforming; South Park's house walls carry no texture and white vertex colours, so it is
// the only thing that gives them their colour.
bool ApplyConstantColour();

// Clear the engine's sticky display-list state -- render flags and the selected texture set
// -- at the start of every frame.
//
// The original does not. Its render-flag word at 0x10087f14 is written by opcodes 0xb7 and
// 0xb6 and by nothing else in the whole DLL, and the texture selection is armed by 0xa9 and
// cleared only when a batch consumes it, never per frame. Resetting them makes the geometry
// drawn before a frame's first 0xb7 or 0xa9 read differently from every other frame: with
// the lighting bit cleared the four bytes at vertex+16 are decoded as a pre-lit colour when
// they are really a normal, which is a wrong colour that appears and disappears with the
// camera.
//
// 0 is faithful. Kept switchable because it is the one change here whose blast radius covers
// every frame's opening geometry.
bool ResetStatePerFrame();

// ------------------------------------------------------------------------------- sky

// Clear to the colour the engine supplies through display-list opcode 0xf8 rather than to
// a fixed colour.
bool UseEngineBackgroundColour();

// Comma-separated texture-set names treated as sky, matched against the name in chunk 3.
// Sky batches are drawn at the far plane, which RTX Remix takes as its sky layer.
const char* SkyTextureNames();

// [Sky] StarrySky: the game's star-field cards (StarTextureNames, the intro's and the main
// menu's wall of star tiles) are not drawn; the sky box wears their texture instead. Re-read
// while the game runs.
bool StarrySky();

// [Materials] EnvironmentMapsAsMetal: a surface the engine environment-maps (its chrome, the
// Iguana logo's gold) is drawn with a flat swatch of its reflection image's mean colour instead
// of the image on the surface's own coordinates; a Remix mod makes the swatches metallic.
bool EnvironmentMapsAsMetal();
// [Materials] ChromeTextureNames: sets that are reflection images painted on with ordinary
// coordinates (the Acclaim logo's chrome), drawn as metal swatches like environment maps.
const char* ChromeTextureNames();

// [Debug] DumpEnvTextures: write each texture variant drawn environment-mapped, and each of
// the sets named in DumpTextureNames (name prefixes), to <game>\env_textures// (Texture::DumpTexels), once per run.
bool DumpEnvTextures();
const char* DumpTextureNames();
// [Debug] HideTextureNames: sets (name prefixes) whose batches are not drawn at all, to see
// what a frame looks like without them. Empty by default.
const char* HideTextureNames();
const char* StarTextureNames();

// Draw a large box around the camera in the background colour, at the far plane, first in
// the frame. The game has no skydome -- it leaves the cleared frame showing -- and Remix
// needs a sky there.
bool SynthesiseSkybox();

// Half-extent of that box in world units, or 0 to derive it from the projection's far
// plane. Only worth setting if the box is being clipped or is landing inside the world.
float SkyboxSize();

// Colour of the box as 0xRRGGBB, or -1 to follow the background colour the engine sends
// through display-list opcode 0xf8.
int SkyboxColour();

// ---------------------------------------------------------------------------- normals

// Pre-lit geometry carries vertex colours where lit geometry carries normals, so its normals
// are computed from its triangles. Faces meeting at less than this angle, in degrees, share a
// smoothed normal; sharper edges stay hard.
float SmoothingAngle();

// --------------------------------------------------------------------------- display

// [Display] Mode: true for a framed window, false for borderless over the whole monitor.
// Alt+Enter switches between the two while the game runs. See window.h.
bool DisplayWindowed();

// The framed window's client size, and its top-left on the desktop (-1 centres it).
int DisplayWindowWidth();
int DisplayWindowHeight();
int DisplayWindowX();
int DisplayWindowY();

// The engine's own picture: 640x480.
const float kOriginalAspect = 4.0f / 3.0f;

// [Display] AspectRatio: the picture's width over height. kOriginalAspect is the game as it
// shipped; a wider ratio widens the 3D view at the sides and keeps the 2D layer 4:3 in the
// middle. 0 means the window's own shape. Whatever the window has left over is black bars;
// nothing is stretched.
float AspectRatio();

// Let the engine pause the game when its window loses focus, as it always has. Off, the game
// keeps running behind other windows.
bool PauseWhenUnfocused();

// [Display] LimitCutscenes: keep the engine's sleep in cutscene frames (off by default).
bool LimitCutscenes();

// [Display] FieldOfView: a multiple of the engine's own field of view (1 = as shipped).
float FieldOfView();

// Re-reads the settings that may change while the game runs when the ini has changed. Cheap
// to call every frame.
void RefreshLive();

// Sets one of those settings without touching the file: the F4 overlay (d3d9.dll) applies its
// changes this way and writes the ini only when asked to save. False for a key that is not
// one of them.
bool SetLive(const char* section, const char* key, float value);

// ----------------------------------------------------------------------------- debug

// Frame number at which to start writing screenshots automatically, or 0 for none, and how
// many frames apart to repeat (0 for a single shot). F11 always takes one on demand; these
// take them without a key press.
int ScreenshotFrame();
int ScreenshotInterval();

// --------------------------------------------------------------------------- lookdev

// Virtual-key code that captures frames when the game runs in lookdev mode (SP_LOOKDEV=1),
// and how many consecutive frames one capture records. See lookdev.h.
int LookdevCaptureKey();
int LookdevCaptureFrames();

// The game window in lookdev mode, where the original renderer draws: client size, and its
// top-left on the desktop. -1 for either coordinate centres the pair of windows on the screen.
int LookdevGameWindowX();
int LookdevGameWindowY();
int LookdevGameWindowWidth();
int LookdevGameWindowHeight();

// The live Remix window in lookdev mode: client size, and its top-left on the desktop. -1 for
// either coordinate places it beside the game window.
int LookdevRemixWindowX();
int LookdevRemixWindowY();
int LookdevRemixWindowWidth();
int LookdevRemixWindowHeight();

// Directory the DLL lives in, with a trailing backslash. The engine changes the working
// directory during startup, so anything written relative to it lands somewhere unhelpful.
const char* BaseDir();

// ------------------------------------------------------------------------------- log

// Log every engine call. Off by default: the 2D layer alone makes ~1.6M calls a run, which
// turns the log into gigabytes and throttles the frame rate.
bool LogEveryCall();

} // namespace Config
