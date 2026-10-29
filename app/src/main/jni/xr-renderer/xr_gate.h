// Pointer source rules: what is bound to each hand, per-controller wake/sleep,
// and handing pointing to the hands when eye tracking is lost. No OpenXR calls.

#ifndef XR_GATE_H
#define XR_GATE_H

#include <stdint.h>

#include "xr_math.h"

// What the runtime has bound to a hand's path. UNKNOWN is the initial value,
// so the first reading of a session is logged as a change.
#define PROFILE_UNKNOWN    0
#define PROFILE_NONE       1
#define PROFILE_HANDS      2
#define PROFILE_CONTROLLER 3

// XrSpaceLocationFlags bits, same values as OpenXR
#define GATE_ORIENTATION_VALID   0x1
#define GATE_POSITION_VALID      0x2
#define GATE_ORIENTATION_TRACKED 0x4
#define GATE_POSITION_TRACKED    0x8

// Nothing, hands, or (any other profile) a controller. An empty profile means
// no controller: runtimes report one when a hand is dropped.
int profileKind(int bound, int handProfile);

// Gaze pose is valid and orientation tracked. Eye trackers can keep reporting
// a valid but frozen pose after losing the eyes.
int gazeUsable(unsigned flags);

// Aim tracked in both position and orientation. A controller at rest keeps a
// gyro-driven orientation, so orientation alone does not count.
int aimFullyTracked(unsigned flags);

// A controller profile, aim fully tracked, and awake on its own rest clock
int controllerInUse(int kind, int tracked, int awake);

// Per-controller wake/sleep state (the shared pointer clock cannot retire one)
typedef struct {
    float movingFor;
    float stillFor;
    int awake;
    // Trigger press that woke it; swallowed until released, since the ray
    // was hidden when it was pressed
    int wakeTrigger;
} ControllerClock;

#define CLOCK_SAME      0
#define CLOCK_PICKED_UP 1
#define CLOCK_PRESSED   2
#define CLOCK_PUT_DOWN  3

void controllerClockReset(ControllerClock* c);

// Advances one frame and returns a CLOCK_ event. holding keeps it awake
// through a drag; with pressWakes only a press wakes it. swallow is set while
// triggerDown is the waking press.
int controllerClockStep(ControllerClock* c, float dt, int moved, int pressed, int holding,
                        int triggerDown, int pressWakes, int sleepOn, float wakeSec,
                        float sleepSec, int* swallow);

// Seconds without usable gaze before the hands take over pointing
#define GAZE_BRIDGE_SEC 10.0f

// Hands pointing in place of lost eye tracking
typedef struct {
    // When gaze was first requested and unusable; 0 while usable or not requested
    int64_t missingSince;
    // Last requested gaze was usable
    int back;
    int bridged;
    int64_t bridgedSince;
} GazeBridge;

#define BRIDGE_SAME 0
#define BRIDGE_ON   1
#define BRIDGE_OFF  2

// Once per frame. Only switches while no press is held, so a press ends on
// the source that started it.
int gazeBridgeUpdate(GazeBridge* b, int gazeOn, int controllerAwake, int focused,
                     int pressHeld, int64_t nowNs, float bridgeSec, float* outSec);

// Records this frame's gaze locate. Not requested does not count as missing.
void gazeBridgeTrack(GazeBridge* b, int asked, int usable, int64_t nowNs);

#endif
