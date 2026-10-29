// Tests for xr_notice: panel fades, splash timing and wedges, and the toast queue.
#include "check.h"
#include "xr_notice.h"
#include "xr_shared.h"

#define MS 1000000LL
// ~72 Hz frame.
#define FRAME (14 * MS)

static void testFadeRisesAndLands(void) {
    Fade fade = { 0 };
    long long now = 1000 * MS;
    // First frame has no previous time, so the level does not move.
    CHECK(fadeStep(&fade, 1, now, FADE_NS, 1) == FADE_NONE);
    CHECK_NEAR(fade.level, 0.0, 1e-6);

    int done = FADE_NONE;
    int frames = 0;
    while (done == FADE_NONE && frames < 100) {
        now += FRAME;
        done = fadeStep(&fade, 1, now, FADE_NS, 1);
        frames++;
        if (done == FADE_NONE) {
            CHECK(fade.level > 0.0f && fade.level < 1.0f);
        }
    }
    CHECK(done == FADE_IN_DONE);
    CHECK_NEAR(fade.level, 1.0, 1e-6);
    // 150 ms at 14 ms/frame completes on the 11th frame after the first.
    CHECK(frames == 11);
    CHECK(fade.frames == 12);
    CHECK(now - fade.fromNs == 11 * FRAME);

    // Holding reports nothing further.
    now += FRAME;
    CHECK(fadeStep(&fade, 1, now, FADE_NS, 1) == FADE_NONE);
    CHECK_NEAR(fade.level, 1.0, 1e-6);
}

static void testFadeFallsAndTurnsRound(void) {
    Fade fade = { 0 };
    long long now = 5000 * MS;
    fadeStep(&fade, 1, now, FADE_NS, 0);
    CHECK_NEAR(fade.level, 1.0, 1e-6);

    // Reversing mid-fade continues from the current level and restarts the run.
    now += FRAME;
    fadeStep(&fade, 0, now, FADE_NS, 1);
    for (int i = 0; i < 4; i++) {
        now += FRAME;
        CHECK(fadeStep(&fade, 0, now, FADE_NS, 1) == FADE_NONE);
    }
    float halfway = fade.level;
    CHECK_NEAR(halfway, 1.0 - 5.0 * 14.0 / 150.0, 1e-4);
    now += FRAME;
    CHECK(fadeStep(&fade, 1, now, FADE_NS, 1) == FADE_NONE);
    CHECK(fade.level > halfway);
    CHECK(fade.fromNs == now);
    CHECK(fade.frames == 1);

    int done = FADE_NONE;
    for (int i = 0; i < 100 && done == FADE_NONE; i++) {
        now += FRAME;
        done = fadeStep(&fade, 1, now, FADE_NS, 1);
    }
    CHECK(done == FADE_IN_DONE);

    done = FADE_NONE;
    for (int i = 0; i < 100 && done == FADE_NONE; i++) {
        now += FRAME;
        done = fadeStep(&fade, 0, now, FADE_NS, 1);
    }
    CHECK(done == FADE_OUT_DONE);
    CHECK_NEAR(fade.level, 0.0, 1e-6);
}

static void testFadeWithoutTheExtensionCuts(void) {
    // Without layer fade support the level snaps.
    Fade fade = { 0 };
    long long now = 100 * MS;
    CHECK(fadeStep(&fade, 1, now, FADE_NS, 0) == FADE_NONE);
    CHECK_NEAR(fade.level, 1.0, 1e-6);
    now += FRAME;
    CHECK(fadeStep(&fade, 0, now, FADE_NS, 0) == FADE_NONE);
    CHECK_NEAR(fade.level, 0.0, 1e-6);
    // Zero-length fade also snaps.
    now += FRAME;
    CHECK(fadeStep(&fade, 1, now, 0, 1) == FADE_NONE);
    CHECK_NEAR(fade.level, 1.0, 1e-6);
}

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
    splashStep(&splash, start, SPLASH_WAIT_ROOM, SPLASH_FADE_NS);
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
    CHECK(splashStep(&splash, start + 2100 * MS, SPLASH_WAIT_ROOM, SPLASH_FADE_NS) == 0);
    CHECK(splashWedges(&splash, start + 2100 * MS) == 0);
    CHECK(splashStep(&splash, start + 2110 * MS, 0, SPLASH_FADE_NS) == 1);
    CHECK(splashWedges(&splash, start + 2110 * MS) == SPLASH_WEDGES);
    CHECK(splashWedges(&splash, start + 2110 * MS + SPLASH_FADE_NS / 2) == SPLASH_WEDGES);
}

static void testNoticeShowsForItsTime(void) {
    NoticeBoard board;
    noticeInit(&board);
    Notice up = { -1, 0 };
    long long now = 1000 * MS;
    CHECK(!noticeShowing(&board, now));
    CHECK(noticeAdvance(&board, now, 0, &up) == 0);

    noticePush(&board, TOAST_RATE, 90);
    CHECK(noticeAdvance(&board, now, 0, &up) == 1);
    CHECK(up.kind == TOAST_RATE && up.arg == 90);
    CHECK(noticeShowing(&board, now));
    // Reported once, shown for NOTICE_SHOW_NS.
    CHECK(noticeAdvance(&board, now + FRAME, 0, &up) == 0);
    CHECK(noticeShowing(&board, now + NOTICE_SHOW_NS - MS));
    noticeAdvance(&board, now + NOTICE_SHOW_NS, 0, &up);
    CHECK(!noticeShowing(&board, now + NOTICE_SHOW_NS));
    CHECK(board.current.kind == -1);
}

