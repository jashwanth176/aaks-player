// Refresh rate choice, frame budget, depth capture gate and depth rate governor,
// against the rate lists real headsets report.
#include "check.h"
#include "xr_rate.h"

static const float QUEST2[] = { 60.0f, 72.0f, 80.0f, 90.0f, 120.0f };
static const float QUEST3[] = { 72.0f, 80.0f, 90.0f, 120.0f };
static const float PICO[] = { 72.0f, 90.0f };
// Rates reported at NTSC-style fractional values.
static const float NTSC[] = { 71.93f, 89.91f, 119.88f };

#define N(list) ((int)(sizeof(list) / sizeof(list[0])))

static void testStreamGetsItsOwnRate(void) {
    // An exact match wins whether or not multiples are allowed.
    for (int m = 0; m <= 1; m++) {
        CHECK_NEAR(rateForStream(120, QUEST3, N(QUEST3), m), 120, 0);
        CHECK_NEAR(rateForStream(90, QUEST3, N(QUEST3), m), 90, 0);
        CHECK_NEAR(rateForStream(72, QUEST3, N(QUEST3), m), 72, 0);
        CHECK_NEAR(rateForStream(60, QUEST2, N(QUEST2), m), 60, 0);
        CHECK_NEAR(rateForStream(120, QUEST2, N(QUEST2), m), 120, 0);
        CHECK_NEAR(rateForStream(90, PICO, N(PICO), m), 90, 0);
        // Near matches count, and the runtime's exact value is returned.
        CHECK_NEAR(rateForStream(120, NTSC, N(NTSC), m), 119.88f, 1e-4);
        CHECK_NEAR(rateForStream(90, NTSC, N(NTSC), m), 89.91f, 1e-4);
    }
}

static void testNoMatchTakesTheNearestAbove(void) {
    // Without multiples: the nearest rate above, so no frame is dropped.
    CHECK_NEAR(rateForStream(60, QUEST3, N(QUEST3), 0), 72, 0);
    CHECK_NEAR(rateForStream(60, PICO, N(PICO), 0), 72, 0);
    CHECK_NEAR(rateForStream(30, QUEST2, N(QUEST2), 0), 60, 0);
    CHECK_NEAR(rateForStream(30, QUEST3, N(QUEST3), 0), 72, 0);
    CHECK_NEAR(rateForStream(45, QUEST3, N(QUEST3), 0), 72, 0);
    CHECK_NEAR(rateForStream(100, QUEST3, N(QUEST3), 0), 120, 0);
    CHECK_NEAR(rateForStream(75, QUEST2, N(QUEST2), 0), 80, 0);
}

static void testAWholeMultipleBeforeTheNearestAbove(void) {
    // 60 on 120 shows every frame twice; on 72 one frame in five would be held twice.
    CHECK_NEAR(rateForStream(60, QUEST3, N(QUEST3), 1), 120, 0);
    CHECK_NEAR(rateForStream(45, QUEST3, N(QUEST3), 1), 90, 0);
    CHECK_NEAR(rateForStream(40, QUEST3, N(QUEST3), 1), 80, 0);
    CHECK_NEAR(rateForStream(30, QUEST3, N(QUEST3), 1), 90, 0);
    // Lowest multiple, not highest.
    CHECK_NEAR(rateForStream(24, QUEST3, N(QUEST3), 1), 72, 0);
    CHECK_NEAR(rateForStream(30, QUEST2, N(QUEST2), 1), 60, 0);
    // Near multiples count.
    CHECK_NEAR(rateForStream(60, NTSC, N(NTSC), 1), 119.88f, 1e-4);
    CHECK_NEAR(rateForStream(24, NTSC, N(NTSC), 1), 71.93f, 1e-4);
    // No multiple offered: nearest above.
    CHECK_NEAR(rateForStream(60, PICO, N(PICO), 1), 72, 0);
    CHECK_NEAR(rateForStream(50, QUEST3, N(QUEST3), 1), 72, 0);
    CHECK_NEAR(rateForStream(100, QUEST3, N(QUEST3), 1), 120, 0);
    CHECK_NEAR(rateForStream(75, QUEST2, N(QUEST2), 1), 80, 0);
}

static void testFasterThanAnythingTakesTheHighest(void) {
    for (int m = 0; m <= 1; m++) {
        CHECK_NEAR(rateForStream(120, PICO, N(PICO), m), 90, 0);
        CHECK_NEAR(rateForStream(144, QUEST3, N(QUEST3), m), 120, 0);
        CHECK_NEAR(rateForStream(240, NTSC, N(NTSC), m), 119.88f, 1e-4);
    }
}

static void testNothingToAskFor(void) {
    for (int m = 0; m <= 1; m++) {
        CHECK(rateForStream(90, QUEST3, 0, m) == 0.0f);
        CHECK(rateForStream(90, NULL, 4, m) == 0.0f);
        CHECK(rateForStream(0, QUEST3, N(QUEST3), m) == 0.0f);
        CHECK(rateForStream(-60, QUEST3, N(QUEST3), m) == 0.0f);
        float junk[] = { 0.0f, -1.0f };
        CHECK(rateForStream(90, junk, 2, m) == 0.0f);
    }
    CHECK(rateChoose(90, QUEST3, 0, 1, 0, 20) == 0.0f);
}

