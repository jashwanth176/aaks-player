// Tests for xr_pinch: pinch sources and thresholds, triple pinch lock, gaze drag.
#include "check.h"
#include "xr_pinch.h"

#define MS 1000000LL
// 90 Hz frame.
#define FRAME_NS 11111111LL

static void testPinchSource(void) {
    // Priority: bound pinch value, aim flag, joints, none.
    CHECK(pinchSource(1, 1, 1) == PINCH_SRC_VALUE);
    CHECK(pinchSource(1, 0, 0) == PINCH_SRC_VALUE);
    CHECK(pinchSource(0, 1, 1) == PINCH_SRC_AIM);
    CHECK(pinchSource(0, 0, 1) == PINCH_SRC_JOINTS);
    CHECK(pinchSource(0, 0, 0) == PINCH_SRC_NONE);
    CHECK(!pinchStep(PINCH_SRC_NONE, 0, 0, 1.0f, 1, 1, 0.0f));
}

// Ramps the input from `from` to `to` over overNs and returns the first frame
// the pinch is down, or -1. Drives value and tip gap together; ext selects the
// EXT profile thresholds.
static int firstDown(int source, int ext, float from, float to, int64_t overNs) {
    int frames = (int)(overNs / FRAME_NS);
    int down = 0;
    for (int i = 0; i <= frames; i++) {
        float x = from + (to - from) * i / frames;
        down = pinchStep(source, ext, down, x, 0, 1, x);
        if (down) {
            return i;
        }
    }
    return -1;
}

static void testPinchValue(void) {
    // Press at 0.65, release at 0.35.
    CHECK(!pinchStep(PINCH_SRC_VALUE, 0, 0, 0.64f, 0, 0, 0.0f));
    CHECK(pinchStep(PINCH_SRC_VALUE, 0, 0, 0.66f, 0, 0, 0.0f));
    CHECK(pinchStep(PINCH_SRC_VALUE, 0, 1, 0.36f, 0, 0, 0.0f));
    CHECK(!pinchStep(PINCH_SRC_VALUE, 0, 1, 0.34f, 0, 0, 0.0f));
    CHECK(PINCH_VALUE_ON == 0.65f && PINCH_VALUE_OFF == 0.35f);

    // 0 to 1 over 9 frames: frame 6 is the first past 0.65.
    CHECK(firstDown(PINCH_SRC_VALUE, 0, 0.0f, 1.0f, 100 * MS) == 6);
    // 0 to 1 over 135 frames: frame 88 is the first past 0.65.
    int slow = firstDown(PINCH_SRC_VALUE, 0, 0.0f, 1.0f, 1500 * MS);
    CHECK(slow == 88);
    CHECK(firstDown(PINCH_SRC_VALUE, 0, 0.0f, 0.6f, 500 * MS) == -1);

    // The value source ignores the tips and the aim flag.
    CHECK(!pinchStep(PINCH_SRC_VALUE, 0, 0, 0.1f, 1, 1, 0.001f));
}

static void testPinchAim(void) {
    CHECK(pinchStep(PINCH_SRC_AIM, 0, 0, 0.0f, 1, 0, 0.0f));
    CHECK(!pinchStep(PINCH_SRC_AIM, 0, 1, 1.0f, 0, 1, 0.001f));
}

