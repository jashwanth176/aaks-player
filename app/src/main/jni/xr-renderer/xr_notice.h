// Timing for panel fades and the startup splash (shown while the depth
// model load). No GL or OpenXR.

#ifndef XR_NOTICE_H
#define XR_NOTICE_H

#include <stdint.h>

// Full fade durations for panels and the splash
#define FADE_NS 150000000LL
#define SPLASH_FADE_NS 300000000LL

// Splash shown for at least FLOOR and at most CEILING; logo wedges open one
// per tick, hold, then restart
#define SPLASH_FLOOR_NS 1500000000LL
#define SPLASH_CEILING_NS 8000000000LL
#define SPLASH_TICK_NS 250000000LL
#define SPLASH_HOLD_NS 500000000LL

// Player panels come in over PANEL_IN_NS, eased out, growing from PANEL_GROW_FROM of
// their size and rising PANEL_RISE of their height; they leave over PANEL_OUT_NS, quicker,
// the same way back
#define PANEL_IN_NS 240000000LL
#define PANEL_OUT_NS 150000000LL
#define PANEL_GROW_FROM 0.94f
#define PANEL_RISE 0.04f

// One frame's step of a panel's level (0 gone, 1 shown) toward shown or gone
float panelStep(float level, int shown, int64_t dtNs);

// The level eased (cubic out): opacity, and how far grown and risen
float panelEase(float level);

// Size factor for a level, PANEL_GROW_FROM to 1
float panelGrow(float level);

// Drop below the resting place for a level, as a share of the panel's height
float panelDrop(float level);

// Splash wait bits
#define SPLASH_WAIT_PANELS 1
#define SPLASH_WAIT_DEPTH 2
#define SPLASH_PARTS 2

#define SPLASH_UP 0
#define SPLASH_FADING 1
#define SPLASH_GONE 2

typedef struct {
    // First frame time, 0 before it
    int64_t firstNs;
    // Lift time and what was still pending then
    int64_t liftNs;
    int waitingAtLift;
    int phase;
} Splash;

// waiting: SPLASH_WAIT_ bits still pending. Returns 1 on the frame it lifts.
// fadeNs 0 cuts it.
int splashStep(Splash* splash, int64_t now, int waiting, int64_t fadeNs);

// Opacity: 1 while up, falling to 0 over the fade
float splashLevel(const Splash* splash, int64_t now, int64_t fadeNs);

// Open logo wedges (also the sprite sheet row), cycling 0..SPLASH_WEDGES while
// up; all of them once lifting, so it fades out whole.
int splashWedges(const Splash* splash, int64_t now);

#endif
