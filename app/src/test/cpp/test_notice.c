// Tests for xr_notice: panel fades, splash timing and wedges.
#include "check.h"
#include "xr_notice.h"
#include "xr_shared.h"

#define MS 1000000LL
// ~72 Hz frame.
#define FRAME (14 * MS)

static void testSplashHoldsItsFloor(void) {
    // Ready at once still holds the splash for SPLASH_FLOOR_NS.
    Splash splash = { 0 };
    long long start = 2000 * MS;
    long long now = start;
    CHECK(splashStep(&splash, now, 0, SPLASH_FADE_NS) == 0);
    CHECK(splash.firstNs == start);
    CHECK_NEAR(splashLevel(&splash, now, SPLASH_FADE_NS), 1.0, 1e-6);
    int lifted = 0;
    while (!lifted && now - start < 10000 * MS) {
        now += FRAME;
        lifted = splashStep(&splash, now, 0, SPLASH_FADE_NS);
    }
    CHECK(lifted);
    CHECK(now - start >= SPLASH_FLOOR_NS);
    CHECK(now - start < SPLASH_FLOOR_NS + FRAME);
    CHECK(splash.waitingAtLift == 0);
    CHECK(splash.phase == SPLASH_FADING);

    // Then a 300 ms fade.
    CHECK_NEAR(splashLevel(&splash, now, SPLASH_FADE_NS), 1.0, 1e-6);
    CHECK_NEAR(splashLevel(&splash, now + 150 * MS, SPLASH_FADE_NS), 0.5, 1e-4);
    now += 299 * MS;
    splashStep(&splash, now, 0, SPLASH_FADE_NS);
    CHECK(splash.phase == SPLASH_FADING);
    now += 2 * MS;
    CHECK(splashStep(&splash, now, 0, SPLASH_FADE_NS) == 0);
    CHECK(splash.phase == SPLASH_GONE);
    CHECK_NEAR(splashLevel(&splash, now, SPLASH_FADE_NS), 0.0, 1e-6);
}

static void testSplashWaitsThenGivesUp(void) {
    // Waiting past the floor keeps it up until the last wait clears.
    Splash splash = { 0 };
    long long start = 100 * MS;
    splashStep(&splash, start, SPLASH_WAIT_PANELS | SPLASH_WAIT_DEPTH, SPLASH_FADE_NS);
    CHECK(splashStep(&splash, start + 3000 * MS, SPLASH_WAIT_DEPTH, SPLASH_FADE_NS) == 0);
    CHECK(splash.phase == SPLASH_UP);
    CHECK(splashStep(&splash, start + 3100 * MS, 0, SPLASH_FADE_NS) == 1);
    CHECK(splash.waitingAtLift == 0);
    CHECK(splash.liftNs == start + 3100 * MS);

    // Gives up at the ceiling and records what was still pending.
    Splash stuck = { 0 };
    splashStep(&stuck, start, SPLASH_WAIT_DEPTH, SPLASH_FADE_NS);
    CHECK(splashStep(&stuck, start + SPLASH_CEILING_NS - MS, SPLASH_WAIT_DEPTH,
                     SPLASH_FADE_NS) == 0);
    CHECK(splashStep(&stuck, start + SPLASH_CEILING_NS, SPLASH_WAIT_DEPTH, SPLASH_FADE_NS) == 1);
    CHECK(stuck.waitingAtLift == SPLASH_WAIT_DEPTH);
    // Lift reported once.
    CHECK(splashStep(&stuck, start + SPLASH_CEILING_NS + FRAME, SPLASH_WAIT_DEPTH,
                     SPLASH_FADE_NS) == 0);
}

static void testSplashCutsWithoutAFade(void) {
    Splash splash = { 0 };
    splashStep(&splash, 10 * MS, 0, 0);
    CHECK(splashStep(&splash, 10 * MS + SPLASH_FLOOR_NS, 0, 0) == 1);
    CHECK(splash.phase == SPLASH_GONE);
    CHECK_NEAR(splashLevel(&splash, 10 * MS + SPLASH_FLOOR_NS, 0), 0.0, 1e-6);
}

