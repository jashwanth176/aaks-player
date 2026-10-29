// GL setup and the video passes: depth upsample, occlusion offset search, stereo warp,
// and GPU timing.
#include "xr_renderer.h"
#include "xr_shaders.h"

PFNGENQUERIESEXT pfnGenQueries;
PFNBEGINQUERYEXT pfnBeginQuery;
PFNENDQUERYEXT pfnEndQuery;
PFNGETQUERYOBJECTUIVEXT pfnGetQueryObjectuiv;
PFNGETQUERYOBJECTUI64VEXT pfnGetQueryObjectui64v;
PFNDELETEQUERIESEXT pfnDeleteQueries;

// Fullscreen strip, x y u v
const float VERTEX_DATA[16] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

GLuint compileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Fills the depth textures for the synthetic test modes. Depth: 0 far, 1 near, 0.5 on
// the screen plane (zero disparity).
static int fillSyntheticDepth(XrCtx* ctx) {
    const int w = ctx->depthTexW;
    const int h = ctx->depthTexH;
    // RGBA: depth in alpha, guide colour in rgb. Neutral guide, so the upsample just blurs.
    unsigned char* buf = malloc((size_t)w * h * 4);
    if (buf == NULL) {
        LOGE("no memory for the depth texture");
        return 0;
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float fx = x / (float)(w - 1);
            float fy = y / (float)(h - 1);
            float d;
            switch (ctx->stereoMode) {
                case DEPTH_MODE_RAMP:
                    d = fx;
                    break;
                case DEPTH_MODE_BLOB: {
                    float dx = fx - 0.5f;
                    float dy = fy - 0.5f;
                    float sigma = 0.15f;
                    d = 0.35f + 0.5f * expf(-(dx * dx + dy * dy) / (2.0f * sigma * sigma));
                    break;
                }
                case DEPTH_MODE_SHIFTTEST:
                    d = 0.85f;
                    break;
                case DEPTH_MODE_FLAT:
                default:
                    d = 0.5f;
                    break;
            }
            if (d < 0.0f) d = 0.0f;
            if (d > 1.0f) d = 1.0f;
            unsigned char* px = buf + ((size_t)y * w + x) * 4;
            px[0] = px[1] = px[2] = 128;
            px[3] = (unsigned char)(d * 255.0f + 0.5f);
        }
    }

    for (int i = 0; i < DEPTH_TEX_COUNT; i++) {
        glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    free(buf);
    return 1;
}

int linkProgram(GLuint* out, const char* fragmentSrc, const char* what) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOGE("%s program link failed: %s", what, log);
        return 0;
    }
    *out = program;
    return 1;
}

int linkVideoProgram(XrCtx* ctx, GLuint* out, const char* fragmentSrc, const char* what) {
    char* src = videoShaderSource(fragmentSrc, &ctx->videoColourBuilt);
    if (src == NULL) {
        LOGE("%s shader could not be built for %s video", what,
             videoTransferName(ctx->videoColourBuilt.transfer));
        return 0;
    }
    int ok = linkProgram(out, src, what);
    free(src);
    return ok;
}

// Sets the picture grade uniforms of a program built with GRADE_GLSL
void setGradeUniforms(XrCtx* ctx, GLint onUniform, GLint gradeUniform, int on) {
    glUniform1f(onUniform, on ? 1.0f : 0.0f);
    if (on) {
        glUniform4f(gradeUniform, ctx->grade.offset, ctx->grade.contrast, ctx->grade.exponent,
                    ctx->grade.saturation);
    }
}

