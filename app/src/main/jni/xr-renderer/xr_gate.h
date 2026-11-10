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

// What a source offers the pointer this frame
typedef struct {
    // Its ray is on the picture, a handle or a panel
    int on;
    // Its ray is only in the margin around the picture
    int halo;
    // Its trigger or pinch is down, and went down while its ray was on something
    int pressing;
    // That press began this frame
    int fresh;
    // Its fingertip is near a surface (1) or pressing it (2)
    int touch;
} PointerOffer;

// The source that points, -1 for none. A press beginning on something takes it; the one
// already holding a press keeps it; then a fingertip pressing, a press on something, a
// fingertip about to touch, and otherwise the first in order with its ray on something,
// the margin last.
int pickPointer(const PointerOffer* offers, const int* order, int count, int prev);

// A raised hand that keeps vanishing for a moment and coming back is hard for the cameras to
// see, usually for want of light. HAND_BLIPS such losses, each under HAND_BLIP_NS, within
// HAND_BLIP_WINDOW_NS count as that. A hand lowered out of view stays gone longer.
#define HAND_BLIPS 5
#define HAND_BLIP_NS 600000000LL
#define HAND_BLIP_WINDOW_NS 10000000000LL

typedef struct {
    int seen;
    int64_t lostAt;
    int64_t blips[HAND_BLIPS];
    int count;
} HandBlips;

// One frame of a hand: whether it is seen now. 1 on the frame the losses add up.
int handBlipStep(HandBlips* b, int seen, int64_t nowNs);

// Hand tracking this good counts as a deliberate hand
#define PINCH_WAKE_TRUST 0.7f

// Whether a hand's pinch wakes its sleeping pointer: a new pinch from a well tracked
// hand aimed at the picture, a handle or a panel. Hands resting in the lap pinch too,
// but aim at the floor.
int pinchWakes(int fresh, int aimed, float trust);

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
