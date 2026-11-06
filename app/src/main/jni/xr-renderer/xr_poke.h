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
// A hover starts only from in front: further behind than this is a hand reaching past
#define POKE_BEHIND_M 0.02f
// A press holds while the finger goes this far through the surface
#define POKE_THROUGH_M 0.15f
// The touch ray starts this far in front of the surface, too short to draw a beam
#define POKE_RAY_BACK_M 0.06f

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

#endif
