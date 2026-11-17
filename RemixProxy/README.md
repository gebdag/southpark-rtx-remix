# RemixProxy (`d3d9.dll`)

A small `d3d9.dll` between South Park's renderer and the RTX Remix bridge client. It passes all
rendering straight through. It sends the game's own lights to Remix through the Remix API (the
lights some particle effects carry, each level's light zones, and an optional player light for
dark enclosed places), and it draws the F4 overlay, a settings panel of its own.

```
SouthPark.exe -> Video_D3D.dll (renderer) -> d3d9.dll (this proxy) -> d3d9_remix.dll (Remix bridge client)
```

`include/remix/remix_c.h` is NVIDIA's Remix API header (MIT licence). Everything else in this
module was written for it.

## Installation

In `<game dir>`, with RTX Remix installed (its bridge client `d3d9.dll` and `.trex` folder):

1. Rename the bridge client `d3d9.dll` to `d3d9_remix.dll`.
2. Copy `build\d3d9.dll` from this module into `<game dir>`.
3. Add this line to `<game dir>\.trex\bridge.conf`:

   ```
   exposeRemixApi = True
   ```

The renderer (`Video_D3D.dll`) loads the `d3d9.dll` beside the game executable, so it picks up
the proxy. The proxy loads `d3d9_remix.dll` from its own folder. It logs to
`<game dir>\RemixProxy.log`, which is recreated at each start.

## How it works

### Hooks

The proxy implements one entry point, `Direct3DCreate9` (`d3d9.def`). Every other export of the
bridge client is forwarded to `d3d9_remix.dll` by linker directives in `src/proxy.cpp`.

The device is not wrapped. The proxy patches `CreateDevice` in the `IDirect3D9` vtable and five
device slots: `SetTransform` (keeps the camera's view and projection, not the 2D layer's),
`BeginScene` (reads the light pool and the latest zones), `EndScene` (hands overlay changes to
their modules, draws the overlay and markers), and `Present` and `Reset` (where the picture is, to
map the mouse). Every other call goes straight to the bridge.

Once the device exists, the proxy subclasses the game window for the overlay and connects to the
Remix API through the bridge's `remixapi_InitializeLibrary`. The bridge accepts the API only once
its server runs, which is when a device exists. The proxy then registers a Remix `BeginScene`
callback (`remixapi_RegisterCallbacks`). In that callback it destroys last frame's Remix lights
and creates this frame's. Each light carries a stable hash, so Remix sees the same light across
frames: a particle light is named by its pool slot and definition, a zone by its position and
size, and the player and test lights by a fixed name each.

### Finding the game's data

All game addresses in the source are those of the US build of `SouthPark.exe`. Every access goes
through `GameBuild::Place()` in `../common/game_build.h`, which returns the address unchanged on
the US build and finds it by the code signatures in `../common/game_build_signatures.h` on other
builds.

Before reading or patching anything, the proxy compares the game's code at the relevant addresses
with the bytes it expects. Each feature has its own check; a failed check turns off only that
feature.

| Feature | Code checked | If it does not match |
|---|---|---|
| Particle lights | light upload `0x426970`, definition lookup `0x43f470` | no particle lights |
| Zones, player light colour | zone search `0x441550`, object light `0x441d90`, the call at `0x43d5db` | no zones, no player light |
| Player light "in play" test | in-level `0x443650`, paused `0x443660`, cutscene `0x49bf00` | the player is assumed to be playing |

### Particle lights