static void testSplashWedgesTick(void) {
    Splash splash = { 0 };
    CHECK(splashWedges(&splash, 500 * MS) == 0);
    long long start = 1000 * MS;
    splashStep(&splash, start, SPLASH_WAIT_DEPTH, SPLASH_FADE_NS);
    // One wedge per 250 ms tick after the first: 1, 3, 5 at 0.3, 0.8, 1.3 s; all six at the floor.
    CHECK(splashWedges(&splash, start) == 0);
    CHECK(splashWedges(&splash, start + 249 * MS) == 0);
    CHECK(splashWedges(&splash, start + 250 * MS) == 1);
    CHECK(splashWedges(&splash, start + 300 * MS) == 1);
    CHECK(splashWedges(&splash, start + 800 * MS) == 3);
    CHECK(splashWedges(&splash, start + 1300 * MS) == 5);
    CHECK(splashWedges(&splash, start + 1499 * MS) == 5);
    CHECK(splashWedges(&splash, start + SPLASH_FLOOR_NS) == SPLASH_WEDGES);
    // Held for 500 ms, then cleared and repeated.
    CHECK(splashWedges(&splash, start + 1999 * MS) == SPLASH_WEDGES);
    CHECK(splashWedges(&splash, start + 2000 * MS) == 0);
    CHECK(splashWedges(&splash, start + 2250 * MS) == 1);

    // Over four 2 s rounds: counts step up by one or clear from full.
    int seen[SPLASH_WEDGES + 1] = { 0 };
    int last = 0;
    int steps = 0;
    for (long long t = 0; t < 8000; t++) {
        int open = splashWedges(&splash, start + t * MS);
        CHECK(open >= 0 && open <= SPLASH_WEDGES);
        if (open != last) {
            CHECK(open == last + 1 || (last == SPLASH_WEDGES && open == 0));
            steps++;
        }
        seen[open]++;
        last = open;
    }
    CHECK(steps == 4 * (SPLASH_WEDGES + 1) - 1);
    for (int open = 0; open < SPLASH_WEDGES; open++) {
        CHECK(seen[open] == 4 * 250);
    }
    CHECK(seen[SPLASH_WEDGES] == 4 * 500);

    // On lift all wedges show through the fade, wherever the round was.
    CHECK(splashStep(&splash, start + 2100 * MS, SPLASH_WAIT_DEPTH, SPLASH_FADE_NS) == 0);
    CHECK(splashWedges(&splash, start + 2100 * MS) == 0);
    CHECK(splashStep(&splash, start + 2110 * MS, 0, SPLASH_FADE_NS) == 1);
    CHECK(splashWedges(&splash, start + 2110 * MS) == SPLASH_WEDGES);
    CHECK(splashWedges(&splash, start + 2110 * MS + SPLASH_FADE_NS / 2) == SPLASH_WEDGES);
}

static void testPanelsComeAndGo(void) {
    // In over 240 ms, out over 150 ms, from either end and from part way
    float level = 0.0f;
    level = panelStep(level, 1, 120 * MS);
    CHECK(level > 0.49f && level < 0.51f);
    level = panelStep(level, 1, 200 * MS);
    CHECK(level == 1.0f);
    level = panelStep(level, 0, 75 * MS);
    CHECK(level > 0.49f && level < 0.51f);
    level = panelStep(level, 0, 100 * MS);
    CHECK(level == 0.0f);
    CHECK(panelStep(0.3f, 1, -5 * MS) == 0.3f);

    // Eased out: fast at first, settling at the end, never past either end
    CHECK(panelEase(0.0f) == 0.0f && panelEase(1.0f) == 1.0f);
    CHECK(panelEase(0.5f) > 0.85f);
    CHECK(panelEase(-1.0f) == 0.0f && panelEase(2.0f) == 1.0f);
    float last = 0.0f;
    for (int i = 1; i <= 100; i++) {
        float e = panelEase(i / 100.0f);
        CHECK(e >= last);
        last = e;
    }
    // Grown and risen to rest when shown, smaller and lower on the way in
    CHECK(panelGrow(1.0f) == 1.0f && panelDrop(1.0f) == 0.0f);
    CHECK(panelGrow(0.0f) == PANEL_GROW_FROM);
    CHECK(panelDrop(0.0f) == PANEL_RISE);
}

int main(void) {
    testPanelsComeAndGo();
    testSplashHoldsItsFloor();
    testSplashWaitsThenGivesUp();
    testSplashCutsWithoutAFade();
    testSplashWedgesTick();
    return checksDone("xr_notice");
}
