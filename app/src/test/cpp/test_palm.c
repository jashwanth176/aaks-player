// Tests for xr_palm: the open-palm gesture that wakes the hand pointer.
#include "check.h"
#include "xr_palm.h"

#define PI 3.14159265f
// 90 Hz frame.
#define FRAME 0.011111f

static const XrPosef HEAD = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };

// Hand at (x, y, z), palm toward the eyes unless turned, fingertips `spread`
// metres above the palm centre.
static PalmHand hand(float x, float y, float z, int turned, float spread) {
    PalmHand h;
    Vec3 xAxis = { 1.0f, 0.0f, 0.0f };
    // -90 degrees about x maps palm -Y to +Z (toward the eyes); +90 faces away.
    h.palm.orientation = axisAngleQuat(xAxis, turned ? PI * 0.5f : -PI * 0.5f);
    h.palm.position.x = x;
    h.palm.position.y = y;
    h.palm.position.z = z;
    for (int i = 0; i < 4; i++) {
        Vec3 tip = { x + (i - 1.5f) * 0.02f, y + spread, z };
        h.tips[i] = tip;
    }
    Vec3 thumb = { x - 0.07f, y + 0.02f, z };
    h.thumbTip = thumb;
    return h;
}

static void testWhereItIs(void) {
    PalmHand h = hand(0.0f, 0.0f, -0.4f, 0, 0.09f);
    CHECK(palmShown(&h, HEAD, 0));
    // Slightly off centre counts; far off does not.
    h = hand(0.1f, -0.05f, -0.4f, 0, 0.09f);
    CHECK(palmShown(&h, HEAD, 0));
    h = hand(0.4f, 0.0f, -0.4f, 0, 0.09f);
    CHECK(!palmShown(&h, HEAD, 0));
    // Below the view.
    h = hand(0.0f, -0.5f, -0.3f, 0, 0.09f);
    CHECK(!palmShown(&h, HEAD, 0));
    // Too close and beyond arm's length.
    h = hand(0.0f, 0.0f, -0.1f, 0, 0.09f);
    CHECK(!palmShown(&h, HEAD, 0));
    h = hand(0.0f, 0.0f, -0.9f, 0, 0.09f);
    CHECK(!palmShown(&h, HEAD, 0));
}

static void testWhichWayItFaces(void) {
    // Back of the hand does not count, unless the runtime reports palm-facing.
    PalmHand h = hand(0.0f, 0.0f, -0.4f, 1, 0.09f);
    CHECK(!palmShown(&h, HEAD, 0));
    CHECK(palmShown(&h, HEAD, 1));
    // The view centre follows head yaw.
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    XrPosef turned = HEAD;
    turned.orientation = axisAngleQuat(up, PI * 0.5f);
    h = hand(0.0f, 0.0f, -0.4f, 0, 0.09f);
    CHECK(!palmShown(&h, turned, 0));
    PalmHand left = hand(-0.4f, 0.0f, 0.0f, 0, 0.09f);
    Vec3 yAxis = { 0.0f, 1.0f, 0.0f };
    Vec3 xAxis = { 1.0f, 0.0f, 0.0f };
    left.palm.orientation = quatMul(axisAngleQuat(yAxis, PI * 0.5f),
                                    axisAngleQuat(xAxis, -PI * 0.5f));
    CHECK(palmShown(&left, turned, 0));
}

static void testHowOpenItIs(void) {
    // Fist.
    PalmHand h = hand(0.0f, 0.0f, -0.4f, 0, 0.03f);
    CHECK(!palmShown(&h, HEAD, 0));
    // Pinch: thumb on the index tip.
    h = hand(0.0f, 0.0f, -0.4f, 0, 0.09f);
    h.thumbTip = h.tips[0];
    h.thumbTip.x += 0.01f;
    CHECK(!palmShown(&h, HEAD, 0));
    // One curled finger is still open; two is not.
    h = hand(0.0f, 0.0f, -0.4f, 0, 0.09f);
    h.tips[3].y = 0.03f;
    CHECK(palmShown(&h, HEAD, 0));
    h.tips[2].y = 0.03f;
    CHECK(!palmShown(&h, HEAD, 0));
}

static void testHowLongItIsHeld(void) {
    PalmHold hold = { 0 };
    int frames = 0;
    int fired = 0;
    while (!fired && frames < 200) {
        fired = palmHoldStep(&hold, 1, FRAME);
        frames++;
    }
    // Fires once, after PALM_HOLD_SEC.
    CHECK(fired);
    CHECK_NEAR(frames * FRAME, PALM_HOLD_SEC, FRAME * 1.5f);
    for (int i = 0; i < 90; i++) {
        CHECK(!palmHoldStep(&hold, 1, FRAME));
    }
    // A one-frame drop restarts the hold.
    palmHoldStep(&hold, 0, FRAME);
    CHECK(!palmHoldStep(&hold, 1, FRAME));
    CHECK(hold.heldFor < PALM_HOLD_SEC);
    // A brief palm does not fire.
    PalmHold brief = { 0 };
    for (int i = 0; i < 10; i++) {
        CHECK(!palmHoldStep(&brief, 1, FRAME));
    }
    CHECK(!palmHoldStep(&brief, 0, FRAME));
}

int main(void) {
    testWhereItIs();
    testWhichWayItFaces();
    testHowOpenItIs();
    testHowLongItIsHeld();
    return checksDone("palm");
}
