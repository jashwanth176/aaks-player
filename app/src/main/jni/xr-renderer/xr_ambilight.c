// Ambilight: the frame downsampled to a small colour texture, letterbox
// detection to exclude black bars, and the glow drawn from it behind the screen.
#include "xr_renderer.h"
#include "xr_shaders.h"
#include "xr_relight.h"

// Frame colour sample program, built through the video colour path so the
// glow and relight match what the screen shows
int linkAmbiProgram(XrCtx* ctx) {
    if (!linkVideoProgram(ctx, &ctx->ambiProgram, AMBI_FRAGMENT_SRC, "frame colour sample")) {
        return 0;
    }
    ctx->ambiTexMatrixUniform = glGetUniformLocation(ctx->ambiProgram, "u_texmatrix");
    ctx->ambiCropUniform = glGetUniformLocation(ctx->ambiProgram, "u_crop");
    ctx->ambiGradeOnUniform = glGetUniformLocation(ctx->ambiProgram, "u_gradeOn");
    ctx->ambiGradeUniform = glGetUniformLocation(ctx->ambiProgram, "u_grade");
    glUseProgram(ctx->ambiProgram);
    glUniform1i(glGetUniformLocation(ctx->ambiProgram, "u_texture"), 0);
    return 1;
}

int initAmbilight(XrCtx* ctx) {
    if (!linkAmbiProgram(ctx)) {
        return 0;
    }

    glGenTextures(1, &ctx->ambiTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // The glow samples outside 0..1; clamping extends the edge colours outward
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->ambiFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->ambiTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("ambilight framebuffer incomplete: 0x%x", status);
        return 0;
    }

    // Letterbox detector target, only drawn and read back
    glGenTextures(1, &ctx->ambiDetectTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiDetectTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->ambiDetectFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiDetectFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->ambiDetectTexture, 0);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("letterbox framebuffer incomplete: 0x%x", status);
        return 0;
    }

    // Readback PBO, read a frame later to avoid a stall
    glGenBuffers(1, &ctx->ambiDetectPbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->ambiDetectPbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, AMBI_SAMPLE_TEX * AMBI_SAMPLE_TEX * 4, NULL,
                 GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    if (!linkProgram(&ctx->glowProgram, GLOW_FRAGMENT_SRC, "glow")) {
        return 0;
    }
    ctx->glowIntensityUniform = glGetUniformLocation(ctx->glowProgram, "u_intensity");
    ctx->glowBlurUniform = glGetUniformLocation(ctx->glowProgram, "u_blur");
    ctx->glowScaleUniform = glGetUniformLocation(ctx->glowProgram, "u_scale");
    ctx->glowFrameUniform = glGetUniformLocation(ctx->glowProgram, "u_frame");
    ctx->glowFrameLookUniform = glGetUniformLocation(ctx->glowProgram, "u_frameLook");
    glUseProgram(ctx->glowProgram);
    glUniform1i(glGetUniformLocation(ctx->glowProgram, "u_texture"), 0);

    // Normalised edge copy the glow is drawn from; same filtering and clamping
    // as the sample texture. Constant uniforms set once.
    if (!linkProgram(&ctx->glowEdgeProgram, GLOW_EDGE_FRAGMENT_SRC, "glow edge")) {
        return 0;
    }
    glUseProgram(ctx->glowEdgeProgram);
    glUniform1i(glGetUniformLocation(ctx->glowEdgeProgram, "u_texture"), 0);
    glUniform3f(glGetUniformLocation(ctx->glowEdgeProgram, "u_luma"),
                GLOW_LUMA_TARGET, GLOW_LUMA_FLOOR, GLOW_LUMA_FULL);
    glUniform2f(glGetUniformLocation(ctx->glowEdgeProgram, "u_lit"), GLOW_LIT_LO, GLOW_LIT_HI);
    glUniform1f(glGetUniformLocation(ctx->glowEdgeProgram, "u_reach"), glowReachTexels());
    glUniform1i(glGetUniformLocation(ctx->glowEdgeProgram, "u_steps"), glowInwardSteps());

    glGenTextures(1, &ctx->glowEdgeTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->glowEdgeTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->glowEdgeFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->glowEdgeFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->glowEdgeTexture, 0);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("glow edge framebuffer incomplete: 0x%x", status);
        return 0;
    }

    // Attached per frame to the acquired swapchain image
    glGenFramebuffers(1, &ctx->glowFbo);

    LOGI("ambilight ready, sampling %dx%d into a %dx%d glow, luma %.2f from %.2f to %.2f, "
         "edge reach %.1f texels, inward %d",
         AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX, GLOW_TEX, GLOW_TEX, GLOW_LUMA_TARGET,
         GLOW_LUMA_FLOOR, GLOW_LUMA_FULL, glowReachTexels(), glowInwardSteps());
    return 1;
}