static void testOffered(void) {
    CHECK_NEAR(rateOffered(80, QUEST3, N(QUEST3)), 80, 0);
    CHECK_NEAR(rateOffered(120.3f, QUEST3, N(QUEST3)), 120, 0);
    CHECK(rateOffered(60, QUEST3, N(QUEST3)) == 0.0f);
    CHECK(rateOffered(0, QUEST3, N(QUEST3)) == 0.0f);
}

static void testStepDownStopsAtTheFloor(void) {
    CHECK_NEAR(rateStepDown(120, QUEST3, N(QUEST3), RATE_FLOOR_HZ), 90, 0);
    CHECK_NEAR(rateStepDown(90, QUEST3, N(QUEST3), RATE_FLOOR_HZ), 80, 0);
    CHECK_NEAR(rateStepDown(80, QUEST3, N(QUEST3), RATE_FLOOR_HZ), 72, 0);
    CHECK(rateStepDown(72, QUEST3, N(QUEST3), RATE_FLOOR_HZ) == 0.0f);
    // Quest 2's 60 is below RATE_FLOOR_HZ, so 72 is the lowest step.
    CHECK(rateStepDown(72, QUEST2, N(QUEST2), RATE_FLOOR_HZ) == 0.0f);
    CHECK_NEAR(rateStepDown(90, PICO, N(PICO), RATE_FLOOR_HZ), 72, 0);
    // A floor slightly above the runtime's fractional value still allows it.
    CHECK_NEAR(rateStepDown(89.91f, NTSC, N(NTSC), RATE_FLOOR_HZ), 71.93f, 1e-4);
    CHECK(rateStepDown(90, NULL, 3, RATE_FLOOR_HZ) == 0.0f);
}

static void testWarpOffIsTheStreamRate(void) {
    // With 3D off, frame time is ignored.
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 0, 0, 0), 120, 0);
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 0, 90, 30), 120, 0);
    CHECK_NEAR(rateChoose(60, QUEST2, N(QUEST2), 0, 0, 30), 60, 0);
}

static void testWarpOnStartsAtTheStreamRate(void) {
    // Nothing measured yet: start at the video's rate.
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 0, 0), 120, 0);
    CHECK_NEAR(rateChoose(90, QUEST3, N(QUEST3), 1, 0, 0), 90, 0);
    // A frame time that fits the period keeps the rate, edge included.
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 0, 8.0f), 120, 0);
    CHECK_NEAR(rateChoose(90, QUEST3, N(QUEST3), 1, 0, 11.1f), 90, 0);
}

static void testWarpOverBudgetStepsDownOne(void) {
    // 9 ms exceeds 120 Hz's 8.3 ms: one step down, not straight to the floor.
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 0, 9.0f), 90, 0);
    // 12.35 ms (depth on every frame of 4K video on Quest 3) exceeds 90 Hz's 11.1 ms.
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 90, 12.35f), 80, 0);
    CHECK_NEAR(rateChoose(90, PICO, N(PICO), 1, 0, 12.0f), 72, 0);
    CHECK_NEAR(rateChoose(120, NTSC, N(NTSC), 1, 0, 9.0f), 89.91f, 1e-4);
}

static void testWarpNeverClimbsBackPastTheHeldRate(void) {
    // Once stepped down to 90, never climbs back.
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 90, 0), 90, 0);
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 90, 4.0f), 90, 0);
    // A held rate above the video's does not raise it.
    CHECK_NEAR(rateChoose(72, QUEST3, N(QUEST3), 1, 90, 0), 72, 0);
    // A held rate the runtime doesn't offer is ignored.
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 85, 0), 120, 0);
}

static void testTheMultipleUnlessASteppedDownRateIsHeld(void) {
    // 60 fps goes to 120 with 3D off, or on with nothing held.
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 0, 0, 0), 120, 0);
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 0, 0), 120, 0);
    CHECK_NEAR(rateChoose(45, QUEST3, N(QUEST3), 1, 0, 0), 90, 0);
    CHECK_NEAR(rateChoose(90, QUEST3, N(QUEST3), 1, 0, 0), 90, 0);
    CHECK_NEAR(rateChoose(24, QUEST3, N(QUEST3), 1, 0, 0), 72, 0);
    // A held step-down below the multiple rules it out while 3D is on,
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 90, 0), 72, 0);
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 72, 0), 72, 0);
    CHECK_NEAR(rateChoose(45, QUEST3, N(QUEST3), 1, 80, 0), 72, 0);
    // but not with 3D off.
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 0, 90, 30), 120, 0);
    // A held rate equal to the multiple is not a step-down.
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 120, 0), 120, 0);
    // Step-down from the multiple goes one offered rate at a time.
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 120, 9.0f), 90, 0);
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 0, 9.0f), 90, 0);
    // then to the nearest rate above the video's, which fits.
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 90, 12.0f), 72, 0);
}

static void testFloorHolds(void) {
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 72, 20.0f), 72, 0);
    CHECK_NEAR(rateChoose(72, QUEST2, N(QUEST2), 1, 0, 20.0f), 72, 0);
    // Video below the floor keeps its own rate.
    CHECK_NEAR(rateChoose(60, QUEST2, N(QUEST2), 1, 0, 20.0f), 60, 0);
}

