#pragma once

// The display-list interpreter and world-space geometry submission.
//
// The whole point of the port lives here. The original renderer transformed vertices to
// screen space in software and handed D3D pre-projected D3DTLVERTEX. This interpreter stops
// short of that: it keeps the engine's object-space positions untouched and forwards the
// model, view and projection matrices to D3D9 separately, so a path tracer downstream sees
// real world-space geometry instead of something it would have to unproject.
namespace Geometry {

void SetZPlanes(float zNear, float zFar);

// Screen dimensions the engine asked for, used to build the projection.
void SetViewportSize(int width, int height);

void BeginFrame();
void EndFrame();

// Walks one display list. Recurses for the sub-list opcode.
void ProcessDisplayList(const unsigned int* list);

// One-shot dump of which opcodes the first frame's lists actually used.
void LogOpcodeCensus();

// How often a texture was selected and actually bound.
void LogTextureStats();

// Objective coherency metrics for the interpreted geometry.
void LogGeometryAudit();

void ReleaseResources();

} // namespace Geometry
