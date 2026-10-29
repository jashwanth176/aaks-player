// Controller model, rendered to its own transparent projection layer above
// the picture and panels so a controller is never hidden behind the screen
#include "xr_renderer.h"
#include "xr_shaders.h"

// Lazily created on the first frame a model is shown. Swapchain matches the
// room pass (side-by-side eyes). A failure is not retried.
static int initModelPass(XrCtx* ctx) {
    if (ctx->modelPassReady) {
        return 1;
    }
    if (ctx->modelPassFailed || ctx->session == XR_NULL_HANDLE) {
        return 0;
    }
    ctx->modelPassFailed = 1;

    GLuint vs = compileShader(GL_VERTEX_SHADER, MODEL_VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, MODEL_FRAGMENT_SRC);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_normal");
    glBindAttribLocation(program, 2, "a_color");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOGE("controller model program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    ctx->modelProgram = program;
    ctx->modelViewProjUniform = glGetUniformLocation(program, "u_viewproj");
    ctx->modelMatrixUniform = glGetUniformLocation(program, "u_model");

    int eyeW;
    int eyeH;
    worldEyeSize(ctx, &eyeW, &eyeH);
    if (!createArtSwapchain(ctx, eyeW * ROOM_EYES, eyeH, "create controller model swapchain",
                            &ctx->modelSwapchain, &ctx->modelImages, &ctx->modelImageCount)) {
        return 0;
    }
    LOGEV("swapchains alive %d with the controller models'", ctx->swapchainsAlive);
    glGenRenderbuffers(1, &ctx->modelDepthBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, ctx->modelDepthBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eyeW * ROOM_EYES, eyeH);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &ctx->modelFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->modelFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              ctx->modelDepthBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (ctx->timerSupported) {
        pfnGenQueries(2, ctx->modelTimerQueries);
    }

    ctx->modelEyeWidth = eyeW;
    ctx->modelEyeHeight = eyeH;
    ctx->modelPassReady = 1;
    ctx->modelPassFailed = 0;
    LOGEV("controller model pass ready at %dx%d per eye", eyeW, eyeH);
    return 1;
}

// Locates both eye views; on failure the previous image is kept
static int locateModelViews(XrCtx* ctx) {
    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = ctx->predictedDisplayTime;
    locateInfo.space = ctx->localSpace;
    XrViewState state = { XR_TYPE_VIEW_STATE };
    XrView views[ROOM_EYES];
    for (int eye = 0; eye < ROOM_EYES; eye++) {
        views[eye].type = XR_TYPE_VIEW;
        views[eye].next = NULL;
    }
    uint32_t count = 0;
    if (XR_FAILED(xrLocateViews(ctx->session, &locateInfo, &state, ROOM_EYES, &count, views))
            || count < ROOM_EYES) {
        return 0;
    }
    XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT
            | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & needed) != needed) {
        return 0;
    }
    for (int eye = 0; eye < ROOM_EYES; eye++) {
        ctx->modelViews[eye] = views[eye];
    }
    return 1;
}

// Collects a finished GPU timer query, with the same sanity filter as the
// warp and room passes
static void collectModelTimer(XrCtx* ctx) {
    int other = ctx->modelTimerSlot;
    if (!ctx->modelTimerPending[other]) {
        return;
    }
    GLuint ready = 0;
    pfnGetQueryObjectuiv(ctx->modelTimerQueries[other], GL_QUERY_RESULT_AVAILABLE_EXT, &ready);
    if (ready) {
        GLuint64 elapsed = 0;
        pfnGetQueryObjectui64v(ctx->modelTimerQueries[other], GL_QUERY_RESULT_EXT, &elapsed);
        ctx->modelTimerPending[other] = 0;
        ctx->modelTimerPendingFrames[other] = 0;
        if (elapsed > 0 && elapsed < 50000000ull) {
            ctx->modelGpuTotalNs += (int64_t)elapsed;
            ctx->modelGpuSamples++;
        }
        else {
            ctx->modelGpuDropped++;
        }
    }
    else if (++ctx->modelTimerPendingFrames[other] > 90) {
        ctx->modelTimerPending[other] = 0;
        ctx->modelTimerPendingFrames[other] = 0;
        LOGW("controller models: gave up on a GPU timer query that never landed");
    }
}