// Feeds one budget window of frames at hz with the given CPU and GPU times.
static int feedWindow(RateBudget* b, int64_t* now, float hz, float cpuMs, float gpuMs,
                      int warps) {
    int64_t period = (int64_t)(1e9 / hz);
    int64_t end = *now + RATE_WINDOW_NS;
    int verdict = RATE_WINDOW_FILLING;
    int frame = 0;
    int frames = (int)(RATE_WINDOW_NS / period);
    while (verdict == RATE_WINDOW_FILLING) {
        rateBudgetCpu(b, (int64_t)(cpuMs * 1e6));
        // Spread the warps evenly, as with video slower than the display.
        if (warps > 0 && (long)frame * warps / frames != (long)(frame + 1) * warps / frames) {
            rateBudgetGpu(b, (int64_t)(gpuMs * 1e6));
        }
        frame++;
        *now += period;
        verdict = rateBudgetTick(b, *now, hz);
        CHECK(*now <= end + period);
    }
    return verdict;
}

static void testBudgetSettlesThenHolds(void) {
    RateBudget b;
    int64_t now = 1000000000LL;
    rateBudgetStart(&b, now, RATE_SETTLE_WINDOWS);
    CHECK(rateBudgetTick(&b, now + 1, 90) == RATE_WINDOW_FILLING);
    // Settling windows are discarded.
    CHECK(feedWindow(&b, &now, 90, 1.0f, 30.0f, 60) == RATE_WINDOW_SETTLING);
    CHECK_NEAR(b.frameMs, 30.0f, 0.01);
    CHECK(feedWindow(&b, &now, 90, 1.0f, 5.8f, 60) == RATE_WINDOW_HELD);
    CHECK_NEAR(b.gpuMs, 5.8f, 0.01);
    CHECK_NEAR(b.cpuMs, 1.0f, 0.01);
    CHECK_NEAR(b.frameMs, 5.8f, 0.01);
    CHECK(b.lastGpuFrames >= 59 && b.lastGpuFrames <= 61);
}

static void testBudgetNeedsWindowsInARow(void) {
    RateBudget b;
    int64_t now = 0;
    rateBudgetStart(&b, now, 0);
    // A good window resets the run.
    CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_SLIPPING);
    CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_SLIPPING);
    CHECK(feedWindow(&b, &now, 120, 1.0f, 7.0f, 120) == RATE_WINDOW_HELD);
    // RATE_OVER_WINDOWS in a row means step down,
    for (int i = 1; i < RATE_OVER_WINDOWS; i++) {
        CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_SLIPPING);
    }
    CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_OVER);
    // and the count restarts.
    CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_SLIPPING);
}

static void testBudgetTakesTheSlowerSide(void) {
    RateBudget b;
    int64_t now = 0;
    rateBudgetStart(&b, now, 0);
    // Slow CPU is over budget even with a fast GPU.
    CHECK(feedWindow(&b, &now, 90, 12.0f, 2.0f, 60) == RATE_WINDOW_SLIPPING);
    CHECK_NEAR(b.frameMs, 12.0f, 0.01);
}

static void testBudgetIgnoresAFewWarps(void) {
    RateBudget b;
    int64_t now = 0;
    rateBudgetStart(&b, now, 0);
    // Too few GPU samples to judge.
    CHECK(feedWindow(&b, &now, 90, 1.0f, 20.0f, RATE_MIN_GPU_SAMPLES - 5) == RATE_WINDOW_HELD);
    CHECK(b.gpuMs == 0.0f);
    CHECK_NEAR(b.frameMs, 1.0f, 0.01);
}

static void testBudgetAddsTheRoom(void) {
    RateBudget b;
    int64_t now = 0;
    rateBudgetStart(&b, now, 0);
    for (int i = 0; i < 60; i++) {
        rateBudgetRoom(&b, 3000000L);
    }
    // 6 ms warp + 3 ms room = 9 ms, over 120 Hz's 8.3 ms.
    CHECK(feedWindow(&b, &now, 120, 1.0f, 6.0f, 60) == RATE_WINDOW_SLIPPING);
    CHECK_NEAR(b.gpuMs, 9.0f, 0.01);
}

static void testBudgetCountsMissedRefreshes(void) {
    RateBudget b;
    int64_t now = 0;
    rateBudgetStart(&b, now, 0);
    rateBudgetMissed(&b, 2);
    rateBudgetMissed(&b, 0);
    rateBudgetMissed(&b, -3);
    rateBudgetMissed(&b, 1);
    feedWindow(&b, &now, 90, 1.0f, 1.0f, 60);
    CHECK(b.lastMissed == 3);
    // Reset per window.
    feedWindow(&b, &now, 90, 1.0f, 1.0f, 60);
    CHECK(b.lastMissed == 0);
}

// One window at hz with 3 ms warps, where every lateEvery-th frame misses a refresh.
static int feedLate(RateBudget* b, int64_t* now, float hz, int lateEvery) {
    int64_t period = (int64_t)(1e9 / hz);
    int verdict = RATE_WINDOW_FILLING;
    int frame = 0;
    while (verdict == RATE_WINDOW_FILLING) {
        rateBudgetCpu(b, 1000000L);
        rateBudgetGpu(b, 3000000L);
        int late = lateEvery > 0 && frame % lateEvery == 0;
        *now += late ? 2 * period : period;
        rateBudgetMissed(b, late ? 1 : 0);
        frame++;
        verdict = rateBudgetTick(b, *now, hz);
    }
    return verdict;
}

