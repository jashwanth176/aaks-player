// Tests for xr_gamepad: controller-to-pad mapping, dead zones, hold-back, the mode
// toggle, the chord shortcuts and rumble.
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "xr_gamepad.h"

// 72 Hz, the lowest display rate.
#define FRAME_NS 13888889LL

static PadHand idle(void) {
    PadHand h;
    memset(&h, 0, sizeof(h));
    return h;
}

// Grips up, Start allowed, default dead zone.
static PadState mapped(const PadHand* left, const PadHand* right) {
    int grips[2] = { 0, 0 };
    PadState s;
    padMap(left, right, grips, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    return s;
}

static void testTheBits(void) {
    // Bit values match ControllerPacket.
    CHECK(PAD_A == 0x1000 && PAD_B == 0x2000 && PAD_X == 0x4000 && PAD_Y == 0x8000);
    CHECK(PAD_LB == 0x0100 && PAD_RB == 0x0200);
    CHECK(PAD_LS_CLICK == 0x0040 && PAD_RS_CLICK == 0x0080);
    CHECK(PAD_START == 0x0010);
    // Held-analogue flags sit above the 16 button bits.
    CHECK(PAD_BUTTONS == (PAD_A | PAD_B | PAD_X | PAD_Y | PAD_LB | PAD_RB | PAD_LS_CLICK
                          | PAD_RS_CLICK | PAD_START));
    CHECK(PAD_BUTTONS == 0xf3d0);
    int analogue = PAD_HELD_LT | PAD_HELD_RT | PAD_HELD_LS | PAD_HELD_RS;
    CHECK((analogue & PAD_BUTTONS) == 0);
    CHECK((analogue & 0xffff) == 0);
}

static void testTheButtons(void) {
    PadHand left = idle();
    PadHand right = idle();
    CHECK(mapped(&left, &right).buttons == 0);

    right.lower = 1;
    CHECK(mapped(&left, &right).buttons == PAD_A);
    right.lower = 0;
    right.upper = 1;
    CHECK(mapped(&left, &right).buttons == PAD_B);
    right.upper = 0;
    left.lower = 1;
    CHECK(mapped(&left, &right).buttons == PAD_X);
    left.lower = 0;
    left.upper = 1;
    CHECK(mapped(&left, &right).buttons == PAD_Y);
    left.upper = 0;

    left.stickClick = 1;
    CHECK(mapped(&left, &right).buttons == PAD_LS_CLICK);
    left.stickClick = 0;
    right.stickClick = 1;
    CHECK(mapped(&left, &right).buttons == PAD_RS_CLICK);
    right.stickClick = 0;

    // Left menu is Start unless the toggle has taken it.
    left.menu = 1;
    CHECK(mapped(&left, &right).buttons == PAD_START);
    int grips[2] = { 0, 0 };
    PadState s;
    padMap(&left, &right, grips, 0, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == 0);
    // Right menu belongs to the system.
    left.menu = 0;
    right.menu = 1;
    CHECK(mapped(&left, &right).buttons == 0);
    right.menu = 0;

    // Grips are bumpers, via padGripDown.
    grips[0] = 1;
    padMap(&left, &right, grips, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == PAD_LB);
    grips[0] = 0;
    grips[1] = 1;
    padMap(&left, &right, grips, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == PAD_RB);
    // Grip travel alone sets no bits.
    left.grip = 1.0f;
    right.grip = 1.0f;
    grips[1] = 0;
    padMap(&left, &right, grips, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == 0);

    PadHand all = idle();
    all.lower = all.upper = all.stickClick = all.menu = 1;
    int both[2] = { 1, 1 };
    padMap(&all, &all, both, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == PAD_BUTTONS);
}

static void testTheGrips(void) {
    // Hysteresis: down at 0.65, up below 0.36.
    CHECK(!padGripDown(0.0f, 0));
    CHECK(!padGripDown(0.64f, 0));
    CHECK(padGripDown(0.65f, 0));
    CHECK(padGripDown(1.0f, 0));
    CHECK(padGripDown(0.5f, 1));
    CHECK(padGripDown(0.36f, 1));
    CHECK(!padGripDown(0.35f, 1));
    CHECK(!padGripDown(0.0f, 1));
}

static void testTheTriggers(void) {
    // 13% dead zone, as ControllerHandler uses for pads without their own.
    CHECK(padTrigger(0.0f) == 0);
    CHECK(padTrigger(0.13f) == 0);
    CHECK(padTrigger(0.131f) == 33);
    // Not rescaled past the dead zone.
    CHECK(padTrigger(0.5f) == 127);
    CHECK(padTrigger(1.0f) == 255);
    CHECK(padTrigger(1.4f) == 255);
    CHECK(padTrigger(-0.2f) == 0);
    CHECK(padTrigger(NAN) == 0);

    PadHand left = idle();
    PadHand right = idle();
    left.trigger = 0.25f;
    right.trigger = 0.9f;
    PadState s = mapped(&left, &right);
    CHECK(s.leftTrigger == 63);
    CHECK(s.rightTrigger == 229);
    CHECK(s.buttons == 0);
}

static void testTheSticks(void) {
    int x, y;
    // OpenXR y is already up-positive, so no flip (unlike Android's axis).
    padStick(1.0f, 0.0f, 0.07f, &x, &y);
    CHECK(x == PAD_STICK_FULL && y == 0);
    padStick(-1.0f, 0.0f, 0.07f, &x, &y);
    CHECK(x == -PAD_STICK_FULL && y == 0);
    padStick(0.0f, 1.0f, 0.07f, &x, &y);
    CHECK(x == 0 && y == PAD_STICK_FULL);
    padStick(0.0f, -1.0f, 0.07f, &x, &y);
    CHECK(x == 0 && y == -PAD_STICK_FULL);
    CHECK(PAD_STICK_FULL == 0x7ffe);

    // Radial dead zone, edge inclusive.
    padStick(0.05f, 0.0f, 0.07f, &x, &y);
    CHECK(x == 0 && y == 0);
    padStick(0.0f, -0.069f, 0.07f, &x, &y);
    CHECK(x == 0 && y == 0);
    padStick(0.5f, 0.0f, 0.5f, &x, &y);
    CHECK(x == 0 && y == 0);
    padStick(0.049f, 0.049f, 0.07f, &x, &y);
    CHECK(x == 0 && y == 0);
    // |(0.05, 0.05)| = 0.0707 > 0.07.
    padStick(0.05f, 0.05f, 0.07f, &x, &y);
    CHECK(x == 1638 && y == 1638);
    // Not rescaled.
    padStick(0.5f, -0.25f, 0.07f, &x, &y);
    CHECK(x == 16383 && y == -8191);
    padStick(0.5f, 0.0f, padDeadzoneFromPercent(50), &x, &y);
    CHECK(x == 0 && y == 0);
    padStick(0.51f, 0.0f, padDeadzoneFromPercent(50), &x, &y);
    CHECK(x == 16710 && y == 0);

    // Each axis clamped.
    padStick(1.3f, -1.2f, 0.07f, &x, &y);
    CHECK(x == PAD_STICK_FULL && y == -PAD_STICK_FULL);
    padStick(1.0f, 1.0f, 0.07f, &x, &y);
    CHECK(x == PAD_STICK_FULL && y == PAD_STICK_FULL);

    PadHand left = idle();
    PadHand right = idle();
    left.stickX = -1.0f;
    right.stickY = 0.5f;
    PadState s = mapped(&left, &right);
    CHECK(s.leftX == -PAD_STICK_FULL && s.leftY == 0);
    CHECK(s.rightX == 0 && s.rightY == 16383);
    CHECK(s.buttons == 0);
}

static void testTheDeadZoneSetting(void) {
    // Whole percent, minimum 1, as ControllerHandler.
    CHECK_NEAR(padDeadzoneFromPercent(7), 0.07, 1e-6);
    CHECK_NEAR(padDeadzoneFromPercent(0), 0.01, 1e-6);
    CHECK_NEAR(padDeadzoneFromPercent(-5), 0.01, 1e-6);
    CHECK_NEAR(padDeadzoneFromPercent(20), 0.2, 1e-6);
    CHECK_NEAR(padDeadzoneFromPercent(250), 1.0, 1e-6);
    CHECK_NEAR(PAD_STICK_DEADZONE_DEFAULT, 0.07, 1e-6);
}

static void testRestAndSame(void) {
    PadState a, b;
    padRest(&a);
    CHECK(padAtRest(&a));
    CHECK(padHeldIn(&a) == 0);
    b = a;
    CHECK(padSame(&a, &b));
    b.rightY = 1;
    CHECK(!padSame(&a, &b));
    CHECK(!padAtRest(&b));
    CHECK(padHeldIn(&b) == PAD_HELD_RS);
    b.rightY = 0;
    b.leftTrigger = 40;
    CHECK(padHeldIn(&b) == PAD_HELD_LT);
    b.buttons = PAD_A | PAD_LB;
    b.leftX = -9;
    b.rightTrigger = 1;
    CHECK(padHeldIn(&b) == (PAD_A | PAD_LB | PAD_HELD_LT | PAD_HELD_RT | PAD_HELD_LS));
}

static void testTheHoldBack(void) {
    // Trigger and A still held from a panel press.
    PadState s;
    padRest(&s);
    s.rightTrigger = 255;
    s.buttons = PAD_A;
    int held = padHeldIn(&s);
    CHECK(held == (PAD_A | PAD_HELD_RT));

    // Held inputs are masked; new ones pass.
    s.buttons = PAD_A | PAD_B;
    s.leftX = 20000;
    padHoldBack(&held, &s);
    CHECK(s.rightTrigger == 0);
    CHECK(s.buttons == PAD_B);
    CHECK(s.leftX == 20000);
    CHECK(held == (PAD_A | PAD_HELD_RT));

    // Partially released trigger is still held back.
    padRest(&s);
    s.rightTrigger = 120;
    s.buttons = PAD_A;
    padHoldBack(&held, &s);
    CHECK(s.rightTrigger == 0 && s.buttons == 0);

    // Released inputs are freed.
    padRest(&s);
    s.buttons = PAD_A;
    padHoldBack(&held, &s);
    CHECK(held == PAD_A);
    CHECK(s.buttons == 0);
    padRest(&s);
    s.rightTrigger = 200;
    padHoldBack(&held, &s);
    CHECK(s.rightTrigger == 200);
    padRest(&s);
    padHoldBack(&held, &s);
    CHECK(held == 0);
    s.buttons = PAD_A;
    padHoldBack(&held, &s);
    CHECK(s.buttons == PAD_A);

    // Held stick reads centred until it returns to the dead zone.
    padRest(&s);
    s.rightX = -30000;
    s.rightY = 4000;
    held = padHeldIn(&s);
    s.rightX = -10000;
    padHoldBack(&held, &s);
    CHECK(s.rightX == 0 && s.rightY == 0);
    padRest(&s);
    padHoldBack(&held, &s);
    CHECK(held == 0);
    s.rightX = -10000;
    padHoldBack(&held, &s);
    CHECK(s.rightX == -10000);

    held = 0;
    s.buttons = PAD_BUTTONS;
    s.leftTrigger = 255;
    padHoldBack(&held, &s);
    CHECK(s.buttons == PAD_BUTTONS && s.leftTrigger == 255);
}

// Steps the toggle for n frames at 72 Hz; returns the number of switches.
static int holdFor(PadToggle* t, int frames, int64_t* clock, int menu, int grip, int* startOk) {
    int fired = 0;
    for (int i = 0; i < frames; i++) {
        fired += padToggleStep(t, menu, grip, *clock, startOk);
        *clock += FRAME_NS;
    }
    return fired;
}

static void testTheToggle(void) {
    PadToggle t;
    padToggleReset(&t);
    int64_t clock = 5000000000LL;
    int startOk = -1;

    CHECK(holdFor(&t, 100, &clock, 0, 0, &startOk) == 0);
    CHECK(startOk == 1);
    CHECK(holdFor(&t, 100, &clock, 0, 1, &startOk) == 0);

    // Grip then menu: never Start; switches at 0.5 s, the 37th frame at 72 Hz.
    CHECK(holdFor(&t, 36, &clock, 1, 1, &startOk) == 0);
    CHECK(startOk == 0);
    int fired = padToggleStep(&t, 1, 1, clock, &startOk);
    clock += FRAME_NS;
    CHECK(fired == 1);
    CHECK(startOk == 0);
    // Once per hold.
    CHECK(holdFor(&t, 500, &clock, 1, 1, &startOk) == 0);
    // Releasing the grip with menu down is still not Start.
    CHECK(holdFor(&t, 10, &clock, 1, 0, &startOk) == 0);
    CHECK(startOk == 0);
    CHECK(holdFor(&t, 1, &clock, 0, 0, &startOk) == 0);
    CHECK(startOk == 1);
    CHECK(holdFor(&t, 1, &clock, 1, 0, &startOk) == 0);
    CHECK(startOk == 1);

    // Menu first is Start until the grip joins; the hold counts from the join.
    CHECK(holdFor(&t, 20, &clock, 1, 0, &startOk) == 0);
    CHECK(startOk == 1);
    int64_t joined = clock;
    CHECK(holdFor(&t, 1, &clock, 1, 1, &startOk) == 0);
    CHECK(startOk == 0);
    CHECK(holdFor(&t, 35, &clock, 1, 1, &startOk) == 0);
    CHECK(clock - joined >= PAD_TOGGLE_HOLD_NS);
    CHECK(holdFor(&t, 1, &clock, 1, 1, &startOk) == 1);

    // A short hold does nothing; the next starts afresh.
    holdFor(&t, 1, &clock, 0, 0, &startOk);
    CHECK(holdFor(&t, 30, &clock, 1, 1, &startOk) == 0);
    CHECK(holdFor(&t, 1, &clock, 1, 0, &startOk) == 0);
    CHECK(holdFor(&t, 30, &clock, 1, 1, &startOk) == 0);
    CHECK(holdFor(&t, 7, &clock, 1, 1, &startOk) == 1);
    holdFor(&t, 1, &clock, 0, 1, &startOk);
    CHECK(holdFor(&t, 40, &clock, 1, 1, &startOk) == 1);

    // Timed on the clock, not in frames.
    padToggleReset(&t);
    CHECK(padToggleStep(&t, 1, 1, 1000, &startOk) == 0);
    CHECK(padToggleStep(&t, 1, 1, 1000 + PAD_TOGGLE_HOLD_NS - 1, &startOk) == 0);
    CHECK(padToggleStep(&t, 1, 1, 1000 + PAD_TOGGLE_HOLD_NS, &startOk) == 1);
    padToggleReset(&t);
    CHECK(padToggleStep(&t, 1, 1, 1000, &startOk) == 0);
    CHECK(padToggleStep(&t, 1, 1, 1000 + 2 * PAD_TOGGLE_HOLD_NS, &startOk) == 1);
    CHECK(PAD_TOGGLE_HOLD_NS == 500000000LL);

    padToggleReset(&t);
    holdFor(&t, 20, &clock, 1, 1, &startOk);
    padToggleReset(&t);
    CHECK(t.holding == 0 && t.menuTaken == 0);
    CHECK(holdFor(&t, 1, &clock, 1, 0, &startOk) == 0);
    CHECK(startOk == 1);
}

// Toggle feeding padMap as the frame loop does: no Start for a grip-first switch,
// grip stays LB throughout.
static void testTheSwitchOnThePad(void) {
    PadToggle t;
    padToggleReset(&t);
    int64_t clock = 0;
    PadHand left = idle();
    PadHand right = idle();
    int grips[2] = { 0, 0 };
    int startOk;
    int sawStart = 0;
    int sawBumper = 0;
    int fired = 0;
    for (int i = 0; i < 60; i++) {
        left.grip = 1.0f;
        left.menu = i >= 5;
        grips[0] = padGripDown(left.grip, grips[0]);
        fired += padToggleStep(&t, left.menu, grips[0], clock, &startOk);
        PadState s;
        padMap(&left, &right, grips, startOk, PAD_STICK_DEADZONE_DEFAULT, &s);
        sawStart |= (s.buttons & PAD_START) != 0;
        sawBumper |= (s.buttons & PAD_LB) != 0;
        clock += FRAME_NS;
    }
    CHECK(fired == 1);
    CHECK(!sawStart);
    CHECK(sawBumper);
}

// Runs both hands through the chord and into padMap at 72 Hz, recording what was
// output.
typedef struct {
    PadChord chord;
    int64_t clock;
    int grips[2];
    int fired;
    int abandoned;
    // OR of buttons, max trigger per side, max stick deflection.
    int sentButtons;
    int sentLT;
    int sentRT;
    int sentStick;
    PadState last;
    int event;
} ChordRun;

static void chordStart(ChordRun* r, int shortcut) {
    memset(r, 0, sizeof(*r));
    padChordReset(&r->chord, padChordParts(shortcut));
    r->clock = 7000000000LL;
    // Idle frame to clear the reset's wait for rest.
    PadHand none = idle();
    padChordStep(&r->chord, &none, &none, r->clock);
    r->clock += FRAME_NS;
}

static void chordFrames(ChordRun* r, int frames, const PadHand* left, const PadHand* right) {
    for (int i = 0; i < frames; i++) {
        r->event = padChordStep(&r->chord, left, right, r->clock);
        r->fired += r->event == PAD_CHORD_FIRED;
        r->abandoned += r->event == PAD_CHORD_ABANDONED;
        PadHand l, rr;
        padChordApply(&r->chord, left, right, &l, &rr);
        r->grips[0] = padGripDown(l.grip, r->grips[0]);
        r->grips[1] = padGripDown(rr.grip, r->grips[1]);
        padMap(&l, &rr, r->grips, 1, PAD_STICK_DEADZONE_DEFAULT, &r->last);
        r->sentButtons |= r->last.buttons;
        if (r->last.leftTrigger > r->sentLT) {
            r->sentLT = r->last.leftTrigger;
        }
        if (r->last.rightTrigger > r->sentRT) {
            r->sentRT = r->last.rightTrigger;
        }
        int moved = abs(r->last.leftX) + abs(r->last.leftY) + abs(r->last.rightX)
                + abs(r->last.rightY);
        if (moved > r->sentStick) {
            r->sentStick = moved;
        }
        r->clock += FRAME_NS;
    }
}

static void chordForget(ChordRun* r) {
    r->sentButtons = 0;
    r->sentLT = 0;
    r->sentRT = 0;
    r->sentStick = 0;
    r->fired = 0;
    r->abandoned = 0;
}

static void testTheChordsParts(void) {
    CHECK(PAD_SHORTCUT_MENU_GRIP == 0 && PAD_SHORTCUT_STICKS == 1
          && PAD_SHORTCUT_TRIGGERS_GRIPS == 2);
    CHECK(padChordParts(PAD_SHORTCUT_MENU_GRIP) == 0);
    CHECK(padChordParts(PAD_SHORTCUT_STICKS) == (PAD_PART_LS | PAD_PART_RS));
    CHECK(padChordParts(PAD_SHORTCUT_TRIGGERS_GRIPS)
          == (PAD_PART_LT | PAD_PART_RT | PAD_PART_LG | PAD_PART_RG));
    CHECK(padChordParts(7) == 0);
    CHECK(padChordParts(-1) == 0);
    CHECK(PAD_CHORD_HOLD_NS == 300000000LL);
    CHECK(PAD_CHORD_WINDOW_NS == 150000000LL);
}

// Both stick clicks: clicks and sticks are masked while forming and after firing.
static void testBothSticksSwitch(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_STICKS);
    PadHand left = idle();
    PadHand right = idle();

    left.stickClick = 1;
    left.stickX = 0.4f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_FORMING);
    CHECK(r.chord.held == PAD_PART_LS);
    chordFrames(&r, 4, &left, &right);
    right.stickClick = 1;
    right.stickY = -0.3f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.chord.held == (PAD_PART_LS | PAD_PART_RS));
    CHECK(r.chord.allDown);
    // 0.3 s is 21.6 frames at 72 Hz: fires on the 22nd.
    chordFrames(&r, 21, &left, &right);
    CHECK(r.fired == 0);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.event == PAD_CHORD_FIRED);
    CHECK(r.chord.taken == (PAD_PART_LS | PAD_PART_RS));
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.sentButtons == 0);
    CHECK(r.sentStick == 0);
    CHECK(r.abandoned == 0);

    // Re-pressing L3 with R3 still in: chord spent, L3 passes through.
    left.stickClick = 0;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.chord.held == PAD_PART_RS);
    left.stickClick = 1;
    chordFrames(&r, 50, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.sentButtons == PAD_LS_CLICK);
    CHECK((r.last.buttons & PAD_RS_CLICK) == 0);
    // Full release re-arms.
    left = idle();
    right = idle();
    chordFrames(&r, 2, &left, &right);
    CHECK(r.chord.held == 0 && !r.chord.spent);
    chordForget(&r);
    left.stickClick = 1;
    right.stickClick = 1;
    chordFrames(&r, 40, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.sentButtons == 0);
}

