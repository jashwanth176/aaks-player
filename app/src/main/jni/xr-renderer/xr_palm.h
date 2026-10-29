// Open-palm gesture that wakes a sleeping hand pointer: fingers open, palm
// toward the eyes, near the centre of view, held briefly. No OpenXR calls.

#ifndef XR_PALM_H
#define XR_PALM_H

#include "xr_math.h"

// Palm facing angle, angle from view centre, and distance range from the head
#define PALM_FACING_DEG 45.0f
#define PALM_CENTRE_DEG 25.0f
#define PALM_NEAR_M 0.15f
#define PALM_FAR_M 0.75f
// A finger is open when its tip is this far from the palm centre. Needs three
// of four open and thumb and index tips apart, to reject fists and pinches.
#define PALM_FINGER_OPEN_M 0.065f
#define PALM_TIPS_APART_M 0.035f
// Hold time before the pointer wakes
#define PALM_HOLD_SEC 0.3f

typedef struct {
    // Palm joint; -Y points out of the palm
    XrPosef palm;
    // Index, middle, ring and little tips
    Vec3 tips[4];
    Vec3 thumbTip;
} PalmHand;

// facing: the runtime already reports the palm facing the eyes (the system
// gesture flag of XR_FB_hand_tracking_aim); otherwise the palm pose is tested.
int palmShown(const PalmHand* hand, XrPosef head, int facing);

typedef struct {
    float heldFor;
    int fired;
} PalmHold;

// Returns 1 once when the hold time is reached; re-arms when the palm drops
int palmHoldStep(PalmHold* hold, int shown, float dt);

#endif
