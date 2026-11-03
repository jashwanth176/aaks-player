// Ambilight: the frame downsampled to a small colour texture, and the glow drawn
// from it behind the screen.
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

    if (!linkProgram(&ctx->glowProgram, GLOW_FRAGMENT_SRC, "glow")) {
        return 0;
    }
    ctx->glowIntensityUniform = glGetUniformLocation(ctx->glowProgram, "u_intensity");
    ctx->glowBlurUniform = glGetUniformLocation(ctx->glowProgram, "u_blur");
    ctx->glowScaleUniform = glGetUniformLocation(ctx->glowProgram, "u_scale");
    ctx->glowRoomUniform = glGetUniformLocation(ctx->glowProgram, "u_room");
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
    // The picture only: one eye, bars cropped (set each frame by relightStep)
    glUniform4fv(ctx->ambiCropUniform, 1, ctx->ambiCrop);
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

    glUseProgram(ctx->glowProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, norm ? ctx->glowEdgeTexture : ctx->ambiTexture);
    glUniform1f(ctx->glowIntensityUniform, 1.0f);
    glUniform1f(ctx->glowBlurUniform, norm ? 1.0f : 0.0f);
    glUniform1f(ctx->glowScaleUniform, glowScaleFor(ctx));
    glUniform2f(ctx->glowRoomUniform, relightCover(ctx->relightAmbient),
                relightReach(ctx->relightAmbient));
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
}

int glowStale(XrCtx* ctx) {
    return !ctx->glowDrawnOn || ctx->glowDrawnAmbient != ctx->relightAmbient
           || ctx->glowDrawnBrightness != ctx->relightBrightness;
}

// Redraws the glow without a new video frame (e.g. paused)
void redrawGlow(XrCtx* ctx, const float* texMatrix) {
    if (!ctx->glowDrawnOn) {
        runFrameColorSample(ctx, texMatrix);
    }
    runGlowRender(ctx);
}

// Glow size relative to the screen, growing with the immersion
float glowScaleFor(XrCtx* ctx) {
    return relightGlowScale(ctx->relightAmbient);
}

void setFrameUniforms(XrCtx* ctx, GLint frame, GLint look, float crop, float shift) {
    float aspect = ctx->videoHeight > 0 ? 1.0f / screenAspect(ctx) : 16.0f / 9.0f;
    glUniform4f(frame, 1.0f, aspect, RELIGHT_FEATHER, RELIGHT_CORNER);
    glUniform4f(look, crop, ctx->relightAmbient, relightGlowStrength(ctx->relightBrightness), shift);
}
