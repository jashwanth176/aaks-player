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

int main(void) {
    testImmersion();
    return checksDone("relight");
}