static void testNoticeOfTheSameKindReplacesAtOnce(void) {
    NoticeBoard board;
    noticeInit(&board);
    Notice up = { -1, 0 };
    long long now = 0;
    noticePush(&board, TOAST_HANDS_LOCKED, 0);
    CHECK(noticeAdvance(&board, now, 0, &up) == 1);
    // Same group replaces immediately.
    now += 200 * MS;
    noticePush(&board, TOAST_HANDS_UNLOCKED, 0);
    CHECK(noticeAdvance(&board, now, 0, &up) == 1);
    CHECK(up.kind == TOAST_HANDS_UNLOCKED);
    // And restarts the show time.
    CHECK(noticeShowing(&board, now + NOTICE_SHOW_NS - MS));

    CHECK(noticeGroup(TOAST_3D_OFF) == noticeGroup(TOAST_3D_ON));
    CHECK(noticeGroup(TOAST_RATE) != noticeGroup(TOAST_3D_ON));
    CHECK(noticeGroup(TOAST_HANDS_LOCKED) != noticeGroup(TOAST_3D_OFF));
    CHECK(noticeGroup(TOAST_TEXT) != noticeGroup(TOAST_RATE));
    CHECK(noticeGroup(TOAST_HEAD_AIM_OFF) == noticeGroup(TOAST_HEAD_AIM_ON));
    CHECK(noticeGroup(TOAST_HEAD_AIM_ON) != noticeGroup(TOAST_3D_ON));
    CHECK(noticeGroup(TOAST_HEAD_AIM_ON) != noticeGroup(TOAST_TEXT));
    CHECK(noticeGroup(TOAST_GAMEPAD_MODE) == noticeGroup(TOAST_POINTER_MODE));
    CHECK(noticeGroup(TOAST_GAMEPAD_MODE) != noticeGroup(TOAST_HEAD_AIM_ON));
    CHECK(noticeGroup(TOAST_POINTER_MODE) != noticeGroup(TOAST_TEXT));
    CHECK(TOAST_GAMEPAD_MODE == TOAST_HEAD_AIM_ON + 1);
    CHECK(TOAST_POINTER_MODE == TOAST_GAMEPAD_MODE + 1);
}

static void testAnotherKindWaitsItsTurn(void) {
    NoticeBoard board;
    noticeInit(&board);
    Notice up = { -1, 0 };
    long long start = 500 * MS;
    noticePush(&board, TOAST_3D_OFF, 0);
    noticeAdvance(&board, start, 0, &up);
    // A different group waits for NOTICE_MIN_NS.
    noticePush(&board, TOAST_RATE, 72);
    CHECK(noticeAdvance(&board, start + 100 * MS, 0, &up) == 0);
    CHECK(board.current.kind == TOAST_3D_OFF);
    CHECK(noticeAdvance(&board, start + NOTICE_MIN_NS - MS, 0, &up) == 0);
    CHECK(noticeAdvance(&board, start + NOTICE_MIN_NS, 0, &up) == 1);
    CHECK(up.kind == TOAST_RATE && up.arg == 72);
    CHECK(board.count == 0);
}

static void testTheSplashHoldsThemBack(void) {
    NoticeBoard board;
    noticeInit(&board);
    Notice up = { -1, 0 };
    // Held while the splash is up; same-group pushes collapse to the latest.
    noticePush(&board, TOAST_RATE, 90);
    noticePush(&board, TOAST_RATE, 72);
    CHECK(board.count == 1);
    CHECK(noticeAdvance(&board, 100 * MS, 1, &up) == 0);
    CHECK(noticeAdvance(&board, 1500 * MS, 1, &up) == 0);
    CHECK(noticeAdvance(&board, 1800 * MS, 0, &up) == 1);
    CHECK(up.kind == TOAST_RATE && up.arg == 72);
}

static void testTheQueueDropsItsOldest(void) {
    NoticeBoard board;
    noticeInit(&board);
    Notice up = { -1, 0 };
    noticePush(&board, -1, 0);
    CHECK(board.count == 0);
    noticePush(&board, TOAST_TEXT, 0);
    noticeAdvance(&board, 0, 0, &up);
    // Overflow drops the oldest.
    noticePush(&board, TOAST_RATE, 90);
    noticePush(&board, TOAST_HANDS_LOCKED, 0);
    noticePush(&board, TOAST_3D_OFF, 0);
    noticePush(&board, 40, 0);
    CHECK(board.count == NOTICE_QUEUE);
    noticePush(&board, 41, 0);
    CHECK(board.count == NOTICE_QUEUE);
    CHECK(board.waiting[0].kind == TOAST_HANDS_LOCKED);
    CHECK(board.waiting[NOTICE_QUEUE - 1].kind == 41);
    // One at a time, at most one per frame.
    long long now = NOTICE_MIN_NS;
    int said = 0;
    for (int i = 0; i < 10; i++) {
        said += noticeAdvance(&board, now, 0, &up);
        said += noticeAdvance(&board, now + FRAME, 0, &up);
        now += NOTICE_MIN_NS;
    }
    CHECK(said == NOTICE_QUEUE);
    CHECK(up.kind == 41);
    CHECK(board.count == 0);
}

int main(void) {
    testFadeRisesAndLands();
    testFadeFallsAndTurnsRound();
    testFadeWithoutTheExtensionCuts();
    testSplashHoldsItsFloor();
    testSplashWaitsThenGivesUp();
    testSplashCutsWithoutAFade();
    testSplashWedgesTick();
    testNoticeShowsForItsTime();
    testNoticeOfTheSameKindReplacesAtOnce();
    testAnotherKindWaitsItsTurn();
    testTheSplashHoldsThemBack();
    testTheQueueDropsItsOldest();
    return checksDone("xr_notice");
}
