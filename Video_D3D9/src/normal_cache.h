#pragma once

// Smooth normals for pre-lit world geometry, across draws.
//
// The engine uploads the world a few triangles at a time, so a draw averages under seven
// triangles, and it often gives each triangle its own copies of shared corners. Smoothing
// within a draw therefore leaves most of a slope faceted: a quarter of corner positions are
// split across vertex slots inside a draw, and over a quarter are shared with other draws.
//
// This keeps every pre-lit face drawn recently, keyed by its corners' world positions, so a
// corner is smoothed over all the faces that meet at that point, whichever draw they came in.
// The world is static, so a face drawn last frame is still there this frame; faces not drawn
// for a while are forgotten, which keeps moving pre-lit objects from lingering.
namespace NormalCache {

// Once per frame: ages out faces not drawn recently.
void BeginFrame(unsigned int frame);

// Records a face (corners in world space). Area-weighted normal from its own winding.
void AddFace(const float* a, const float* b, const float* c);

// The smoothed normal at `position` for a face whose unit normal is `own`: the area-weighted
// sum of the recorded faces touching that point within the crease angle (`creaseCos`).
void CornerNormal(const float* position, const float* own, float creaseCos, float* out);

} // namespace NormalCache