static void testBudgetCountsABusyGpuByItsPace(void) {
    RateBudget b;
    int64_t now = 0;
    rateBudgetStart(&b, now, 0);
    // Warps fit 80 Hz's 12.5 ms but a third of refreshes are missed, so pace
    // (1.5 periods = 18.75 ms) becomes the frame time.
    CHECK(feedLate(&b, &now, 80, 2) == RATE_WINDOW_SLIPPING);
    CHECK_NEAR(b.gpuMs, 3.0f, 0.01);
    CHECK_NEAR(b.paceMs, 18.75f, 0.2);
    CHECK_NEAR(b.frameMs, b.paceMs, 1e-6);
    CHECK(b.lastMissed * 3 >= b.lastFrames + b.lastMissed - 2);
    // One miss in twenty is below the threshold; warp time stands.
    rateBudgetStart(&b, now, 0);
    CHECK(feedLate(&b, &now, 80, 20) == RATE_WINDOW_HELD);
    CHECK_NEAR(b.frameMs, 3.0f, 0.01);
    CHECK(b.paceMs > 12.5f);
    // One in nine is within the allowed share, one in eight is past it.
    rateBudgetStart(&b, now, 0);
    CHECK(feedLate(&b, &now, 80, 9) == RATE_WINDOW_HELD);
    rateBudgetStart(&b, now, 0);
    CHECK(feedLate(&b, &now, 80, 8) == RATE_WINDOW_SLIPPING);
    CHECK(b.frameMs > 12.5f);
    // Which steps 80 down to 72.
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 80, b.frameMs), 72, 0);
}

static void testBudgetWithNothingMeasured(void) {
    RateBudget b;
    int64_t now = 0;
    rateBudgetStart(&b, now, 0);
    now += RATE_WINDOW_NS;
    CHECK(rateBudgetTick(&b, now, 90) == RATE_WINDOW_HELD);
    CHECK(b.frameMs == 0.0f);
    // No rate to judge against: never over.
    rateBudgetStart(&b, now, 0);
    for (int i = 0; i < 100; i++) {
        rateBudgetCpu(&b, 30000000L);
    }
    now += RATE_WINDOW_NS;
    CHECK(rateBudgetTick(&b, now, 0) == RATE_WINDOW_HELD);
    CHECK_NEAR(b.frameMs, 30.0f, 0.01);
}

// The initial rate is not toasted: settled only after a focused frame and the
// first rate request has landed.
static void testTheFirstRateIsNoNews(void) {
    // Not focused.
    CHECK(!rateSettled(0, 0, 90.0f, 0));
    CHECK(!rateSettled(0, 0, 90.0f, 1));
    // Focused, request pending.
    CHECK(!rateSettled(0, 1, 90.0f, 0));
    // Focused and landed.
    CHECK(rateSettled(0, 1, 90.0f, 1));
    // Nothing requested (no refresh-rate extension): focus alone settles.
    CHECK(rateSettled(0, 1, 0.0f, 0));
    CHECK(!rateSettled(0, 0, 0.0f, 0));
    CHECK(rateSettled(0, 1, NAN, 0));
    // Once settled, later pending requests don't unsettle it.
    CHECK(rateSettled(1, 1, 72.0f, 0));
    CHECK(rateSettled(1, 0, 72.0f, 0));

    // Session start, frame by frame, as the frame loop latches it.
    int settled = 0;
    int toasts = 0;
    // Requested before focus; lands while visible.
    float asked = 90.0f;
    int confirmed = 0;
    settled = rateSettled(settled, 0, asked, confirmed);
    confirmed = 1;
    toasts += settled;
    settled = rateSettled(settled, 0, asked, confirmed);
    CHECK(!settled);
    // First focused frame settles.
    settled = rateSettled(settled, 1, asked, confirmed);
    CHECK(settled);
    // A later request is toasted.
    asked = 120.0f;
    confirmed = 0;
    settled = rateSettled(settled, 1, asked, confirmed);
    toasts += settled;
    CHECK(toasts == 1);
}

// Deterministic jitter in [-amp, amp] ns.
static int64_t jitter(int i, int64_t amp) {
    unsigned h = (unsigned)i * 2654435761u;
    return (int64_t)(h % (unsigned)(2 * amp + 1)) - amp;
}

// Runs frames at fps through the gate at perSecond; returns captures and min/max gap.
static int gateRun(float fps, int perSecond, float seconds, int64_t amp, int64_t* minGap,
                   int64_t* maxGap) {
    DepthGate g = { 0, 0 };
    int frames = (int)(fps * seconds);
    int taken = 0;
    int64_t last = -1;
    *minGap = INT64_MAX;
    *maxGap = 0;
    for (int i = 0; i < frames; i++) {
        int64_t t = 1000000000LL + (int64_t)(i * (1e9 / fps)) + (i > 0 ? jitter(i, amp) : 0);
        if (depthGateDue(&g, t, perSecond)) {
            if (last >= 0) {
                int64_t gap = t - last;
                *minGap = gap < *minGap ? gap : *minGap;
                *maxGap = gap > *maxGap ? gap : *maxGap;
            }
            last = t;
            taken++;
        }
    }
    return taken;
}