// Effective glow state: the setting (per room when in one), overridden by
// the debug property if set
void ambiEffective(XrCtx* ctx, int* on, float* level) {
    // Always drawn in player mode; the ambient light level is applied via the
    // frame uniforms
    if (ctx->playerMode) {
        *on = 1;
        *level = 1.0f;
        return;
    }
    int enabled = roomGlowOn(ctx, roomEffective(ctx));
    float value = ctx->ambiIntensity;
    if (ctx->ambiOverride >= 0) {
        enabled = ctx->ambiOverride > 0;
        value = ctx->ambiOverride / 100.0f;
    }
    *on = enabled;
    *level = value;
}

// Uncropped frame, used by the detector
static const float AMBI_CROP_FULL[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

// Per-edge hysteresis so brief dark scenes do not move the crop
static void ambiBarEdge(XrCtx* ctx, int edge, int measured) {
    if (measured > ctx->ambiBarApplied[edge]) {
        ctx->ambiBarShrinkTicks[edge] = 0;
        if (ctx->ambiBarGrowTicks[edge] == 0 || measured < ctx->ambiBarGrowMin[edge]) {
            ctx->ambiBarGrowMin[edge] = measured;
        }
        ctx->ambiBarGrowTicks[edge]++;
        if (ctx->ambiBarGrowTicks[edge] >= AMBI_BAR_APPLY) {
            ctx->ambiBarApplied[edge] = ctx->ambiBarGrowMin[edge];
            ctx->ambiBarGrowTicks[edge] = 0;
        }
        return;
    }
    if (measured < ctx->ambiBarApplied[edge]) {
        ctx->ambiBarGrowTicks[edge] = 0;
        if (ctx->ambiBarShrinkTicks[edge] == 0 || measured > ctx->ambiBarShrinkMax[edge]) {
            ctx->ambiBarShrinkMax[edge] = measured;
        }
        ctx->ambiBarShrinkTicks[edge]++;
        if (ctx->ambiBarShrinkTicks[edge] >= AMBI_BAR_RELEASE) {
            ctx->ambiBarApplied[edge] = ctx->ambiBarShrinkMax[edge];
            ctx->ambiBarShrinkTicks[edge] = 0;
        }
        return;
    }
    ctx->ambiBarGrowTicks[edge] = 0;
    ctx->ambiBarShrinkTicks[edge] = 0;
}

// Measures black bars and updates the crop. Rows are in readback order (may be
// flipped vs the picture); the crop is applied in the same space.
static void detectAmbiBars(XrCtx* ctx, const unsigned char* rgba) {
    const int n = AMBI_SAMPLE_TEX;
    float rowSum[AMBI_SAMPLE_TEX];
    float colSum[AMBI_SAMPLE_TEX];
    for (int i = 0; i < n; i++) {
        rowSum[i] = 0.0f;
        colSum[i] = 0.0f;
    }
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            const unsigned char* p = rgba + ((size_t)y * n + x) * 4;
            // Max channel, not luma, so saturated blue is not taken for a bar
            int m = p[0] > p[1] ? p[0] : p[1];
            if (p[2] > m) {
                m = p[2];
            }
            float l = m * (1.0f / 255.0f);
            rowSum[y] += l;
            colSum[x] += l;
        }
    }

    int rowBar[AMBI_SAMPLE_TEX];
    int colBar[AMBI_SAMPLE_TEX];
    int rowsBar = 0;
    int colsBar = 0;
    for (int i = 0; i < n; i++) {
        rowBar[i] = rowSum[i] * (1.0f / n) < AMBI_BAR_LUMA;
        colBar[i] = colSum[i] * (1.0f / n) < AMBI_BAR_LUMA;
        rowsBar += rowBar[i];
        colsBar += colBar[i];
    }

    // An axis that is bar end to end is a fade or black frame, not a letterbox;
    // leave it unmeasured
    int measured[AMBI_EDGES];
    memset(measured, 0, sizeof(measured));
    if (rowsBar < n) {
        while (measured[AMBI_EDGE_BOTTOM] < n && rowBar[measured[AMBI_EDGE_BOTTOM]]) {
            measured[AMBI_EDGE_BOTTOM]++;
        }
        while (measured[AMBI_EDGE_TOP] < n && rowBar[n - 1 - measured[AMBI_EDGE_TOP]]) {
            measured[AMBI_EDGE_TOP]++;
        }
    }
    if (colsBar < n) {
        while (measured[AMBI_EDGE_LEFT] < n && colBar[measured[AMBI_EDGE_LEFT]]) {
            measured[AMBI_EDGE_LEFT]++;
        }
        while (measured[AMBI_EDGE_RIGHT] < n && colBar[n - 1 - measured[AMBI_EDGE_RIGHT]]) {
            measured[AMBI_EDGE_RIGHT]++;
        }
    }

    int changed = 0;
    for (int e = 0; e < AMBI_EDGES; e++) {
        if (measured[e] > AMBI_BAR_MAX) {
            measured[e] = AMBI_BAR_MAX;
        }
        int before = ctx->ambiBarApplied[e];
        ambiBarEdge(ctx, e, measured[e]);
        if (ctx->ambiBarApplied[e] != before) {
            changed = 1;
        }
    }
    if (!changed) {
        return;
    }

    int left = ctx->ambiBarApplied[AMBI_EDGE_LEFT];
    int right = ctx->ambiBarApplied[AMBI_EDGE_RIGHT];
    int bottom = ctx->ambiBarApplied[AMBI_EDGE_BOTTOM];
    int top = ctx->ambiBarApplied[AMBI_EDGE_TOP];
    ctx->ambiCrop[0] = left / (float)n;
    ctx->ambiCrop[1] = bottom / (float)n;
    ctx->ambiCrop[2] = (n - left - right) / (float)n;
    ctx->ambiCrop[3] = (n - bottom - top) / (float)n;
    // Top/bottom are readback v = 1 / v = 0, see above
    LOGI("ambilight bars: top %d bottom %d left %d right %d", top, bottom, left, right);
}

