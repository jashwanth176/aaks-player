// Tests for ray visibility, controller model placement and pointer dot size.
#include "check.h"
#include "xr_controller.h"
#include "xr_gate.h"

static void testRaySwitch(void) {
    // The setting is the initial state; a flip inverts it.
    CHECK(raySwitchOn(1, 0) == 1);
    CHECK(raySwitchOn(1, 1) == 0);
    CHECK(raySwitchOn(0, 0) == 0);
    CHECK(raySwitchOn(0, 1) == 1);
    // Any non-zero counts as set.
    CHECK(raySwitchOn(5, 0) == 1);
    CHECK(raySwitchOn(5, 7) == 0);

    // rayFlipFor reaches the wanted state from either setting.
    for (int setting = 0; setting < 2; setting++) {
        for (int want = 0; want < 2; want++) {
            CHECK(raySwitchOn(setting, rayFlipFor(setting, want)) == want);
        }
    }
    CHECK(rayFlipFor(1, 1) == 0);
    CHECK(rayFlipFor(0, 0) == 0);
}

static void testRayDrawn(void) {
    // Drawn while the switch is on or any panel is open.
    for (int setting = 0; setting < 2; setting++) {
        for (int flipped = 0; flipped < 2; flipped++) {
            for (int panel = 0; panel < 2; panel++) {
                int on = setting != flipped;
                CHECK(rayDrawn(setting, flipped, panel) == (on || panel));
            }
        }
    }

    // Setting off: shown only while a panel is open.
    CHECK(!rayDrawn(0, 0, 0));
    CHECK(rayDrawn(0, 0, 1));
    CHECK(!rayDrawn(0, 0, 0));
    // Setting off, flipped on from the bar.
    CHECK(rayDrawn(0, 1, 0));
    CHECK(rayDrawn(0, 1, 1));
    // Setting on, flipped off: same as setting off.
    CHECK(!rayDrawn(1, 1, 0));
    CHECK(rayDrawn(1, 1, 1));
}

static void testModelShown(void) {
    const unsigned located = GATE_POSITION_VALID | GATE_ORIENTATION_VALID
            | GATE_ORIENTATION_TRACKED;
    const unsigned all = located | GATE_POSITION_TRACKED;
    CHECK(controllerModelShown(1, 0, PROFILE_CONTROLLER, 1, all));
    // Position not tracked (runtime estimate) still shows the model.
    CHECK(controllerModelShown(1, 0, PROFILE_CONTROLLER, 1, located));
    // Each condition alone hides it: setting, passthrough, profile, inactive
    // grip, untracked orientation, invalid position or orientation.
    CHECK(!controllerModelShown(0, 0, PROFILE_CONTROLLER, 1, all));
    CHECK(!controllerModelShown(1, 1, PROFILE_CONTROLLER, 1, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_HANDS, 1, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_NONE, 1, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_UNKNOWN, 1, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 0, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 1,
                                GATE_POSITION_VALID | GATE_ORIENTATION_VALID));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 1,
                                GATE_ORIENTATION_VALID | GATE_ORIENTATION_TRACKED));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 1,
                                GATE_POSITION_VALID | GATE_ORIENTATION_TRACKED));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 1, 0));
}

// Column-major matrix times point.
static Vec3 apply(const float* m, Vec3 p) {
    Vec3 r = { m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
               m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
               m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] };
    return r;
}

static float det3(const float* m) {
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2])
            + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

static XrPosef pose(float x, float y, float z, XrQuaternionf q) {
    XrPosef p;
    p.orientation = q;
    p.position.x = x;
    p.position.y = y;
    p.position.z = z;
    return p;
}

