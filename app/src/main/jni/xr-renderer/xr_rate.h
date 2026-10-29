// Display rate selection, frame budget monitor and depth rate governor

#ifndef XR_RATE_H
#define XR_RATE_H

#include <stdint.h>

#include "xr_shared.h"

// Rates within this are treated as equal (e.g. 120 vs 119.88)
#define RATE_TOLERANCE 0.5f
// Budget step-downs never go below this; content below it keeps its own rate
#define RATE_FLOOR_HZ 72.0f
// Max rates kept from the runtime's list
#define RATE_MAX 16
// Max rate the debug property can force
#define RATE_KNOB_MAX 240

// Step down only after RATE_OVER_WINDOWS consecutive windows over budget
#define RATE_WINDOW_NS 2000000000L
#define RATE_OVER_WINDOWS 3
// Windows discarded after a rate, 3D or focus change
#define RATE_SETTLE_WINDOWS 1
// Minimum warps per window for its GPU time to be trusted
#define RATE_MIN_GPU_SAMPLES 30
// Above this missed-refresh share the submit interval counts as the frame
// time (a saturated GPU makes frames late without any one being slow)
#define RATE_MISSED_PERCENT 10

// rateBudgetTick results
#define RATE_WINDOW_FILLING  0
#define RATE_WINDOW_SETTLING 1
#define RATE_WINDOW_HELD     2
#define RATE_WINDOW_SLIPPING 3
#define RATE_WINDOW_OVER     4

// The offered rate matching hz, or 0
float rateOffered(float hz, const float* rates, int count);

// fps if offered, else the lowest whole multiple (if multiples), else the
// nearest rate above, else the highest; 0 if none
float rateForStream(float fps, const float* rates, int count, int multiples);

// Next offered rate below hz and at least floorHz, or 0
float rateStepDown(float hz, const float* rates, int count, float floorHz);

// Rate to request. With the warp on it is capped at heldHz (0 for none) and
// stepped down if frameMs (0 if unmeasured) exceeds the period.
float rateChoose(float fps, const float* rates, int count, int warpOn, float heldHz,
                 float frameMs);

// Settled (later changes get a toast) after a focused frame and the first
// request resolved. Sticky.
int rateSettled(int settled, int focusedFrame, float asked, int confirmed);

typedef struct {
    int64_t startNs;
    int settle;
    int overWindows;
    // CPU time from xrBeginFrame to xrEndFrame
    int64_t cpuTotalNs;
    long cpuFrames;
    // Warp and room GPU time, per frame that drew them
    int64_t gpuTotalNs;
    long gpuFrames;
    int64_t roomTotalNs;
    long roomFrames;
    // Missed display refreshes
    long missed;
    // Last full window, for logging. frameMs is the slowest of cpu, gpu and pace.
    float cpuMs;
    float gpuMs;
    float paceMs;
    float frameMs;
    long lastMissed;
    long lastFrames;
    long lastGpuFrames;
} RateBudget;

// Restarts measuring, discarding the next settle windows
void rateBudgetStart(RateBudget* b, int64_t nowNs, int settle);
void rateBudgetCpu(RateBudget* b, int64_t frameNs);
void rateBudgetGpu(RateBudget* b, int64_t gpuNs);
void rateBudgetRoom(RateBudget* b, int64_t gpuNs);
void rateBudgetMissed(RateBudget* b, long refreshes);

// Once per frame; returns a RATE_WINDOW_ verdict. OVER means step down.
int rateBudgetTick(RateBudget* b, int64_t nowNs, float hz);

// Depth model rate in maps per second; captures are timed, not per frame

// Wait after a cut before judging again
#define DEPTH_HOLD_NS 10000000000LL
// Held time before a step up; doubled per failed step up, reset after
// DEPTH_RAISE_HELD_NS
#define DEPTH_RECOVER_NS 30000000000LL
#define DEPTH_RECOVER_MAX_NS 300000000000LL
#define DEPTH_RAISE_HELD_NS 120000000000LL

int depthRateClamp(int perSecond);

// Next capture due time and the last frame seen
typedef struct {
    int64_t dueNs;
    int64_t lastNs;
} DepthGate;

// Whether to capture this frame. Schedules from the due time so the average
// rate holds; restarts rather than bursting when a period behind.
int depthGateDue(DepthGate* g, int64_t nowNs, int perSecond);

// Records an out-of-schedule capture (e.g. 3D turned back on)
void depthGateTaken(DepthGate* g, int64_t nowNs, int perSecond);

// Governor results
#define DEPTH_MOVE_NONE    0
// Target halved
#define DEPTH_MOVE_CUT     1
// Target doubled, up to the preference
#define DEPTH_MOVE_RAISED  2
// Over budget, but within DEPTH_HOLD_NS of the last cut
#define DEPTH_MOVE_HOLDING 3
// Over budget with depth at DEPTH_RATE_MIN or not running: step the display down
#define DEPTH_MOVE_DISPLAY 4
// Last step up held; recovery wait reset to DEPTH_RECOVER_NS
#define DEPTH_MOVE_RAISE_HELD 5
// Could climb, but capped below the failed target. Reported once per cut.
#define DEPTH_MOVE_CAPPED 6

// Halves the depth rate before the display rate is cut; doubles back after
// the recovery wait, capped at half the last failed target
typedef struct {
    // Preference and current target, maps per second
    int cap;
    int target;
    // Last cut time, 0 for never
    int64_t cutNs;
    // Start of the current run of held windows, 0 if none
    int64_t heldSinceNs;
    // Runtime reports CPU or GPU throttling; blocks step-ups
    int throttled;
    // Last target that failed the budget, 0 for none (throttles do not count)
    int failedTarget;
    // Current recovery wait, and the last step-up time (0 once judged)
    int64_t recoverNs;
    int64_t raisedNs;
    // DEPTH_MOVE_CAPPED already reported since the last cut
    int cappedSaid;
} DepthGovernor;

void depthGovernorStart(DepthGovernor* g, int cap);

// Restarts the governor and returns 1 if the preference changed
int depthGovernorSetCap(DepthGovernor* g, int cap);

// Step-up ceiling: the preference, or half the failed target (min DEPTH_RATE_MIN)
int depthGovernorCeiling(const DepthGovernor* g);

// Doubles the recovery wait, up to DEPTH_RECOVER_MAX_NS
int64_t depthRecoverBackoff(int64_t waitNs);

// Feeds a rateBudgetTick verdict. live: the depth model is running with 3D on.
int depthGovernorWindow(DepthGovernor* g, int64_t nowNs, int verdict, int live);

// Runtime performance notice; cuts like a budget miss but never steps the display
int depthGovernorThrottle(DepthGovernor* g, int64_t nowNs, int worse, int throttled, int live);

#endif
