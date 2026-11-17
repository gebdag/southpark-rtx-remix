# Video_D3D9 (`Video_D3D.dll`)

A Direct3D 9 renderer for South Park (1999, PC), written so that RTX Remix can path trace the
game. It replaces the game's own `Video_D3D.dll` video plugin.

The game's original renderer transforms and projects every vertex on the CPU and hands Direct3D
finished screen-space triangles. Remix cannot path trace those: it needs world geometry and a
camera. This renderer reads the same display lists the original reads, and instead gives D3D9
world-space triangles with a real view and projection matrix, normals, stable vertex ranges and
draw state that Remix can classify (sky, UI, decals, view model, translucency).

It also patches `SouthPark.exe` in memory, because the engine culls the world before the renderer
sees it and a path tracer needs the geometry around the camera too. It adds window modes, wide
aspect ratios, a field-of-view setting, a wide-screen HUD layout, a sky and metal swatches.

```
SouthPark.exe -> Video_D3D.dll (this renderer) -> d3d9.dll (RemixProxy) -> RTX Remix bridge
```

## How it fits into the game

South Park loads its video renderers as plugins. At start-up it loads every `Video_*.dll` in the
game folder, asks each one for its drivers through `GFXDLL_GetDriverInfo`, unloads them, and
loads the chosen one again. This DLL reports one driver, **Direct3D9 Renderer (D3D)**.

- It implements all 46 `GFXDLL_*` exports, with the same ordinals as the original
  (`Video_D3D.def`). All are `__cdecl`.
- `Direct3DCreate9` is not imported. The DLL loads the `d3d9.dll` beside `SouthPark.exe` by full
  path, because the game's DirectDraw start-up has already loaded the system `d3d9.dll` and an
  import by name would bind to that one. With no `d3d9.dll` in the game folder it falls back to
  the system runtime and renders without Remix. The load happens in `GFXDLL_GetDriverInfo`, the
  first export the engine calls.
- The engine patches are applied in `GFXDLL_InitializeDriver`. By then the packed executable has
  unpacked itself.
- `Video_D3D9.ini` and `Video_D3D9.log` live beside the DLL, in `<game dir>`.

The renderer exports two functions for the other modules:

| Export | Used by | Purpose |
|---|---|---|
| `SouthPark_SetSetting(section, key, value)` | RemixProxy (F4 overlay) | Changes one live setting without writing the ini. Returns 1 if the key is one of the live settings. |
| `SouthPark_CoverAbove(points, count)` | RemixProxy (player light) | How many of the points have recently drawn level geometry above them. -1 when the renderer holds no geometry near any of them. |

## Installing

1. Keep a copy of the game's original `<game dir>\Video_D3D.dll`.
2. Copy `build\Video_D3D.dll` over it.
3. For RTX Remix, install RemixProxy and the Remix bridge as their README describes, so a
   `d3d9.dll` sits beside `SouthPark.exe`.
4. Pick **Direct3D9 Renderer (D3D)** in the game's driver list.

On first start the renderer writes `<game dir>\Video_D3D9.ini` with every setting and a comment
for each. Delete the file to get the defaults back.

## Settings (`Video_D3D9.ini`)

The defaults suit RTX Remix. Most settings are read once at start-up. Five are **live**: the
renderer re-reads them when the ini file changes, and the F4 overlay sets them directly through
`SouthPark_SetSetting`. They are marked *live* below.

### `[Display]`

