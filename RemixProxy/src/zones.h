#pragma once

#include <vector>

// South Park's level light zones: the lights a level's designers placed.
//
// A zone is a volume -- sphere, box or cone -- with a colour. The engine never draws them as
// lights: each frame it blends the zones around every lit object (characters, props) into one
// directional light and an ambient term for that object (FUN_00441d90), which is what tints a
// character in the colour of the lamps beside it. Read here so they can be given to RTX Remix
// as lights.
//
// Zones come with the grid sections the engine streams in around the camera. It registers
// those sections at the start of its world tick and releases them at the end, before the frame
// is drawn, so they are copied on the game's own thread just before the release.
namespace Zones {

enum Shape { Sphere = 0, Box = 1, Cone = 2 };

struct Zone {
    int shape = Sphere;
    bool subtractive = false;  // takes light away; Remix has no such light
    bool flickers = false;
    float colour[3] = {};      // as stored (0..255, brighter fills go above), with this frame's flicker
    // How much of the colour the engine gives an object as ambient; the rest is directional.
    // Not a brightness: an object inside the zone receives the whole colour either way.
    float ambientShare = 0.0f;
    float centre[3] = {};      // world space
    float radius = 0.0f;       // largest semi-axis, world units
    float axis[3] = {};        // world direction: a cone's axis, a box's light direction
};

// Checks this is the expected game build and hooks the release of the zone sets. False leaves
// Latest() empty.
bool Install();

// The zones registered in the engine's last world tick, each once: neighbouring sections
// repeat zones that cross between them.
std::vector<Zone> Latest();

} // namespace Zones