static void testGateHoldsTheRateWhateverTheFrameRate(void) {
    int64_t lo, hi;
    // 20/s at 60 fps: every third frame (50 ms), despite jitter.
    CHECK(gateRun(60, 20, 10, 1000000, &lo, &hi) == 200);
    CHECK(lo >= 48000000 && hi <= 52000000);
    // Still 20/s at 120 fps; a fixed cadence of 3 would give 40.
    CHECK(gateRun(120, 20, 10, 1000000, &lo, &hi) == 200);
    CHECK(lo >= 48000000 && hi <= 52000000);
    // 90/20 is not whole: gaps alternate 4 and 5 frames, average holds.
    int taken = gateRun(90, 20, 10, 500000, &lo, &hi);
    CHECK(taken >= 199 && taken <= 201);
    CHECK(lo >= 44000000 && hi <= 56000000);
    // 12/s at 72 fps.
    taken = gateRun(72, 12, 10, 1000000, &lo, &hi);
    CHECK(taken >= 119 && taken <= 121);
    // Video slower than the target: every frame.
    CHECK(gateRun(24, 45, 10, 1000000, &lo, &hi) == 240);
    CHECK(gateRun(24, 20, 10, 0, &lo, &hi) >= 199);
    // Floor and ceiling rates.
    CHECK(gateRun(60, 5, 10, 1000000, &lo, &hi) == 50);
    taken = gateRun(60, 45, 10, 1000000, &lo, &hi);
    CHECK(taken >= 449 && taken <= 451);
}

// 60 fps video sampled on a 72 Hz frame loop: gaps are one or two refreshes.
static void testGateThroughARefreshGrid(void) {
    DepthGate g = { 0, 0 };
    int taken = 0;
    int64_t refresh = 1000000000LL / 72;
    int64_t lastFrame = -1;
    for (int r = 0; r < 72 * 10; r++) {
        int64_t t = 1000000000LL + r * refresh;
        int64_t frame = (r * refresh) / (1000000000LL / 60);
        if (frame == lastFrame) {
            continue;
        }
        lastFrame = frame;
        taken += depthGateDue(&g, t, 20);
    }
    CHECK(taken >= 199 && taken <= 201);
}

static void testGateAfterAStallDoesNotBurst(void) {
    DepthGate g = { 0, 0 };
    int64_t t = 1000000000LL;
    int64_t frame = 1000000000LL / 60;
    CHECK(depthGateDue(&g, t, 20));
    // One second with no frames,
    t += 1000000000LL;
    CHECK(depthGateDue(&g, t, 20));
    // then back to every third frame, no catch-up burst.
    int taken = 0;
    for (int i = 1; i <= 6; i++) {
        taken += depthGateDue(&g, t + i * frame, 20);
    }
    CHECK(taken == 2);
}

static void testGateFollowsATargetChange(void) {
    DepthGate g = { 0, 0 };
    int64_t frame = 1000000000LL / 120;
    int taken = 0;
    for (int i = 0; i < 120; i++) {
        taken += depthGateDue(&g, 1000000000LL + i * frame, 20);
    }
    CHECK(taken == 20);
    // Halved target, half the captures.
    taken = 0;
    for (int i = 120; i < 240; i++) {
        taken += depthGateDue(&g, 1000000000LL + i * frame, 10);
    }
    CHECK(taken >= 10 && taken <= 11);
    // Zero target: nothing.
    CHECK(!depthGateDue(&g, 9000000000LL, 0));
}

static void testGateCountsFromAForcedCapture(void) {
    DepthGate g = { 0, 0 };
    int64_t frame = 1000000000LL / 60;
    int64_t t = 5000000000LL;
    // A forced capture (3D re-enabled) restarts the gate's count.
    depthGateTaken(&g, t, 20);
    CHECK(!depthGateDue(&g, t + frame, 20));
    CHECK(!depthGateDue(&g, t + 2 * frame, 20));
    CHECK(depthGateDue(&g, t + 3 * frame, 20));
}

static void testDepthRateClamp(void) {
    CHECK(depthRateClamp(0) == DEPTH_RATE_MIN);
    CHECK(depthRateClamp(-3) == DEPTH_RATE_MIN);
    CHECK(depthRateClamp(5) == 5);
    CHECK(depthRateClamp(20) == 20);
    CHECK(depthRateClamp(45) == 45);
    CHECK(depthRateClamp(60) == DEPTH_RATE_MAX);
}

#define SEC 1000000000LL

