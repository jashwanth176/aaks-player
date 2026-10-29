#include <math.h>

#include "xr_handles.h"

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void handleWidthRange(float dist, float* minWidth, float* maxWidth) {
    float t = clamp01((dist - HANDLE_DIST_MIN) / (HANDLE_DIST_MAX - HANDLE_DIST_MIN));
    *minWidth = HANDLE_BASE_WIDTH * (HANDLE_SCALE_MIN_NEAR + HANDLE_SCALE_MIN_SPAN * t);
    *maxWidth = HANDLE_BASE_WIDTH * (HANDLE_SCALE_MAX_NEAR + HANDLE_SCALE_MAX_SPAN * t);
}

float handleSizeRatio(float dist, float width) {
    float lo, hi;
    handleWidthRange(dist, &lo, &hi);
    return hi > lo ? clamp01((width - lo) / (hi - lo)) : 0.0f;
}

float handleWidthFor(float dist, float ratio) {
    float lo, hi;
    handleWidthRange(dist, &lo, &hi);
    return lo + clamp01(ratio) * (hi - lo);
}

// Overshoot past a limit: margin * (1 - (1 - x)^3), x = over / (3 * margin)
static float give(float over, float margin) {
    if (margin <= 0.0f) {
        return 0.0f;
    }
    float x = over / (3.0f * margin);
    if (x > 1.0f) {
        x = 1.0f;
    }
    float r = 1.0f - x;
    return margin * (1.0f - r * r * r);
}

float handleElastic(float value, float lo, float hi, float marginLo, float marginHi) {
    if (value < lo) {
        return lo - give(lo - value, marginLo);
    }
    if (value > hi) {
        return hi + give(value - hi, marginHi);
    }
    return value;
}

float handleReelDistance(float screenDistAtGrab, float handDistAtGrab, float handDistNow) {
    float h0 = handDistAtGrab < HANDLE_HAND_MIN ? HANDLE_HAND_MIN : handDistAtGrab;
    float h = handDistNow < HANDLE_HAND_MIN ? HANDLE_HAND_MIN : handDistNow;
    return screenDistAtGrab * powf(h / h0, HANDLE_REEL_POWER);
}

float handleElasticDistance(float dist) {
    return handleElastic(dist, HANDLE_DIST_MIN, HANDLE_DIST_MAX,
                         HANDLE_DIST_MIN * HANDLE_DIST_STRETCH,
                         HANDLE_DIST_MAX * HANDLE_DIST_STRETCH);
}

float handleElasticWidth(float dist, float width) {
    float lo, hi;
    handleWidthRange(dist, &lo, &hi);
    return handleElastic(width, lo, hi, lo * HANDLE_SIZE_OVERSHOOT, hi * HANDLE_SIZE_OVERSHOOT);
}

float handleFacePitch(float eyeElevation) {
    float deg = fabsf(eyeElevation) * (180.0f / (float)M_PI);
    if (deg <= HANDLE_TILT_LEVEL_DEG) {
        return 0.0f;
    }
    float t = clamp01((deg - HANDLE_TILT_LEVEL_DEG) / HANDLE_TILT_BLEND_DEG);
    return eyeElevation * t * t * (3.0f - 2.0f * t);
}

float handleTurnToward(float current, float target, float dtSec) {
    float diff = target - current;
    while (diff > (float)M_PI) diff -= 2.0f * (float)M_PI;
    while (diff < -(float)M_PI) diff += 2.0f * (float)M_PI;
    float step = HANDLE_TURN_RATE * dtSec;
    if (step > 1.0f) {
        step = 1.0f;
    }
    if (step < 0.0f) {
        step = 0.0f;
    }
    return current + diff * step;
}

float handleSpringEase(int64_t startNs, int64_t nowNs) {
    float t = clamp01((float)(nowNs - startNs) / (float)HANDLE_SPRING_NS);
    float r = 1.0f - t;
    return 1.0f - r * r * r;
}