static void testModelMatrix(void) {
    float m[16];
    XrQuaternionf none = { 0.0f, 0.0f, 0.0f, 1.0f };

    // Identity pose: right hand is identity, left hand mirrors x.
    controllerModelMatrix(pose(0, 0, 0, none), 0, m);
    for (int i = 0; i < 16; i++) {
        CHECK_NEAR(m[i], i % 5 == 0 ? 1.0 : 0.0, 1e-6);
    }
    controllerModelMatrix(pose(0, 0, 0, none), 1, m);
    for (int i = 0; i < 16; i++) {
        CHECK_NEAR(m[i], i == 0 ? -1.0 : (i % 5 == 0 ? 1.0 : 0.0), 1e-6);
    }

    // Grip position goes in the translation column only.
    controllerModelMatrix(pose(0.2f, -0.3f, -0.5f, none), 0, m);
    CHECK_NEAR(m[12], 0.2, 1e-6);
    CHECK_NEAR(m[13], -0.3, 1e-6);
    CHECK_NEAR(m[14], -0.5, 1e-6);
    CHECK_NEAR(m[15], 1.0, 1e-6);
    CHECK_NEAR(m[3], 0.0, 1e-6);
    CHECK_NEAR(m[7], 0.0, 1e-6);
    CHECK_NEAR(m[11], 0.0, 1e-6);

    // +90 degrees about up: right maps to forward, forward to left.
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    XrQuaternionf quarter = axisAngleQuat(up, (float)M_PI * 0.5f);
    controllerModelMatrix(pose(0, 0, 0, quarter), 0, m);
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    Vec3 forward = { 0.0f, 0.0f, -1.0f };
    Vec3 r = apply(m, right);
    Vec3 f = apply(m, forward);
    CHECK_NEAR(r.x, 0.0, 1e-6);
    CHECK_NEAR(r.z, -1.0, 1e-6);
    CHECK_NEAR(f.x, -1.0, 1e-6);
    CHECK_NEAR(f.z, 0.0, 1e-6);

    // Arbitrary grip: matrix agrees with controllerModelPoint for both hands,
    // and the left hand equals the right hand with x mirrored.
    Vec3 axis = vecNorm((Vec3){ 0.3f, -0.8f, 0.5f });
    XrPosef grip = pose(-0.12f, 1.05f, -0.4f, axisAngleQuat(axis, 2.1f));
    Vec3 points[] = { { 0.0f, 0.0f, 0.0f }, { 0.006f, 0.093f, -0.024f },
                      { 0.0f, -0.055f, 0.022f }, { -0.008f, 0.054f, -0.016f },
                      { 0.03f, -0.01f, -0.06f } };
    for (int hand = 0; hand < 2; hand++) {
        controllerModelMatrix(grip, hand, m);
        for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
            Vec3 a = apply(m, points[i]);
            Vec3 b = controllerModelPoint(grip, hand, points[i]);
            CHECK_NEAR(a.x, b.x, 1e-6);
            CHECK_NEAR(a.y, b.y, 1e-6);
            CHECK_NEAR(a.z, b.z, 1e-6);
            Vec3 mirrored = { -points[i].x, points[i].y, points[i].z };
            Vec3 c = controllerModelPoint(grip, !hand, mirrored);
            CHECK_NEAR(b.x, c.x, 1e-6);
            CHECK_NEAR(b.y, c.y, 1e-6);
            CHECK_NEAR(b.z, c.z, 1e-6);
        }
        // Orthonormal; determinant -1 for the mirrored left hand.
        CHECK_NEAR(det3(m), hand ? -1.0 : 1.0, 1e-5);
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                float dot = m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1]
                        + m[a * 4 + 2] * m[b * 4 + 2];
                CHECK_NEAR(dot, a == b ? 1.0 : 0.0, 1e-5);
            }
        }
    }

    // Points on the x = 0 plane land in the same place for both hands.
    controllerModelMatrix(grip, 0, m);
    float left[16];
    controllerModelMatrix(grip, 1, left);
    Vec3 onPlane = { 0.0f, 0.04f, -0.03f };
    Vec3 a = apply(m, onPlane);
    Vec3 b = apply(left, onPlane);
    CHECK_NEAR(a.x, b.x, 1e-6);
    CHECK_NEAR(a.y, b.y, 1e-6);
    CHECK_NEAR(a.z, b.z, 1e-6);
    // Only the first column differs, by sign.
    for (int i = 0; i < 16; i++) {
        CHECK_NEAR(left[i], i < 3 ? -m[i] : m[i], 1e-6);
    }
}

// Angular size of the dot at distance metres.
static double dotAngle(float distance) {
    return 2.0 * atan(0.5 * pointerDotSize(distance) / distance);
}

static void testDotSize(void) {
    // Reference: 2.2 cm at the default 3 m screen distance.
    CHECK_NEAR(pointerDotSize(3.0f), 0.022, 1e-6);
    // Size scales with distance, so the angular size is constant.
    CHECK_NEAR(pointerDotSize(1.0f), 0.022 / 3.0, 1e-6);
    CHECK_NEAR(pointerDotSize(14.0f), 0.022 * 14.0 / 3.0, 1e-6);
    CHECK_NEAR(dotAngle(1.0f), dotAngle(3.0f), 1e-6);
    CHECK_NEAR(dotAngle(14.0f), dotAngle(3.0f), 1e-6);
    CHECK_NEAR(dotAngle(16.0f), dotAngle(3.0f), 1e-6);
    // Monotonic, clamped, and constant-angle between the clamps.
    float floorAt = PTR_DOT_MIN_M * PTR_DOT_REF_M / PTR_DOT_SIZE_M;
    float ceilingAt = PTR_DOT_MAX_M * PTR_DOT_REF_M / PTR_DOT_SIZE_M;
    CHECK(floorAt < 1.0f && ceilingAt > 16.0f);
    int off = 0;
    float last = 0.0f;
    for (float d = 0.05f; d < 40.0f; d += 0.05f) {
        float size = pointerDotSize(d);
        if (size < last) off++;
        last = size;
        if (size < PTR_DOT_MIN_M || size > PTR_DOT_MAX_M) off++;
        if (d > floorAt + 0.01f && d < ceilingAt - 0.01f
                && fabs(dotAngle(d) - dotAngle(3.0f)) > 1e-5) {
            off++;
        }
    }
    CHECK(off == 0);
    // Clamped near and far; zero or negative distance gives the minimum.
    CHECK(pointerDotSize(0.2f) == PTR_DOT_MIN_M);
    CHECK(pointerDotSize(100.0f) == PTR_DOT_MAX_M);
    CHECK(pointerDotSize(0.0f) == PTR_DOT_MIN_M);
    CHECK(pointerDotSize(-1.0f) == PTR_DOT_MIN_M);
}

int main(void) {
    testRaySwitch();
    testRayDrawn();
    testModelShown();
    testModelMatrix();
    testDotSize();
    return checksDone("xr_controller");
}
