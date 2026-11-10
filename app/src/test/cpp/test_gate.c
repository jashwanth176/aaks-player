// Tests for pointer gating: profiles, controller wake/sleep clock, gaze-to-hands bridge.
#include "check.h"
#include "xr_gate.h"

#include <string.h>

#define MS 1000000LL
// One frame at 90 Hz
#define FRAME_NS 11111111LL
#define FRAME_S 0.0111111f

static void testWhoCounts(void) {
    CHECK(profileKind(0, 0) == PROFILE_NONE);
    CHECK(profileKind(0, 1) == PROFILE_NONE);
    CHECK(profileKind(1, 1) == PROFILE_HANDS);
    CHECK(profileKind(1, 0) == PROFILE_CONTROLLER);

    // Gaze needs orientation tracked; aim needs both position and orientation tracked.
    unsigned valid = GATE_POSITION_VALID | GATE_ORIENTATION_VALID;
    CHECK(!gazeUsable(valid));
    CHECK(gazeUsable(valid | GATE_ORIENTATION_TRACKED));
    CHECK(!gazeUsable(GATE_ORIENTATION_VALID | GATE_ORIENTATION_TRACKED));
    CHECK(!aimFullyTracked(valid | GATE_ORIENTATION_TRACKED));
    CHECK(!aimFullyTracked(valid | GATE_POSITION_TRACKED));
    CHECK(aimFullyTracked(valid | GATE_ORIENTATION_TRACKED | GATE_POSITION_TRACKED));

    // Only PROFILE_CONTROLLER can be in use, regardless of clock state.
    CHECK(!controllerInUse(PROFILE_NONE, 1, 1));
    CHECK(!controllerInUse(PROFILE_HANDS, 1, 1));
    CHECK(!controllerInUse(PROFILE_CONTROLLER, 0, 1));
    CHECK(!controllerInUse(PROFILE_CONTROLLER, 1, 0));
    CHECK(controllerInUse(PROFILE_CONTROLLER, 1, 1));
}

// Steps the clock for a number of frames with fixed inputs; returns the last
// event other than CLOCK_SAME.
static int runClock(ControllerClock* c, int frames, int moved, int pressed, int holding,
                    int trigger, int pressWakes, int sleepOn, int* swallow) {
    int last = CLOCK_SAME;
    for (int i = 0; i < frames; i++) {
        int e = controllerClockStep(c, FRAME_S, moved, pressed, holding, trigger, pressWakes,
                                    sleepOn, 0.5f, 5.0f, swallow);
        if (e != CLOCK_SAME) {
            last = e;
        }
    }
    return last;
}

