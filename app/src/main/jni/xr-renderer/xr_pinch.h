// Pinch source selection and detection, the triple-pinch hand lock, and gaze
// drags carried by the pinching hand. No OpenXR calls.

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

// Hand lock toggle: three pinches on one hand within 0.9 s. The third press
// is swallowed until released.
#define TRIPLE_PINCH_WINDOW_NS 900000000L

typedef struct {
    // Last two press times, oldest first, and how many are counted
    int64_t pressNs[2];
    int presses;
    int wasDown;
    // Lock press, swallowed until released
    int holding;
} TriplePinch;

void triplePinchReset(TriplePinch* t);

// Returns 1 on the frame the third press completes the gesture
int triplePinchStep(TriplePinch* t, int down, int64_t nowNs);

// Whether the current press is the swallowed lock press
int triplePinchHeld(const TriplePinch* t);

// Gaze drags follow the pinching hand. Gain ramps in over 0.5 s (then 1.25x
// to catch up) and freezes above 12 deg/s head turn, where hand tracking drifts.
#define GAZE_DRAG_RAMP_NS 500000000L
#define GAZE_DRAG_RAMP_GAIN 1.25f
#define HEAD_TURN_HOLD_DEG_S 12.0f
// Hand travel dead zone for gaze-picked sliders; gearing amplifies pinch shake
#define GAZE_DRAG_DEAD_M 0.015f

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
// Subtracts a radial dead zone from the travel
Vec3 dragDeadZone(Vec3 d, float dead);

// Head turn rate in deg/s from two orientations dt seconds apart
float turnRateDegS(XrQuaternionf now, XrQuaternionf was, float dt);

#endif
