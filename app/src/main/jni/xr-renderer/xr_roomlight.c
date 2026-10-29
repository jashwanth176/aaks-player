// Passthrough relight, GL and OpenXR side: stall-free readback of the
// picture's average colour, the FB passthrough layer, and its per-frame
// scaling. The maths is in xr_relight.c.
#include "xr_renderer.h"
#include "xr_relight.h"

// Mip level of the 1x1 average of the AMBI_SAMPLE_TEX square sample
static int sampleTopLevel(void) {
    int level = 0;
    for (int size = AMBI_SAMPLE_TEX; size > 1; size >>= 1) {
        level++;
    }
    return level;
}

int relightWanted(XrCtx* ctx) {
    return ctx->playerMode && ctx->passthrough && ctx->fbPassthroughSupported
            && ctx->fbPassthroughState >= 0;
}

int relightLayerReady(XrCtx* ctx) {
    return relightWanted(ctx) && ctx->fbPassthroughState == 1;
}

// Double-buffered PBO readback: queue this frame's average, read last frame's
void relightSample(XrCtx* ctx) {
    if (ctx->relightFbo == 0) {
        glGenFramebuffers(1, &ctx->relightFbo);
        glGenBuffers(2, ctx->relightPbo);
        for (int i = 0; i < 2; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->relightPbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, 4, NULL, GL_STREAM_READ);
            ctx->relightPboFull[i] = 0;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->relightFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->ambiTexture, sampleTopLevel());

    int slot = ctx->relightPboSlot;
    int other = 1 - slot;
    if (ctx->relightPboFull[other]) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->relightPbo[other]);
        const unsigned char* px = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4, GL_MAP_READ_BIT);
        if (px != NULL) {
            for (int i = 0; i < 3; i++) {
                ctx->relightAvg[i] = px[i] / 255.0f;
            }
            ctx->relightAvgValid = 1;
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        ctx->relightPboFull[other] = 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->relightPbo[slot]);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    ctx->relightPboFull[slot] = 1;
    ctx->relightPboSlot = other;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static int loadPassthroughProcs(XrCtx* ctx) {
    return XR_SUCCEEDED(xrGetInstanceProcAddr(ctx->instance, "xrCreatePassthroughFB",
                                              (PFN_xrVoidFunction*)&ctx->pfnCreatePassthrough))
        && XR_SUCCEEDED(xrGetInstanceProcAddr(ctx->instance, "xrDestroyPassthroughFB",
                                              (PFN_xrVoidFunction*)&ctx->pfnDestroyPassthrough))
        && XR_SUCCEEDED(xrGetInstanceProcAddr(ctx->instance, "xrCreatePassthroughLayerFB",
                                              (PFN_xrVoidFunction*)&ctx->pfnCreatePassthroughLayer))
        && XR_SUCCEEDED(xrGetInstanceProcAddr(ctx->instance, "xrDestroyPassthroughLayerFB",
                                              (PFN_xrVoidFunction*)&ctx->pfnDestroyPassthroughLayer))
        && XR_SUCCEEDED(xrGetInstanceProcAddr(ctx->instance, "xrPassthroughLayerSetStyleFB",
                                              (PFN_xrVoidFunction*)&ctx->pfnPassthroughLayerSetStyle));
}

static void setOpacity(XrCtx* ctx, float opacity) {
    XrPassthroughStyleFB style = { XR_TYPE_PASSTHROUGH_STYLE_FB };
    style.textureOpacityFactor = opacity;
    style.edgeColor.r = 0.0f;
    style.edgeColor.g = 0.0f;
    style.edgeColor.b = 0.0f;
    style.edgeColor.a = 0.0f;
    if (XR_SUCCEEDED(ctx->pfnPassthroughLayerSetStyle(ctx->fbPassthroughLayer, &style))) {
        ctx->relightOpacitySet = opacity;
    }
}