// A lone stick click passes through after the window.
static void testOneStickGoesThroughLate(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_STICKS);
    PadHand left = idle();
    PadHand right = idle();
    left.stickClick = 1;
    // 150 ms window is 10.8 frames: held for 11, through on the 12th.
    chordFrames(&r, 11, &left, &right);
    CHECK(r.sentButtons == 0);
    CHECK(r.abandoned == 0);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.abandoned == 1);
    CHECK(r.last.buttons == PAD_LS_CLICK);
    CHECK(r.chord.replay == 0);
    chordFrames(&r, 30, &left, &right);
    CHECK(r.last.buttons == PAD_LS_CLICK);
    // A late second click is just R3.
    right.stickClick = 1;
    chordFrames(&r, 60, &left, &right);
    CHECK(r.fired == 0);
    CHECK(r.last.buttons == (PAD_LS_CLICK | PAD_RS_CLICK));
    left = idle();
    right = idle();
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == 0);
}

// A click released inside the window is replayed as a one-frame press.
static void testAQuickClickIsATap(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_STICKS);
    PadHand left = idle();
    PadHand right = idle();
    right.stickClick = 1;
    chordFrames(&r, 4, &left, &right);
    CHECK(r.sentButtons == 0);
    right.stickClick = 0;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_ABANDONED);
    CHECK(r.chord.replay == PAD_PART_RS);
    CHECK(r.last.buttons == PAD_RS_CLICK);
    CHECK(padChordSeen(&r.chord, PAD_PART_RS, 0.0f) == 1.0f);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == 0);
    CHECK(r.chord.replay == 0);
    right.stickClick = 1;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_FORMING);
    CHECK(r.last.buttons == 0);
}