static int linkUpsampleProgram(XrCtx* ctx) {
    if (!linkVideoProgram(ctx, &ctx->upsampleProgram, UPSAMPLE_FRAGMENT_SRC, "upsample")) {
        return 0;
    }
    ctx->upsampleTexMatrixUniform = glGetUniformLocation(ctx->upsampleProgram, "u_texmatrix");
    ctx->upsampleSigmaUniform = glGetUniformLocation(ctx->upsampleProgram, "u_sigmaR");
    ctx->upsampleSharpUniform = glGetUniformLocation(ctx->upsampleProgram, "u_sharp");
    glUseProgram(ctx->upsampleProgram);
    glUniform1i(glGetUniformLocation(ctx->upsampleProgram, "u_texture"), 0);
    glUniform1i(glGetUniformLocation(ctx->upsampleProgram, "u_depth"), 1);
    // Map size is fixed for the session
    glUniform2f(glGetUniformLocation(ctx->upsampleProgram, "u_depthSize"),
                (float)ctx->depthTexW, (float)ctx->depthTexH);
    return 1;
}

// Quarter resolution: full resolution gave the same edge width at 4x the cost.
static int initUpsample(XrCtx* ctx) {
    ctx->upsampleWidth = ctx->videoWidth / 4;
    ctx->upsampleHeight = ctx->videoHeight / 4;

    if (!linkUpsampleProgram(ctx)) {
        return 0;
    }

    glGenTextures(1, &ctx->upsampleTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->upsampleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ctx->upsampleWidth, ctx->upsampleHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->upsampleFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->upsampleFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->upsampleTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("upsample framebuffer incomplete: 0x%x", status);
        return 0;
    }

    if (!linkProgram(&ctx->offsetProgram, OFFSET_FRAGMENT_SRC, "offset")) {
        return 0;
    }
    ctx->offsetDispUniform = glGetUniformLocation(ctx->offsetProgram, "u_dispTexels");
    ctx->offsetConvUniform = glGetUniformLocation(ctx->offsetProgram, "u_convergence");
    glUseProgram(ctx->offsetProgram);
    glUniform1i(glGetUniformLocation(ctx->offsetProgram, "u_depth"), 1);

    glGenTextures(1, &ctx->offsetTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->offsetTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ctx->upsampleWidth, ctx->upsampleHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->offsetFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->offsetFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->offsetTexture, 0);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("offset framebuffer incomplete: 0x%x", status);
        return 0;
    }

    LOGI("depth upsample and offset search ready at %dx%d",
         ctx->upsampleWidth, ctx->upsampleHeight);
    return 1;
}

static int linkMainProgram(XrCtx* ctx) {
    if (!linkVideoProgram(ctx, &ctx->program, FRAGMENT_SRC, "picture")) {
        return 0;
    }
    ctx->texMatrixUniform = glGetUniformLocation(ctx->program, "u_texmatrix");
    ctx->disparityUniform = glGetUniformLocation(ctx->program, "u_disparity");
    ctx->tintUniform = glGetUniformLocation(ctx->program, "u_tint");
    ctx->barTestUniform = glGetUniformLocation(ctx->program, "u_barTest");
    ctx->occlusionUniform = glGetUniformLocation(ctx->program, "u_occlusion");
    ctx->eyeIndexUniform = glGetUniformLocation(ctx->program, "u_eyeIndex");
    ctx->convergenceUniform = glGetUniformLocation(ctx->program, "u_convergence");
    ctx->dispTexelsUniform = glGetUniformLocation(ctx->program, "u_dispTexels");
    ctx->lowResWidthUniform = glGetUniformLocation(ctx->program, "u_lowResWidth");
    ctx->frameWidthUniform = glGetUniformLocation(ctx->program, "u_frameWidth");
    ctx->srcInsetUniform = glGetUniformLocation(ctx->program, "u_srcInset");
    ctx->frameUniform = glGetUniformLocation(ctx->program, "u_frame");
    ctx->frameLookUniform = glGetUniformLocation(ctx->program, "u_frameLook");
    ctx->edgeFadeUniform = glGetUniformLocation(ctx->program, "u_edgeFade");
    ctx->srcRectUniform = glGetUniformLocation(ctx->program, "u_srcRect");
    ctx->depthCubicUniform = glGetUniformLocation(ctx->program, "u_depthCubic");
    ctx->gradeOnUniform = glGetUniformLocation(ctx->program, "u_gradeOn");
    ctx->gradeUniform = glGetUniformLocation(ctx->program, "u_grade");

    // Sampler units are fixed: color on 0, depth on 1
    glUseProgram(ctx->program);
    glUniform1i(glGetUniformLocation(ctx->program, "u_texture"), 0);
    glUniform1i(glGetUniformLocation(ctx->program, "u_depth"), 1);
    glUniform1i(glGetUniformLocation(ctx->program, "u_offsets"), 2);
    glUniform1f(glGetUniformLocation(ctx->program, "u_showDepth"),
                ctx->depthDebug ? 1.0f : 0.0f);
    return 1;
}