| Key | Default | What it does |
|---|---|---|
| `Mode` | `borderless` | `borderless` covers the whole monitor. `windowed` is a framed window you can move and resize. Alt+Enter switches between them while the game runs. |
| `WindowWidth`, `WindowHeight` | `1280`, `960` | Client size of the framed window (at least 320 x 240). It is shrunk, keeping its shape, if the frame would not fit the monitor's work area. |
| `WindowX`, `WindowY` | `-1`, `-1` | Top-left of the framed window on the desktop. -1 centres it. |
| `AspectRatio` | `4:3` | Shape of the picture: `W:H` (`16:9`, `21:9`, `2.39:1`), a plain number (`1.7778`), or `window` to follow the window's shape. Wider than 4:3 widens the 3D view at the sides and keeps its height (Hor+). The menus and loading art stay 4:3 in the middle. Space the picture does not fill is black bars; nothing is stretched. Values outside 0.5 to 4 fall back to 4:3. |
| `FieldOfView` | `1.00` | *Live.* Multiplier on the game's own field of view, 0.5 to 2. Cutscenes keep their own framing. |
| `HudAtSides` | `1` | *Live.* On a picture wider than 4:3, moves the in-game HUD's elements to the picture's edges: health and radar left, armor and weapons right. The crosshair, menus and text screens stay centred. |
| `PauseWhenUnfocused` | `1` | The game pauses while its window is not in front. 0 keeps it running. |
| `LimitCutscenes` | `0` | The engine sleeps in every cutscene frame, which holds cutscenes, the logos and the intro near 38 fps. 0 removes that sleep, 1 keeps it. Cutscenes advance by real time either way. |

### `[FullMap]` (culling and draw distance)

| Key | Default | What it does |
|---|---|---|
| `LoadEntireMap` | `1` | Master switch for the culling patches below (`DisableFrustumTest`, `UncullRooms`, `UncullEntities`, `KeepAroundCamera`). 0 leaves the engine's culling alone. |
| `DisableFrustumTest` | `1` | Bypasses the engine's six-plane frustum test at its six drawing call sites. Required by `UncullRooms` and `KeepAroundCamera`. |
| `UncullRooms` | `1` | Widens the room-visibility test at the three drawing sites. Every object carries a room bitmask and is dropped when it shares no bit with the camera's. |
| `UncullEntities` | `0` | Also widens the room and frustum tests on the entity path. That path also registers entities in a resource cache, so admitting everything starves it; `UncullBudget` caps it. |
| `UncullBudget` | `512` | With `UncullEntities`, how many entities the engine's own test rejects may be drawn anyway per frame. 0 is unlimited. |
| `KeepAroundCamera` | `2048` | *Live.* Draws the level within this many world units of the camera, whichever way it faces: level geometry, objects, particles and entities. Only drawing changes; loading, spawning and simulation stay the engine's. 0 to 16384; 0 is off. Needs `LoadEntireMap=1` and `DisableFrustumTest=1`. |
| `DrawDistancePercent` | `100` | *Live.* Scales the camera's draw distance, 100 to 400. 100 is the level as designed. The F4 overlay's slider covers 100 to 200. |
| `BatchLimitScale` | `8` | Multiplies the engine's three per-frame draw arrays (512 models, 1024 mesh instances, 512 material batches), 1 to 64. Applies whatever `LoadEntireMap` says. 1 keeps the engine's sizes. |
| `CameraDrawListScale` | `1` | Multiplies the camera's 128-entry drawn-object list, 1 to 16. Above 1 the list becomes one shared buffer, which is correct for one viewport and wrong for split screen. Keep 1 for multiplayer. |
| `FarPlane` | `100000` | Far plane of the projection, in world units. The engine's own is a little over a thousand units. 0 keeps the engine's. |

### `[Culling]`

| Key | Default | What it does |
|---|---|---|
| `DisableTriangleSkipMask` | `0` | 1 ignores the per-triangle skip bitmask the display list installs (opcodes `0xa8` / `0xa7`). |
| `DisableOffscreenQuadReject` | `0` | The original drops a whole 2D quad when a corner leaves the 640x480 screen. 1 draws it and lets the GPU clip. |

### `[Sky]`

