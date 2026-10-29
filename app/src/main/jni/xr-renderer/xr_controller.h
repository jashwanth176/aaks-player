// Controller ray visibility, pointer dot size and controller model placement.
// No GL or OpenXR calls.

#ifndef XR_CONTROLLER_H
#define XR_CONTROLLER_H

#include "xr_math.h"

// Ray switch for the session: the saved setting, inverted by each toggle
int raySwitchOn(int settingOn, int flipped);

// The flip value that makes the switch read wantOn
int rayFlipFor(int settingOn, int wantOn);

// Whether the beam is drawn this frame. While a panel is open it is always
// drawn so the panel can be aimed at. The dot and hit test do not depend on it.
int rayDrawn(int settingOn, int flipped, int panelOpen);

// The dot keeps a constant angular size: PTR_DOT_SIZE_M across at
// PTR_DOT_REF_M, clamped to [PTR_DOT_MIN_M, PTR_DOT_MAX_M].
#define PTR_DOT_SIZE_M 0.022f
#define PTR_DOT_REF_M 3.0f
#define PTR_DOT_MIN_M 0.005f
#define PTR_DOT_MAX_M 0.15f

// Dot width in metres at the given distance from the head
float pointerDotSize(float distance);

// Whether a hand's controller model is drawn: setting on, not in passthrough,
// a controller profile, grip pose active, and a valid position with a tracked
// orientation. kind is a PROFILE_ value and flags the GATE_ bits (xr_gate.h).
int controllerModelShown(int settingOn, int passthrough, int kind, int poseActive,
                         unsigned flags);

// Column-major model matrix from the grip pose. The model is a right-hand
// controller in grip space, mirrored across x for the left hand; the 3x3 is a
// rotation (plus mirror), so it can transform normals directly.
void controllerModelMatrix(XrPosef grip, int leftHand, float* out16);

// A model-space point transformed the same way, without the matrix
Vec3 controllerModelPoint(XrPosef grip, int leftHand, Vec3 local);

#endif