static void testGovernorCutsBeforeTheDisplay(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 20);
    CHECK(g.target == 20 && g.cap == 20);
    // First budget miss halves the depth rate; display rate untouched.
    CHECK(depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 10);
    // Misses within the 10 s hold are ignored so the cut can take effect.
    CHECK(depthGovernorWindow(&g, 16 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_HOLDING);
    CHECK(depthGovernorWindow(&g, 20 * SEC - 1, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_HOLDING);
    CHECK(g.target == 10);
    // Still missing after the hold: halved again, to the floor.
    CHECK(depthGovernorWindow(&g, 20 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 5);
    CHECK(depthGovernorWindow(&g, 26 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_HOLDING);
    // Only at the floor does the display rate step down.
    CHECK(depthGovernorWindow(&g, 32 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_DISPLAY);
    CHECK(g.target == 5);
    CHECK(depthGovernorWindow(&g, 40 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_DISPLAY);
}

// Drives the governor as the frame loop does: one verdict per 2 s window, OVER after
// RATE_OVER_WINDOWS in a row. Returns the moves in order.
static int ladder(DepthGovernor* g, int64_t* now, int windows, int over, int live, int* moves,
                  int64_t* when, int max) {
    int n = 0, overRun = 0;
    for (int w = 0; w < windows; w++) {
        *now += RATE_WINDOW_NS;
        int verdict = RATE_WINDOW_HELD;
        if (over) {
            verdict = ++overRun < RATE_OVER_WINDOWS ? RATE_WINDOW_SLIPPING : RATE_WINDOW_OVER;
            if (verdict == RATE_WINDOW_OVER) {
                overRun = 0;
            }
        }
        int move = depthGovernorWindow(g, *now, verdict, live);
        if (move != DEPTH_MOVE_NONE && n < max) {
            moves[n] = move;
            when[n] = *now;
            n++;
        }
    }
    return n;
}

static void testGovernorLadderTiming(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 20);
    int64_t now = 0;
    int moves[16];
    int64_t when[16];
    // 60 s over budget.
    int n = ladder(&g, &now, 30, 1, 1, moves, when, 16);
    // 6 s: cut to 10. 12 s: hold. 18 s: cut to 5. 24 s: hold. From 30 s: display, every 6 s.
    CHECK(n >= 5);
    CHECK(moves[0] == DEPTH_MOVE_CUT && when[0] == 6 * SEC);
    CHECK(moves[1] == DEPTH_MOVE_HOLDING && when[1] == 12 * SEC);
    CHECK(moves[2] == DEPTH_MOVE_CUT && when[2] == 18 * SEC);
    CHECK(moves[3] == DEPTH_MOVE_HOLDING && when[3] == 24 * SEC);
    CHECK(moves[4] == DEPTH_MOVE_DISPLAY && when[4] == 30 * SEC);
    CHECK(g.target == 5);

    // Budget then holds. 10 failed last, so the ceiling is 5: CAPPED is reported
    // once, 30 s in, and nothing climbs.
    n = ladder(&g, &now, 40, 0, 1, moves, when, 16);
    int64_t heldFrom = 60 * SEC;
    CHECK(n == 1);
    CHECK(moves[0] == DEPTH_MOVE_CAPPED && when[0] == heldFrom + 30 * SEC);
    CHECK(g.target == 5 && g.failedTarget == 10);
    n = ladder(&g, &now, 40, 0, 1, moves, when, 16);
    CHECK(n == 0);
    CHECK(g.target == 5);
}

// A throttle cut, released immediately. Throttle cuts don't set failedTarget.
static void throttleCut(DepthGovernor* g, int64_t now) {
    CHECK(depthGovernorThrottle(g, now, 1, 1, 1) == DEPTH_MOVE_CUT);
    CHECK(depthGovernorThrottle(g, now, 0, 0, 1) == DEPTH_MOVE_NONE);
}

// Feeds held windows until the governor moves, at most limit.
static int heldUntilMove(DepthGovernor* g, int64_t* now, int limit) {
    for (int i = 0; i < limit; i++) {
        *now += RATE_WINDOW_NS;
        int move = depthGovernorWindow(g, *now, RATE_WINDOW_HELD, 1);
        if (move != DEPTH_MOVE_NONE) {
            return move;
        }
    }
    return DEPTH_MOVE_NONE;
}

static void testGovernorRecoveryNeedsAnUnbrokenRun(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 45);
    int64_t now = 100 * SEC;
    throttleCut(&g, now);
    CHECK(g.target == 22 && g.failedTarget == 0);
    // 28 s held, then one slipping window restarts the count.
    for (int i = 0; i < 14; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_NONE);
    }
    now += RATE_WINDOW_NS;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_SLIPPING, 1) == DEPTH_MOVE_NONE);
    for (int i = 0; i < 14; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_NONE);
    }
    now += RATE_WINDOW_NS;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_RAISED);
    // Doubled to 44, then capped at the preference.
    CHECK(g.target == 44);
    for (int i = 0; i < 15; i++) {
        now += RATE_WINDOW_NS;
        depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1);
    }
    CHECK(g.target == 45);
    // A budget restart (rate change, focus loss) also breaks the run.
    depthGovernorStart(&g, 20);
    throttleCut(&g, now);
    for (int i = 0; i < 10; i++) {
        now += RATE_WINDOW_NS;
        depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1);
    }
    now += RATE_WINDOW_NS;
    depthGovernorWindow(&g, now, RATE_WINDOW_SETTLING, 1);
    for (int i = 0; i < 14; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_NONE);
    }
    CHECK(g.target == 10);
}

// A failed target is remembered and the climb stops at half of it, so the governor
// doesn't oscillate between a failing rate and its half.
static void testGovernorRemembersTheFailedTarget(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 45);
    int64_t now = 10 * SEC;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 22 && g.failedTarget == 45);
    CHECK(depthGovernorCeiling(&g) == 22);
    // Five minutes held: CAPPED once at 30 s, no climb.
    int capped = 0, other = 0;
    int64_t cappedAt = 0;
    for (int i = 0; i < 150; i++) {
        now += RATE_WINDOW_NS;
        int move = depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1);
        if (move == DEPTH_MOVE_CAPPED) {
            capped++;
            cappedAt = now;
        }
        else if (move != DEPTH_MOVE_NONE) {
            other++;
        }
    }
    CHECK(capped == 1 && other == 0);
    CHECK(cappedAt == 10 * SEC + DEPTH_RECOVER_NS);
    CHECK(g.target == 22);

    // A failed step up is remembered too: throttle cut to 22, climb to 44, fail, stay at 22.
    depthGovernorStart(&g, 45);
    now = 10 * SEC;
    throttleCut(&g, now);
    CHECK(heldUntilMove(&g, &now, 20) == DEPTH_MOVE_RAISED);
    CHECK(g.target == 44);
    now += RATE_WINDOW_NS;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 22 && g.failedTarget == 44 && depthGovernorCeiling(&g) == 22);
    CHECK(heldUntilMove(&g, &now, 150) == DEPTH_MOVE_CAPPED);
    CHECK(heldUntilMove(&g, &now, 150) == DEPTH_MOVE_NONE);
    CHECK(g.target == 22);
    // The most recent failure sets the ceiling.
    now += 20 * SEC;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.failedTarget == 22 && depthGovernorCeiling(&g) == 11);
    // A throttle is not a failure.
    depthGovernorStart(&g, 20);
    throttleCut(&g, now);
    CHECK(g.failedTarget == 0 && depthGovernorCeiling(&g) == 20);
}