The engine attaches point lights to some particle effects. The original PC renderers never drew
them. They live in a pool of 10 nodes at `0x524208` (`+4` is the head of the active list; nodes
start at `+0xc`, `0x18` bytes each). A node holds its next link, a pointer to its definition, a
float intensity (the particle's fade), a position as three 16-bit coordinates, and an enabled
flag. The definition table hangs off the world object at `0x522a68 + 0x84`: per record a 16-bit
reach, a 16-bit falloff and four colour bytes. Each frame the proxy walks the active list, keeps
enabled nodes, and stops at any link that leaves the pool.

### Level light zones

Level designers placed light zones: spheres, boxes and cones with a colour, an ambient share and
an optional flicker. The engine never draws them as lights. It blends the zones around each lit
object into one directional light and an ambient term for that object. Under Remix a room with
no lights is dark, so the proxy sends the zones as lights.

Zones come with the grid sections the engine streams in around the camera. The world tick
(`0x43d490`) registers those sections and releases them before the frame is drawn, so the
registry is empty whenever the renderer runs. The proxy redirects one call, the release at
`0x43d5db`, to its own function. That function copies the zones, samples the engine's object
light at the camera (for the player light), and then calls the original release. Nothing else in
the executable changes.

The registry is at world `+0x1278` (count) and `+0x127c` (8 bytes per section). Zone records are
`0xc4` bytes; `src/zones.cpp` documents their layout. Neighbouring sections repeat zones that
cross between them; each zone is kept once. Nothing is read while the game's own zone-lighting
switch (`0x4d74c8`) is off.

## The light model

Every light is a Remix sphere light of radius `EmitterRadius`, with this brightness:

```
radiance = colour x intensity x scale x (reach / 1024)^2
```

The `(reach / 1024)^2` term applies only while `ScaleByReach` is on. It makes the game's
long-reach lights carry further.

| Light | Colour | Intensity | Scale | Reach |
|---|---|---|---|---|
| Particle light | definition colour / 255 | the particle's fade | `RadianceScale` | definition reach |
| Sphere zone | zone colour / 255, with this frame's flicker | 1 | `ZoneRadianceScale` | zone reach, at most `ZoneReachCap` |
| Cone zone | as a sphere zone, shaped to a 45-degree cone along the zone's axis | 1 | `ZoneRadianceScale` | as a sphere zone |
| Player light | engine ambient + directional at the camera, / 255 | its fade, 0 to 1 | `PlayerLightScale` | `PlayerLightReach` |
| Test light | overlay sliders | overlay slider | `RadianceScale` | overlay slider |

- **Zone reach.** A zone is often an ellipsoid. Its reach is the radius of the sphere with the
  same volume. The cap keeps level-wide fills, thousands of units across, from becoming suns.
- **Zone colour.** Floats on a 0 to 255 scale; bright fills go above 255. The ambient share is
  not a brightness: an object inside a zone receives the whole colour either way.
- **Flicker.** The engine's own: a step through the wave table at `0x4d86f4`, driven by the frame
  counter at `0x53edd0` and the zone's phase.
- **Not sent.** Box zones (a directional light confined to a box, which no Remix light is) and
  subtractive zones (Remix has no light that removes light).

### Fallback light

The game gives Remix no other lights, so every scene is lit by Remix's fallback light. By default
Remix turns that light off as soon as any other light exists, so the first game light would
darken the scene. With `KeepFallbackLight` on, the proxy sets `rtx.fallbackLightMode = 2` when
the API connects; with it off, `1`.

### Zones in the open

With `ZonesInTheOpen = 0` (the default), only zones with level geometry above them are sent.
Outdoors the sun and the sky light the scene. The zones over open ground were fills for the
original's vertex lighting; as lights they would add a second, coloured sun.

The renderer answers through its export `SouthPark_CoverAbove(points, count)`: how many of the
points have level geometry above them, or -1 if it holds no geometry there yet.

- Five points are asked: the zone's centre and four points a quarter of its reach away (16 to
  128 units). Three or more covered make the zone covered, and it stays covered.
- An open zone is asked again every 120 frames, in case its roof had not been drawn yet. At most
  four zones are asked per frame; a zone without an answer is not sent yet, so open fills do not
  flash on while a level loads.
- If the renderer does not export `SouthPark_CoverAbove`, every zone is sent.

### Player light

`PlayerLight = 1` adds one sphere light `PlayerLightHeight` units above the camera. Its colour is
the light the engine would give an object standing there: inside the world tick, while the zones
are registered, the proxy calls the engine's object-light function (`0x441d90`) at the camera and
adds its ambient and directional colours. Where no zone reaches, that is the engine's default
light. The light travels with the player, so it is off by default.

With `PlayerLightOnlyInTheDark = 1` (the default), it shines only while all three hold:

1. **Playing.** In a level, not paused, no cutscene camera running. Leaving play turns it off at
   once.
2. **Under a roof.** `SouthPark_CoverAbove` is asked about the camera and four points 64 units
   around it, every 250 ms. A roof over 4 of the 5 makes the place enclosed; it stays enclosed
   until 2 or fewer are covered. Without the renderer's answer, a roof is assumed.
3. **Away from other lights.** The camera is outside the reach of every light sent this frame
   (particle lights by their definition's reach, zones by their capped reach, the test light by
   its reach). Once inside one, the camera counts as near until it is 1.25 times that reach away.

The light fades in and out over one second, so doorways and the edge of a lamp's reach do not
make it flicker. With `PlayerLightOnlyInTheDark = 0` it is always on. The log records each change.

## The F4 overlay

**F4** opens and closes the overlay; **Esc** also closes it. Drag it by its title bar. It is never
taller than the picture; the rest scrolls with the mouse wheel or the bar on its right edge.

While it is open, the overlay takes the mouse and the keyboard. System keys still pass, so
Alt+Enter and the Remix menu's Alt+X keep working. It posts the registered window message
`UWM_SOUTHPARK_OVERLAY_ACTIVE` (wParam 1 on open, 0 on close); the input shim (`dinput.dll`)
watches for it and stops the game reading its devices.

Sections, top to bottom:

- **Save / Reset** buttons, then **Game**: the game settings below and the current field of view
  in degrees.