static void testPinchExtValue(void) {
    // EXT profile: press 0.9, release 0.7. Microsoft profile: 0.65 and 0.35.
    CHECK(PINCH_EXT_VALUE_ON == 0.9f && PINCH_EXT_VALUE_OFF == 0.7f);
    float on = 0.0f, off = 0.0f;
    pinchValuePair(1, &on, &off);
    CHECK(on == PINCH_EXT_VALUE_ON && off == PINCH_EXT_VALUE_OFF);
    pinchValuePair(0, &on, &off);
    CHECK(on == PINCH_VALUE_ON && off == PINCH_VALUE_OFF);

    CHECK(!pinchStep(PINCH_SRC_VALUE, 1, 0, 0.89f, 0, 0, 0.0f));
    CHECK(pinchStep(PINCH_SRC_VALUE, 1, 0, 0.91f, 0, 0, 0.0f));
    CHECK(pinchStep(PINCH_SRC_VALUE, 1, 1, 0.71f, 0, 0, 0.0f));
    CHECK(!pinchStep(PINCH_SRC_VALUE, 1, 1, 0.70f, 0, 0, 0.0f));

    // Pico 4 Ultra reports 1.0 closed and 0.47..0.7 open. The EXT pair
    // releases at each of these; the Microsoft pair would stay down.
    const float opened[] = { 0.47f, 0.55f, 0.62f, 0.7f };
    for (int i = 0; i < 4; i++) {
        int down = pinchStep(PINCH_SRC_VALUE, 1, 0, opened[i], 0, 0, 0.0f);
        CHECK(!down);
        down = pinchStep(PINCH_SRC_VALUE, 1, down, 1.0f, 0, 0, 0.0f);
        CHECK(down);
        down = pinchStep(PINCH_SRC_VALUE, 1, down, opened[i], 0, 0, 0.0f);
        CHECK(!down);
        CHECK(pinchStep(PINCH_SRC_VALUE, 0, 1, opened[i], 0, 0, 0.0f));
    }

    // 0 to 1 over 9 frames: only the last is past 0.9.
    CHECK(firstDown(PINCH_SRC_VALUE, 1, 0.0f, 1.0f, 100 * MS) == 9);

    // ext only affects the value source.
    CHECK(pinchStep(PINCH_SRC_AIM, 1, 0, 0.0f, 1, 0, 0.0f));
    CHECK(!pinchStep(PINCH_SRC_AIM, 1, 1, 1.0f, 0, 1, 0.001f));
    CHECK(pinchStep(PINCH_SRC_JOINTS, 1, 0, 0.0f, 0, 1, 0.019f));
    CHECK(pinchStep(PINCH_SRC_JOINTS, 1, 1, 0.0f, 0, 1, 0.031f));
    CHECK(!pinchStep(PINCH_SRC_JOINTS, 1, 1, 0.0f, 0, 1, 0.033f));
}

static void testPinchJoints(void) {
    // Tip gap: press under 20 mm, release over 32 mm.
    CHECK(!pinchStep(PINCH_SRC_JOINTS, 0, 0, 0.0f, 0, 1, 0.021f));
    CHECK(pinchStep(PINCH_SRC_JOINTS, 0, 0, 0.0f, 0, 1, 0.019f));
    CHECK(pinchStep(PINCH_SRC_JOINTS, 0, 1, 0.0f, 0, 1, 0.031f));
    CHECK(!pinchStep(PINCH_SRC_JOINTS, 0, 1, 0.0f, 0, 1, 0.033f));
    CHECK(PINCH_ON_M == 0.020f && PINCH_OFF_M == 0.032f);
    // Untracked tips release.
    CHECK(!pinchStep(PINCH_SRC_JOINTS, 0, 1, 0.0f, 0, 0, 0.005f));

    // 60 to 5 mm over 90 frames: frame 66 is the first under 20 mm. A hand
    // that appears already closed presses immediately.
    CHECK(firstDown(PINCH_SRC_JOINTS, 0, 0.060f, 0.005f, 1000 * MS) == 66);
    CHECK(pinchStep(PINCH_SRC_JOINTS, 0, 0, 0.0f, 0, 1, 0.006f));
}

// Steps a series of pinches (start times atMs, each held holdMs) from *now.
// Counts lock toggles and presses that pass through as clicks: none pass while
// locked, and the gesture withholds its own third press.
typedef struct {
    int turned;
    int clicks;
} PinchRun;

static PinchRun pinches(TriplePinch* tp, int64_t* now, int* locked, const int* atMs, int count,
                        int holdMs) {
    PinchRun run = { 0, 0 };
    int64_t from = *now;
    int64_t end = from + (int64_t)(atMs[count - 1] + holdMs + 300) * MS;
    int reached = 0;
    for (int64_t t = from; t <= end; t += FRAME_NS) {
        int down = 0;
        for (int i = 0; i < count; i++) {
            int64_t on = from + (int64_t)atMs[i] * MS;
            down |= t >= on && t < on + (int64_t)holdMs * MS;
        }
        if (triplePinchStep(tp, down, t)) {
            *locked = !*locked;
            run.turned++;
        }
        int reaches = down && !*locked && !triplePinchHeld(tp);
        run.clicks += reaches && !reached;
        reached = reaches;
        *now = t;
    }
    *now += 2000 * MS;
    return run;
}

