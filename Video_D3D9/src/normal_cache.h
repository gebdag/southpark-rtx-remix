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

// Records a face (corners in world space, in its winding).
void AddFace(const float* a, const float* b, const float* c);

// The smoothed normal at corner `corner` (0..2) of the recorded face a, b, c.
//
// An edge is sharp when the faces on either side of it meet at more than the crease angle
// (`creaseCos` is its cosine). The corner averages the faces it reaches around its point
// without crossing a sharp edge, each weighted by its angle at the point. Both faces of a
// smooth edge reach each other, so they agree on the normal there: no seam along it, however
// differently the faces around them lean. Weighting by angle rather than area keeps one long
// triangle from pulling the normal of every corner it touches.
//
// A face not in the cache, or a result further than the crease angle from the face, gives the
// face's own normal.
void CornerNormal(const float* a, const float* b, const float* c, int corner, float creaseCos,
                  float* out);

} // namespace NormalCache