- **Status:** light pool found, Remix API connected, live and peak light counts, camera position.
- **Remix lights** (particle light settings, fallback light) and **Active game lights** (each live
  light's slot, definition, intensity, position and distance).
- **Level light zones:** the zone settings and the 24 nearest zones, with what is above each
  (`roof`, `open`, `?`). Zones not sent are dimmed.
- **Player light:** its settings, its fade, the state of its three tests, and the engine's light
  at the camera.
- **Test light:** a light placed in front of the camera, with distance, intensity, reach and
  colour sliders, to check the Remix side without waiting for the game to spawn a light.
- **Light definitions:** the game's definition table, with colours.

With **Markers** on, each light is ringed and labelled on screen, also with the panel closed.

### Save and reset

- Every change on the panel applies at once. Nothing is written until you press Save.
- **Save to Video_D3D9.ini** writes every panel value to `<game dir>\Video_D3D9.ini`, the
  renderer's settings file. It rewrites only keys whose text differs, so comments and other keys
  stay, and reports how many keys changed.
- **Reset to defaults** puts every value back to its default and applies it at once. It is not
  written until you press Save.
- The test light is neither saved nor reset.

The proxy reads the ini once, at start; hand edits to `[Lights]` take effect at the next start.
The renderer and the input shim re-read the file when it changes, so Save and hand edits reach
their settings while the game runs.

### Light settings (`[Lights]`)

| Key | Default | Panel range | Meaning |
|---|---|---|---|
| `Enabled` | 1 | on / off | Send particle lights to Remix. |
| `RadianceScale` | 1500 | 10 - 100000 | Brightness of particle lights and the test light. |
| `EmitterRadius` | 6 | 0.5 - 64 | Radius of every Remix sphere light, in world units. |
| `ScaleByReach` | 1 | on / off | Multiply brightness by (reach / 1024)^2. |
| `SwapRedBlue` | 0 | on / off | Swap red and blue in particle light colours. |
| `Markers` | 0 | on / off | Ring and label each light on screen. |
| `KeepFallbackLight` | 1 | on / off | `rtx.fallbackLightMode` 2 (on) or 1 (off). |
| `ZoneLights` | 1 | on / off | Send sphere and cone zones to Remix. |
| `ZoneRadianceScale` | 1500 | 10 - 100000 | Brightness of zone lights. |
| `ZoneReachCap` | 2048 | 256 - 16384 | Largest zone reach counted for brightness, in world units. |
| `ZonesInTheOpen` | 0 | on / off | Also send zones with no level geometry above them. |
| `PlayerLight` | 0 | on / off | Light above the camera in the engine's light colour. |
| `PlayerLightOnlyInTheDark` | 1 | on / off | Only in play, under a roof, out of every sent light's reach. |
| `PlayerLightScale` | 1500 | 10 - 100000 | Brightness of the player light. |
| `PlayerLightReach` | 512 | 50 - 4096 | Reach used for the player light's brightness, in world units. |
| `PlayerLightHeight` | 40 | 0 - 200 | Height of the player light above the camera, in world units. |

### Game settings

These belong to the renderer (`Video_D3D.dll`) or the input shim (`dinput.dll`). The overlay hands
each change to its module through that module's `SouthPark_SetSetting(section, key, value)`
export. The panel ignores an ini value outside the range and starts from the default.

| Panel control | Section / key | Default | Range | Module |
|---|---|---|---|---|
| Pad turn sensitivity | `[Input] PadTurnSensitivity` | 0.6 | 0.1 - 1.0 | input shim |
| Pad look sensitivity | `[Input] PadLookSensitivity` | 1.0 | 0.3 - 1.0 | input shim |
| Field of view (multiplier) | `[Display] FieldOfView` | 1.0 | 0.7 - 1.6 | renderer |
| Culling radius: the level is drawn this far around the camera, behind it too | `[FullMap] KeepAroundCamera` | 2048 | 0 - 16384 | renderer |
| Draw distance, % of the level's own | `[FullMap] DrawDistancePercent` | 100 | 100 - 200 (ini: up to 400) | renderer |
| HUD at the sides of a wide picture | `[Display] HudAtSides` | 1 | on / off | renderer |
| Starry sky | `[Sky] StarrySky` | 1 | on / off | renderer |

A draw distance above 200 set in the ini shows as a full bar and is kept by Save until the slider
is moved.

### Drawing

The overlay is its own small immediate-mode UI: pre-transformed triangles with one texture, a
Consolas font atlas rendered once through GDI (15 pixels at 1080 lines, scaled with the picture).
A state block saves and restores the device state around the draw. Remix shows it as UI when
`rtx.conf` sets `rtx.preTransformedVerticesIsUI = True`.

## Remix API version

`remix_c.h` is API version 0.6.11. The proxy asks the bridge for exactly that version and does no
version check of its own; the bridge decides. For API major version 0, Remix accepts only a
client with its own minor version, so the runtime must offer API 0.6.x. If initialisation fails,
the proxy logs the error code, the overlay shows the API as unavailable, and the game runs on
without the proxy's lights.

## Building

Requires Visual Studio 2022 with the "Desktop development with C++" workload. The game is a
32-bit process, so the DLL is 32-bit. Run:

```
build.bat
```

It finds Visual Studio with `vswhere` and writes `build\d3d9.dll`. It needs `../common` (for
`game_build.h`) beside this folder.