// Rebuilds the video programs for videoColourBuilt. Old programs are deleted only once
// replaced; the caller falls back to SDR if a build fails.
static int relinkVideoPrograms(XrCtx* ctx) {
    GLuint old[4] = { ctx->program, ctx->upsampleProgram, ctx->downscaleProgram, ctx->ambiProgram };
    int ok = linkMainProgram(ctx) && linkAmbiProgram(ctx)
            && (old[1] == 0 || linkUpsampleProgram(ctx))
            && (old[2] == 0 || linkDownscaleProgram(ctx));
    for (int i = 0; i < 4; i++) {
        GLuint now = i == 0 ? ctx->program : i == 1 ? ctx->upsampleProgram
                   : i == 2 ? ctx->downscaleProgram : ctx->ambiProgram;
        if (old[i] != 0 && old[i] != now) {
            glDeleteProgram(old[i]);
        }
    }
    return ok;
}

void applyVideoColour(XrCtx* ctx) {
    int asked = atomic_load_explicit(&ctx->videoColourAsked, memory_order_acquire);
    if (asked == ctx->videoColourSeen || ctx->program == 0) {
        return;
    }
    ctx->videoColourSeen = asked;
    VideoColour want = videoColourFor(ctx->videoTransferWanted, ctx->videoPeakWanted,
                                      ctx->yuvTargetSupported);
    if (videoColourSame(&want, &ctx->videoColourBuilt)) {
        return;
    }
    int64_t startNs = nowNs();
    ctx->videoColourBuilt = want;
    int ok = relinkVideoPrograms(ctx);
    if (!ok && want.yuvTarget) {
        // GL_EXT_YUV_target can be refused by the driver; fall back to the driver's RGB
        LOGW("raw YUV read refused for %s video, reading the driver's RGB",
             videoTransferName(want.transfer));
        ctx->videoColourBuilt.yuvTarget = 0;
        ok = relinkVideoPrograms(ctx);
    }
    if (!ok) {
        LOGE("video shaders for %s would not build, the picture stays as decoded",
             videoTransferName(want.transfer));
        ctx->videoColourBuilt = videoColourFor(VIDEO_TRANSFER_SDR, 0.0f, 0);
        relinkVideoPrograms(ctx);
    }
    else {
        LOGEV("video colour: %s%s, peak %.0f nits, shaders rebuilt in %.1f ms",
              videoTransferName(ctx->videoColourBuilt.transfer),
              ctx->videoColourBuilt.transfer == VIDEO_TRANSFER_SDR ? ""
              : ctx->videoColourBuilt.yuvTarget ? ", YUV read raw" : ", driver's RGB",
              ctx->videoColourBuilt.peakNits, (nowNs() - startNs) / 1e6);
    }
    ctx->warpRedraw = 1;
}