// Draws the uncropped frame and queues an async readback for next frame
void runAmbiBarDetect(XrCtx* ctx, const float* texMatrix) {
    const int n = AMBI_SAMPLE_TEX;
    if (ctx->ambiDetectPbo == 0) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiDetectFbo);
    glViewport(0, 0, n, n);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->ambiProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->ambiTexMatrixUniform, 1, GL_FALSE, texMatrix);
    glUniform4fv(ctx->ambiCropUniform, 1, AMBI_CROP_FULL);
    // No crop (would hide the bars), no temporal blend (would lag after cuts)
    // and no grade (could lift black bars to grey)
    glDisable(GL_BLEND);
    setGradeUniforms(ctx, ctx->ambiGradeOnUniform, ctx->ambiGradeUniform, 0);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->ambiDetectPbo);
    glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ctx->ambiDetectPending = 1;
}

// Processes the previous frame's readback; mapping it does not stall
void finishAmbiBarDetect(XrCtx* ctx) {
    const int n = AMBI_SAMPLE_TEX;
    if (!ctx->ambiDetectPending) {
        return;
    }
    ctx->ambiDetectPending = 0;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->ambiDetectPbo);
    const unsigned char* rgba = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, n * n * 4,
                                                 GL_MAP_READ_BIT);
    if (rgba != NULL) {
        detectAmbiBars(ctx, rgba);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

// Downsamples the frame into the colour sample texture (also used by relight)
void runFrameColorSample(XrCtx* ctx, const float* texMatrix) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiFbo);
    glViewport(0, 0, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->ambiProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->ambiTexMatrixUniform, 1, GL_FALSE, texMatrix);
    // Apply the detected crop. With detection off the crop is ignored but kept.
    glUniform4fv(ctx->ambiCropUniform, 1,
                 ctx->ambiBarDetect || ctx->playerMode ? ctx->ambiCrop : AMBI_CROP_FULL);
    // Same grade as the screen
    setGradeUniforms(ctx, ctx->ambiGradeOnUniform, ctx->ambiGradeUniform, ctx->gradeOn);

    // Temporal blend (about ten frames) so cuts do not strobe the glow. The
    // first frame is written directly.
    if (ctx->ambiSeeded) {
        glEnable(GL_BLEND);
        glBlendColor(ctx->ambiSmooth, ctx->ambiSmooth, ctx->ambiSmooth, ctx->ambiSmooth);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    }

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
    ctx->ambiSeeded = 1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Luma normalisation and dark-edge roll-off into the glow edge texture. Only