static void testTriplePinch(void) {
    TriplePinch tp;
    triplePinchReset(&tp);
    int64_t now = 1000 * MS;
    int locked = 0;

    // Three pinches inside the window lock. The first two click; the third is
    // consumed.
    const int quick[3] = { 0, 300, 600 };
    PinchRun run = pinches(&tp, &now, &locked, quick, 3, 120);
    CHECK(run.turned == 1 && locked);
    CHECK(run.clicks == 2);

    // While locked the same three unlock with no clicks.
    run = pinches(&tp, &now, &locked, quick, 3, 120);
    CHECK(run.turned == 1 && !locked);
    CHECK(run.clicks == 0);

    // Spread over 1.3 s: three clicks, no lock.
    const int slow[3] = { 0, 650, 1300 };
    run = pinches(&tp, &now, &locked, slow, 3, 120);
    CHECK(run.turned == 0 && !locked);
    CHECK(run.clicks == 3);

    const int twice[2] = { 0, 250 };
    run = pinches(&tp, &now, &locked, twice, 2, 100);
    CHECK(run.turned == 0 && !locked);
    CHECK(run.clicks == 2);

    // The window is measured from the press two before the latest, not from
    // the first of the run.
    const int late[4] = { 0, 1000, 1300, 1600 };
    run = pinches(&tp, &now, &locked, late, 4, 120);
    CHECK(run.turned == 1 && locked);
    CHECK(run.clicks == 3);

    // Six quick pinches toggle twice; a new run starts at the fourth.
    const int six[6] = { 0, 250, 500, 750, 1000, 1250 };
    run = pinches(&tp, &now, &locked, six, 6, 100);
    CHECK(run.turned == 2 && locked);
    // Starts locked: 1-3 unlock, 4-5 click, 6 locks.
    CHECK(run.clicks == 2);
    locked = 0;
    triplePinchReset(&tp);

    // Overlapping holds are one press.
    const int held[3] = { 0, 100, 200 };
    run = pinches(&tp, &now, &locked, held, 3, 400);
    CHECK(run.turned == 0 && run.clicks == 1);

    // Window edge: third press at exactly 0.9 s toggles, 1 ms later does not.
    triplePinchReset(&tp);
    int64_t t0 = now;
    CHECK(!triplePinchStep(&tp, 1, t0));
    CHECK(!triplePinchStep(&tp, 0, t0 + 50 * MS));
    CHECK(!triplePinchStep(&tp, 1, t0 + 400 * MS));
    CHECK(!triplePinchStep(&tp, 0, t0 + 450 * MS));
    CHECK(triplePinchStep(&tp, 1, t0 + TRIPLE_PINCH_WINDOW_NS));
    // The third press is withheld until released.
    CHECK(triplePinchHeld(&tp));
    CHECK(!triplePinchStep(&tp, 1, t0 + 1000 * MS));
    CHECK(triplePinchHeld(&tp));
    CHECK(!triplePinchStep(&tp, 0, t0 + 1100 * MS));
    CHECK(!triplePinchHeld(&tp));

    triplePinchReset(&tp);
    t0 += 5000 * MS;
    CHECK(!triplePinchStep(&tp, 1, t0));
    CHECK(!triplePinchStep(&tp, 0, t0 + 50 * MS));
    CHECK(!triplePinchStep(&tp, 1, t0 + 400 * MS));
    CHECK(!triplePinchStep(&tp, 0, t0 + 450 * MS));
    CHECK(!triplePinchStep(&tp, 1, t0 + TRIPLE_PINCH_WINDOW_NS + MS));
    CHECK(!triplePinchHeld(&tp));
    CHECK(TRIPLE_PINCH_WINDOW_NS == 900 * MS);
}