// Created on first use. If the runtime refuses, plain blend-mode passthrough
// remains, without relighting.
static void ensurePassthrough(XrCtx* ctx) {
    if (ctx->fbPassthroughState != 0) {
        return;
    }
    ctx->fbPassthroughState = -1;
    if (!loadPassthroughProcs(ctx)) {
        LOGW("room light: passthrough functions missing, plain passthrough");
        return;
    }
    XrPassthroughCreateInfoFB create = { XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
    create.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    XrResult res = ctx->pfnCreatePassthrough(ctx->session, &create, &ctx->fbPassthrough);
    if (XR_FAILED(res)) {
        LOGW("room light: passthrough refused (%d), plain passthrough", res);
        ctx->fbPassthrough = XR_NULL_HANDLE;
        return;
    }
    XrPassthroughLayerCreateInfoFB layer = { XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
    layer.passthrough = ctx->fbPassthrough;
    layer.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    layer.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    res = ctx->pfnCreatePassthroughLayer(ctx->session, &layer, &ctx->fbPassthroughLayer);
    if (XR_FAILED(res)) {
        LOGW("room light: passthrough layer refused (%d), plain passthrough", res);
        ctx->pfnDestroyPassthrough(ctx->fbPassthrough);
        ctx->fbPassthrough = XR_NULL_HANDLE;
        ctx->fbPassthroughLayer = XR_NULL_HANDLE;
        return;
    }
    for (int i = 0; i < 3; i++) {
        ctx->relightScale[i] = 1.0f;
    }
    setOpacity(ctx, 1.0f);
    ctx->fbPassthroughState = 1;
    LOGI("room light: passthrough layer running");
}

// Once per display frame: eases the ambient light and letterbox crop, and
// moves the room colour toward the picture's (or neutral with no picture)
void relightStep(XrCtx* ctx, int64_t nowNs) {
    float dt = ctx->relightStepNs > 0 ? (float)(nowNs - ctx->relightStepNs) / 1e9f : 0.0f;
    if (dt < 0.0f || dt > 0.1f) {
        dt = dt < 0.0f ? 0.0f : 0.1f;
    }
    ctx->relightStepNs = nowNs;
    ctx->relightAmbient = relightEaseAmbient(ctx->relightAmbient, ctx->relightAmbientTarget, dt);
    // Ease letterbox changes rather than jump
    for (int i = 0; i < 2; i++) {
        ctx->contentCur[i] = relightEaseAmbient(ctx->contentCur[i], ctx->contentTarget[i], dt * 2.0f);
        if (fabsf(ctx->contentCur[i] - ctx->contentTarget[i]) < 0.0005f) {
            ctx->contentCur[i] = ctx->contentTarget[i];
        }
    }
    // Colour sample covers the picture only: one eye, bars cropped
    contentRect(ctx, 0, ctx->ambiCrop);

    if (!relightWanted(ctx)) {
        return;
    }
    ensurePassthrough(ctx);
    if (ctx->fbPassthroughState != 1) {
        return;
    }

    float target[3] = { 1.0f, 1.0f, 1.0f };
    float keep = 1.0f - RELIGHT_NEUTRAL_RETURN;
    if (ctx->relightAvgValid && ctx->everRendered && !ctx->playerScreenHidden) {
        relightScaleFor(ctx->relightAvg, ctx->relightBrightness, target);
        keep = RELIGHT_KEEP;
    }
    relightFollow(ctx->relightScale, target, keep);

    // Overall level via layer opacity over black (supported everywhere); hue
    // via the layer colour scale
    float peak = fmaxf(ctx->relightScale[0], fmaxf(ctx->relightScale[1], ctx->relightScale[2]));
    if (fabsf(peak - ctx->relightOpacitySet) > 0.002f) {
        setOpacity(ctx, peak);
    }
}

void relightDestroy(XrCtx* ctx) {
    if (ctx->fbPassthroughLayer != XR_NULL_HANDLE && ctx->pfnDestroyPassthroughLayer != NULL) {
        ctx->pfnDestroyPassthroughLayer(ctx->fbPassthroughLayer);
    }
    if (ctx->fbPassthrough != XR_NULL_HANDLE && ctx->pfnDestroyPassthrough != NULL) {
        ctx->pfnDestroyPassthrough(ctx->fbPassthrough);
    }
    ctx->fbPassthroughLayer = XR_NULL_HANDLE;
    ctx->fbPassthrough = XR_NULL_HANDLE;
    ctx->fbPassthroughState = 0;
    if (ctx->relightFbo != 0) {
        glDeleteFramebuffers(1, &ctx->relightFbo);
        glDeleteBuffers(2, ctx->relightPbo);
        ctx->relightFbo = 0;
    }
}

// Ambient light and brightness, 0..1
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetPlayerLighting(JNIEnv* env, jobject thiz,
                                                                    jlong handle, jfloat ambient,
                                                                    jfloat brightness) {
    (void)env;
    (void)thiz;
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->relightAmbientTarget = ambient < 0.0f ? 0.0f : (ambient > 1.0f ? 1.0f : ambient);
    ctx->relightBrightness = brightness < 0.0f ? 0.0f : (brightness > 1.0f ? 1.0f : brightness);
}

// Stereo crop: outward eye shift as a fraction of width, 0..0.05
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetPlayerCrop(JNIEnv* env, jobject thiz,
                                                                jlong handle, jfloat crop) {
    (void)env;
    (void)thiz;
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->playerCrop = crop < 0.0f ? 0.0f : (crop > 0.05f ? 0.05f : crop);
    LOGI("player depth: eyes moved out by %.2f percent", ctx->playerCrop * 100.0f);
}

// Screen curve, 0 flat to 1 fully wrapped. Uses the same radius mapping as the
// settings slider.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetPlayerCurve(JNIEnv* env, jobject thiz,
                                                                 jlong handle, jfloat curve) {
    (void)env;
    (void)thiz;
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    float t = curve < 0.0f ? 0.0f : (curve > 1.0f ? 1.0f : curve);
    XrVector3f p = ctx->screenPose.position;
    float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
    if (d < 0.3f) {
        d = 2.5f;
    }
    ctx->panelCurve = t;
    ctx->screenRadius = d * (1.0f + 3.0f * (1.0f - t));
    ctx->poseDirty = 1;
}

// Hides the screen (library with no video); panels stay and relight goes neutral
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetPlayerScreenHidden(JNIEnv* env, jobject thiz,
                                                                        jlong handle, jboolean hidden) {
    (void)env;
    (void)thiz;
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx != NULL) {
        ctx->playerScreenHidden = hidden ? 1 : 0;
    }
}

