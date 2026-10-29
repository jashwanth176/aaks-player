// Head aim: with the screen head-locked, head turns produce relative mouse
// deltas, and the controller pointer adds deltas from how far its ray point
// moves. No OpenXR calls.

#ifndef XR_HEADAIM_H
#define XR_HEADAIM_H

#include <stdint.h>

#include "xr_math.h"
#include "xr_shared.h"

// Beyond this pitch yaw is unstable (a small nod near vertical swings it), so
// only pitch is sent
#define HEAD_AIM_PITCH_LIMIT_DEG 80.0f
// Faster than this (deg/s) is a tracking jump; the frame is dropped
#define HEAD_AIM_JUMP_DEG_S 1500.0f
// Frames further apart than this (s) are not measured across
#define HEAD_AIM_GAP_SEC 0.5f

// Per-frame result, for logging. SENT includes sub-pixel moves carried forward.
#define HEAD_AIM_SENT      0
// Below the dead zone
#define HEAD_AIM_STILL     1
// First frame after a gap
#define HEAD_AIM_SEEDED    2
// Switched off, screen not head-locked, or a room shown
#define HEAD_AIM_OFF       3
// Panel open, session unfocused or splash shown
#define HEAD_AIM_PAUSED    4
// Head orientation not tracked
#define HEAD_AIM_LOST      5
// Reference space recentred
#define HEAD_AIM_RECENTRED 6
// Tracking jump
#define HEAD_AIM_JUMPED    7

// Switch state: the saved setting, inverted by each toggle
int headAimSwitchOn(int settingOn, int flipped);

// The flip value that makes the switch read wantOn
int headAimFlipFor(int settingOn, int wantOn);

// Sensitivity (pixels per degree) and dead zone (deg/s) in integer units,
// clamped to the ranges in xr_shared.h
int headAimSensitivityClamp(int units);
int headAimDeadZoneClamp(int units);

// Last measured head angles and the fractional pixels not yet sent
typedef struct {
    int seeded;
    float yaw;
    float pitch;
    int64_t timeNs;
    float carryX;
    float carryY;
} HeadAim;

void headAimReset(HeadAim* a);

// Wraps to (-pi, pi]
float headAimWrap(float radians);

// Yaw about the space's up (positive left, (-pi, pi]) and pitch (positive up),
// in radians. Independent of roll.
void headAimAngles(XrQuaternionf orientation, float* yaw, float* pitch);

// First reason the frame is not measured, in order: inactive, paused, not
// tracked, recentred. HEAD_AIM_SENT if none.
int headAimBlocked(int active, int paused, int tracked, int recentred);

// Advances one frame and returns a HEAD_AIM_ value. A blocked frame or a gap
// resets the state. deadZone (deg/s) is subtracted from the turn rate so
// motion eases in. Outputs pixel deltas, right and down positive.
int headAimStep(HeadAim* a, int blocked, float yaw, float pitch, int64_t timeNs,
                float sensitivity, float deadZone, int* outDx, int* outDy);

// Controller pointer state while head aim is on
typedef struct {
    int seeded;
    int hand;
    long frame;
    float u;
    float v;
    float carryX;
    float carryY;
} PointerNudge;

void pointerNudgeReset(PointerNudge* n);

// Pixel delta of the ray point since the previous frame; a gap or hand change
// only re-seeds, so the cursor never jumps
void pointerNudge(PointerNudge* n, int hand, float u, float v, long frame, int width, int height,
                  int* outDx, int* outDy);

#endif