| Key | Default | What it does |
|---|---|---|
| `UseEngineBackgroundColour` | `1` | Clears the frame to the background colour the display list sends (opcode `0xf8`) instead of a fixed colour. |
| `SynthesiseSkybox` | `1` | Draws a box around the camera at the far plane, first in each frame, so Remix has a sky where the game shows only the cleared background. |
| `SkyboxSize` | `0` | Half-extent of the box in world units. 0 derives it from the projection's far plane. |
| `SkyboxColour` | `-1` | Box colour as `0xRRGGBB`. -1 follows the engine's background colour, including its day and night changes. |
| `SkyTextureNames` | `Dawn,Day,Nite` | Texture sets drawn as sky (at the far plane, into Remix's sky layer). Matched by name prefix, so `Day` covers `Day1` and `Day2`. |
| `StarrySky` | `1` | *Live.* The intro and main menu draw their stars on a flat wall of cards. 1 skips the cards and puts their texture on the sky box, so the stars surround the scene. |
| `StarTextureNames` | `STARS01` | The star-card sets, by name prefix. |

### `[Textures]`, `[Colour]`, `[Materials]`, `[Normals]`

| Key | Default | What it does |
|---|---|---|
| `[Textures] EffectPaletteMode` | `1` | How to read a format-3 palette entry (the effect atlas: blood, smoke, explosions, splats, sky). 1: intensity in the high byte, alpha in the low byte (the N64 IA16 layout). 2: the two swapped. 3: ARGB4444. |
| `[Colour] ApplyConstantColour` | `1` | Multiplies world geometry by the display list's constant colour (opcodes `0xfa` / `0xfb`), as the original does. Flat-coloured house walls have white vertices and get their colour only from this. |
| `[Materials] EnvironmentMapsAsMetal` | `1` | Draws environment-mapped surfaces with a flat swatch of the reflection image's mean colour, for a Remix material to turn into metal. See [Metal and chrome](#metal-and-chrome). |
| `[Materials] ChromeTextureNames` | `RYCHROME,ALCHROME,YLWCHRM` | Sets that are reflection images painted on with ordinary texture coordinates (the Acclaim logo's chrome), drawn the same way, by name prefix. |
| `[Normals] SmoothingAngle` | `80` | Pre-lit geometry gets computed normals. Faces meeting at less than this angle, in degrees, share a smoothed normal; sharper edges stay hard. 0 makes every face flat. |

### Sections read by other modules

The default ini also carries `[Input]`, read by DInputShim (`dinput.dll`), and `[Lights]`, read by
RemixProxy (`d3d9.dll`). This renderer only writes their defaults. Their READMEs document them.

### Debug settings

| Key | Default | What it does |
|---|---|---|
| `[Debug] ScreenshotFrame` | `0` | Frame at which to start writing screenshots automatically. 0 disables. |
| `[Debug] ScreenshotInterval` | `0` | Frames between automatic screenshots after the first. 0 takes one. |
| `[Debug] HideTextureNames` | empty | Sets (name prefixes) whose batches are not drawn. |
| `[Debug] DumpEnvTextures` | `0` | Writes each environment-mapped texture variant, and each set in `DumpTextureNames`, to `<game dir>\env_textures\` once per run, as the exact `A8R8G8B8` texels uploaded (the bytes Remix hashes). |
| `[Debug] DumpTextureNames` | empty | Extra sets (name prefixes) for `DumpEnvTextures`. |
| `[Colour] ResetStatePerFrame` | `0` | Clears the render flags and the texture selection at the start of each frame. The original carries them over; 0 matches it. |
| `[Log] LogEveryCall` | `0` | Logs every engine call. Produces gigabytes and costs frame rate. |

`HideTextureNames`, `DumpEnvTextures`, `DumpTextureNames` and `ResetStatePerFrame` are not in
the generated ini; add them by hand. The `[Lookdev]` keys are described under [Lookdev](#lookdev).

Keys that are always active:

| Key | Action |
|---|---|
| F10 | Logs the next frame draw by draw (texture, transparency, render flags, combiner mode, constant colour, bounds, normals) and writes its triangles to `<game dir>\frame_dump.obj`. |
| F11 | Writes a screenshot to `<game dir>\shot_NNNNN.bmp`. |
| Alt+Enter | Switches between borderless and windowed. |

## How it works

### Display lists

The engine describes each frame as display lists in a format close to the N64's F3D microcode,
handed over through `GFXDLL_ProcessDisplayList`. Commands are 8 bytes; the opcode is the low
byte of the first word. `geometry.cpp` interprets the ones that matter:

| Opcode | Meaning |
|---|---|
| `0x01`, `0xbd` | Matrix (model push, absolute or concatenated, or the camera); matrix pop |
| `0x04` | Vertex upload into a 256-slot vertex window |
| `0xad`, `0xb1`, `0xbf` | Triangles, as byte indices into the vertex window |
| `0xa9`, `0xa6` | Texture set, plus frame and palette selectors |
| `0xaf`, `0xfa`, `0xfb` | Colour-combiner mode (41 modes); primitive and environment constant colours |
| `0xb6`, `0xb7` | Clear / set render flags (lighting, back-face culling, environment map) |
| `0xb9` | Render mode: depth test, depth write, decal, blending |
| `0xae` | First-person weapon on or off |
| `0xa8`, `0xa7` | Per-triangle skip mask on / off |
| `0x06`, `0xbc`, `0xb8` | Sub-list; segment base addresses; end of list |
| `0xf8` | Background colour |

`0xac` and `0xe4` are longer than 8 bytes and are skipped by their real length.

Triangles collect into a batch. The batch is drawn (flushed) whenever something it depends on
changes: texture, frame or palette, combiner mode, an effective constant colour, render mode,
weapon flag, the cull bit, the camera, or a vertex upload over slots it still uses.

### Matrices and what Remix sees

- **Positions.** Each vertex is 20 bytes: an integer object-space position, a texture
  coordinate, and four bytes that are a normal when the lighting flag (`0x20000`) is set and a
  pre-lit colour when it is not. As in the original, the current model matrix is applied when the
  vertices are uploaded, because one batch can mix vertices uploaded under different matrices.
  Positions reach D3D in world space, and `D3DTS_WORLD` is identity.
- **Camera.** The engine's camera matrix is view and projection multiplied together. The renderer
  factors it into a rigid view matrix and a projection matrix and sets them separately, so Remix
  gets a real camera. The engine's projection flips X; the flip stays in the projection so the
  view is a proper rotation and triangle winding stays correct. The far plane is rebuilt at
  `[FullMap] FarPlane`, keeping the engine's near plane.
- **Normals.** Lit normals go through the inverse transpose of the model matrix and are
  normalised, so non-uniformly scaled models shade correctly.
- **Stable hashes.** Remix identifies a mesh by hashing the vertex range of a draw. Each batch is
  compacted to exactly the vertices it indexes, so the same wall hashes the same every frame
  whatever was batched around it.
- **Buffers.** All geometry goes through one dynamic vertex and index buffer ring, never
  `DrawPrimitiveUP`, because Remix rasterises sky draws from the buffers bound to the draw.

### Colour, blending and depth

The original's colour comes from the N64 combiner, emulated through a 41-entry mode table. The
renderer carries that table (`kConstantSource`, `kAlphaSource`, `kModeBindsTexture`):

- **Constant colour.** Each mode takes the primitive colour, the environment colour or none. The
  chosen constant multiplies the vertex colour at draw time, not at upload, because the engine
  may change it between uploading a block and drawing from it. It is kept in the vertex colour,
  which survives Remix's material conversion.
- **Constant alpha.** Modes may replace vertex alpha with the primitive alpha, the environment
  alpha, their product, or one half (the cloud layers). A batch whose constant alpha is 0 is not
  drawn, as the original's alpha test discards it.
- **Texture binding.** Seven modes (`0x12`-`0x15`, `0x17`, `0x19`, `0x1c`) bind no texture even
  when a set is selected. Mode `0x16` adds the texture to the colour, `0x28` uses the texture
  alone.
- **Blending** comes only from the render mode (`0xb9`), never from the combiner. Remix treats a
  blended draw as translucent, so only what the engine blends (clouds, fire, trails, blob
  shadows) is blended.
- **Alpha test**: `ALPHAREF 1`, `GREATEREQUAL`, which discards exactly the fully transparent texels.
- **Depth** test and write follow the render mode. Back-face culling follows render flag
  `0x2000` (counter-clockwise front faces) and is passed to Remix unchanged.

**Decals.** Signs and posters are drawn as ordinary opaque geometry exactly on top of a wall,
which a rasteriser resolves by draw order and a path tracer cannot. Every opaque triangle of the
frame is filed by plane and by 512-unit grid cell. A later batch with a triangle in the same plane,
facing the same way and overlapping an earlier one, is tagged as a decal for Remix by setting
stage 0's `D3DTSS_CONSTANT` to `0xD15DECA1` (`rtx.fixedFunctionDecalMarkerEnabled`). No texture
stage reads that constant, so a plain rasteriser is unaffected.

**View model.** Opcode `0xae` marks the first-person weapon. The engine places it in the world, 75
to 120 units ahead of the camera, where it sinks into slopes and walls. Its batches get the
viewport depth range 0 to 0.1, which keeps it in front for a rasteriser and is what Remix's view
model detection looks for (`rtx.viewModel.maxZThreshold = 0.1`).

### Textures

The engine hands each texture set to `GFXDLL_EnumerateTexture` from a reused staging buffer, so
the renderer copies every set and returns the copy from `GFXDLL_GetTextureSetPtr`. It also fills
the frame and palette counts at chunk 0 `+6`/`+7`, which the engine divides its animation timer by.

A set holds a header, a run of image frames, a run of palettes and a name. Opcode `0xa9` selects
a frame and a palette, which is how mouths, eyes and water animate. Each (frame, palette) pair is
decoded to an `A8R8G8B8` texture on first use and cached. `GFXDLL_FlushTextureCache` drops the
decoded textures and keeps the sets.

| Format | Pixels | Palette |
|---|---|---|
| 0 | 8 bits per pixel | 256 entries, RGBA5551 (1-bit alpha) |
| 1 | 4 bits per pixel | 16 entries, RGBA5551 |
| 3 | indexed | Effect atlas: intensity and alpha, read per `EffectPaletteMode`. The colour comes from the combiner constant. |
| 2, 4, 5, 6 | indexed | None: the sample is opacity over white, coloured by the vertex colour. |

Palette entries are stored byte-swapped. Textures use linear filtering and wrap addressing.

### Normals for pre-lit geometry

Most of the world is pre-lit: its vertices carry colours, not normals. A path tracer needs
normals, so the renderer computes them from the triangles. An edge whose faces meet at more than
`SmoothingAngle` is sharp. Each corner averages the faces around its point without crossing a
sharp edge, weighted by each face's angle at that point. A vertex slot whose corners disagree
becomes several vertices.

The engine draws the world a few triangles at a time, often with its own copy of each corner, so
smoothing within one draw would leave slopes faceted. `normal_cache.cpp` keeps every pre-lit face
drawn recently, keyed by its corners' world positions, and smooths across draws. Faces not drawn
for a while are dropped. The same cache answers `SouthPark_CoverAbove`.

### Sky

The game has no sky geometry; where the world does not cover the screen, the cleared background
shows. The renderer gives Remix a sky in three ways. All of them draw with the viewport depth
range 1 to 1, which Remix treats as sky (`rtx.skyMinZThreshold`, 1 by default) and rasterises
into its sky layer behind the path traced scene.

- **Sky box.** With `SynthesiseSkybox`, a box centred on the camera, in the background colour or
  `SkyboxColour`, is the frame's first draw once the camera is known. A frame that opens with a
  full-screen 2D backdrop gets no box.
- **Sky sets.** Batches using a `SkyTextureNames` set (the cloud layers) go to the far plane and
  write no depth.
- **2D backdrops.** 2D quads drawn before any world geometry (the main menu's night sky) are held
  back. If the frame then draws world geometry, they become its backdrop: real geometry through an
  orthographic projection at the far plane. If it does not, the frame was a 2D screen and they
  are drawn as UI.

With `StarrySky`, the star cards are not drawn and the sky box shows their texture instead, tiled
15 times across each face.

### Metal and chrome

The game environment-maps its chrome and gold surfaces: a reflection image wrapped by the surface
normal. The renderer does not generate those coordinates; the reflection is left to Remix. With
`EnvironmentMapsAsMetal`, a batch with render flag `0x40000`, or one using a `ChromeTextureNames`
set, is drawn with a flat swatch instead of its texture:

- The colour is the mean of the image's visible texels, brightened until its strongest channel
  is `0xDD`, then rounded to 16 levels per channel. That gives at most 4096 swatch colours.
- Metal swatches are 16 x 16, chrome swatches 8 x 8, so a material can tell them apart.
- Remix names a texture by the XXH3-64 hash of its texels. The release's Remix mod
  (`SouthParkMaterials`) carries a metallic material for every possible swatch, and a mirror
  finish for the chrome size.

### 2D layer

The HUD, menus, fonts and fades are 2D quads drawn through `GFXDLL_DrawQuadPCX`, in 640 x 480
coordinates. Images come from the engine's own image objects; the renderer keeps a texture in
sync with the pixels the engine writes. Quads drawn after the world are UI: pre-transformed
vertices, which Remix composites over the path traced frame (`rtx.preTransformedVerticesIsUI`).

Each quad goes into the 4:3 frame, scaled and centred (HUD, menus, loading art), with three
exceptions: a solid-colour quad spanning the frame (a fade) takes the full width, a backdrop
covers the whole picture, and a HUD element under `HudAtSides` goes to the left or right edge.
A frame with no world geometry gets a black underlay, so a 2D screen has black sides.

### Window, aspect ratio and field of view

The engine creates its window before any renderer loads. `window.cpp` subclasses it and controls
its style, size and position, borderless or windowed, with Alt+Enter between them. The engine
minimises its window when it loses focus; those three calls are redirected so the window stays.
With `PauseWhenUnfocused=0`, the focus messages that pause the engine are not passed on.

The back buffer is the largest rectangle of the picture's shape that fits the window, centred,
with black bars around it. It follows the window when it is resized.

For a picture wider than 4:3, the camera code is patched to multiply its aspect ratio by the
display's aspect over 4:3 after the vertical field of view is fixed. That one value feeds the
projection, the stored camera aspect and the frustum used for culling, so the picture widens at
the sides and the culling widens with it. `FieldOfView` scales the horizontal field of view at the
point the camera loads it, which also widens both.

For `HudAtSides`, the HUD's draw calls are tagged with the element they draw. The side comes from
that element's slot in the active HUD layout: anchored right or placed from x 160 goes right, the
rest left; the radar uses the engine's own left-of-320 rule.

## Engine patches

All addresses are the US build's (see [Game builds](#game-builds)). Every patch first checks the
bytes it will overwrite. If they differ, it logs the mismatch and leaves that site alone.

| What | Where | Patch |
|---|---|---|
| Frustum test | `0x004468D0`, called at `0x0043C26A`, `0x00440D38`, `0x00440E63`, `0x004411BC`, `0x00441313`, `0x004432B7` | The six drawing calls go to a stub that returns "visible". The function itself is untouched. |
| Entity frustum test | call at `0x00450724` | Goes through the original test; a rejected entity still passes when it is within `KeepAroundCamera`, or, with `UncullEntities`, while the frame's budget lasts. |
| Room visibility | `0x0043C13C`, `0x0044113A`, `0x00441250`; entity path `0x004506C8` | The branch that drops an object sharing no room bit with the camera is removed. The entity site only with `UncullEntities`. |
| Keep around camera | call to `0x00423020` at `0x0044477C`; box copy `0x005005F0`; 24 reads in six tests | After the engine builds its cull bounds, the copy at `0x005005F0` is widened to a square around each eye. The six drawing tests (level sections, level geometry, placed instances, level objects, particle emitters, entities) read that copy instead of `camera+0x332`. The cull box gameplay uses (`0x00500640`) is untouched. |
| Draw distance and field of view | detour at `0x00421807`; area fog `0x00420A10`, called at `0x00420F7C`, `0x00420FFE` | Scales `camera+0x4E4` and the field of view before the camera derives its volumes. The area-fog update always sees the engine's own distance, so scaling never compounds. Field of view is not scaled while a cutscene camera runs (`0x0049BF00`). |
| Aspect ratio | detour at `0x00421888` | Multiplies the camera aspect. Only when `AspectRatio` is not 4:3. |
| Cutscene sleep | branch at `0x00444701` | Always skips the sleep, unless `LimitCutscenes=1`. |
| Draw arrays | bounds at `0x0042A441`, `0x0042B18D`, `0x0042B114` and their base references | Each array moves to a buffer `BatchLimitScale` times larger. |
| Camera drawn-object list | cap at `0x0043CEA2`, store at `0x0043CEA9`, six readers | Moves `camera+0xC88` to a larger shared buffer (`CameraDrawListScale` > 1). |
| HUD sides | callback `0x0041B010` registered at `0x00443533`; 19 calls in the HUD code | Brackets the HUD callback and tags each element's draw call. |
| Window | `0x0047DCB1`, `0x0047DD21`, `0x0047DD87` | The minimise and restore calls go to a function that leaves the window as it is. |

The unpacked executable calls every Windows API through the copy protection's dispatcher
(`call [0x0156D218]`), so its import table cannot be redirected. `Engine::RedirectApiCall`
rewrites the call site instead.

### Why culling needs patches

The engine culls in the executable, before anything reaches the display list, so no renderer
setting can bring that geometry back. In order, an object is dropped by:

1. the room test, when it shares no room bit with the camera (`UncullRooms`);
2. a 2D box test in X and Z around the view cone. The cone starts at the eye, so nothing behind
   the camera survives, and Remix lights rooms through the missing walls (`KeepAroundCamera`);
3. the frustum test (`DisableFrustumTest`);
4. the draw distance at `camera+0x4E4`, the fog distance of the camera's area, which sizes the
   frustum, the box above and the published cull box (`DrawDistancePercent`);
5. fixed-size lists that stop accepting entries when full, without any message: the camera's
   128-entry drawn-object list and the three per-frame draw arrays (`CameraDrawListScale`,
   `BatchLimitScale`).

The frustum test is bypassed per call site because its seventh caller is the entity path, which
also writes a visibility flag on the entity and registers it in a resource cache (a hash table at
`0x00599054`). Bypassing it there for every entity starves that cache, and enemies stop spawning;
hence the separate, budgeted `UncullEntities`.

The log reports the peak fill of every list and array, says when one fills, and samples the
entity resource cache.

## Game builds

Every address in the code is the US build's (`SouthPark.exe` header timestamp `0x36AF2FCA`). The
other retail builds, Europe (En, Fr, Es) and Germany, have the same code at other addresses.
`common/game_build.h`, shared with RemixProxy and DInputShim, maps them:

- The build is identified by the header timestamp, which the copy protection leaves intact. On
  the US build, `GameBuild::Place()` returns its input.
- On any other build, the code section is searched once for the signatures in
  `common/game_build_signatures.h` (178 signatures for 140 US addresses): code bytes with some
  left open, resolving to the match position, an operand in it, or a branch target. When an
  address has several signatures, the most common answer wins.
- An address without a signature, or whose signatures are not found, moves with the nearest found
  address within `0x100` bytes. If there is none, it resolves to 0 and the patch that needs it is
  skipped.
- US addresses inside expected or relocated instruction bytes are rewritten the same way
  (`PlaceEmbedded`).
- The search needs the decrypted code, so it runs on the first patch, not at load. The first module
  to search shares the table with the others through a per-process file mapping, because a module
  that has already patched code would break the others' signatures.

The log's first engine line names the build and how many addresses were found.

## Lookdev

The renderer contains the game side of a developer tool for comparing renderers on identical
frames. The host program and scripts that replay captures are not part of this repository. None
of it is active unless an environment variable is set when the game starts:

| Variable | Effect |
|---|---|
| `SP_LOOKDEV=1` | Tee mode: the original renderer draws the game window, and this renderer draws the same frames through Remix into a second window. Frames can be captured. |
| `SP_LOOKDEV_HOST=1` | The DLL runs inside the replay host: no executable to patch, no engine window. Logs to `Video_D3D9.lookdev.log`. |
| `SP_LOOKDEV_PLAIN=1` | In the host, uses the system `d3d9.dll` instead of the game folder's. |
| `SP_LOOKDEV_DUMP`, `SP_LOOKDEV_DUMP_AT` | In the host, logs one replayed frame draw by draw, as F10 does. |

- **Capture** (`lookdev.cpp`). A frame is fully described by what crosses the plugin boundary:
  the display lists, the engine memory they read (matrices, vertices, sub-lists, skip masks), the
  texture sets, the start-up export arguments, and the last value of every state command. A capture
  records exactly that into `<game dir>\lookdev\`. It starts on the capture key or when a
  `<game dir>\lookdev\capture.request` file appears.
- **Tee** (`original.cpp`, `exports.cpp`). The original renderer is loaded from
  `<game dir>\lookdev\ref\d3d_original.dll`. It needs DirectDraw and Direct3D 3, provided by
  dgVoodoo2 from the same folder (`DDraw.dll`, `D3DImm.dll`); its DirectDraw imports are pointed at
  dgVoodoo2's. dgVoodoo2 also needs `dgVoodoo.conf` in `<game dir>`; without it the tee does not
  start. The 2D, frame-pacing, option and texture-set lookup exports go to the original only.
  Start-up, texture enumeration, scenes and display lists go to both. The aspect-ratio patch is
  not installed in tee mode.

The `[Lookdev]` ini keys apply only in tee mode: `CaptureKey` (virtual-key code, default `0x78`,
F9), `CaptureFrames` (frames per capture, default 1, up to 600), and `GameWindowX/Y/Width/Height`
and `RemixWindowX/Y/Width/Height` (defaults -1, -1, 640, 480; -1 centres the game window and puts
the Remix window beside it).

## Logs and files

- `<game dir>\Video_D3D9.log` is rewritten on every start. It records the configuration, the build
  and every patch applied or skipped, the d3d9 runtime chosen (`d3d9 from game folder:` means
  Remix is in the loop), the texture set census, draw-array and list peaks, and periodic batch
  statistics.
- F10, F11 and the `[Debug]` dump settings write the files described above.

## Building

```
build.bat
```

- Needs Visual Studio 2022 with the *Desktop development with C++* workload. `build.bat` finds it
  with `vswhere` and uses the 32-bit (`vcvars32`) toolchain; the game is a 32-bit process.
- Compiles every `src\*.cpp` with `/MT /O2` and includes `..\common`.
- Links `user32.lib` and `gdi32.lib` only; `d3d9.lib` is deliberately not linked.
- Output: `build\Video_D3D.dll`, with `Video_D3D.map` beside it.

## Source files

| File | Contents |
|---|---|
| `exports.cpp` | The `GFXDLL_*` and `SouthPark_*` exports; lookdev tee routing |
| `geometry.cpp` | Display lists, matrices, combiner table, batching, sky box, decals |
| `normal_cache.cpp` | Cross-draw normal smoothing; `CoverAbove` |
| `texture.cpp`, `pcx.cpp` | Texture sets and metal swatches; the 2D layer |
| `device.cpp`, `window.cpp` | D3D9 device, back buffer, screenshots; window modes and focus |
| `engine.cpp` | Patches to `SouthPark.exe` |
| `config.cpp`, `log.cpp`, `dllmain.cpp` | The ini, the log, the DLL entry |
| `lookdev.cpp`, `original.cpp` | Lookdev capture and tee |