// Releasing one part before the hold: it replays as a tap, the other passes, no fire.
static void testAChordLetGoShort(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_STICKS);
    PadHand left = idle();
    PadHand right = idle();
    left.stickClick = 1;
    right.stickClick = 1;
    left.stickY = 0.8f;
    chordFrames(&r, 15, &left, &right);
    CHECK(r.sentButtons == 0 && r.sentStick == 0);
    left.stickClick = 0;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_ABANDONED);
    CHECK(r.fired == 0);
    CHECK(r.last.buttons == (PAD_LS_CLICK | PAD_RS_CLICK));
    // Stick output resumes.
    CHECK(r.last.leftY == (int)(0.8f * PAD_STICK_FULL));
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == PAD_RS_CLICK);
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 0);
}

// Window and hold boundaries, in ns.
static void testTheChordsTiming(void) {
    PadChord c;
    PadHand left = idle();
    PadHand right = idle();
    padChordReset(&c, padChordParts(PAD_SHORTCUT_STICKS));
    padChordStep(&c, &left, &right, 0);
    left.stickClick = 1;
    CHECK(padChordStep(&c, &left, &right, 1000) == PAD_CHORD_FORMING);
    right.stickClick = 1;
    // Window end is inclusive.
    CHECK(padChordStep(&c, &left, &right, 1000 + PAD_CHORD_WINDOW_NS) == PAD_CHORD_NOTHING);
    int64_t all = 1000 + PAD_CHORD_WINDOW_NS;
    CHECK(padChordStep(&c, &left, &right, all + PAD_CHORD_HOLD_NS - 1) == PAD_CHORD_NOTHING);
    CHECK(padChordStep(&c, &left, &right, all + PAD_CHORD_HOLD_NS) == PAD_CHORD_FIRED);

    left = idle();
    right = idle();
    padChordReset(&c, padChordParts(PAD_SHORTCUT_STICKS));
    padChordStep(&c, &left, &right, 0);
    left.stickClick = 1;
    padChordStep(&c, &left, &right, 1000);
    right.stickClick = 1;
    CHECK(padChordStep(&c, &left, &right, 1001 + PAD_CHORD_WINDOW_NS) == PAD_CHORD_ABANDONED);
    CHECK(c.held == 0);
    CHECK(padChordStep(&c, &left, &right, 1000 + 5 * PAD_CHORD_HOLD_NS) == PAD_CHORD_NOTHING);

    left = idle();
    right = idle();
    padChordReset(&c, padChordParts(PAD_SHORTCUT_STICKS));
    padChordStep(&c, &left, &right, 0);
    left.stickClick = 1;
    right.stickClick = 1;
    CHECK(padChordStep(&c, &left, &right, 50) == PAD_CHORD_FORMING);
    CHECK(c.allDown);
    CHECK(padChordStep(&c, &left, &right, 50 + PAD_CHORD_HOLD_NS) == PAD_CHORD_FIRED);
}

