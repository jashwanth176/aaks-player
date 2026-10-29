
// Screen move/resize handle maths: reel distance, distance and size limits,
// elastic overshoot and spring-back. Applied to the screen in xr_input.c.

#ifndef XR_HANDLES_H
#define XR_HANDLES_H

#include <stdint.h>

// Screen distance limits from the eyes, metres
#define HANDLE_DIST_MIN 0.5f
#define HANDLE_DIST_MAX 10.0f

// Width range scales linearly with distance: 1.92 m x [0.3, 1.6] at the near
// limit up to 1.92 m x [2.0, 7.0] at the far limit. 1.92 m is 16:9 at scale 1.
#define HANDLE_BASE_WIDTH 1.92f
#define HANDLE_SCALE_MIN_NEAR 0.3f
#define HANDLE_SCALE_MIN_SPAN 1.7f
#define HANDLE_SCALE_MAX_NEAR 1.6f
#define HANDLE_SCALE_MAX_SPAN 5.4f

// Default screen placement
#define HANDLE_DEFAULT_DIST 2.5f
#define HANDLE_DEFAULT_WIDTH 2.4f

// Elastic overshoot past a limit, as a fraction of that limit
#define HANDLE_DIST_STRETCH 0.05f
#define HANDLE_SIZE_OVERSHOOT 0.1f

// Spring-back duration after release past a limit
#define HANDLE_SPRING_NS 180000000LL

// Screen distance scales as (hand distance ratio)^HANDLE_REEL_POWER, so
// pulling the hand in by a fifth brings a 2.5 m screen to about 1.6 m
#define HANDLE_REEL_POWER 2.0f
// Minimum hand distance, avoids dividing by zero for a grab at the face
#define HANDLE_HAND_MIN 0.15f

// Allowed widths at a distance, metres
void handleWidthRange(float dist, float* minWidth, float* maxWidth);

// Position of a width within its range at a distance, 0..1
float handleSizeRatio(float dist, float width);

// Width at a distance for a 0..1 position in the range
float handleWidthFor(float dist, float ratio);

// Clamps to lo..hi with a cubic ease that stops a margin past either end
float handleElastic(float value, float lo, float hi, float marginLo, float marginHi);

// Unclamped screen distance from a reel: grab distance scaled by the change in
// hand distance
float handleReelDistance(float screenDistAtGrab, float handDistAtGrab, float handDistNow);

// Distance with elastic limits applied
float handleElasticDistance(float dist);

// Width with elastic limits for its distance
float handleElasticWidth(float dist, float width);

// Ease-out spring-back progress, 0 at start, 1 when done
float handleSpringEase(int64_t startNs, int64_t nowNs);

// While carried, the screen stays upright within HANDLE_TILT_LEVEL_DEG of eye
// level, then smoothsteps to facing the eyes over HANDLE_TILT_BLEND_DEG
#define HANDLE_TILT_LEVEL_DEG 6.0f
#define HANDLE_TILT_BLEND_DEG 14.0f
// Facing catch-up rate, fraction of the remaining turn per second
#define HANDLE_TURN_RATE 10.0f

// Screen pitch in radians for the eyes' elevation as seen from the screen
// (positive with the eyes above it)
float handleFacePitch(float eyeElevation);

// One frame's step of an angle toward a target, the short way round
float handleTurnToward(float current, float target, float dtSec);

#endif
