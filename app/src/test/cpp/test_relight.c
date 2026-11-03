// Tests for xr_relight: how the immersion setting sizes the glow and covers the room.
#include "check.h"
#include "xr_relight.h"

static void testImmersion(void) {
    // None: the plain glow, nothing covered.
    CHECK_NEAR(relightGlowScale(0.0f), RELIGHT_GLOW_SCALE, 1e-5f);
    CHECK_NEAR(relightCover(0.0f), 0.0f, 1e-5f);
    CHECK_NEAR(relightReach(0.0f), 1.0f, 1e-5f);
    // Full: the largest glow over most of the room.
    CHECK_NEAR(relightGlowScale(1.0f), RELIGHT_GLOW_SCALE_FULL, 1e-5f);
    CHECK_NEAR(relightCover(1.0f), RELIGHT_COVER_FULL, 1e-5f);
    CHECK_NEAR(relightReach(1.0f), RELIGHT_REACH_FULL, 1e-5f);
    // The top half of the slider does most of it.
    CHECK(relightCover(0.5f) < RELIGHT_COVER_FULL * 0.3f);
    // Rising all the way, and clamped outside 0..1.
    for (int i = 1; i <= 10; i++) {
        CHECK(relightCover(i / 10.0f) > relightCover((i - 1) / 10.0f));
        CHECK(relightGlowScale(i / 10.0f) > relightGlowScale((i - 1) / 10.0f));
    }
    CHECK_NEAR(relightCover(2.0f), RELIGHT_COVER_FULL, 1e-5f);
    CHECK_NEAR(relightReach(-1.0f), 1.0f, 1e-5f);
}

static void testHouseLights(void) {
    // Up: plain passthrough whatever the picture. Down: the picture's dimming untouched.
    float dim[3] = { 0.2f, 0.1f, 0.05f };
    float up[3] = { 0.2f, 0.1f, 0.05f };
    relightHouseMix(up, 1.0f);
    for (int i = 0; i < 3; i++) {
        CHECK_NEAR(up[i], 1.0f, 1e-5f);
    }
    float down[3] = { 0.2f, 0.1f, 0.05f };
    relightHouseMix(down, 0.0f);
    for (int i = 0; i < 3; i++) {
        CHECK_NEAR(down[i], dim[i], 1e-5f);
    }
    float half[3] = { 0.2f, 0.1f, 0.05f };
    relightHouseMix(half, 0.5f);
    CHECK_NEAR(half[0], 0.6f, 1e-5f);
    CHECK_NEAR(half[2], 0.525f, 1e-5f);
    // In the dark the room shows a little, never fully.
    CHECK_NEAR(relightHouseDark(0.0f), 0.0f, 1e-5f);
    CHECK_NEAR(relightHouseDark(1.0f), RELIGHT_HOUSE_DARK, 1e-5f);
    CHECK_NEAR(relightHouseDark(3.0f), RELIGHT_HOUSE_DARK, 1e-5f);
    // The fade is slower than the ambient light's and settles within a few seconds.
    float level = 1.0f;
    for (int i = 0; i < 30; i++) {
        level = relightEaseHouse(level, 0.0f, 1.0f / 60.0f);
    }
    CHECK(level > 0.3f && level < 0.7f);
    for (int i = 0; i < 300; i++) {
        level = relightEaseHouse(level, 0.0f, 1.0f / 60.0f);
    }
    CHECK(level < 0.01f);
    CHECK_NEAR(relightEaseHouse(0.0f, 1.0f, 100.0f), 1.0f, 1e-5f);
}

int main(void) {
    testImmersion();
    testHouseLights();
    return checksDone("relight");
}