// Locates each grip at the predicted display time and decides visibility.
// Syncs actions here if the input pass returned before syncing.
void updateControllerModels(XrCtx* ctx) {
    int shown[HAND_COUNT] = { 0, 0 };
    int live = ctx->modelOn && ctx->modelReady && !ctx->modelPassFailed && ctx->inputReady
            && ctx->gripAction != XR_NULL_HANDLE && !ctx->passthrough && ctx->shouldRender
            && ctx->sessionState == XR_SESSION_STATE_FOCUSED
            && ctx->splash.phase == SPLASH_GONE;
    if (live && !ctx->actionsSynced) {
        XrActiveActionSet active;
        active.actionSet = ctx->actionSet;
        active.subactionPath = XR_NULL_PATH;
        XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        live = XR_SUCCEEDED(xrSyncActions(ctx->session, &sync));
    }
    ctx->actionsSynced = 0;

    for (int h = 0; live && h < HAND_COUNT; h++) {
        if (ctx->gripSpaces[h] == XR_NULL_HANDLE) {
            continue;
        }
        XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
        get.action = ctx->gripAction;
        get.subactionPath = ctx->handPaths[h];
        XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
        int active = XR_SUCCEEDED(xrGetActionStatePose(ctx->session, &get, &state))
                && state.isActive;
        // Always in local space, regardless of what the picture is locked to
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        if (!active || XR_FAILED(xrLocateSpace(ctx->gripSpaces[h], ctx->localSpace,
                                               ctx->predictedDisplayTime, &loc))) {
            continue;
        }
        shown[h] = controllerModelShown(ctx->modelOn, ctx->passthrough, ctx->profileKind[h],
                                        active, (unsigned)loc.locationFlags);
        if (shown[h]) {
            ctx->modelGrip[h] = loc.pose;
        }
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        ctx->modelShown[h] = shown[h];
    }
    ctx->modelsShowing = shown[HAND_LEFT] || shown[HAND_RIGHT];
    // Do not show a stale image when models reappear
    if (!ctx->modelsShowing) {
        ctx->modelRendered = 0;
    }
    int said = shown[HAND_LEFT] | (shown[HAND_RIGHT] << 1);
    if (said != ctx->modelsSaid) {
        ctx->modelsSaid = said;
        LOGI("controller models: left %s, right %s", shown[HAND_LEFT] ? "drawn" : "not drawn",
             shown[HAND_RIGHT] ? "drawn" : "not drawn");
    }
}