static void testDragRamp(void) {
    CHECK_NEAR(dragRampGain(0), 0.0, 1e-6);
    CHECK_NEAR(dragRampGain(GAZE_DRAG_RAMP_NS / 2), 0.5, 1e-6);
    CHECK_NEAR(dragRampGain(GAZE_DRAG_RAMP_NS), 1.0, 1e-6);
    CHECK_NEAR(dragRampGain(GAZE_DRAG_RAMP_NS * 4), 1.0, 1e-6);
    CHECK(dragRampGain(GAZE_DRAG_RAMP_NS / 4) < 0.25f);

    // Steady hand motion: little passes early, full gain after the ramp.
    DragRamp r;
    int64_t t = 0;
    dragRampStart(&r, t);
    int held = 0;
    Vec3 d = { 0.0f, 0.0f, 0.0f };
    Vec3 out = d;
    for (int i = 0; i < 10; i++) {
        t += FRAME_NS;
        d.x += 0.001f;
        out = dragRampStep(&r, d, 0.0f, t, &held);
    }
    CHECK(!held);
    CHECK(out.x < 0.002f);
    for (int i = 0; i < 90; i++) {
        t += FRAME_NS;
        d.x += 0.001f;
        out = dragRampStep(&r, d, 0.0f, t, &held);
    }
    // After the ramp, 1 mm of hand is 1.25 mm of drag.
    Vec3 before = out;
    t += FRAME_NS;
    d.x += 0.001f;
    out = dragRampStep(&r, d, 0.0f, t, &held);
    CHECK_NEAR(out.x - before.x, 0.00125, 1e-6);

    // A head turn freezes the drag and rebases on the hand, so no jump after.
    before = out;
    for (int i = 0; i < 20; i++) {
        t += FRAME_NS;
        d.x += 0.002f;
        out = dragRampStep(&r, d, 30.0f, t, &held);
        CHECK(held);
    }
    CHECK_NEAR(out.x, before.x, 1e-6);
    t += FRAME_NS;
    d.x += 0.001f;
    out = dragRampStep(&r, d, 5.0f, t, &held);
    CHECK(!held);
    CHECK_NEAR(out.x - before.x, 0.00125, 1e-6);
    // Exactly at the threshold is not held.
    t += FRAME_NS;
    out = dragRampStep(&r, d, HEAD_TURN_HOLD_DEG_S, t, &held);
    CHECK(!held);

    // Gearing is target distance over hand distance (3 / 0.5 = 6), clamped to
    // 0.5..64; 1 when the hand is at the head.
    Vec3 head = { 0.0f, 0.0f, 0.0f };
    Vec3 target = { 0.0f, 0.0f, -3.0f };
    Vec3 hand = { 0.0f, -0.3f, -0.4f };
    CHECK_NEAR(dragScale(head, target, hand), 6.0, 1e-4);
    Vec3 near = { 0.0f, 0.0f, -0.1f };
    Vec3 far = { 0.0f, 0.0f, -100.0f };
    CHECK_NEAR(dragScale(head, near, hand), 0.5, 1e-6);
    CHECK_NEAR(dragScale(head, far, hand), 64.0, 1e-6);
    Vec3 atHead = { 0.0f, 0.01f, 0.0f };
    CHECK_NEAR(dragScale(head, target, atHead), 1.0, 1e-6);

    Vec3 small = { 0.010f, 0.0f, 0.0f };
    Vec3 big = { 0.025f, 0.0f, 0.0f };
    CHECK_NEAR(dragDeadZone(small, GAZE_DRAG_DEAD_M).x, 0.0, 1e-6);
    CHECK_NEAR(dragDeadZone(big, GAZE_DRAG_DEAD_M).x, 0.010, 1e-6);

    // 1 degree in one 90 Hz frame is 90 deg/s.
    XrQuaternionf was = { 0.0f, 0.0f, 0.0f, 1.0f };
    float half = 0.5f * 3.14159265f / 180.0f;
    XrQuaternionf now = { 0.0f, sinf(half), 0.0f, cosf(half) };
    CHECK_NEAR(turnRateDegS(now, was, 1.0f / 90.0f), 90.0, 0.5);
    CHECK_NEAR(turnRateDegS(now, was, 0.0f), 0.0, 1e-6);
    // q and -q are the same orientation.
    XrQuaternionf neg = { 0.0f, -sinf(half), 0.0f, -cosf(half) };
    CHECK_NEAR(turnRateDegS(neg, was, 1.0f / 90.0f), 90.0, 0.5);
}

int main(void) {
    testPinchSource();
    testPinchValue();
    testPinchAim();
    testPinchExtValue();
    testPinchJoints();
    testTriplePinch();
    testDragRamp();
    return checksDone("xr_pinch");
}