static void testControllerClock(void) {
    ControllerClock c;
    int swallow = 0;

    // Without gaze: 0.5 s of movement wakes it (45 frames), 5 s still sleeps it (450).
    controllerClockReset(&c);
    CHECK(runClock(&c, 30, 1, 0, 0, 0, 0, 1, &swallow) == CLOCK_SAME && !c.awake);
    CHECK(runClock(&c, 20, 1, 0, 0, 0, 0, 1, &swallow) == CLOCK_PICKED_UP && c.awake);
    CHECK(runClock(&c, 440, 0, 0, 0, 0, 0, 1, &swallow) == CLOCK_SAME && c.awake);
    CHECK(runClock(&c, 20, 0, 0, 0, 0, 0, 1, &swallow) == CLOCK_PUT_DOWN && !c.awake);

    // With gaze active, movement never wakes it.
    controllerClockReset(&c);
    CHECK(runClock(&c, 300, 1, 0, 0, 0, 1, 1, &swallow) == CLOCK_SAME && !c.awake);
    // A press wakes it at once; the waking trigger is swallowed until released.
    CHECK(runClock(&c, 1, 0, 1, 1, 1, 1, 1, &swallow) == CLOCK_PRESSED && c.awake);
    CHECK(swallow);
    runClock(&c, 10, 0, 1, 1, 1, 1, 1, &swallow);
    CHECK(swallow);
    runClock(&c, 1, 0, 0, 0, 0, 1, 1, &swallow);
    CHECK(!swallow);
    runClock(&c, 1, 0, 1, 1, 1, 1, 1, &swallow);
    CHECK(!swallow);
    // Waking by a non-trigger input swallows nothing.
    controllerClockReset(&c);
    CHECK(runClock(&c, 1, 0, 1, 0, 0, 1, 1, &swallow) == CLOCK_PRESSED && !swallow);

    // A held input keeps a still controller awake.
    controllerClockReset(&c);
    runClock(&c, 50, 1, 0, 0, 0, 0, 1, &swallow);
    CHECK(c.awake);
    CHECK(runClock(&c, 900, 0, 0, 1, 1, 0, 1, &swallow) == CLOCK_SAME && c.awake);
    CHECK(runClock(&c, 460, 0, 0, 0, 0, 0, 1, &swallow) == CLOCK_PUT_DOWN);

    // Sleep disabled.
    controllerClockReset(&c);
    runClock(&c, 50, 1, 0, 0, 0, 0, 0, &swallow);
    CHECK(c.awake);
    CHECK(runClock(&c, 9000, 0, 0, 0, 0, 0, 0, &swallow) == CLOCK_SAME && c.awake);
}

