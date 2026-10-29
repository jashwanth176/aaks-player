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

int main(void) {
    testWhoCounts();
    testControllerClock();
    testGazeBridge();
    return checksDone("xr_gate");
}
