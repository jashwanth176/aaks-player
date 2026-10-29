// Tests for xr_headaim: head turn to mouse pixels, and the pointer nudge.
#include <stdlib.h>

#include "check.h"
#include "xr_headaim.h"

#define PI_F 3.14159265358979f
#define RAD(deg) ((deg) * PI_F / 180.0f)
// 72 Hz, the lowest display rate.
#define FRAME_NS 13888889LL

// Head orientation from yaw, then pitch, then roll, in degrees.
static XrQuaternionf headAt(float yawDeg, float pitchDeg, float rollDeg) {
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    Vec3 back = { 0.0f, 0.0f, 1.0f };
    XrQuaternionf q = quatMul(axisAngleQuat(up, RAD(yawDeg)), axisAngleQuat(right, RAD(pitchDeg)));
    return quatNorm(quatMul(q, axisAngleQuat(back, RAD(rollDeg))));
}

// One tracked frame, going through the quaternion as the frame loop does.
static int stepAt(HeadAim* a, float yawDeg, float pitchDeg, int64_t timeNs, float sensitivity,
                  float deadZone, int* dx, int* dy) {
    float yaw, pitch;
    headAimAngles(headAt(yawDeg, pitchDeg, 0.0f), &yaw, &pitch);
    return headAimStep(a, HEAD_AIM_SENT, yaw, pitch, timeNs, sensitivity, deadZone, dx, dy);
}

// A steady turn over some frames; returns the total sent. The first frame only seeds.
static void turn(float yaw0, float pitch0, float yaw1, float pitch1, int frames,
                 float sensitivity, float deadZone, int* sumX, int* sumY, int* sentFrames) {
    HeadAim a;
    headAimReset(&a);
    int dx, dy;
    *sumX = 0;
    *sumY = 0;
    *sentFrames = 0;
    CHECK(stepAt(&a, yaw0, pitch0, 1000, sensitivity, deadZone, &dx, &dy) == HEAD_AIM_SEEDED);
    CHECK(dx == 0 && dy == 0);
    for (int i = 1; i <= frames; i++) {
        float t = (float)i / (float)frames;
        float yaw = yaw0 + (yaw1 - yaw0) * t;
        float pitch = pitch0 + (pitch1 - pitch0) * t;
        int what = stepAt(&a, yaw, pitch, 1000 + i * FRAME_NS, sensitivity, deadZone, &dx, &dy);
        if (what == HEAD_AIM_SENT) {
            (*sentFrames)++;
        }
        *sumX += dx;
        *sumY += dy;
    }
}

static void testTheSwitch(void) {
    CHECK(headAimSwitchOn(1, 0) == 1);
    CHECK(headAimSwitchOn(1, 1) == 0);
    CHECK(headAimSwitchOn(0, 0) == 0);
    CHECK(headAimSwitchOn(0, 1) == 1);
    for (int setting = 0; setting < 2; setting++) {
        for (int want = 0; want < 2; want++) {
            CHECK(headAimSwitchOn(setting, headAimFlipFor(setting, want)) == want);
        }
    }
}

static void testTheLanes(void) {
    CHECK(headAimSensitivityClamp(HEAD_AIM_SENSITIVITY_DEFAULT) == 8);
    CHECK(headAimSensitivityClamp(0) == 1);
    CHECK(headAimSensitivityClamp(31) == 30);
    CHECK(headAimSensitivityClamp(-5) == 1);
    CHECK(headAimDeadZoneClamp(HEAD_AIM_DEADZONE_DEFAULT) == 2);
    CHECK(headAimDeadZoneClamp(-1) == 0);
    CHECK(headAimDeadZoneClamp(0) == 0);
    CHECK(headAimDeadZoneClamp(21) == 20);
    for (int i = HEAD_AIM_SENSITIVITY_MIN; i <= HEAD_AIM_SENSITIVITY_MAX; i++) {
        CHECK(headAimSensitivityClamp(i) == i);
    }
    for (int i = HEAD_AIM_DEADZONE_MIN; i <= HEAD_AIM_DEADZONE_MAX; i++) {
        CHECK(headAimDeadZoneClamp(i) == i);
    }
}

