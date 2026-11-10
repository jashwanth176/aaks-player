// Direct touch: a fingertip near a menu or the picture hovers it, and touching
// the surface presses. No GL or OpenXR calls.

#ifndef XR_POKE_H
#define XR_POKE_H

#include "xr_math.h"
#include "xr_shared.h"

// A fingertip this close in front of a surface hovers it
#define POKE_HOVER_M 0.08f
// The fingertip's skin at the surface presses; it lets go once this far back
#define POKE_PRESS_M 0.0f
#define POKE_RELEASE_M 0.012f
// A hover starts only from clearly in front. A finger behind a surface, or drawing back
// through one that opened in front of it, is not about to press it.
#define POKE_ARM_M 0.015f
// A press holds while the finger goes this far through the surface
#define POKE_THROUGH_M 0.15f
// The touch ray starts this far in front of the surface, too short to draw a beam
#define POKE_RAY_BACK_M 0.06f
// Fingers pinching this close to the white bar take hold of it where they are
#define NEAR_BAR_REACH_M 0.06f

enum { POKE_IDLE = TOUCH_NONE, POKE_HOVER = TOUCH_NEAR, POKE_PRESSED = TOUCH_PRESSING };

typedef struct {
    int state;
    // Surface hovered or held, -1 for none
    int target;
} Poke;

// One frame. target is the surface the fingertip is nearest (-1 none), or while
// pressed the held one (-1 once it is gone); depth is the fingertip skin's distance
// in front of it; over whether the fingertip is within its edges. Returns the state.
int pokeStep(Poke* p, int target, float depth, int over);

// A fingertip in a surface's local frame (x right, y up, z toward the viewer)
// measured against it: x along the surface (arc length when curved), depth in front
// of it, yaw of the surface's normal there, and whether it is within the edges.
typedef struct {
    float x;
    float depth;
    float yaw;
    int over;
} PokeLocal;

void pokeLocal(Vec3 local, float width, float height, float radius, int curved,
               PokeLocal* out);

// What a held pinch holds with. It keeps that until let go, so a drag is not handed
// to a touch or to the bar when the hand passes near one.
enum { HOLD_NONE, HOLD_RAY, HOLD_NEAR };

// One frame of a hand's pinch. pressing: the pinch is this frame's press on something,
// not by touch; near: made with the fingers at the bar.
int pinchHoldStep(int hold, int pinch, int pressing, int near);

// A pinch seen while the fingertip touches is the hand's shape, not a click, and stays
// ignored until let go. Returns whether to ignore it.
int pinchAfterTouch(int ignored, int pinch, int touching);

// The white bar's row in its own frame (x along it from the bar's centre, y up, z
// toward the viewer), with the Back button beside the bar if there is one.
typedef struct {
    float halfW;
    float halfH;
    int hasBack;
    float backX;
    float backHalfW;
} NearBarRow;

enum { NEAR_AWAY, NEAR_BAR, NEAR_BACK };

// Where fingers by the row are: at the bar, which a pinch there takes hold of; at the
// Back button, theirs to press; or away. aimX is where their ray enters the row.
int nearBarZone(Vec3 local, const NearBarRow* row, float* aimX);

// A press counts for the menus only if it began on one; it then lasts until let go,
// wherever it is dragged. Returns whether the press is a menu's.
int uiPressStep(int was, int down, int edge, int onPanel);

#endif
