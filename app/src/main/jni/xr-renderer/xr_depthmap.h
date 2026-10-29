
// CPU filtering of depth model output: robust range, low pass (shape vs
// detail), temporal averaging, and scene cut detection on the model input.

#ifndef XR_DEPTHMAP_H
#define XR_DEPTHMAP_H

#include <math.h>

#include "xr_shared.h"

// Bins for the percentile search over the model output
#define DEPTH_HIST_BINS 512

// 2nd..98th percentile of the finite values
void robustRange(const float* v, int count, float* outLo, float* outHi);

// Minimum range span at lo, so 1 / (hi - lo) stays finite
float depthSpanFloor(float lo);

// Non-finite values read as lo (far) so they never enter the smoothing
static inline float depthRead(float v, float lo) {
    return isfinite(v) ? v : lo;
}

// Approximate gaussian on a w x h map; rx, ry are the box radii
void lowPass(const float* src, float* dst, float* scratch, float* colSums, int w, int h,
             int rx, int ry);

// 64..DEPTH_TEX_W_MAX by 64..DEPTH_TEX_H_MAX, both multiples of 8
int depthSizeOk(int w, int h);

// Averages are time-based; dt is clamped to [1/60 s, 1 s]
#define DEPTH_TAU_DT_MIN_S (1.0f / 60.0f)
#define DEPTH_TAU_DT_MAX_S 1.0f

// Time constants, ms. The range is slower so it follows the scene, not the frame.
#define DEPTH_TAU_DEFAULT_MS 30
#define DEPTH_TAU_MAX_MS 10000
#define DEPTH_RANGE_TAU_DEFAULT_MS 150
#define DEPTH_RANGE_TAU_MAX_MS 10000

// 1 - exp(-dt / tau) with dt clamped; 1 (no averaging) when tau is 0
float depthTauAlpha(float dtSec, float tauSec);

// Blends avg toward v (copies on seed); a NaN can never persist
void depthTauBlend(float* avg, const float* v, int count, float alpha, int seed);

// Smoothed normalisation range, so relative depth does not "breathe" on pans
typedef struct {
    float lo, hi;
    int valid;
} DepthRange;

// Steps toward lo/hi; takes them directly when empty or tau is 0
void depthRangeStep(DepthRange* r, float lo, float hi, float dtSec, float tauSec);

// 1 / span, with span at least depthSpanFloor(lo)
float depthRangeScale(const DepthRange* r);

// Scene cut detection: each capture is reduced to a luma grid and histogram
#define DEPTH_CUT_GRID 16
#define DEPTH_CUT_BINS 16

typedef struct {
    float grid[DEPTH_CUT_GRID * DEPTH_CUT_GRID];
    float hist[DEPTH_CUT_BINS];
} DepthThumb;

// From a w x h RGB float image, 0..1. Smaller than the grid gives an empty thumb.
void depthThumbMake(DepthThumb* t, const float* rgb, int w, int h);
// Mean absolute grid difference, 0..1
float depthThumbDiff(const DepthThumb* a, const DepthThumb* b);
// Half the histogram L1 distance: 0 same, 1 no overlap
float depthThumbHistDiff(const DepthThumb* a, const DepthThumb* b);
// Grid correlation, -1..1; 0 when either grid is flat
float depthThumbCorr(const DepthThumb* a, const DepthThumb* b);

// Thresholds tuned on a Quest 3 log of an 81 s reel with 31 cuts. A false
// jump only costs one unaveraged map; the gates below reject them.
#define DEPTH_CUT_JUMP_DIFF 0.06f
#define DEPTH_CUT_JUMP_HIST 0.50f
// The capture after a cut stays this close to it (a flash jumps back)
#define DEPTH_CUT_SAME_DIFF 0.06f
#define DEPTH_CUT_SAME_HIST 0.20f
// Non-cut jumps: correlation >= 0.725 (cuts <= 0.505), <= 2.3x neighbouring
// steps (cuts >= 5x), and >= 1.23x further from two back (cuts about 1x)
#define DEPTH_CUT_CORR_MAX 0.60f
#define DEPTH_CUT_STAND_OUT 3.0f
#define DEPTH_CUT_TWO_BACK 1.12f
// Grid variance below this is flat
#define DEPTH_CUT_FLAT 1e-5f

typedef struct {
    DepthThumb last;
    // Capture before last
    DepthThumb older;
    // Capture before a jump awaiting confirmation
    DepthThumb before;
    int haveLast;
    int haveOlder;
    int lastJumped;
    int pending;
    // Step diffs around the pending jump
    float lastDiff;
    float settledDiff;
    float twoBackDiff;
    float jumpDiff;
    float jumpHist;
    // Last confirmed cut's correlation, for logging
    float corr;
} DepthCut;

// Big change from the last capture: reset the per-texel average
#define DEPTH_CUT_JUMP 1
// Jump confirmed as a cut by the following capture: also reset the range
#define DEPTH_CUT_CONFIRMED 2

// Returns DEPTH_CUT_ flags
int depthCutStep(DepthCut* c, const DepthThumb* t, float* diff, float* hist);

// Next capture only seeds the detector
static inline void depthCutClear(DepthCut* c) {
    c->haveLast = 0;
    c->haveOlder = 0;
    c->lastJumped = 0;
    c->pending = 0;
}

// Resets to apply to the map made from a capture
#define DEPTH_RESET_TEXEL 1
#define DEPTH_RESET_RANGE 2

// DEPTH_RESET_ flags for a depthCutStep result
static inline int depthCutResets(int cut) {
    return ((cut & DEPTH_CUT_JUMP) ? DEPTH_RESET_TEXEL : 0)
            | ((cut & DEPTH_CUT_CONFIRMED) ? DEPTH_RESET_RANGE : 0);
}

// Pending resets per staging pair; a capture with no map carries them forward
typedef struct {
    int pair[DEPTH_PAIRS];
    int carry;
} DepthResets;

void depthResetsSet(DepthResets* r, int pair, int flags);
// Returns and clears the resets due for this pair's map
int depthResetsTake(DepthResets* r, int pair);
// No map from this pair: carry its resets forward
void depthResetsDrop(DepthResets* r, int pair);

#endif