// Triggers + grips: triggers are masked from first travel, so nothing leaks.
static void testTriggersAndGripsSwitch(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_TRIGGERS_GRIPS);
    PadHand left = idle();
    PadHand right = idle();
    float ramp[] = { 0.2f, 0.5f, 0.9f, 1.0f };
    for (int i = 0; i < 4; i++) {
        right.trigger = ramp[i];
        chordFrames(&r, 1, &left, &right);
    }
    CHECK(r.chord.held == PAD_PART_RT);
    right.grip = 0.7f;
    left.trigger = 0.95f;
    chordFrames(&r, 1, &left, &right);
    left.grip = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.chord.allDown);
    chordFrames(&r, 25, &left, &right);
    CHECK(r.fired == 1);
    chordFrames(&r, 50, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.sentButtons == 0);
    CHECK(r.sentLT == 0 && r.sentRT == 0);
    // Staggered release leaks nothing.
    right.trigger = 0.0f;
    chordFrames(&r, 3, &left, &right);
    left.grip = 0.0f;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.sentButtons == 0 && r.sentLT == 0 && r.sentRT == 0);
    left = idle();
    right = idle();
    chordFrames(&r, 2, &left, &right);
    CHECK(r.chord.held == 0);
    CHECK(r.sentButtons == 0 && r.sentLT == 0 && r.sentRT == 0);
}

