// Debug tuning properties (setprop) and frame capture of the warp inputs.
// Property polling is compiled out of release builds: it costs syscalls on
// the frame loop, and a stale property would silently override settings.
#include "xr_renderer.h"

#ifdef XR_DEBUG_KNOBS

// Integer property scaled into target; unchanged if unset or invalid
static void propScaled(const char* name, float* target, float scale, long maxRaw) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end != value && v >= 0 && v <= maxRaw) {
        *target = v * scale;
    }
}

static void propPercent(const char* name, float* target) {
    propScaled(name, target, 0.01f, 100);
}

void propFlag(const char* name, int* target) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    *target = value[0] != '0';
}

// Integer property; unchanged if unset or invalid
static void propInt(const char* name, int* target, long maxRaw) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end != value && v >= 0 && v <= maxRaw) {
        *target = (int)v;
    }
}

// As propInt, logging changes as events
static void propIntEvent(const char* name, int* target, long maxRaw, const char* label) {
    int was = *target;
    propInt(name, target, maxRaw);
    if (*target != was) {
        LOGEV("%s %d", label, *target);
    }
}

// Panel fade length in ms, 0 for the default
static void readFadeKnob(XrCtx* ctx) {
    int ms = ctx->fadeKnobMs;
    propInt(PROP_FADE_MS, &ms, FADE_KNOB_MAX_MS);
    if (ms != ctx->fadeKnobMs) {
        ctx->fadeKnobMs = ms;
        ctx->fadeNs = ms > 0 ? ms * 1000000LL : FADE_NS;
        LOGEV("fade knob %d ms", ms);
    }
}

// Properties used at session start, read once
void readStartKnobs(XrCtx* ctx) {
    // Before the first frame, so the splash uses it
    readFadeKnob(ctx);
    propInt(PROP_REFRESH, &ctx->refreshKnob, RATE_KNOB_MAX);
    if (ctx->refreshKnob > 0) {
        LOGEV("refresh knob %d Hz", ctx->refreshKnob);
    }
    int perf = ctx->perfLevel;
    propInt(PROP_PERF_LEVEL, &ctx->perfLevel, PERF_LEVEL_BOOST);
    if (ctx->perfLevel != perf) {
        LOGEV("performance level knob %d", ctx->perfLevel);
    }
    char fail[PROP_VALUE_MAX];
    fail[0] = '\0';
    if (__system_property_get(PROP_START_FAIL, fail) > 0 && strcmp(fail, "exit") == 0) {
        ctx->exitKnob = 1;
        LOGEV("exit knob: the session will be asked to end %lld s after it is focused",
              EXIT_KNOB_DELAY_NS / 1000000000LL);
    }
}

// Whether startup should be forced to fail at this step (the step then makes
// a request the runtime will refuse)
int startFailKnob(const char* step) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(PROP_START_FAIL, value) <= 0 || strcmp(value, step) != 0) {
        return 0;
    }
    LOGEV("start fail knob: %s will fail", step);
    return 1;
}