static void testTheWrap(void) {
    CHECK_NEAR(headAimWrap(0.0f), 0.0, 1e-6);
    CHECK_NEAR(headAimWrap(RAD(10.0f)), RAD(10.0f), 1e-6);
    CHECK_NEAR(headAimWrap(RAD(350.0f)), RAD(-10.0f), 1e-5);
    CHECK_NEAR(headAimWrap(RAD(-350.0f)), RAD(10.0f), 1e-5);
    CHECK_NEAR(headAimWrap(RAD(720.0f + 30.0f)), RAD(30.0f), 1e-4);
    // +-pi both wrap to +pi.
    CHECK_NEAR(headAimWrap(PI_F), PI_F, 1e-6);
    CHECK_NEAR(headAimWrap(-PI_F), PI_F, 1e-6);
    // Across behind the head: 170 to -170 is 20 left, not 340 right.
    CHECK_NEAR(headAimWrap(RAD(-170.0f) - RAD(170.0f)), RAD(20.0f), 1e-5);
    CHECK_NEAR(headAimWrap(RAD(170.0f) - RAD(-170.0f)), RAD(-20.0f), 1e-5);
}

static void testTheAngles(void) {
    float yaw, pitch;
    headAimAngles(headAt(0.0f, 0.0f, 0.0f), &yaw, &pitch);
    CHECK_NEAR(yaw, 0.0, 1e-6);
    CHECK_NEAR(pitch, 0.0, 1e-6);
    // Left and up are positive.
    headAimAngles(headAt(30.0f, 0.0f, 0.0f), &yaw, &pitch);
    CHECK_NEAR(yaw, RAD(30.0f), 1e-5);
    headAimAngles(headAt(0.0f, 20.0f, 0.0f), &yaw, &pitch);
    CHECK_NEAR(pitch, RAD(20.0f), 1e-5);
    // Round trip over all yaws and pitches; roll affects neither.
    for (int y = -175; y <= 180; y += 25) {
        for (int p = -75; p <= 75; p += 25) {
            for (int r = -40; r <= 40; r += 40) {
                headAimAngles(headAt((float)y, (float)p, (float)r), &yaw, &pitch);
                CHECK_NEAR(headAimWrap(yaw - RAD((float)y)), 0.0, 1e-4);
                CHECK_NEAR(pitch, RAD((float)p), 1e-4);
            }
        }
    }
}

static void testWhatBlocks(void) {
    CHECK(headAimBlocked(1, 0, 1, 0) == HEAD_AIM_SENT);
    CHECK(headAimBlocked(0, 0, 1, 0) == HEAD_AIM_OFF);
    CHECK(headAimBlocked(1, 1, 1, 0) == HEAD_AIM_PAUSED);
    CHECK(headAimBlocked(1, 0, 0, 0) == HEAD_AIM_LOST);
    CHECK(headAimBlocked(1, 0, 1, 1) == HEAD_AIM_RECENTRED);
    // Priority when several apply.
    CHECK(headAimBlocked(0, 1, 0, 1) == HEAD_AIM_OFF);
    CHECK(headAimBlocked(1, 1, 0, 1) == HEAD_AIM_PAUSED);
    CHECK(headAimBlocked(1, 0, 0, 1) == HEAD_AIM_LOST);
}

// 90 degrees over 1 s at 72 Hz and 8 px/degree: 720 px with no dead zone. The
// default dead zone (2 deg/s) is subtracted from the rate: 88 * 8 = 704 px.
static void testAQuarterTurn(void) {
    int x, y, sent;
    turn(0.0f, 0.0f, -90.0f, 0.0f, 72, 8.0f, 0.0f, &x, &y, &sent);
    CHECK(x == 720);
    CHECK(y == 0);
    turn(0.0f, 0.0f, -90.0f, 0.0f, 72, 8.0f, 2.0f, &x, &y, &sent);
    CHECK(x == 704);
    CHECK(y == 0);
    CHECK(sent == 72);
    // Left, across behind the head.
    turn(135.0f, 0.0f, 225.0f, 0.0f, 72, 8.0f, 2.0f, &x, &y, &sent);
    CHECK(x == -704);
    CHECK(y == 0);
    // Up is negative y, same scale.
    turn(0.0f, -45.0f, 0.0f, 45.0f, 72, 8.0f, 2.0f, &x, &y, &sent);
    CHECK(x == 0);
    CHECK(y == -704);
    // Diagonal 60 and 30 degrees: 67.08 deg/s, 65.08 after the dead zone,
    // so 465.7 and 232.8 px.
    turn(10.0f, 5.0f, -50.0f, -25.0f, 72, 8.0f, 2.0f, &x, &y, &sent);
    CHECK(x == 466);
    CHECK(y == 233);
    turn(10.0f, 5.0f, -50.0f, -25.0f, 72, 8.0f, 0.0f, &x, &y, &sent);
    CHECK(x == 480);
    CHECK(y == 240);
    // Sub-pixel frames still accumulate: 9 deg/s less 2 is 7, so 70 of 90.
    turn(0.0f, 0.0f, -90.0f, 0.0f, 720, 1.0f, 2.0f, &x, &y, &sent);
    CHECK(x == 70);
    turn(0.0f, 0.0f, -90.0f, 0.0f, 720, 1.0f, 0.0f, &x, &y, &sent);
    CHECK(x == 90);
}