// Single parts pass through after the window.
static void testTriggersAndGripsAlone(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_TRIGGERS_GRIPS);
    PadHand left = idle();
    PadHand right = idle();
    // Partial trigger: held for the window, then passes at its current value.
    right.trigger = 0.4f;
    chordFrames(&r, 11, &left, &right);
    CHECK(r.sentRT == 0);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.abandoned == 1);
    CHECK(r.last.rightTrigger == padTrigger(0.4f));
    right.trigger = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.rightTrigger == 255);
    // Once a part has passed, the chord cannot form.
    left.trigger = 1.0f;
    left.grip = 1.0f;
    right.grip = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.leftTrigger == 255);
    CHECK(r.last.buttons == (PAD_LB | PAD_RB));
    chordFrames(&r, 60, &left, &right);
    CHECK(r.fired == 0);

    // A trigger tap replays at its peak value.
    left = idle();
    right = idle();
    chordFrames(&r, 2, &left, &right);
    chordForget(&r);
    left.trigger = 0.5f;
    chordFrames(&r, 1, &left, &right);
    left.trigger = 0.8f;
    chordFrames(&r, 1, &left, &right);
    left.trigger = 0.6f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.sentLT == 0);
    left.trigger = 0.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.leftTrigger == padTrigger(0.8f));
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.leftTrigger == 0);

    // A grip tap is one frame of LB.
    chordForget(&r);
    left.grip = 0.9f;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.sentButtons == 0);
    left.grip = 0.1f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == PAD_LB);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == 0);

    // A half grip is below the press threshold and starts nothing.
    chordForget(&r);
    left.grip = 0.5f;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.chord.held == 0 && !r.chord.forming);

    // Nor does a trigger inside its dead zone.
    left = idle();
    left.trigger = 0.1f;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.chord.held == 0 && !r.chord.forming);
}

