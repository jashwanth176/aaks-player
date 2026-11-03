// Pinch source selection and detection, and gaze drags carried by the pinching hand.
// No OpenXR calls.

#ifndef XR_PINCH_H
#define XR_PINCH_H

#include <stdint.h>

#include "xr_math.h"

// Pinch sources, in order of preference: the hand profile's pinch value, the
// XR_FB_hand_tracking_aim pinching flag, then thumb-index tip distance.
#define PINCH_SRC_NONE 0
#define PINCH_SRC_VALUE 1
#define PINCH_SRC_AIM 2
#define PINCH_SRC_JOINTS 3

// Press/release thresholds: value 0.65/0.35 (same as the trigger), tip
// distance 20/32 mm
#define PINCH_VALUE_ON 0.65f
#define PINCH_VALUE_OFF 0.35f
#define PINCH_ON_M 0.020f
#define PINCH_OFF_M 0.032f
// EXT hand profile thresholds. Pico 4 Ultra reads 1.0 pinched and only drops
// to 0.47-0.7 when open, which the pair above would never release.
#define PINCH_EXT_VALUE_ON 0.9f
#define PINCH_EXT_VALUE_OFF 0.7f

// Picks a PINCH_SRC_ from what is available on the hand
int pinchSource(int valueBound, int aimOffered, int jointsOffered);

// Value thresholds; ext selects the EXT hand profile over the Microsoft one
void pinchValuePair(int ext, float* outOn, float* outOff);

// Pinch state this frame from the chosen source. wasDown is the previous
// result.
int pinchStep(int source, int ext, int wasDown, float value, int aimPinching, int tipsValid,
              float gap);

// Analog press with hysteresis
int pressHysteresis(float value, int wasDown, float on, float off);

// Gaze drags follow the pinching hand. Gain ramps in over 0.5 s (then 1.25x
// to catch up) and freezes above 12 deg/s head turn, where hand tracking drifts.
#define GAZE_DRAG_RAMP_NS 500000000L
#define GAZE_DRAG_RAMP_GAIN 1.25f
#define HEAD_TURN_HOLD_DEG_S 12.0f

typedef struct {
    Vec3 prev;
    Vec3 carried;
    int64_t startNs;
} DragRamp;

// Ramp gain, 0 at the pinch to 1 after GAZE_DRAG_RAMP_NS, smoothstepped
float dragRampGain(int64_t elapsedNs);
void dragRampStart(DragRamp* r, int64_t nowNs);
// Accumulated drag for hand travel d since the pinch. outHeld is set when a
// head turn froze this frame's step.
Vec3 dragRampStep(DragRamp* r, Vec3 d, float headTurnDegS, int64_t nowNs, int* outHeld);

// Drag gearing: target distance over hand distance from the head, 0.5 to 64
float dragScale(Vec3 head, Vec3 target, Vec3 hand);

// Head turn rate in deg/s from two orientations dt seconds apart
float turnRateDegS(XrQuaternionf now, XrQuaternionf was, float dt);

#endif