static void testGovernorCeiling(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 20);
    CHECK(depthGovernorCeiling(&g) == 20);
    g.failedTarget = 30;
    CHECK(depthGovernorCeiling(&g) == 15);
    // Never above the preference,
    g.failedTarget = 50;
    CHECK(depthGovernorCeiling(&g) == 20);
    // never below the floor.
    g.failedTarget = 6;
    CHECK(depthGovernorCeiling(&g) == 5);
    g.failedTarget = 5;
    CHECK(depthGovernorCeiling(&g) == 5);
}

static void testGovernorSettingChangeForgets(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 45);
    int64_t now = 10 * SEC;
    throttleCut(&g, now);
    heldUntilMove(&g, &now, 20);
    now += RATE_WINDOW_NS;
    depthGovernorWindow(&g, now, RATE_WINDOW_OVER, 1);
    CHECK(g.failedTarget == 44 && g.recoverNs == 2 * DEPTH_RECOVER_NS);
    // Same (or clamped-same) setting: no change.
    CHECK(depthGovernorSetCap(&g, 45) == 0);
    CHECK(depthGovernorSetCap(&g, 60) == 0);
    CHECK(g.target == 22 && g.failedTarget == 44);
    // A new setting resets all learnt state,
    CHECK(depthGovernorSetCap(&g, 20) == 1);
    CHECK(g.cap == 20 && g.target == 20 && g.failedTarget == 0);
    CHECK(g.recoverNs == DEPTH_RECOVER_NS && g.raisedNs == 0 && g.cutNs == 0);
    CHECK(heldUntilMove(&g, &now, 150) == DEPTH_MOVE_NONE);
    CHECK(g.target == 20);
    // but an active throttle stays.
    depthGovernorThrottle(&g, now, 0, 1, 1);
    CHECK(depthGovernorSetCap(&g, 30) == 1);
    CHECK(g.throttled == 1 && g.target == 30);
}

static void testRecoverBackoff(void) {
    CHECK(depthRecoverBackoff(30 * SEC) == 60 * SEC);
    CHECK(depthRecoverBackoff(60 * SEC) == 120 * SEC);
    CHECK(depthRecoverBackoff(120 * SEC) == 240 * SEC);
    CHECK(depthRecoverBackoff(240 * SEC) == 300 * SEC);
    CHECK(depthRecoverBackoff(300 * SEC) == 300 * SEC);
}

// Each failed step up doubles the wait before the next: 30 s, 60 s, 120 s.
static void testGovernorBacksOffAfterFailedSteps(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 45);
    int64_t now = 10 * SEC;
    int64_t expect[] = { 30 * SEC, 60 * SEC, 120 * SEC };
    int targets[] = { 44, 22, 10 };
    for (int i = 0; i < 3; i++) {
        // Throttle cut gives room to climb; spaced past the hold.
        now += DEPTH_HOLD_NS;
        throttleCut(&g, now);
        CHECK(g.recoverNs == expect[i]);
        int64_t from = now;
        CHECK(heldUntilMove(&g, &now, 200) == DEPTH_MOVE_RAISED);
        CHECK(now - from == expect[i]);
        CHECK(g.target == targets[i]);
        // Budget fails the step.
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    }
    CHECK(g.recoverNs == 240 * SEC);
    CHECK(g.target == 5 && depthGovernorCeiling(&g) == 5);
}

// A step up that holds for DEPTH_RAISE_HELD_NS resets the wait to 30 s.
static void testGovernorBackoffResetsWhenAStepHolds(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 45);
    int64_t now = 10 * SEC;
    throttleCut(&g, now);
    heldUntilMove(&g, &now, 20);
    now += RATE_WINDOW_NS;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.recoverNs == 60 * SEC && g.target == 22);
    // Throttle cut, then up to the ceiling after the longer wait.
    now += DEPTH_HOLD_NS;
    throttleCut(&g, now);
    CHECK(heldUntilMove(&g, &now, 100) == DEPTH_MOVE_RAISED);
    int64_t raised = now;
    CHECK(g.target == 22);
    // At the ceiling: CAPPED when the wait expires, RAISE_HELD at two minutes.
    CHECK(heldUntilMove(&g, &now, 100) == DEPTH_MOVE_CAPPED);
    CHECK(now - raised == 60 * SEC);
    CHECK(heldUntilMove(&g, &now, 100) == DEPTH_MOVE_RAISE_HELD);
    CHECK(now - raised == DEPTH_RAISE_HELD_NS);
    CHECK(g.recoverNs == DEPTH_RECOVER_NS && g.raisedNs == 0);
    // A later cut is a fresh failure, not the step's.
    now += RATE_WINDOW_NS;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.recoverNs == DEPTH_RECOVER_NS);
    // A throttle cut doesn't count against a step up.
    depthGovernorStart(&g, 45);
    now += DEPTH_HOLD_NS;
    throttleCut(&g, now);
    heldUntilMove(&g, &now, 20);
    now += DEPTH_HOLD_NS;
    throttleCut(&g, now);
    CHECK(g.recoverNs == DEPTH_RECOVER_NS && g.raisedNs == 0);
}

