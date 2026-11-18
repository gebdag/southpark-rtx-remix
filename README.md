# South Park RTX Remix

Path-traced South Park (Acclaim, 1999, PC) with NVIDIA RTX Remix. Requires a retail copy of the game. (US version tested, others may work too)

https://github.com/user-attachments/assets/e34a65f4-a6bd-4294-80d2-a7d854a04542

Replaces the games D3D renderer with an RTX Remix compatible version:

- **`Video_D3D9`** builds `Video_D3D.dll`, a Direct3D 9 renderer for the game's display lists
  that hands Remix object-space geometry with separate model, view and projection matrices. It
  also patches the engine so that level geometry behind the camera and off screen is drawn, and
  adds widescreen, field-of-view and draw-distance settings, and textures, normals, sky and
  materials for Remix.
- **`RemixProxy`** builds `d3d9.dll`, which sits in front of the Remix bridge and sends the
  game's own lights to Remix: particle lights, the levels' light zones and an optional light for
  dark enclosed places. **F4** opens an overlay with the settings.
- **`DInputShim`** builds `dinput.dll`, which lets the game share mouse and keyboard with the
  Remix menu and the desktop, adds gamepad support, and plays the game's music from WAV files.

## Install

1. Install the game.
2. Download [RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix/releases/),
   file `rtx-remix-for-x86-games-release-remix-1.6.1.zip`. From it, copy the `.trex` folder into
   the game folder, and copy `d3d9.dll` into the game folder renamed to `d3d9_remix.dll`.
3. Keep a copy of the game's own `Video_D3D.dll`, then copy the contents of this mod's release
   into the game folder: `Video_D3D.dll`, `d3d9.dll`, `dinput.dll`, `rtx.conf`,
   `.trex\bridge.conf` and `rtx-remix\mods\SouthParkMaterials`.
4. Start the game and pick **Direct3D9 Renderer (D3D)** in the video driver list.

Music: put the disc's audio tracks 2-14 as WAV files named `track02.wav` … `track14.wav` in a
`music` folder in the game directory.

Settings are in `Video_D3D9.ini`, which the renderer writes on first start, and in the F4
overlay.

## Build

Visual Studio 2022 with the C++ desktop workload. Run `Video_D3D9\build.bat`,
`RemixProxy\build.bat` and `DInputShim\build.bat`; each writes a 32-bit DLL to its `build\`
folder. `config\rtx.conf` and `config\bridge.conf` are the Remix settings the release ships.

## Credits

The Remix API header (`RemixProxy/include/remix/remix_c.h`) is NVIDIA's, under the MIT license
(`THIRD_PARTY_NOTICES.md`).

## License

MIT (`LICENSE`).