// Easing a trigger past its release threshold abandons the chord.
static void testAChordEasedOff(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_TRIGGERS_GRIPS);
    PadHand left = idle();
    PadHand right = idle();
    left.trigger = right.trigger = 1.0f;
    left.grip = right.grip = 1.0f;
    chordFrames(&r, 10, &left, &right);
    CHECK(r.chord.allDown);
    // Between release and press thresholds still counts as held.
    right.trigger = 0.5f;
    chordFrames(&r, 5, &left, &right);
    CHECK(r.abandoned == 0);
    right.trigger = 0.3f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.abandoned == 1);
    CHECK(r.fired == 0);
    // Everything still held passes through at once.
    CHECK(r.last.leftTrigger == 255);
    CHECK(r.last.rightTrigger == padTrigger(0.3f));
    CHECK(r.last.buttons == (PAD_LB | PAD_RB));
    chordFrames(&r, 60, &left, &right);
    CHECK(r.fired == 0);
}

// Menu + grip has no chord, so nothing is held back. Each chord only holds its own parts.
static void testNoChordHoldsNothing(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_MENU_GRIP);
    PadHand left = idle();
    PadHand right = idle();
    left.stickClick = right.stickClick = 1;
    left.trigger = right.trigger = 1.0f;
    left.grip = right.grip = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_NOTHING);
    CHECK(r.last.buttons == (PAD_LS_CLICK | PAD_RS_CLICK | PAD_LB | PAD_RB));
    CHECK(r.last.leftTrigger == 255 && r.last.rightTrigger == 255);
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 0 && r.chord.held == 0);
    CHECK(padChordSeen(&r.chord, PAD_PART_LT, 0.7f) == 0.7f);

    chordStart(&r, PAD_SHORTCUT_STICKS);
    left = idle();
    right = idle();
    left.trigger = right.trigger = 1.0f;
    left.grip = right.grip = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_NOTHING);
    CHECK(r.last.leftTrigger == 255 && r.last.buttons == (PAD_LB | PAD_RB));
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 0);
    chordStart(&r, PAD_SHORTCUT_TRIGGERS_GRIPS);
    left = idle();
    right = idle();
    left.stickClick = right.stickClick = 1;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == (PAD_LS_CLICK | PAD_RS_CLICK));
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 0);
}