// Polls the tuning properties. A capture fires once per new value of the
// capture property; the value is used as the filename tag.
void pollCaptureRequest(XrCtx* ctx) {
    if (++ctx->capturePollCounter < CAPTURE_POLL_FRAMES) {
        return;
    }
    ctx->capturePollCounter = 0;

    // Hz, 0 for automatic
    int refresh = ctx->refreshKnob;
    propInt(PROP_REFRESH, &refresh, RATE_KNOB_MAX);
    if (refresh != ctx->refreshKnob) {
        setRefreshKnob(ctx, refresh);
    }
    // Maps per second, 0 for the setting
    int depthRate = ctx->depthRateKnob;
    propInt(PROP_DEPTH_RATE, &depthRate, DEPTH_RATE_MAX);
    if (depthRate != ctx->depthRateKnob) {
        ctx->depthRateKnob = depthRate;
        setDepthRate(ctx, depthRate > 0 ? depthRate : ctx->depthRateSetting,
                     depthRate > 0 ? "depth rate knob" : "knob cleared, back to the setting");
    }
    // 0 none, 1 sustained high, 2 boost
    int perf = ctx->perfLevel;
    propInt(PROP_PERF_LEVEL, &perf, PERF_LEVEL_BOOST);
    if (perf != ctx->perfLevel) {
        setPerfLevel(ctx, perf);
    }

    readFadeKnob(ctx);
    // ms, 0 for none; picked up by the stage thread on its next map
    propIntEvent(PROP_DEPTH_TAU, &ctx->depthTauMs, DEPTH_TAU_MAX_MS, "depth tau ms");
    propIntEvent(PROP_RANGE_TAU, &ctx->rangeTauMs, DEPTH_RANGE_TAU_MAX_MS, "depth range tau ms");
    // 0 off, 1 on, 2 on with a line per capture
    propIntEvent(PROP_DEPTH_CUT, &ctx->depthCutLevel, 2, "depth cut level");
    // Warp edge fixes; fade in pixels
    propIntEvent(PROP_SRC_INSET, &ctx->srcInsetOn, 1, "warp source inset");
    propIntEvent(PROP_EDGE_FADE, &ctx->edgeFadePx, EDGE_FADE_MAX_PX, "warp edge fade px");
    propIntEvent(PROP_DEPTH_CUBIC, &ctx->depthCubic, 1, "warp cubic depth");
    propIntEvent(PROP_SEAM_INSET, &ctx->seamInset, 1, "eye seam inset");
    propPercent(PROP_UPSAMPLE_SIGMA, &ctx->upsampleSigmaR);
    propPercent(PROP_DEPTH_SHARP, &ctx->depthSharp);
    propFlag(PROP_OVERLAY, &ctx->overlayVisible);
    propFlag(PROP_UPSAMPLE, &ctx->upsampleEnabled);
    propFlag(PROP_OCCLUSION, &ctx->occlusionEnabled);
    propPercent(PROP_CONVERGENCE, &ctx->convergence);
    // 0.1% of frame width, as the preference
    propScaled(PROP_SEPARATION, &ctx->separationOverride, 0.001f, 50);
    // 0.1 m, as the preferences
    propScaled(PROP_DISTANCE, &ctx->distanceOverride, 0.1f, 80);
    propScaled(PROP_SCREEN, &ctx->screenOverride, 0.1f, 120);
    propPercent(PROP_DEPTH_GLOBAL, &ctx->depthGlobal);
    propScaled(PROP_DEPTH_LOCAL, &ctx->depthLocal, 0.01f, 400);
    // Cutoff in 0.1 Hz, beta in 0.5 units
    propScaled(PROP_POINTER_CUTOFF, &ctx->pointerMinCutoff, 0.1f, 200);
    propScaled(PROP_POINTER_BETA, &ctx->pointerBeta, 0.5f, 100);
    propScaled(PROP_AIM_CUTOFF, &ctx->aimMinCutoff, 0.1f, 200);
    propScaled(PROP_AIM_BETA, &ctx->aimBeta, 0.5f, 100);
    // Millimetres
    propScaled(PROP_BEAM_WIDTH, &ctx->beamWidth, 0.001f, 100);
    // 0.1 s
    propScaled(PROP_POINTER_WAKE, &ctx->pointerWake, 0.1f, 100);
    propScaled(PROP_POINTER_SLEEP, &ctx->pointerSleep, 0.1f, 600);
    // 0 off, 1 normal, 2 quality
    propInt(PROP_SHARPEN, &ctx->sharpenMode, 2);
    propInt(PROP_SUPERSAMPLE, &ctx->supersampleMode, 2);
    // 0 forces the glow off, 1..100 forces that intensity, unset uses the setting
    propInt(PROP_AMBILIGHT, &ctx->ambiOverride, 100);
    propPercent(PROP_AMBI_SMOOTH, &ctx->ambiSmooth);
    // 0 samples the whole frame including black bars; 1 or unset crops to the picture
    propFlag(PROP_LETTERBOX, &ctx->ambiBarDetect);
    // 1 (default) normalises glow luma and rolls off dark edges; 0 uses the raw sample
    propIntEvent(PROP_GLOW_NORM, &ctx->glowNorm, 1, "glow normalisation");
    // 0 forces no room, 1..ROOM_STYLE_LAST forces a style (1 home theater,
    // 2 grand cinema, 3 synthwave). Only the resident room can load.
    propInt(PROP_ROOM, &ctx->roomOverride, ROOM_STYLE_LAST);
    // Percent, 0 for the room's own value. Scale applies to baked rooms only
    // and rebuilds the geometry.
    propScaled(PROP_ROOM_SCALE, &ctx->roomScaleOverride, 0.01f, 400);
    propScaled(PROP_ROOM_DIM, &ctx->roomDimOverride, 0.01f, 200);

    if (ctx->captureDir[0] == '\0') {
        return;
    }

    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(CAPTURE_PROP, value) <= 0 || value[0] == '\0') {
        return;
    }
    if (strcmp(value, ctx->lastCaptureTag) == 0) {
        return;
    }
    strncpy(ctx->lastCaptureTag, value, sizeof(ctx->lastCaptureTag) - 1);
    strncpy(ctx->captureTag, value, sizeof(ctx->captureTag) - 1);
    ctx->captureRequested = 1;
    LOGI("capture: request %s", ctx->captureTag);
}

#else

// Release builds read no properties and cannot request captures
void readStartKnobs(XrCtx* ctx) {
    (void)ctx;
}

int startFailKnob(const char* step) {
    (void)step;
    return 0;
}

void propFlag(const char* name, int* target) {
    (void)name;
    (void)target;
}

void pollCaptureRequest(XrCtx* ctx) {
    (void)ctx;
}

#endif

void writeCapture(XrCtx* ctx, const char* what, const void* data, size_t bytes) {
    if (data == NULL) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/cap_%s_%s.raw", ctx->captureDir, ctx->captureTag, what);
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        LOGE("capture: cannot write %s", path);
        return;
    }
    size_t written = fwrite(data, 1, bytes, f);
    fclose(f);
    LOGI("capture: %s %zu bytes", path, written);
}

// Reads back the depth texture the warp sampled this frame, for exact
// offline reproduction. Depth is in alpha, the guide image in rgb.
void writeCaptureDepthTexture(XrCtx* ctx) {
    const int w = ctx->depthTexW;
    const int h = ctx->depthTexH;
    const int count = w * h;
    unsigned char* rgba = malloc((size_t)count * 4);
    unsigned char* red = malloc((size_t)count);
    if (rgba == NULL || red == NULL) {
        free(rgba);
        free(red);
        return;
    }

    waitForDepthSlot(ctx);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->depthTextures[ctx->depthReadIndex], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        for (int i = 0; i < count; i++) {
            red[i] = rgba[i * 4 + 3];
        }
        writeCapture(ctx, "depthtex", red, (size_t)count);
        writeCapture(ctx, "guidetex", rgba, (size_t)count * 4);
        // Map size, then the covered frame rect (all of it); read by
        // tools/warp_lab.py
        float shape[6] = { (float)w, (float)h, 0.0f, 0.0f, 1.0f, 1.0f };
        writeCapture(ctx, "depthrect", shape, sizeof(shape));
    }
    else {
        LOGW("capture: depth texture not readable");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    free(rgba);
    free(red);
}
