// Panel fades, splash timing and the toast queue. No GL.
#include "xr_notice.h"
#include "xr_shared.h"

int fadeStep(Fade* fade, int shown, int64_t now, int64_t durationNs, int smooth) {
    float target = shown ? 1.0f : 0.0f;
    int64_t dt = fade->lastNs != 0 ? now - fade->lastNs : 0;
    fade->lastNs = now;
    if (!smooth || durationNs <= 0) {
        fade->level = target;
        fade->running = 0;
        return FADE_NONE;
    }
    if (fade->level == target) {
        fade->running = 0;
        return FADE_NONE;
    }
    // A new run starts on leaving rest or reversing direction
    if (!fade->running || fade->rising != shown) {
        fade->running = 1;
        fade->rising = shown;
        fade->fromNs = now;
        fade->frames = 0;
    }
    fade->frames++;
    float step = dt > 0 ? (float)dt / (float)durationNs : 0.0f;
    fade->level += shown ? step : -step;
    if ((shown && fade->level >= 1.0f) || (!shown && fade->level <= 0.0f)) {
        fade->level = target;
        fade->running = 0;
        return shown ? FADE_IN_DONE : FADE_OUT_DONE;
    }
    return FADE_NONE;
}

int splashStep(Splash* splash, int64_t now, int waiting, int64_t fadeNs) {
    if (splash->firstNs == 0) {
        splash->firstNs = now;
    }
    int64_t up = now - splash->firstNs;
    if (splash->phase == SPLASH_UP) {
        if (up < SPLASH_FLOOR_NS || (waiting != 0 && up < SPLASH_CEILING_NS)) {
            return 0;
        }
        splash->liftNs = now;
        splash->waitingAtLift = waiting;
        splash->phase = fadeNs > 0 ? SPLASH_FADING : SPLASH_GONE;
        return 1;
    }
    if (splash->phase == SPLASH_FADING && now - splash->liftNs >= fadeNs) {
        splash->phase = SPLASH_GONE;
    }
    return 0;
}

float splashLevel(const Splash* splash, int64_t now, int64_t fadeNs) {
    if (splash->phase == SPLASH_UP) {
        return 1.0f;
    }
    if (splash->phase == SPLASH_GONE || fadeNs <= 0) {
        return 0.0f;
    }
    float level = 1.0f - (float)(now - splash->liftNs) / (float)fadeNs;
    return level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
}

int splashWedges(const Splash* splash, int64_t now) {
    if (splash->phase != SPLASH_UP) {
        return SPLASH_WEDGES;
    }
    if (splash->firstNs == 0 || now < splash->firstNs) {
        return 0;
    }
    int64_t round = SPLASH_WEDGES * SPLASH_TICK_NS + SPLASH_HOLD_NS;
    int open = (int)(((now - splash->firstNs) % round) / SPLASH_TICK_NS);
    return open > SPLASH_WEDGES ? SPLASH_WEDGES : open;
}