int initGl(XrCtx* ctx) {
    const char* glExts = (const char*)glGetString(GL_EXTENSIONS);
    ctx->yuvTargetSupported = glExts != NULL && strstr(glExts, "GL_EXT_YUV_target") != NULL;
    LOGEV("raw YUV reads (GL_EXT_YUV_target) %s", ctx->yuvTargetSupported
          ? "available, HDR video is converted exactly"
          : "not offered, HDR video is converted from the driver's RGB");
    // Start as SDR. Forcing videoColourSeen stale makes the first frame re-check a colour
    // requested before GL came up.
    ctx->videoColourBuilt = videoColourFor(VIDEO_TRANSFER_SDR, 0.0f, 0);
    ctx->videoColourSeen = atomic_load_explicit(&ctx->videoColourAsked, memory_order_acquire) - 1;
    if (!linkMainProgram(ctx)) {
        return 0;
    }

    glGenTextures(DEPTH_TEX_COUNT, ctx->depthTextures);
    if (!fillSyntheticDepth(ctx)) {
        return 0;
    }

    glGenTextures(1, &ctx->oesTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->fbo);

    ctx->srgbWriteControl = glExts != NULL && strstr(glExts, "GL_EXT_sRGB_write_control") != NULL;

    // Room atlas capabilities, queried once
    ctx->astcSupported = glExts != NULL
            && strstr(glExts, "GL_KHR_texture_compression_astc_ldr") != NULL;
    ctx->roomAnisotropy = 1.0f;
    GLfloat anisoMost = 1.0f;
    if (glExts != NULL && strstr(glExts, "GL_EXT_texture_filter_anisotropic") != NULL) {
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &anisoMost);
        if (anisoMost > 1.0f) {
            ctx->roomAnisotropy = anisoMost < ROOM_ANISOTROPY_MAX ? anisoMost
                                                                  : ROOM_ANISOTROPY_MAX;
        }
    }
    LOGEV("room atlas support: ASTC LDR %s, anisotropy %.0f of %.0f",
          ctx->astcSupported ? "yes" : "no", ctx->roomAnisotropy, anisoMost);

    if (glExts != NULL && strstr(glExts, "GL_EXT_disjoint_timer_query") != NULL) {
        pfnGenQueries = (PFNGENQUERIESEXT)eglGetProcAddress("glGenQueriesEXT");
        pfnBeginQuery = (PFNBEGINQUERYEXT)eglGetProcAddress("glBeginQueryEXT");
        pfnEndQuery = (PFNENDQUERYEXT)eglGetProcAddress("glEndQueryEXT");
        pfnGetQueryObjectuiv = (PFNGETQUERYOBJECTUIVEXT)eglGetProcAddress("glGetQueryObjectuivEXT");
        pfnGetQueryObjectui64v =
                (PFNGETQUERYOBJECTUI64VEXT)eglGetProcAddress("glGetQueryObjectui64vEXT");
        pfnDeleteQueries = (PFNDELETEQUERIESEXT)eglGetProcAddress("glDeleteQueriesEXT");
        if (pfnGenQueries != NULL && pfnBeginQuery != NULL && pfnEndQuery != NULL &&
                pfnGetQueryObjectuiv != NULL && pfnGetQueryObjectui64v != NULL &&
                pfnDeleteQueries != NULL) {
            pfnGenQueries(2, ctx->timerQueries);
            pfnGenQueries(2, ctx->roomTimerQueries);
            ctx->timerSupported = 1;
        }
    }
    if (!ctx->timerSupported) {
        LOGW("GL_EXT_disjoint_timer_query missing, GPU times unavailable");
    }

    // Video frames are already gamma encoded, so sRGB write must be disabled on an sRGB
    // swapchain; without the extension the gamma is wrong.
    if (ctx->swapchainFormat == GL_SRGB8_ALPHA8 && !ctx->srgbWriteControl) {
        LOGW("GL_EXT_sRGB_write_control not available, expect wrong gamma");
    }

    if (!initAmbilight(ctx)) {
        return 0;
    }

    if (ctx->stereoMode == DEPTH_MODE_MODEL) {
        if (!initDepthModel(ctx) || !initUpsample(ctx)) {
            return 0;
        }
    }

    return 1;
}

// Runs every video frame so an older depth map is re-snapped to the current frame's edges.
static void runUpsample(XrCtx* ctx, const float* texMatrix) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->upsampleFbo);
    glViewport(0, 0, ctx->upsampleWidth, ctx->upsampleHeight);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->upsampleProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glActiveTexture(GL_TEXTURE1);
    waitForDepthSlot(ctx);
    glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[ctx->depthReadIndex]);
    glUniformMatrix4fv(ctx->upsampleTexMatrixUniform, 1, GL_FALSE, texMatrix);
    glUniform1f(ctx->upsampleSigmaUniform, ctx->upsampleSigmaR);
    glUniform1f(ctx->upsampleSharpUniform, ctx->depthSharp);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Both eyes in one pass; they read the same depth