// Renders every frame a model is shown, after the warp. Own depth buffer so
// one controller can occlude the other.
void renderControllerModels(XrCtx* ctx) {
    if (ctx->modelPassReady && ctx->timerSupported) {
        collectModelTimer(ctx);
    }
    if (!ctx->modelsShowing || !initModelPass(ctx) || !locateModelViews(ctx)) {
        return;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->modelSwapchain, &acquire, &index),
                 "acquire controller model image")) {
        return;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->modelSwapchain, &wait);

    // Timer starts after the image is acquired, as in the room pass
    int timing = ctx->timerSupported && !ctx->captureRequested
            && !ctx->modelTimerPending[ctx->modelTimerSlot];
    if (timing) {
        pfnBeginQuery(GL_TIME_ELAPSED_EXT, ctx->modelTimerQueries[ctx->modelTimerSlot]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->modelFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->modelImages[index].image, 0);
    if (!ctx->modelRendered) {
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("controller model framebuffer incomplete: 0x%x", status);
        }
    }
    // Colours are already gamma encoded
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }
    // Transparent clear; models write alpha 1, valid as premultiplied
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    float models[HAND_COUNT][16];
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->modelShown[h]) {
            controllerModelMatrix(ctx->modelGrip[h], h == HAND_LEFT, models[h]);
        }
    }
    glUseProgram(ctx->modelProgram);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->modelVertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->modelIndexBuffer);
    // File vertex layout: position, normal, uv (unused), colour
    GLsizei stride = ROOM_MESH_VERTEX_FLOATS * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(8 * sizeof(float)));
    glEnableVertexAttribArray(2);
    for (int eye = 0; eye < ROOM_EYES; eye++) {
        glViewport(eye * ctx->modelEyeWidth, 0, ctx->modelEyeWidth, ctx->modelEyeHeight);
        float proj[16];
        float view[16];
        float viewProj[16];
        // Near plane close enough for a controller at the face
        projectionFromFov(proj, ctx->modelViews[eye].fov, ROOM_NEAR_M, ROOM_FAR_MIN_M);
        viewFromPose(view, ctx->modelViews[eye].pose);
        matMul(viewProj, proj, view);
        glUniformMatrix4fv(ctx->modelViewProjUniform, 1, GL_FALSE, viewProj);
        for (int h = 0; h < HAND_COUNT; h++) {
            if (!ctx->modelShown[h]) {
                continue;
            }
            glUniformMatrix4fv(ctx->modelMatrixUniform, 1, GL_FALSE, models[h]);
            glDrawElements(GL_TRIANGLES, (GLsizei)ctx->modelIndexCount, GL_UNSIGNED_INT,
                           (const void*)0);
        }
    }

    if (timing) {
        pfnEndQuery(GL_TIME_ELAPSED_EXT);
        ctx->modelTimerPending[ctx->modelTimerSlot] = 1;
        ctx->modelTimerPendingFrames[ctx->modelTimerSlot] = 0;
        ctx->modelTimerSlot = 1 - ctx->modelTimerSlot;
    }

    // Restore state for the other passes: no buffers bound, only attribs 0-1 on
    glDisable(GL_DEPTH_TEST);
    glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->modelSwapchain, &release);
    ctx->modelRendered = 1;
}

// Uploads the model (a vertex-coloured .room mesh). Frame loop thread only
// (needs the GL context). Static buffers.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadControllerModel(JNIEnv* env, jobject thiz,
                                                                         jlong handle,
                                                                         jobject buffer,
                                                                         jint length) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || length <= 0 || ctx->modelReady) {
        return;
    }
    const unsigned char* data = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (data == NULL || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)length) {
        return;
    }
    RoomMeshInfo info;
    if (!roomMeshParse(data, (size_t)length, &info)) {
        LOGW("controller model is not a mesh the renderer reads (%d bytes)", length);
        return;
    }
    // Drawn in one call from vertex colours; atlas parts are unsupported
    if (info.atlasCount != 0 || roomMeshPaintedParts(&info) != (int)info.partCount) {
        LOGW("controller model asks for %u atlases, it is drawn from its colours alone",
             info.atlasCount);
        return;
    }
    glGenBuffers(1, &ctx->modelVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->modelVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)info.vertexCount * ROOM_MESH_VERTEX_FLOATS * sizeof(float),
                 data + info.vertexOffset, GL_STATIC_DRAW);
    glGenBuffers(1, &ctx->modelIndexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->modelIndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)info.indexCount * sizeof(uint32_t),
                 data + info.indexOffset, GL_STATIC_DRAW);
    // Other passes use client arrays with nothing bound
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    ctx->modelIndexCount = (int)info.indexCount;
    ctx->modelReady = 1;
    LOGEV("controller model ready: %u vertices, %u triangles, %d bytes, %s", info.vertexCount,
          info.indexCount / 3, length, ctx->modelOn ? "shown" : "off for now");
}

// Initial controller model setting, set before the first frame
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetControllerModel(JNIEnv* env, jobject thiz,
                                                                      jlong handle, jboolean on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->modelOn = on ? 1 : 0;
    LOGEV("controller model %s at the start of the session", on ? "on" : "off");
}
