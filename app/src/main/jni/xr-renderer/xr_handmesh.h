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
// Share of the runtime's hand size estimate taken each frame (about two seconds to settle)
#define HAND_SCALE_FOLLOW 0.02f

// Tracking drops for a few frames at the edge of view: the outline holds its last
// good pose that long, then fades, and fades in when the hand is back
#define HAND_FADE_IN_SEC 0.12f
#define HAND_HOLD_SEC 0.25f
#define HAND_FADE_OUT_SEC 0.2f

// The outline's level 0..1 after dt seconds, the hand tracked now or lost for lostFor
float handFadeStep(float level, int tracked, float lostFor, float dt);

// Column-major matrix of a pose
void matFromPose(float* m, XrPosef pose);

// Skinning matrix of a joint: from its bind pose in the mesh, scaled to the hand's
// size, to where it is now
void handSkinMatrix(float* out, XrPosef joint, XrPosef bind, float scale);

#endif