static void testTheSensitivity(void) {
    int x, y, sent;
    turn(0.0f, 0.0f, -10.0f, 0.0f, 36, 1.0f, 0.0f, &x, &y, &sent);
    CHECK(x == 10);
    turn(0.0f, 0.0f, -10.0f, 0.0f, 36, 30.0f, 0.0f, &x, &y, &sent);
    CHECK(x == 300);
    turn(0.0f, 0.0f, 0.0f, 10.0f, 36, 12.0f, 0.0f, &x, &y, &sent);
    CHECK(y == -120);
}

static void testTheDeadZone(void) {
    int x, y, sent;
    // 1 deg/s is under the 2 deg/s dead zone.
    turn(0.0f, 0.0f, -1.0f, 0.0f, 72, 8.0f, 2.0f, &x, &y, &sent);
    CHECK(x == 0);
    CHECK(sent == 0);
    // Exactly at it is still zero.
    turn(0.0f, 0.0f, -2.0f, 0.0f, 72, 8.0f, 2.0f, &x, &y, &sent);
    CHECK(x == 0);
    // 3 deg/s sends only the 1 above the dead zone: it is subtracted, not a
    // gate, so motion eases in.
    turn(0.0f, 0.0f, -3.0f, 0.0f, 72, 8.0f, 2.0f, &x, &y, &sent);
    CHECK(x == 8);
    CHECK(sent == 72);
    // Linear above the dead zone.
    int last = 0;
    for (int over = 1; over <= 6; over++) {
        turn(0.0f, 0.0f, -(2.0f + (float)over), 0.0f, 72, 8.0f, 2.0f, &x, &y, &sent);
        CHECK(x == 8 * over);
        CHECK(x > last);
        last = x;
    }
    // +-0.001 degree tracking jitter is ignored.
    HeadAim a;
    headAimReset(&a);
    int dx, dy, total = 0;
    stepAt(&a, 0.0f, 0.0f, 0, 8.0f, 2.0f, &dx, &dy);
    for (int i = 1; i <= 720; i++) {
        float wobble = (i % 2 == 0 ? 0.001f : -0.001f);
        CHECK(stepAt(&a, wobble, -wobble, i * FRAME_NS, 8.0f, 2.0f, &dx, &dy)
              == HEAD_AIM_STILL);
        total += abs(dx) + abs(dy);
    }
    CHECK(total == 0);
    // With no dead zone a slow drift goes through.
    turn(0.0f, 0.0f, -1.0f, 0.0f, 72, 8.0f, 0.0f, &x, &y, &sent);
    CHECK(x == 8);
    // The dead zone applies to the combined rate: 2.12 deg/s diagonal clears
    // it by 0.12, about 0.7 px on each axis.
    turn(0.0f, 0.0f, -1.5f, 1.5f, 72, 8.0f, 2.0f, &x, &y, &sent);
    CHECK(x == 1);
    CHECK(y == -1);
    CHECK(sent == 72);
}

// Sub-pixel remainders carry to the next frame.
static void testThePixelsCarry(void) {
    HeadAim a;
    headAimReset(&a);
    int dx, dy;
    stepAt(&a, 0.0f, 0.0f, 0, 1.0f, 2.0f, &dx, &dy);
    int total = 0;
    int sentAny = 0;
    int zeroFrames = 0;
    // 0.3 degree/frame (21.6 deg/s) at 1 px/degree.
    for (int i = 1; i <= 10; i++) {
        CHECK(stepAt(&a, -0.3f * (float)i, 0.0f, i * FRAME_NS, 1.0f, 2.0f, &dx, &dy)
              == HEAD_AIM_SENT);
        CHECK(dx == 0 || dx == 1);
        total += dx;
        sentAny |= dx != 0;
        zeroFrames += dx == 0;
    }
    CHECK(total == 3);
    CHECK(sentAny);
    CHECK(zeroFrames >= 6);
    CHECK(fabsf(a.carryX) <= 0.5f + 1e-4f);
}

