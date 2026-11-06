// Hand outlines: skinning maths for the runtime's hand mesh. No GL or OpenXR calls.

#ifndef XR_HANDMESH_H
#define XR_HANDMESH_H

#include "xr_math.h"

// Floats per vertex in the uploaded mesh: position, normal, four joints, four weights
#define HAND_VERTEX_FLOATS 14
// In passthrough only the rim is drawn over the real hand; in the dark the hand is
// also filled faintly so it can be seen at all
#define HAND_RIM 0.85f
#define HAND_FILL_DARK 0.3f

// Column-major matrix of a pose
void matFromPose(float* m, XrPosef pose);

// Skinning matrix of a joint: from its bind pose in the mesh to where it is now
void handSkinMatrix(float* out, XrPosef joint, XrPosef bind);

#endif