// After a reset with the parts already down, they must be released before a chord.
static void testAResetWaitsForRest(void) {
    PadChord c;
    PadHand left = idle();
    PadHand right = idle();
    left.stickClick = right.stickClick = 1;
    padChordReset(&c, padChordParts(PAD_SHORTCUT_STICKS));
    int64_t t = 0;
    int fired = 0;
    for (int i = 0; i < 100; i++) {
        fired += padChordStep(&c, &left, &right, t) == PAD_CHORD_FIRED;
        CHECK(c.held == 0);
        t += FRAME_NS;
    }
    CHECK(fired == 0);
    left = idle();
    right = idle();
    padChordStep(&c, &left, &right, t);
    CHECK(!c.spent);
    padChordReset(&c, 0);
    left.stickClick = right.stickClick = 1;
    CHECK(padChordStep(&c, &left, &right, t) == PAD_CHORD_NOTHING);
    CHECK(c.held == 0 && c.replay == 0);
}

// NaN readings are ignored.
static void testTheChordIgnoresNonsense(void) {
    PadChord c;
    PadHand left = idle();
    PadHand right = idle();
    padChordReset(&c, padChordParts(PAD_SHORTCUT_TRIGGERS_GRIPS));
    padChordStep(&c, &left, &right, 0);
    left.trigger = NAN;
    left.grip = NAN;
    CHECK(padChordStep(&c, &left, &right, FRAME_NS) == PAD_CHORD_NOTHING);
    CHECK(c.held == 0);
}

// 16-bit motor levels to amplitude; also accepts a sign-extended Java short.
static void testTheRumbleLevels(void) {
    CHECK(padRumbleAmplitude(0) == 0.0f);
    CHECK(padRumbleAmplitude(0xffff) == 1.0f);
    CHECK_NEAR(padRumbleAmplitude(0x8000), 32768.0 / 65535.0, 1e-6);
    CHECK_NEAR(padRumbleAmplitude(0x4000), 16384.0 / 65535.0, 1e-6);
    CHECK_NEAR(padRumbleAmplitude(1), 1.0 / 65535.0, 1e-9);
    CHECK(padRumbleAmplitude(-1) == 1.0f);
    CHECK_NEAR(padRumbleAmplitude((short)0x8000), 32768.0 / 65535.0, 1e-6);
    CHECK_NEAR(padRumbleAmplitude((short)0xc000), 49152.0 / 65535.0, 1e-6);
    // Clamped to [0, 1].
    for (int m = -70000; m <= 70000; m += 997) {
        float a = padRumbleAmplitude(m);
        CHECK(a >= 0.0f && a <= 1.0f);
    }
    // Low-frequency motor left, high right.
    PadRumble r;
    padRumbleReset(&r);
    padRumbleAsk(&r, 0xffff, 0x4000);
    CHECK(r.want[0] == 1.0f);
    CHECK_NEAR(r.want[1], 16384.0 / 65535.0, 1e-6);
    CHECK(r.fresh);
}