static void testGovernorWithNoModelRunning(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 20);
    // No model running (3D off or test pattern): straight to the display.
    CHECK(depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 0) == DEPTH_MOVE_DISPLAY);
    CHECK(g.target == 20);
    // No climb while not running.
    depthGovernorStart(&g, 20);
    depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1);
    int64_t now = 10 * SEC;
    for (int i = 0; i < 30; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 0) == DEPTH_MOVE_NONE);
    }
    CHECK(g.target == 10);
    // Filling windows are not judged.
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_FILLING, 1) == DEPTH_MOVE_NONE);
}

static void testGovernorFloorAndCap(void) {
    DepthGovernor g;
    // Preference at the floor: straight to the display.
    depthGovernorStart(&g, 5);
    CHECK(depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_DISPLAY);
    // 6 halves to 3, clamped to 5.
    depthGovernorStart(&g, 6);
    CHECK(depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 5);
    // 12: 6, then 5.
    depthGovernorStart(&g, 12);
    depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1);
    CHECK(g.target == 6);
    depthGovernorWindow(&g, 20 * SEC, RATE_WINDOW_OVER, 1);
    CHECK(g.target == 5);
    // Out-of-range preferences are clamped.
    depthGovernorStart(&g, 99);
    CHECK(g.cap == 45 && g.target == 45);
    depthGovernorStart(&g, 1);
    CHECK(g.cap == 5 && g.target == 5);
}

static void testGovernorOnAThrottle(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 20);
    // A runtime throttle warning cuts immediately.
    CHECK(depthGovernorThrottle(&g, 10 * SEC, 1, 1, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 10);
    // A second domain within the hold is held,
    CHECK(depthGovernorThrottle(&g, 11 * SEC, 1, 1, 1) == DEPTH_MOVE_HOLDING);
    CHECK(g.target == 10);
    // as is a budget miss.
    CHECK(depthGovernorWindow(&g, 16 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_HOLDING);
    // No climb while throttled.
    int64_t now = 20 * SEC;
    for (int i = 0; i < 30; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_NONE);
    }
    // The recovery run starts when the throttle clears.
    CHECK(depthGovernorThrottle(&g, now, 0, 0, 1) == DEPTH_MOVE_NONE);
    int raised = 0;
    for (int i = 0; i < 15; i++) {
        now += RATE_WINDOW_NS;
        raised += depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_RAISED;
    }
    CHECK(raised == 1);
    CHECK(g.target == 20);
    // A throttle never moves the display rate: at the floor, or with no model running.
    depthGovernorStart(&g, 5);
    CHECK(depthGovernorThrottle(&g, now, 1, 1, 1) == DEPTH_MOVE_NONE);
    depthGovernorStart(&g, 20);
    CHECK(depthGovernorThrottle(&g, now, 1, 1, 0) == DEPTH_MOVE_NONE);
    CHECK(g.target == 20 && g.throttled);
}

int main(void) {
    testStreamGetsItsOwnRate();
    testNoMatchTakesTheNearestAbove();
    testAWholeMultipleBeforeTheNearestAbove();
    testFasterThanAnythingTakesTheHighest();
    testNothingToAskFor();
    testOffered();
    testStepDownStopsAtTheFloor();
    testWarpOffIsTheStreamRate();
    testWarpOnStartsAtTheStreamRate();
    testWarpOverBudgetStepsDownOne();
    testWarpNeverClimbsBackPastTheHeldRate();
    testTheMultipleUnlessASteppedDownRateIsHeld();
    testFloorHolds();
    testBudgetSettlesThenHolds();
    testBudgetNeedsWindowsInARow();
    testBudgetTakesTheSlowerSide();
    testBudgetIgnoresAFewWarps();
    testBudgetAddsTheRoom();
    testBudgetCountsMissedRefreshes();
    testBudgetCountsABusyGpuByItsPace();
    testBudgetWithNothingMeasured();
    testTheFirstRateIsNoNews();
    testGateHoldsTheRateWhateverTheFrameRate();
    testGateThroughARefreshGrid();
    testGateAfterAStallDoesNotBurst();
    testGateFollowsATargetChange();
    testGateCountsFromAForcedCapture();
    testDepthRateClamp();
    testGovernorCutsBeforeTheDisplay();
    testGovernorLadderTiming();
    testGovernorRecoveryNeedsAnUnbrokenRun();
    testGovernorRemembersTheFailedTarget();
    testGovernorCeiling();
    testGovernorSettingChangeForgets();
    testRecoverBackoff();
    testGovernorBacksOffAfterFailedSteps();
    testGovernorBackoffResetsWhenAStepHolds();
    testGovernorWithNoModelRunning();
    testGovernorFloorAndCap();
    testGovernorOnAThrottle();
    return checksDone("xr_rate");
}