// the 124 ring texels do real work (about 8000 fetches of a 4 KB texture).
static void runGlowEdge(XrCtx* ctx) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->glowEdgeFbo);
    glViewport(0, 0, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }
    glDisable(GL_BLEND);

    glUseProgram(ctx->glowEdgeProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Renders the glow image the compositor places behind the screen
void runGlowRender(XrCtx* ctx) {
    if (ctx->glowSwapchain == XR_NULL_HANDLE) {
        return;
    }
    // Read once so the edge pass and the draw agree
    int norm = ctx->glowNorm;
    if (norm) {
        runGlowEdge(ctx);
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->glowSwapchain, &acquire, &index),
                 "acquire glow image")) {
        return;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->glowSwapchain, &wait);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->glowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->glowImages[index].image, 0);
    glViewport(0, 0, GLOW_TEX, GLOW_TEX);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    int on;
    float level;
    ambiEffective(ctx, &on, &level);

    glUseProgram(ctx->glowProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, norm ? ctx->glowEdgeTexture : ctx->ambiTexture);
    glUniform1f(ctx->glowIntensityUniform, level);
    glUniform1f(ctx->glowBlurUniform, norm ? 1.0f : 0.0f);
    glUniform1f(ctx->glowScaleUniform, glowScaleFor(ctx));
    // Same frame crop as the eye so they line up
    setFrameUniforms(ctx, ctx->glowFrameUniform, ctx->glowFrameLookUniform,
                     ctx->stereoMode == DEPTH_MODE_NATIVE_SBS || ctx->stereoMode == DEPTH_MODE_NATIVE_OU
                             ? ctx->playerCrop : 0.0f, 0.0f);
    ctx->glowDrawnAmbient = ctx->relightAmbient;
    ctx->glowDrawnBrightness = ctx->relightBrightness;

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->glowSwapchain, &release);
    ctx->glowRendered = 1;
    ctx->glowDrawnOn = 1;
    ctx->glowDrawnLevel = level;
}

int glowStale(XrCtx* ctx) {
    int on;
    float level;
    ambiEffective(ctx, &on, &level);
    return on && (!ctx->glowDrawnOn || level != ctx->glowDrawnLevel
                  || (ctx->playerMode && (ctx->glowDrawnAmbient != ctx->relightAmbient
                                          || ctx->glowDrawnBrightness != ctx->relightBrightness)));
}

// Redraws the glow without a new video frame (e.g. paused)
void redrawGlow(XrCtx* ctx, const float* texMatrix) {
    if (!ctx->glowDrawnOn) {
        runFrameColorSample(ctx, texMatrix);
    }
    runGlowRender(ctx);
}

// Glow size relative to the screen; larger in player mode
float glowScaleFor(XrCtx* ctx) {
    return ctx->playerMode ? RELIGHT_GLOW_SCALE : GLOW_SCALE;
}

void setFrameUniforms(XrCtx* ctx, GLint frame, GLint look, float crop, float shift) {
    if (!ctx->playerMode) {
        glUniform4f(frame, 0.0f, 1.0f, 1.0f, 0.0f);
        return;
    }
    float aspect = ctx->videoHeight > 0 ? 1.0f / screenAspect(ctx) : 16.0f / 9.0f;
    glUniform4f(frame, 1.0f, aspect, RELIGHT_FEATHER, RELIGHT_CORNER);
    glUniform4f(look, crop, ctx->relightAmbient, relightGlowStrength(ctx->relightBrightness), shift);
}
