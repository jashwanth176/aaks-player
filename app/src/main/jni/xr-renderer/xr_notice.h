// Timing for panel fades, the startup splash (shown while panels, room and
// depth model load) and the toast queue. No GL or OpenXR.

#ifndef XR_NOTICE_H
#define XR_NOTICE_H

#include <stdint.h>

// Full fade durations for panels and the splash
#define FADE_NS 150000000LL
#define SPLASH_FADE_NS 300000000LL

// fadeStep results
#define FADE_NONE 0
#define FADE_IN_DONE 1
#define FADE_OUT_DONE 2

// Layer opacity plus the current run, so the caller can log its duration
typedef struct {
    float level;
    int64_t lastNs;
    int64_t fromNs;
    int frames;
    int rising;
    int running;
} Fade;

// Moves toward 1 (shown) or 0 over durationNs, or jumps if !smooth. Returns
// FADE_IN_DONE / FADE_OUT_DONE on the frame a fade completes.
int fadeStep(Fade* fade, int shown, int64_t now, int64_t durationNs, int smooth);

// Splash shown for at least FLOOR and at most CEILING; logo wedges open one
// per tick, hold, then restart
#define SPLASH_FLOOR_NS 1500000000LL
#define SPLASH_CEILING_NS 8000000000LL
#define SPLASH_TICK_NS 250000000LL
#define SPLASH_HOLD_NS 500000000LL

// Splash wait bits
#define SPLASH_WAIT_PANELS 1
#define SPLASH_WAIT_ROOM 2
#define SPLASH_WAIT_DEPTH 4

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

// One toast at a time. Same-group notices replace the current one at once;
// others wait NOTICE_MIN_NS. A full queue drops the oldest.
#define NOTICE_SHOW_NS 4000000000LL
#define NOTICE_MIN_NS 1500000000LL
#define NOTICE_QUEUE 4

typedef struct {
    int kind;
    int arg;
} Notice;

typedef struct {
    Notice waiting[NOTICE_QUEUE];
    int count;
    // Current notice and its start; kind -1 when none
    Notice current;
    int64_t sinceNs;
} NoticeBoard;

void noticeInit(NoticeBoard* board);

// Group key: on/off pairs of the same toggle share a group
int noticeGroup(int kind);

// Queues a notice; one in the same group already waiting is replaced in place
void noticePush(NoticeBoard* board, int kind, int arg);

// Shows the next notice when due, unless held. Returns 1 when one is shown.
int noticeAdvance(NoticeBoard* board, int64_t now, int held, Notice* out);

// Whether a notice is currently showing
int noticeShowing(const NoticeBoard* board, int64_t now);

#endif
