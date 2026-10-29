// Passthrough relight maths, see xr_relight.h
#include <math.h>

#include "xr_relight.h"

static float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static void rgbToHsv(const float rgb[3], float* h, float* s, float* v) {
    float r = rgb[0], g = rgb[1], b = rgb[2];
    float hi = fmaxf(r, fmaxf(g, b));
    float lo = fminf(r, fminf(g, b));
    float d = hi - lo;
    *v = hi;
    *s = hi > 0.0f ? d / hi : 0.0f;
    if (d <= 0.0f) {
        *h = 0.0f;
        return;
    }
    float hue;
    if (hi == r) {
        hue = (g - b) / d;
    }
    else if (hi == g) {
        hue = 2.0f + (b - r) / d;
    }
    else {
        hue = 4.0f + (r - g) / d;
    }
    hue /= 6.0f;
    *h = hue < 0.0f ? hue + 1.0f : hue;
}

static void hsvToRgb(float h, float s, float v, float out[3]) {
    float f = h * 6.0f;
    int sector = (int)floorf(f);
    float t = f - (float)sector;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * t);
    float u = v * (1.0f - s * (1.0f - t));
    switch (((sector % 6) + 6) % 6) {
        case 0: out[0] = v; out[1] = u; out[2] = p; break;
        case 1: out[0] = q; out[1] = v; out[2] = p; break;
        case 2: out[0] = p; out[1] = v; out[2] = u; break;
        case 3: out[0] = p; out[1] = q; out[2] = v; break;
        case 4: out[0] = u; out[1] = p; out[2] = v; break;
        default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

void relightScaleFor(const float avg[3], float brightness, float out[3]) {
    float b = clamp01(brightness);
    float h, s, v;
    rgbToHsv(avg, &h, &s, &v);

    // Desaturate more at low brightness; power runs 1.2 (b = 0) to 0.8 (b = 1)
    float power = 1.2f + 0.4f * (sigmoid(8.0f * (b - 0.85f)) - 0.0011125f) / -0.7674122f;
    s = s * ((power + 1.0f) - powf(s, power)) / (power + 1.0f);

    // Value through an S curve set by brightness, offset so black maps to 0.
    // At the default a dark scene leaves a few percent of the room and a bright
    // one about two thirds; brightness 0 gives black.
    float steep = sigmoid(8.0f * (b - 0.4f)) - 0.0391657f;
    float lift = sigmoid(8.0f * (b - 0.8f)) - 0.0016588f;
    float centre = 0.2f - 0.1f * lift;
    float k = 8.0f * steep;
    v = sigmoid(k * (v - centre)) - sigmoid(-k * (0.2f + lift));

    hsvToRgb(h, clamp01(s), clamp01(v), out);
}

float relightGlowStrength(float brightness) {
    return clamp01((sigmoid(8.0f * (clamp01(brightness) - 0.2f)) - 0.16798f) / 0.83036f);
}

void relightFollow(float current[3], const float target[3], float keep) {
    for (int i = 0; i < 3; i++) {
        current[i] = current[i] * keep + target[i] * (1.0f - keep);
    }
}

float relightEaseAmbient(float current, float target, float dt) {
    float step = dt * RELIGHT_AMBIENT_RATE;
    if (step > 1.0f) step = 1.0f;
    if (step < 0.0f) step = 0.0f;
    return current + (target - current) * step;
}