static void testTheDrops(void) {
    HeadAim a;
    int dx, dy;
    float yaw, pitch;

    // Paused (panel open): frame dropped and the head forgotten; resume reseeds.
    headAimReset(&a);
    stepAt(&a, 0.0f, 0.0f, 0, 8.0f, 2.0f, &dx, &dy);
    CHECK(stepAt(&a, -2.0f, 0.0f, FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SENT);
    CHECK(dx == 16);
    headAimAngles(headAt(-45.0f, 10.0f, 0.0f), &yaw, &pitch);
    CHECK(headAimStep(&a, HEAD_AIM_PAUSED, yaw, pitch, 2 * FRAME_NS, 8.0f, 2.0f, &dx, &dy)
          == HEAD_AIM_PAUSED);
    CHECK(dx == 0 && dy == 0);
    CHECK(!a.seeded);
    CHECK(stepAt(&a, -60.0f, 20.0f, 3 * FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SEEDED);
    CHECK(dx == 0 && dy == 0);
    CHECK(stepAt(&a, -61.0f, 20.0f, 4 * FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SENT);
    CHECK(dx == 8 && dy == 0);

    // Recentre: frame dropped, next frame reseeds.
    headAimReset(&a);
    stepAt(&a, 0.0f, 0.0f, 0, 8.0f, 2.0f, &dx, &dy);
    headAimAngles(headAt(170.0f, 0.0f, 0.0f), &yaw, &pitch);
    CHECK(headAimStep(&a, HEAD_AIM_RECENTRED, yaw, pitch, FRAME_NS, 8.0f, 2.0f, &dx, &dy)
          == HEAD_AIM_RECENTRED);
    CHECK(dx == 0 && dy == 0);
    CHECK(stepAt(&a, 170.0f, 0.0f, 2 * FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SEEDED);
    CHECK(stepAt(&a, 169.0f, 0.0f, 3 * FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SENT);
    CHECK(dx == 8);

    // Tracking lost: nothing sent, including across the gap.
    headAimReset(&a);
    stepAt(&a, 0.0f, 0.0f, 0, 8.0f, 2.0f, &dx, &dy);
    CHECK(headAimStep(&a, HEAD_AIM_LOST, 0.0f, 0.0f, FRAME_NS, 8.0f, 2.0f, &dx, &dy)
          == HEAD_AIM_LOST);
    CHECK(dx == 0 && dy == 0);
    CHECK(stepAt(&a, -30.0f, 0.0f, 20 * FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SEEDED);
    CHECK(dx == 0);

    // Off clears the seed and the carry.
    headAimReset(&a);
    stepAt(&a, 0.0f, 0.0f, 0, 1.0f, 0.0f, &dx, &dy);
    stepAt(&a, -0.4f, 0.0f, FRAME_NS, 1.0f, 0.0f, &dx, &dy);
    CHECK(dx == 0 && a.carryX > 0.3f);
    CHECK(headAimStep(&a, HEAD_AIM_OFF, 0.0f, 0.0f, 2 * FRAME_NS, 1.0f, 0.0f, &dx, &dy)
          == HEAD_AIM_OFF);
    CHECK(a.carryX == 0.0f);

    // An impossibly fast jump is dropped; the next frame measures from it.
    headAimReset(&a);
    stepAt(&a, 0.0f, 0.0f, 0, 8.0f, 2.0f, &dx, &dy);
    CHECK(stepAt(&a, -60.0f, 0.0f, FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_JUMPED);
    CHECK(dx == 0 && dy == 0);
    CHECK(stepAt(&a, -61.0f, 0.0f, 2 * FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SENT);
    CHECK(dx == 8);
    // A 600 deg/s flick is not a jump: (600 - 2) / 72 * 8 = 66 px.
    headAimReset(&a);
    stepAt(&a, 0.0f, 0.0f, 0, 8.0f, 2.0f, &dx, &dy);
    CHECK(stepAt(&a, -600.0f / 72.0f, 0.0f, FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SENT);
    CHECK(dx == 66);

    // A 0.5 s gap is a stall and reseeds; just under is a turn.
    headAimReset(&a);
    stepAt(&a, 0.0f, 0.0f, 0, 8.0f, 2.0f, &dx, &dy);
    CHECK(stepAt(&a, -30.0f, 0.0f, 500000000LL, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SEEDED);
    CHECK(dx == 0 && dy == 0);
    CHECK(stepAt(&a, -31.0f, 0.0f, 500000000LL + FRAME_NS, 8.0f, 2.0f, &dx, &dy)
          == HEAD_AIM_SENT);
    CHECK(dx == 8);
    headAimReset(&a);
    stepAt(&a, 0.0f, 0.0f, 0, 8.0f, 2.0f, &dx, &dy);
    CHECK(stepAt(&a, -30.0f, 0.0f, 490000000LL, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SENT);
    CHECK(dx == 232);

    // A repeated display time sends nothing and keeps the previous head.
    headAimReset(&a);
    stepAt(&a, 0.0f, 0.0f, FRAME_NS, 8.0f, 2.0f, &dx, &dy);
    CHECK(stepAt(&a, -5.0f, 0.0f, FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_STILL);
    CHECK(dx == 0);
    CHECK(stepAt(&a, -6.0f, 0.0f, 2 * FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SENT);
    CHECK(dx == 48);
}

// Near vertical, yaw is noise and only pitch is sent.
static void testNearVertical(void) {
    HeadAim a;
    int dx, dy;
    headAimReset(&a);
    stepAt(&a, 0.0f, 85.0f, 0, 8.0f, 2.0f, &dx, &dy);
    CHECK(stepAt(&a, -20.0f, 84.0f, FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SENT);
    CHECK(dx == 0);
    CHECK(dy == 8);
    // Below the limit yaw counts again.
    headAimReset(&a);
    stepAt(&a, 0.0f, 70.0f, 0, 8.0f, 2.0f, &dx, &dy);
    CHECK(stepAt(&a, -2.0f, 70.0f, FRAME_NS, 8.0f, 2.0f, &dx, &dy) == HEAD_AIM_SENT);
    CHECK(dx == 16);
}

static void testTheNudge(void) {
    PointerNudge n;
    pointerNudgeReset(&n);
    int dx, dy;
    // The first frame only seeds.
    pointerNudge(&n, 1, 0.50f, 0.50f, 10, 2560, 1440, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
    // Moves by the UV difference in picture pixels: 0.01 * 2560 = 26.
    pointerNudge(&n, 1, 0.51f, 0.49f, 11, 2560, 1440, &dx, &dy);
    CHECK(dx == 26);
    CHECK(dy == -14);
    // Sub-pixel moves accumulate.
    int total = 0;
    float u = 0.51f;
    for (long f = 12; f < 22; f++) {
        u += 0.0001f;
        pointerNudge(&n, 1, u, 0.49f, f, 2560, 1440, &dx, &dy);
        total += dx;
    }
    CHECK(total >= 2 && total <= 3);
    CHECK(fabsf(n.carryX) <= 0.5f + 1e-3f);
    // A frame gap reseeds instead of jumping.
    pointerNudge(&n, 1, 0.10f, 0.90f, 30, 2560, 1440, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
    pointerNudge(&n, 1, 0.11f, 0.90f, 31, 2560, 1440, &dx, &dy);
    CHECK(dx == 26 && dy == 0);
    // A hand change reseeds.
    pointerNudge(&n, 0, 0.80f, 0.20f, 32, 2560, 1440, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
    pointerNudge(&n, 0, 0.79f, 0.20f, 33, 2560, 1440, &dx, &dy);
    CHECK(dx == -26 && dy == 0);
    pointerNudgeReset(&n);
    pointerNudge(&n, 0, 0.70f, 0.20f, 34, 2560, 1440, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
}

int main(void) {
    testTheSwitch();
    testTheLanes();
    testTheWrap();
    testTheAngles();
    testWhatBlocks();
    testAQuarterTurn();
    testTheSensitivity();
    testTheDeadZone();
    testThePixelsCarry();
    testTheDrops();
    testNearVertical();
    testTheNudge();
    return checksDone("xr_headaim");
}
