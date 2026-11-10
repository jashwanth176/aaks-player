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

// How far the mesh reaches from each joint's bind position, over the vertices the joint
// moves. `vertices` is the uploaded layout.
void handJointReach(float* reach, int jointCount, const float* vertices, int vertexCount,
                    const XrPosef* bind);

// The hands are drawn only where they are: the part of an eye's view that holds them,
// as the tangents of its edges (x right, y up), and the image it is drawn into
typedef struct {
    float left, right, down, up;
    int width, height;
    // Points added and their summed distance along the view axis
    int count;
    float depthSum;
} ViewBox;

// Skinning blends between joints, so the mesh can sit a little outside their reach
#define VIEW_BOX_SLACK_M 0.012f
// Nearer than this a point's tangents run away; the fit clips them to the view
#define VIEW_BOX_NEAR_M 0.03f
// Edges move in steps of this many pixels, so the image's pixels stay put in the view
// while the hands move
#define VIEW_BOX_SNAP_PX 32

void viewBoxClear(ViewBox* box);

// Grows the box to hold a sphere, seen from the eye
void viewBoxAdd(ViewBox* box, XrPosef eye, XrVector3f centre, float radius);

// Clips the box to the eye's view and sizes its image at the eye's own pixel density,
// less once a side would pass maxSide. 0 when nothing of it is in view.
int viewBoxFit(ViewBox* box, XrFovf fov, int eyeWidth, int eyeHeight, int maxSide);

// The box as a field of view, to draw it with
XrFovf viewBoxFov(const ViewBox* box);

// A quad that covers the box for an eye at this pose, at the points' mean distance
XrPosef viewBoxQuad(const ViewBox* box, XrPosef eye, float* outWidth, float* outHeight);

#endif