// Letterbox bars, top and bottom, as fractions of height (detected in Java)
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetPlayerContent(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jfloat top,
                                                                   jfloat bottom) {
    (void)env;
    (void)thiz;
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->contentTarget[0] = top < 0.0f ? 0.0f : (top > 0.4f ? 0.4f : top);
    ctx->contentTarget[1] = bottom < 0.0f ? 0.0f : (bottom > 0.4f ? 0.4f : bottom);
    LOGI("player content: bars top %.1f percent, bottom %.1f percent",
         ctx->contentTarget[0] * 100.0f, ctx->contentTarget[1] * 100.0f);
}

float screenAspect(const XrCtx* ctx) {
    // With no video the screen takes the library sheet's aspect, so the
    // handles frame the page
    if (ctx->playerMode && ctx->playerScreenHidden) {
        const int sheet = UI_PANEL_SHEET;
        return ctx->ui[sheet].texW > 0 ? (float)ctx->ui[sheet].texH / (float)ctx->ui[sheet].texW
                                       : PLAYER_BROWSE_ASPECT;
    }
    float full = (float)ctx->videoHeight / (float)ctx->videoWidth;
    return full * (1.0f - ctx->contentCur[0] - ctx->contentCur[1]);
}

// Frame uv rect (bottom up) an eye reads: whole frame for mono, left/top half
// for eye 0 of SBS/OU, the other half for eye 1, minus letterbox bars
void contentRect(const XrCtx* ctx, int eye, float out[4]) {
    float base[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (ctx->stereoMode == DEPTH_MODE_NATIVE_SBS) {
        base[0] = eye == 0 ? 0.0f : 0.5f;
        base[2] = 0.5f;
    }
    else if (ctx->stereoMode == DEPTH_MODE_NATIVE_OU) {
        base[1] = eye == 0 ? 0.5f : 0.0f;
        base[3] = 0.5f;
    }
    // Bottom-up uv: the bottom bar is at low y
    float top = ctx->contentCur[0];
    float bottom = ctx->contentCur[1];
    out[0] = base[0];
    out[1] = base[1] + bottom * base[3];
    out[2] = base[2];
    out[3] = (1.0f - top - bottom) * base[3];
}