static void testGazeBridge(void) {
    GazeBridge b;
    memset(&b, 0, sizeof(b));
    float sec = 0.0f;
    int64_t t = 1000 * MS;

    gazeBridgeTrack(&b, 1, 1, t);
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 0, t, GAZE_BRIDGE_SEC, &sec) == BRIDGE_SAME);

    // Gaze lost for just under GAZE_BRIDGE_SEC (10 s): no bridge yet.
    t += FRAME_NS;
    gazeBridgeTrack(&b, 1, 0, t);
    int64_t lost = t;
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 0, lost + 9900 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    // Controller in use, unfocused session or held press each block the bridge.
    CHECK(gazeBridgeUpdate(&b, 1, 1, 1, 0, lost + 10100 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    CHECK(gazeBridgeUpdate(&b, 1, 0, 0, 0, lost + 10100 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 1, lost + 10100 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    t = lost + 10100 * MS;
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 0, t, GAZE_BRIDGE_SEC, &sec) == BRIDGE_ON);
    CHECK(b.bridged);
    CHECK_NEAR(sec, 10.1, 0.01);

    // Gaze returning mid-pinch waits for the pinch to end.
    t += FRAME_NS;
    gazeBridgeTrack(&b, 1, 1, t);
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 1, t, GAZE_BRIDGE_SEC, &sec) == BRIDGE_SAME);
    CHECK(b.bridged);
    t += 2000 * MS;
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 0, t, GAZE_BRIDGE_SEC, &sec) == BRIDGE_OFF);
    CHECK(!b.bridged);
    CHECK_NEAR(sec, 2.011, 0.01);

    // Gaze not requested does not count as missing.
    gazeBridgeTrack(&b, 0, 0, t);
    CHECK(gazeBridgeUpdate(&b, 1, 1, 1, 0, t + 60000 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    CHECK(b.missingSince == 0);

    // Disabling gaze while bridged ends the bridge.
    gazeBridgeTrack(&b, 1, 0, t);
    gazeBridgeUpdate(&b, 1, 0, 1, 0, t + 11000 * MS, GAZE_BRIDGE_SEC, &sec);
    CHECK(b.bridged);
    CHECK(gazeBridgeUpdate(&b, 0, 0, 1, 0, t + 12000 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_OFF);
    CHECK(!b.bridged && b.missingSince == 0);
}

static void testWhoPoints(void) {
    // Sources: left hand 0, right hand 1, gaze 2; right first, as the renderer asks
    static const int order[3] = { 1, 0, 2 };
    PointerOffer o[3];

    // Nothing anywhere
    memset(o, 0, sizeof(o));
    CHECK(pickPointer(o, order, 3, -1) == -1);

    // Both rays resting on something: the right hand's
    o[0].on = 1;
    o[1].on = 1;
    CHECK(pickPointer(o, order, 3, -1) == 1);
    // The left pinches on what it points at: the left acts, though the right rests there
    o[0].pressing = 1;
    CHECK(pickPointer(o, order, 3, 1) == 0);
    // A new pinch by the right on what it points at takes over at once
    o[1].pressing = 1;
    o[1].fresh = 1;
    CHECK(pickPointer(o, order, 3, 0) == 1);
    // Held on, the right keeps it though the left is still pinched
    o[1].fresh = 0;
    CHECK(pickPointer(o, order, 3, 1) == 1);
    // Let go, the left's press is the one left
    o[1].pressing = 0;
    CHECK(pickPointer(o, order, 3, 1) == 0);
    // A left hand resting curled reads as a pinch it never aimed (pressing 0): a fresh
    // right pinch on the picture is the right's, and so is the pointer meanwhile
    memset(o, 0, sizeof(o));
    o[0].on = 1;
    o[1].on = 1;
    CHECK(pickPointer(o, order, 3, 0) == 1);
    o[1].pressing = 1;
    o[1].fresh = 1;
    CHECK(pickPointer(o, order, 3, 1) == 1);
    // A fresh pinch aimed at nothing takes nothing
    memset(o, 0, sizeof(o));
    o[0].on = 1;
    o[0].pressing = 1;
    o[1].fresh = 1;
    CHECK(pickPointer(o, order, 3, 0) == 0);

    // A left fingertip about to touch beats the right ray resting on the picture
    memset(o, 0, sizeof(o));
    o[1].on = 1;
    o[0].on = 1;
    o[0].touch = 1;
    CHECK(pickPointer(o, order, 3, 1) == 0);
    // and its touch is the press, even while the right hand pinches at the picture
    o[0].touch = 2;
    o[0].pressing = 1;
    CHECK(pickPointer(o, order, 3, 0) == 0);
    o[1].pressing = 1;
    CHECK(pickPointer(o, order, 3, 0) == 0);
    // A right-hand drag already under way is not taken by a left fingertip coming near
    memset(o, 0, sizeof(o));
    o[1].on = 1;
    o[1].pressing = 1;
    o[0].on = 1;
    o[0].touch = 1;
    CHECK(pickPointer(o, order, 3, 1) == 1);

    // A pinch in the air, its ray on nothing, takes nothing from a resting ray
    memset(o, 0, sizeof(o));
    o[1].pressing = 1;
    o[0].on = 1;
    CHECK(pickPointer(o, order, 3, 1) == 0);
    // The margin around the picture comes last
    memset(o, 0, sizeof(o));
    o[1].halo = 1;
    CHECK(pickPointer(o, order, 3, -1) == 1);
    o[0].on = 1;
    CHECK(pickPointer(o, order, 3, -1) == 0);
    // Gaze, with the hands only pinching
    memset(o, 0, sizeof(o));
    o[2].on = 1;
    o[2].pressing = 1;
    CHECK(pickPointer(o, order, 3, -1) == 2);
}

static void testPinchWakes(void) {
    // A new pinch from a well seen hand aimed at the picture wakes the pointer
    CHECK(pinchWakes(1, 1, 0.9f));
    // A pinch already held when the ray wanders onto the picture does not
    CHECK(!pinchWakes(0, 1, 0.9f));
    // nor one aimed at the floor, as hands in the lap are, nor a half seen hand's
    CHECK(!pinchWakes(1, 0, 0.9f));
    CHECK(!pinchWakes(1, 1, 0.5f));
}

int main(void) {
    testWhoCounts();
    testControllerClock();
    testGazeBridge();
    testWhoPoints();
    testPinchWakes();
    return checksDone("xr_gate");
}