static void testTheRumblePulses(void) {
    PadRumble r;
    int live[2] = { 1, 1 };
    int action[2];
    float amp[2];
    padRumbleReset(&r);
    padRumbleStep(&r, live, 0, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);

    int64_t t = 1000000000LL;
    padRumbleAsk(&r, 0x8000, 0xffff);
    padRumbleStep(&r, live, t, action, amp);
    CHECK(action[0] == PAD_RUMBLE_APPLY && action[1] == PAD_RUMBLE_APPLY);
    CHECK_NEAR(amp[0], 32768.0 / 65535.0, 1e-6);
    CHECK(amp[1] == 1.0f);
    CHECK(!r.fresh);
    // Re-armed well inside each pulse.
    CHECK(PAD_RUMBLE_REARM_NS < PAD_RUMBLE_PULSE_NS);
    CHECK(PAD_RUMBLE_PULSE_NS - PAD_RUMBLE_REARM_NS > 2 * FRAME_NS);
    int64_t last = t;
    int applies = 0;
    int64_t worstGap = 0;
    for (int64_t now = t + FRAME_NS; now < t + 1000000000LL; now += FRAME_NS) {
        padRumbleStep(&r, live, now, action, amp);
        CHECK(action[0] == action[1]);
        CHECK(action[0] != PAD_RUMBLE_STOP);
        if (action[0] == PAD_RUMBLE_APPLY) {
            applies++;
            if (now - last > worstGap) {
                worstGap = now - last;
            }
            last = now;
        }
    }
    // Re-armed about every 50 ms, so pulses overlap without gaps.
    CHECK(applies >= 15 && applies <= 20);
    CHECK(worstGap < PAD_RUMBLE_PULSE_NS);
    CHECK(worstGap >= PAD_RUMBLE_REARM_NS);

    // A new request applies at once, even with the same values.
    padRumbleStep(&r, live, last + FRAME_NS, action, amp);
    padRumbleAsk(&r, 0x8000, 0xffff);
    padRumbleStep(&r, live, last + 2 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_APPLY && action[1] == PAD_RUMBLE_APPLY);
    last += 2 * FRAME_NS;

    // Only the changed side applies.
    padRumbleStep(&r, live, last + FRAME_NS, action, amp);
    r.want[1] = 0.25f;
    padRumbleStep(&r, live, last + 2 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_APPLY);
    CHECK(amp[1] == 0.25f);
    last += 2 * FRAME_NS;

    // Zero stops that side once.
    padRumbleAsk(&r, 0, 0x4000);
    padRumbleStep(&r, live, last + FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_STOP && action[1] == PAD_RUMBLE_APPLY);
    padRumbleStep(&r, live, last + 2 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP);

    padRumbleAsk(&r, 0, 0);
    padRumbleStep(&r, live, last + 3 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_STOP);
    for (int i = 0; i < 20; i++) {
        padRumbleStep(&r, live, last + (4 + i) * FRAME_NS, action, amp);
        CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);
    }
}

// Controllers not driving the pad are stopped once and resume the current request
// when they return.
static void testTheRumbleOffThePad(void) {
    PadRumble r;
    int both[2] = { 1, 1 };
    int leftOnly[2] = { 1, 0 };
    int none[2] = { 0, 0 };
    int action[2];
    float amp[2];
    padRumbleReset(&r);
    padRumbleAsk(&r, 0xffff, 0xffff);
    padRumbleStep(&r, leftOnly, 0, action, amp);
    CHECK(action[0] == PAD_RUMBLE_APPLY && action[1] == PAD_RUMBLE_KEEP);
    padRumbleStep(&r, both, FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_APPLY);
    padRumbleStep(&r, none, 2 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_STOP && action[1] == PAD_RUMBLE_STOP);
    padRumbleStep(&r, none, 3 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);
    // Requests while out apply nothing.
    padRumbleAsk(&r, 0xffff, 0);
    padRumbleStep(&r, none, 4 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);
    // On return the current request applies at once.
    padRumbleStep(&r, both, 5 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_APPLY && action[1] == PAD_RUMBLE_KEEP);
    CHECK(amp[0] == 1.0f);
    // Reset forgets the request but stops nothing; callers stop motors first.
    padRumbleReset(&r);
    padRumbleStep(&r, both, 6 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);
}

int main(void) {
    testTheBits();
    testTheButtons();
    testTheGrips();
    testTheTriggers();
    testTheSticks();
    testTheDeadZoneSetting();
    testRestAndSame();
    testTheHoldBack();
    testTheToggle();
    testTheSwitchOnThePad();
    testTheChordsParts();
    testBothSticksSwitch();
    testOneStickGoesThroughLate();
    testAQuickClickIsATap();
    testAChordLetGoShort();
    testTheChordsTiming();
    testTriggersAndGripsSwitch();
    testTriggersAndGripsAlone();
    testAChordEasedOff();
    testNoChordHoldsNothing();
    testAResetWaitsForRest();
    testTheChordIgnoresNonsense();
    testTheRumbleLevels();
    testTheRumblePulses();
    testTheRumbleOffThePad();
    return checksDone("xr_gamepad");
}