static void runOffsetSearch(XrCtx* ctx, float separation) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->offsetFbo);
    glViewport(0, 0, ctx->upsampleWidth, ctx->upsampleHeight);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->offsetProgram);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->upsampleTexture);
    glUniform1f(ctx->offsetDispUniform, separation * ctx->upsampleWidth);
    glUniform1f(ctx->offsetConvUniform, ctx->convergence);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static int nativeStereo(const XrCtx* ctx) {
    return ctx->stereoMode == DEPTH_MODE_NATIVE_SBS || ctx->stereoMode == DEPTH_MODE_NATIVE_OU;
}

// Eye's source region in frame uv (bottom up): whole frame for mono, left/top half for
// eye 0 of a stereo source, the other half for eye 1.
static void setSourceRect(XrCtx* ctx, int eye) {
    float rect[4];
    contentRect(ctx, eye, rect);
    glUniform4f(ctx->srcRectUniform, rect[0], rect[1], rect[2], rect[3]);
}

void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation) {
    // Latest staged depth slot. The acquire makes the slot's fence visible; whoever samples
    // the texture waits on it.
    ctx->depthReadIndex = atomic_load_explicit(&ctx->depthStagedIndex, memory_order_acquire);
    ctx->warpRedraw = 0;

    // Flat single draw when 3D is off or waiting for a fresh map
    int warping = ctx->stereoMode != DEPTH_MODE_OFF && ctx->stereoLive && !ctx->stereoWaiting;
    int upsampling = warping && ctx->stereoMode == DEPTH_MODE_MODEL && ctx->upsampleEnabled;
    // Stereo sources are never shifted
    if (nativeStereo(ctx)) {
        separation = 0.0f;
    }
    int occluding = upsampling && ctx->occlusionEnabled && separation > 0.0f;

    // Capture frames read back and write files; on this driver that inside a query can leave
    // it never available, so they are not timed.
    int timing = ctx->timerSupported && !ctx->captureRequested;

    // Before the query opens: creating the room's swapchain/buffers inside it breaks later
    // samples. Drawing happens at the end.
    int roomOn = roomEffective(ctx) > 0;
    if (roomOn) {
        prepareRoom(ctx);
    }

    if (timing && !ctx->timerPending[ctx->timerSlot]) {
        pfnBeginQuery(GL_TIME_ELAPSED_EXT, ctx->timerQueries[ctx->timerSlot]);
    }

    if (upsampling) {
        runUpsample(ctx, texMatrix);
    }
    if (occluding) {
        runOffsetSearch(ctx, separation);
    }

    // Inside the query window so the glow is included in the GPU time
    int glowOn;
    float glowLevel;
    ambiEffective(ctx, &glowOn, &glowLevel);
    // The glow, room lighting and room light all read the same colour sample; taken here so
    // the room is lit from this frame.
    int relight = relightWanted(ctx);
    int sampled = glowOn || roomOn || relight;
    if (!sampled) {
        // Reset so the next sample starts fresh instead of easing in
        ctx->ambiSeeded = 0;
    }
    if (sampled) {
        runFrameColorSample(ctx, texMatrix);
    }
    if (relight) {
        relightSample(ctx);
    }
    if (glowOn) {
        runGlowRender(ctx);
    }
    else {
        ctx->glowDrawnOn = 0;
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->swapchain, &acquireInfo, &imageIndex), "acquire image")) {
        // Close the open query, or the next frame's begin would nest inside it
        if (timing && !ctx->timerPending[ctx->timerSlot]) {
            pfnEndQuery(GL_TIME_ELAPSED_EXT);
            ctx->timerPending[ctx->timerSlot] = 1;
            ctx->timerPendingFrames[ctx->timerSlot] = 0;
            ctx->timerSlot = 1 - ctx->timerSlot;
        }
        return;
    }
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->swapchain, &waitInfo);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->swapchainImages[imageIndex].image, 0);

    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glActiveTexture(GL_TEXTURE1);
    // Raw map or its upsample; both carry depth in alpha. With the upsample on,
    // runUpsample already waited on this slot.
    waitForDepthSlot(ctx);
    glBindTexture(GL_TEXTURE_2D, upsampling ? ctx->upsampleTexture
                                            : ctx->depthTextures[ctx->depthReadIndex]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx->offsetTexture);
    glUniformMatrix4fv(ctx->texMatrixUniform, 1, GL_FALSE, texMatrix);
    glUniform1f(ctx->occlusionUniform, occluding ? 1.0f : 0.0f);
    glUniform1f(ctx->convergenceUniform, ctx->convergence);
    glUniform1f(ctx->dispTexelsUniform, separation * ctx->upsampleWidth);
    glUniform1f(ctx->lowResWidthUniform, (float)ctx->upsampleWidth);
    glUniform1f(ctx->frameWidthUniform, (float)ctx->videoWidth);

    // Mono: one full width draw, zero disparity, into the left half of the double wide
    // swapchain. Stereo: one draw per half, opposite disparity signs.
    int eyes = warping ? 2 : 1;

    // Half a texel inset to clamp the shifted sample, or -1 (no clamp)
    glUniform1f(ctx->srcInsetUniform,
                ctx->srcInsetOn ? 0.5f / (float)ctx->videoWidth : -1.0f);
    glUniform1f(ctx->edgeFadeUniform, (float)ctx->edgeFadePx / (float)ctx->videoWidth);
    // Mono doesn't use depth, so skip the cubic fetch
    glUniform1f(ctx->depthCubicUniform, ctx->depthCubic && eyes == 2 ? 1.0f : 0.0f);
    setGradeUniforms(ctx, ctx->gradeOnUniform, ctx->gradeUniform, ctx->gradeOn);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);

    // Debug capture: draw the unwarped frame first (the eye passes overwrite it).
    // Stalls the frame loop.
    unsigned char* captureBuf = NULL;
    size_t captureBytes = (size_t)ctx->videoWidth * ctx->videoHeight * 4;
    if (ctx->captureRequested) {
        captureBuf = malloc(captureBytes);
        if (captureBuf != NULL) {
            glViewport(0, 0, ctx->videoWidth, ctx->videoHeight);
            glUniform1f(ctx->disparityUniform, 0.0f);
            glUniform1f(ctx->barTestUniform, 0.0f);
            glUniform3f(ctx->tintUniform, 1.0f, 1.0f, 1.0f);
            glUniform4f(ctx->srcRectUniform, 0.0f, 0.0f, 1.0f, 1.0f);
            // Ungraded, as the depth passes saw it
            glUniform1f(ctx->gradeOnUniform, 0.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glUniform1f(ctx->gradeOnUniform, ctx->gradeOn ? 1.0f : 0.0f);
            glReadPixels(0, 0, ctx->videoWidth, ctx->videoHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                         captureBuf);
            writeCapture(ctx, "source", captureBuf, captureBytes);
        }
    }

    for (int eye = 0; eye < eyes; eye++) {
        glViewport(eye * ctx->videoWidth, 0, ctx->videoWidth, ctx->videoHeight);
        float disparity = 0.0f;
        if (eyes == 2 && ctx->stereoMode != DEPTH_MODE_EYETEST) {
            disparity = (eye == 0) ? separation : -separation;
        }
        glUniform1f(ctx->disparityUniform, disparity);
        glUniform1f(ctx->eyeIndexUniform, (float)eye);
        glUniform1f(ctx->barTestUniform, ctx->stereoMode == DEPTH_MODE_SHIFTTEST ? 1.0f : 0.0f);
        // Flat: a stereo source shows its left eye to both
        setSourceRect(ctx, eye);
        // Edge fade, and for stereo sources the eyes moved outward by the crop
        float crop = nativeStereo(ctx) ? ctx->playerCrop : 0.0f;
        setFrameUniforms(ctx, ctx->frameUniform, ctx->frameLookUniform, crop,
                         eye == 0 ? 0.5f * crop : -0.5f * crop);

        if (ctx->stereoMode == DEPTH_MODE_EYETEST) {
            if (eye == 0) {
                glUniform3f(ctx->tintUniform, 1.0f, 0.2f, 0.2f);
            }
            else {
                glUniform3f(ctx->tintUniform, 0.2f, 0.2f, 1.0f);
            }
        }
        else {
            glUniform3f(ctx->tintUniform, 1.0f, 1.0f, 1.0f);
        }

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Measure the bar's position in each drawn half; positive shift = moved right.
    if (ctx->stereoMode == DEPTH_MODE_SHIFTTEST && ctx->barTestFramesLogged < 3) {
        int rowWidth = ctx->videoWidth * eyes;
        unsigned char* row = malloc((size_t)rowWidth * 4);
        if (row != NULL) {
            glReadPixels(0, ctx->videoHeight / 2, rowWidth, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
            for (int half = 0; half < eyes; half++) {
                long sum = 0, count = 0;
                for (int x = 0; x < ctx->videoWidth; x++) {
                    if (row[(size_t)((half * ctx->videoWidth) + x) * 4] > 128) {
                        sum += x;
                        count++;
                    }
                }
                if (count > 0) {
                    double center = (double)sum / (double)count / (double)ctx->videoWidth;
                    LOGI("bar test: half %d (%s) bar center %.4f, shift %+.4f",
                         half, eyes == 1 ? "flat, both eyes" : (half == 0 ? "left eye" : "right eye"),
                         center, center - 0.5);
                }
                else {
                    LOGI("bar test: half %d no bar found", half);
                }
            }
            free(row);
        }
        ctx->barTestFramesLogged++;
    }

    if (ctx->captureRequested) {
        if (captureBuf != NULL) {
            for (int eye = 0; eye < eyes; eye++) {
                glReadPixels(eye * ctx->videoWidth, 0, ctx->videoWidth, ctx->videoHeight,
                             GL_RGBA, GL_UNSIGNED_BYTE, captureBuf);
                writeCapture(ctx, eye == 0 ? "left" : "right", captureBuf, captureBytes);
            }
            free(captureBuf);
        }
        writeCaptureDepthTexture(ctx);
        if (upsampling) {
            size_t count = (size_t)ctx->upsampleWidth * ctx->upsampleHeight;
            unsigned char* rgba = malloc(count * 4);
            unsigned char* alpha = malloc(count);
            if (rgba != NULL && alpha != NULL) {
                glBindFramebuffer(GL_FRAMEBUFFER, ctx->upsampleFbo);
                glReadPixels(0, 0, ctx->upsampleWidth, ctx->upsampleHeight, GL_RGBA,
                             GL_UNSIGNED_BYTE, rgba);
                for (size_t i = 0; i < count; i++) {
                    alpha[i] = rgba[i * 4 + 3];
                }
                writeCapture(ctx, "upsampled", alpha, count);
            }
            free(rgba);
            free(alpha);
        }
        // Best effort: the depth threads may be refilling this pair
        int pair = atomic_load_explicit(&ctx->depthLastPair, memory_order_relaxed);
        writeCapture(ctx, "modelinput", ctx->modelInput[pair],
                     (size_t)ctx->depthTexW * ctx->depthTexH * 3 * sizeof(float));
        writeCapture(ctx, "depthraw", ctx->modelOutput[pair],
                     (size_t)ctx->depthTexW * ctx->depthTexH * sizeof(float));
        ctx->captureRequested = 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->swapchain, &releaseInfo);
    ctx->drawnEyes = eyes;

    // Close this frame's query and collect an earlier one if ready; never blocks.
    if (timing) {
        if (!ctx->timerPending[ctx->timerSlot]) {
            pfnEndQuery(GL_TIME_ELAPSED_EXT);
            ctx->timerPending[ctx->timerSlot] = 1;
            ctx->timerPendingFrames[ctx->timerSlot] = 0;
            ctx->timerSlot = 1 - ctx->timerSlot;
        }
        int other = ctx->timerSlot;
        if (ctx->timerPending[other]) {
            GLuint ready = 0;
            pfnGetQueryObjectuiv(ctx->timerQueries[other], GL_QUERY_RESULT_AVAILABLE_EXT, &ready);
            if (ready) {
                GLuint64 elapsed = 0;
                pfnGetQueryObjectui64v(ctx->timerQueries[other], GL_QUERY_RESULT_EXT, &elapsed);
                ctx->timerPending[other] = 0;
                ctx->timerPendingFrames[other] = 0;
                // Keep only plausible samples (0 < t < 50 ms). The disjoint flag is ignored: this
                // driver sets it on every GPU clock change.
                if (elapsed > 0 && elapsed < 50000000ull) {
                    ctx->gpuTotalNs += (int64_t)elapsed;
                    ctx->gpuSamples++;
                    ctx->overlayGpuTotalNs += (int64_t)elapsed;
                    ctx->overlayGpuSamples++;
                    rateBudgetGpu(&ctx->rateBudget, (int64_t)elapsed);
                    if ((int64_t)elapsed > ctx->gpuMaxNs) {
                        ctx->gpuMaxNs = (int64_t)elapsed;
                    }
                }
                else {
                    // Kept for the stats line
                    ctx->gpuDropped++;
                    ctx->gpuLastDroppedNs = elapsed;
                }
            }
            else if (++ctx->timerPendingFrames[other] > 90) {
                // Give up on a query that never lands
                ctx->timerPending[other] = 0;
                ctx->timerPendingFrames[other] = 0;
                LOGW("XR warp: gave up on a GPU timer query that never landed");
            }
        }
    }

    // Letterbox detection, every AMBI_BAR_PERIOD sampled frames. Outside both query windows
    // because a readback inside one can leave the query never available on this driver.
    if (sampled && ctx->ambiBarDetect) {
        // Previous readback first, then a new request if due
        finishAmbiBarDetect(ctx);
        ctx->ambiBarCounter++;
        if (ctx->ambiBarCounter >= AMBI_BAR_PERIOD) {
            ctx->ambiBarCounter = 0;
            runAmbiBarDetect(ctx, texMatrix);
        }
    }

    // Room last: the compositor often preempts this full resolution pass under head motion,
    // so anything queued behind it would stall. renderRoom opens its own timer query.
    if (roomOn) {
        renderRoom(ctx);
    }

    // Collected every frame, so a query from the last rendered room frame still lands
    if (timing) {
        int roomOther = ctx->roomTimerSlot;
        if (ctx->roomTimerPending[roomOther]) {
            GLuint ready = 0;
            pfnGetQueryObjectuiv(ctx->roomTimerQueries[roomOther], GL_QUERY_RESULT_AVAILABLE_EXT,
                                 &ready);
            if (ready) {
                GLuint64 elapsed = 0;
                pfnGetQueryObjectui64v(ctx->roomTimerQueries[roomOther], GL_QUERY_RESULT_EXT,
                                       &elapsed);
                ctx->roomTimerPending[roomOther] = 0;
                ctx->roomTimerPendingFrames[roomOther] = 0;
                if (elapsed > 0 && elapsed < 50000000ull) {
                    ctx->roomGpuTotalNs += (int64_t)elapsed;
                    ctx->roomGpuSamples++;
                    rateBudgetRoom(&ctx->rateBudget, (int64_t)elapsed);
                }
                else {
                    ctx->roomGpuDropped++;
                }
            }
            else if (++ctx->roomTimerPendingFrames[roomOther] > 90) {
                ctx->roomTimerPending[roomOther] = 0;
                ctx->roomTimerPendingFrames[roomOther] = 0;
                LOGW("room: gave up on a GPU timer query that never landed");
            }
        }
    }

    ctx->everRendered = 1;
}
