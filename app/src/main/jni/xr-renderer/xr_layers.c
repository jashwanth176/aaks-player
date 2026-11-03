// End of frame: assembles every composition layer in draw order and submits it.
#include "xr_renderer.h"

// The runtime limit is 16 layers on the Pico, and on the Quest 3 in practice (it
// reports 32). Modals hide the bar furniture on the frame they open, so the two never
// stack. The fullest settings tabs, with controller models and the toast, can still
// exceed 16; nativeEndFrame then sheds layers in a fixed order until the frame fits.
// The array is sized well past the worst case, since an overflow would smash the stack.
#define FRAME_MAX_LAYERS 24

// Every layer a frame can carry; lives on nativeEndFrame's stack until xrEndFrame.
typedef struct {
    // Real room under everything, scaled by the picture.
    XrCompositionLayerPassthroughFB passthrough;
    XrCompositionLayerColorScaleBiasKHR passthroughTint;
    XrCompositionLayerQuad glow;
    // Used in place of the quad around a curved picture.
    XrCompositionLayerCylinderKHR glowCylinder;
    XrCompositionLayerQuad video[2];
    XrCompositionLayerCylinderKHR cylinder[2];
    XrCompositionLayerQuad handle;
    XrCompositionLayerQuad uiPanel[UI_PANEL_COUNT];
    XrCompositionLayerQuad beam;
    XrCompositionLayerQuad dot;
    // Over the picture and panels, under the beam and dot.
    XrCompositionLayerProjection models;
    XrCompositionLayerProjectionView modelViews[MODEL_EYES];
    XrCompositionLayerQuad splashGround;
    XrCompositionLayerQuad splashSheet;
    XrCompositionLayerSettingsFB settings;
    // NULL when sharpening and supersampling are off or unsupported.
    const void* settingsChain;
    // Colour scale per fading slot: [1] ahead of the settings chain, [0] alone.
    XrCompositionLayerColorScaleBiasKHR fades[FADE_SLOTS][2];
    const XrCompositionLayerBaseHeader* order[FRAME_MAX_LAYERS];
    uint32_t count;
} FrameLayers;

// Chain for a fading layer: a colour scale ahead of the settings chain while
// the fade is part way. All channels are scaled since the art is premultiplied.
static const void* fadeNext(XrCtx* ctx, FrameLayers* layers, int slot, int sharp, float level) {
    const void* next = sharp ? layers->settingsChain : NULL;
    if (!ctx->colorScaleSupported || level >= 1.0f) {
        return next;
    }
    XrCompositionLayerColorScaleBiasKHR* scale = &layers->fades[slot][sharp ? 1 : 0];
    memset(scale, 0, sizeof(*scale));
    scale->type = XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR;
    scale->next = next;
    scale->colorScale.r = level;
    scale->colorScale.g = level;
    scale->colorScale.b = level;
    scale->colorScale.a = level;
    return scale;
}

// Per-frame inputs shared by the layer builders.
typedef struct {
    XrSpace space;
    XrPosef screenPose;
    float screenWidth;
    float screenHeight;
    float aspect;
    float curve;
    int screenCurved;
    int stereo;
    int headLocked;
    int eyeSwap;
    // The bar and the buttons beside it share a hover area. Cleared
    // on the frame a modal opens, or furniture and panel would both
    // go up for one frame and exceed the runtime's layer limit.
    int barArea;
    BarFrame bar;
} FrameView;

static void dropLayer(FrameLayers* layers, const void* layer) {
    for (uint32_t i = 0; i < layers->count; i++) {
        if (layers->order[i] == (const XrCompositionLayerBaseHeader*)layer) {
            memmove(&layers->order[i], &layers->order[i + 1],
                    (layers->count - i - 1) * sizeof(layers->order[0]));
            layers->count--;
            return;
        }
    }
}

// Adds a layer, or drops it with a one-time error if the array is full.
static void pushLayer(XrCtx* ctx, FrameLayers* layers, const void* layer) {
    if (layers->count >= FRAME_MAX_LAYERS) {
        if (!ctx->layerDropWarned) {
            ctx->layerDropWarned = 1;
            LOGE("frame needs more than %d composition layers, dropping the rest",
                 FRAME_MAX_LAYERS);
        }
        return;
    }
    layers->order[layers->count++] = (const XrCompositionLayerBaseHeader*)layer;
}

// A quad showing a texW x texH rect of one swapchain image from texX, texY.
static void quadLayerRect(XrCompositionLayerQuad* quad, const void* next,
                          XrCompositionLayerFlags flags, XrSwapchain chain, int texX, int texY,
                          int texW, int texH, XrSpace space, XrPosef pose, float width,
                          float height) {
    memset(quad, 0, sizeof(*quad));
    quad->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad->next = next;
    quad->layerFlags = flags;
    quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad->subImage.swapchain = chain;
    quad->subImage.imageRect.offset.x = texX;
    quad->subImage.imageRect.offset.y = texY;
    quad->subImage.imageRect.extent.width = texW;
    quad->subImage.imageRect.extent.height = texH;
    quad->subImage.imageArrayIndex = 0;
    quad->space = space;
    quad->pose = pose;
    quad->size.width = width;
    quad->size.height = height;
}

static void quadLayer(XrCompositionLayerQuad* quad, const void* next,
                      XrCompositionLayerFlags flags, XrSwapchain chain, int texW, int texH,
                      XrSpace space, XrPosef pose, float width, float height) {
    quadLayerRect(quad, next, flags, chain, 0, 0, texW, texH, space, pose, width, height);
}

// Applies a local offset to a base pose, keeping its orientation.
static XrPosef poseOffset(XrPosef base, Vec3 local) {
    Vec3 offset = quatRotate(base.orientation, local);
    XrPosef pose;
    pose.orientation = base.orientation;
    pose.position.x = base.position.x + offset.x;
    pose.position.y = base.position.y + offset.y;
    pose.position.z = base.position.z + offset.z;
    return pose;
}

// Logs warp timings every STATS_LOG_INTERVAL_FRAMES frames, then resets.
static void logWarpStats(XrCtx* ctx) {
    if (ctx->statFrames == STATS_LOG_INTERVAL_FRAMES) {
        // The models have their own timers; report samples kept of harvested
        char modelLine[128];
        modelLine[0] = '\0';
        if (ctx->modelGpuSamples > 0) {
            snprintf(modelLine, sizeof(modelLine), ", models avg %.2f ms (%ld of %ld)",
                     ctx->modelGpuTotalNs / (double)ctx->modelGpuSamples / 1e6,
                     ctx->modelGpuSamples, ctx->modelGpuSamples + ctx->modelGpuDropped);
        }
        // Submit is CPU time to queue the draws; GPU is the real cost.
        if (ctx->gpuSamples > 0) {
            LOGI("XR warp: %ld frames, GPU avg %.2f ms, GPU max %.2f ms, submit avg %.2f ms, dropped %ld%s",
                 ctx->statFrames, ctx->gpuTotalNs / (double)ctx->gpuSamples / 1e6,
                 ctx->gpuMaxNs / 1e6,
                 ctx->statTotalNs / (double)ctx->statFrames / 1e6,
                 ctx->gpuDropped, modelLine);
        }
        else {
            // The raw value distinguishes zeros from wrapped negatives.
            LOGI("XR warp: %ld frames, submit avg %.2f ms, max %.2f ms (no GPU timer, dropped %ld, last raw %llu)%s",
                 ctx->statFrames, ctx->statTotalNs / (double)ctx->statFrames / 1e6,
                 ctx->statMaxNs / 1e6, ctx->gpuDropped,
                 (unsigned long long)ctx->gpuLastDroppedNs, modelLine);
        }
        ctx->statFrames = 0;
        ctx->statTotalNs = 0;
        ctx->statMaxNs = 0;
        ctx->gpuTotalNs = 0;
        ctx->gpuMaxNs = 0;
        ctx->gpuSamples = 0;
        ctx->gpuDropped = 0;
        ctx->modelGpuTotalNs = 0;
        ctx->modelGpuSamples = 0;
        ctx->modelGpuDropped = 0;
    }
}

// Compositor sharpening and supersampling, done in its sampling
// pass at no app cost. Supersampling helps where a layer is drawn
// smaller than its texture. The extension allows one bit of each.
static void setLayerSettings(XrCtx* ctx, FrameLayers* layers) {
    // Logged on change, so a session log shows each A/B stretch.
    int logged = ctx->sharpenMode * 3 + ctx->supersampleMode + 1;
    if (logged != ctx->layerFlagsLogged) {
        ctx->layerFlagsLogged = logged;
        LOGEV("layer flags: sharpen %d supersample %d%s", ctx->sharpenMode,
              ctx->supersampleMode, ctx->layerSettingsSupported ? "" : " (not offered, ignored)");
    }
    layers->settingsChain = NULL;
    if (!ctx->layerSettingsSupported) {
        return;
    }
    XrCompositionLayerSettingsFlagsFB flags = 0;
    if (ctx->sharpenMode == 1) {
        flags |= XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
    }
    else if (ctx->sharpenMode == 2) {
        flags |= XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
    }
    if (ctx->supersampleMode == 1) {
        flags |= XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
    }
    else if (ctx->supersampleMode == 2) {
        flags |= XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB;
    }
    if (flags != 0) {
        memset(&layers->settings, 0, sizeof(layers->settings));
        layers->settings.type = XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB;
        layers->settings.layerFlags = flags;
        layers->settingsChain = &layers->settings;
    }
}

// The real room through the passthrough layer. Level is the layer's opacity; hue is a
// colour scale normalised to the brightest channel so the level is not applied twice.
static void addPassthroughLayer(XrCtx* ctx, FrameLayers* layers) {
    XrCompositionLayerPassthroughFB* pt = &layers->passthrough;
    memset(pt, 0, sizeof(*pt));
    pt->type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB;
    pt->flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    pt->space = XR_NULL_HANDLE;
    pt->layerHandle = ctx->fbPassthroughLayer;
    float peak = fmaxf(ctx->relightScale[0], fmaxf(ctx->relightScale[1], ctx->relightScale[2]));
    if (ctx->colorScaleSupported && peak > 0.001f) {
        XrCompositionLayerColorScaleBiasKHR* tint = &layers->passthroughTint;
        memset(tint, 0, sizeof(*tint));
        tint->type = XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR;
        tint->colorScale.r = ctx->relightScale[0] / peak;
        tint->colorScale.g = ctx->relightScale[1] / peak;
        tint->colorScale.b = ctx->relightScale[2] / peak;
        tint->colorScale.a = 1.0f;
        pt->next = tint;
    }
    pushLayer(ctx, layers, pt);
}

// The ambilight glow behind the picture.
static void addGlowLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // Not sharpened: softness is the effect.
    if (ctx->glowRendered && ctx->everRendered && ctx->shouldRender) {
        // A curved picture's sides would cover a flat glow, so the
        // glow curves with it: same axis, slightly smaller radius.
        GlowCylinder shape;
        float fit = cylinderFit(view->screenWidth, ctx->screenRadius);
        if (view->screenCurved
                && glowCylinderFor(view->screenWidth * fit, view->screenHeight * fit,
                                   ctx->screenRadius, glowScaleFor(ctx), &shape)) {
            XrCompositionLayerCylinderKHR* cyl = &layers->glowCylinder;
            memset(cyl, 0, sizeof(*cyl));
            cyl->type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
            cyl->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            cyl->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cyl->subImage.swapchain = ctx->glowSwapchain;
            cyl->subImage.imageRect.offset.x = shape.rectX;
            cyl->subImage.imageRect.offset.y = 0;
            cyl->subImage.imageRect.extent.width = shape.rectWidth;
            cyl->subImage.imageRect.extent.height = GLOW_TEX;
            cyl->subImage.imageArrayIndex = 0;
            cyl->space = view->space;
            Vec3 axisLocal = { 0.0f, 0.0f, ctx->screenRadius };
            cyl->pose = poseOffset(view->screenPose, axisLocal);
            cyl->radius = shape.radius;
            cyl->centralAngle = shape.centralAngle;
            cyl->aspectRatio = shape.aspectRatio;
            pushLayer(ctx, layers, cyl);
            return;
        }
        // Local +z faces the viewer, so this sits just proud of the picture; layer
        // order, not depth, keeps it underneath. The player's glow lies on the picture.
        Vec3 proudLocal = { 0.0f, 0.0f, 0.002f };
        quadLayer(&layers->glow, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->glowSwapchain, GLOW_TEX, GLOW_TEX, view->space,
                  poseOffset(view->screenPose, proudLocal),
                  view->screenWidth * glowScaleFor(ctx), view->screenHeight * glowScaleFor(ctx));
        pushLayer(ctx, layers, &layers->glow);
    }
}

// The picture, one layer per eye, on a cylinder when curved. With 3D off the
// left half goes to both eyes as one layer. Driven by drawnEyes rather than
// the switch, so an eye is never shown a half the draw has not caught up with.
static void addVideoLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    int viewCount = view->stereo && ctx->drawnEyes == 2 ? 2 : 1;
    for (int eye = 0; eye < viewCount; eye++) {
        XrSwapchainSubImage subImage;
        subImage.swapchain = ctx->swapchain;
        // Eye swap reroutes which half each eye sees.
        int half = viewCount == 2 && view->eyeSwap ? (1 - eye) : eye;
        int x = view->stereo ? half * ctx->videoWidth : 0;
        int w = ctx->videoWidth;
        // The compositor's filter reads a texel past the rect, which at the seam
        // is the other eye. Both ends come in by a texel so the eyes lose the same
        // columns and no disparity is added; the flat picture uses the same crop.
        if (view->stereo && ctx->seamInset && w > 2 * SEAM_INSET_TEXELS) {
            x += SEAM_INSET_TEXELS;
            w -= 2 * SEAM_INSET_TEXELS;
        }
        subImage.imageRect.offset.x = x;
        subImage.imageRect.offset.y = 0;
        subImage.imageRect.extent.width = w;
        subImage.imageRect.extent.height = ctx->videoHeight;
        subImage.imageArrayIndex = 0;

        XrEyeVisibility visibility = viewCount == 1 ? XR_EYE_VISIBILITY_BOTH :
                (eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT);

        if (view->curve > 0.01f && ctx->cylinderSupported) {
            XrCompositionLayerCylinderKHR* cyl = &layers->cylinder[eye];
            memset(cyl, 0, sizeof(*cyl));
            cyl->type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
            // The player's picture has a premultiplied soft edge in
            // alpha; without blending it shows as a dark border.
            cyl->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            cyl->next = layers->settingsChain;
            // Radius runs from 4x distance (slight curve)
            // down to the distance itself (wrapped).
            float radius = ctx->screenRadius;
            cyl->eyeVisibility = visibility;
            cyl->subImage = subImage;
            cyl->space = view->space;
            // The layer pose is the axis, a radius behind the surface.
            Vec3 axisLocal = { 0.0f, 0.0f, radius };
            cyl->pose = poseOffset(view->screenPose, axisLocal);
            cyl->radius = radius;
            // Kept under a full turn or the runtime can refuse the frame; the aspect
            // ratio holds, so a wider picture comes out smaller rather than squashed.
            cyl->centralAngle = cylinderAngle(view->screenWidth, radius);
            cyl->aspectRatio = 1.0f / view->aspect;
            int clamped = cyl->centralAngle < view->screenWidth / radius;
            if (eye == 0 && clamped != ctx->cylinderClampSaid) {
                ctx->cylinderClampSaid = clamped;
                LOGI("screen cylinder %.2f rad, %s %.2f rad at %.0f percent size",
                     view->screenWidth / radius, clamped ? "held to" : "back to",
                     cyl->centralAngle, 100.0f * cylinderFit(view->screenWidth, radius));
            }
            pushLayer(ctx, layers, cyl);
        }
        else {
            XrCompositionLayerQuad* quad = &layers->video[eye];
            // Soft edge in alpha, as above.
            quadLayer(quad, layers->settingsChain, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->swapchain, ctx->videoWidth,
                      ctx->videoHeight, view->space, view->screenPose, view->screenWidth,
                      view->screenHeight);
            quad->eyeVisibility = visibility;
            quad->subImage = subImage;
            pushLayer(ctx, layers, quad);
        }
    }
}

// The move bar or resize corner under the ray.
static void addHandleLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // In the screen's frame
    int isBar = view->barArea;
    int isCorner = !view->barArea && ctx->hoverKind == HOVER_CORNER && cornerSide(ctx) > 0.0f;
    if (ctx->handleArtReady && (isBar || isCorner)) {
        Vec3 local;
        float sizeW, sizeH;
        float roll = 0.0f;

        if (isBar) {
            // Outside a room the bar's frame is the picture
            // as drawn, which a cylinder clamp shrinks.
            sizeW = view->bar.width * BAR_WIDTH_FRAC;
            sizeH = view->bar.width * BAR_HEIGHT_FRAC;
            local.x = 0.0f;
            local.y = -(view->bar.height * 0.5f + view->bar.width * BAR_DROP_FRAC);
        }
        else {
            sizeW = sizeH = cornerSide(ctx);
            int right = ctx->hoverCorner == 1 || ctx->hoverCorner == 3;
            int bottom = ctx->hoverCorner >= 2;
            // Half a bracket outside the corner, so its inner tip touches it.
            local.x = (right ? 0.5f : -0.5f) * (view->screenWidth + sizeW);
            local.y = (bottom ? -0.5f : 0.5f) * (view->screenHeight + sizeH);
            // The art is a top-left bracket, rolled for the other corners.
            if (ctx->hoverCorner == 1) roll = -1.5707963f;
            else if (ctx->hoverCorner == 2) roll = 1.5707963f;
            else if (ctx->hoverCorner == 3) roll = 3.1415927f;
        }
        // Just off the surface, to avoid z-fighting.
        local.z = 0.005f;
        // Corners on a curved screen follow the surface in position and facing.
        float yaw = 0.0f;
        curveLocal(&local, ctx->screenRadius, view->screenCurved, &yaw);

        XrQuaternionf rollQ = { 0.0f, 0.0f, sinf(roll * 0.5f), cosf(roll * 0.5f) };
        Vec3 yawAxis = { 0.0f, 1.0f, 0.0f };
        XrQuaternionf turnQ = quatMul(axisAngleQuat(yawAxis, yaw), rollQ);

        quadLayer(&layers->handle, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  isBar ? ctx->barSwapchain : ctx->cornerSwapchain,
                  isBar ? BAR_TEX_W : CORNER_TEX_W, isBar ? BAR_TEX_H : CORNER_TEX_H,
                  view->space, poseOffset(view->screenPose, local), sizeW, sizeH);
        layers->handle.pose.orientation = quatNorm(quatMul(view->screenPose.orientation, turnQ));
        pushLayer(ctx, layers, &layers->handle);
    }
}

// Logs where the bar hangs when it moves by 1 cm or more. Skipped while a
// handle is dragged, which moves it every frame.
static void logBarPlacement(XrCtx* ctx, const BarFrame* frame) {
    if (ctx->grabMode != GRAB_NONE) {
        return;
    }
    Vec3 edgeLocal = { 0.0f, -frame->height * 0.5f, 0.0f };
    Vec3 barLocal = { 0.0f, -(frame->height * 0.5f + frame->width * BAR_DROP_FRAC), 0.0f };
    XrPosef edge = poseOffset(frame->pose, edgeLocal);
    XrPosef bar = poseOffset(frame->pose, barLocal);
    Vec3 e = { edge.position.x, edge.position.y, edge.position.z };
    Vec3 b = { bar.position.x, bar.position.y, bar.position.z };
    Vec3 moved = vecSub(e, ctx->barSaidEdge);
    if (fabsf(frame->width - ctx->barSaidWidth) < 0.01f && vecDot(moved, moved) < 1e-4f) {
        return;
    }
    ctx->barSaidWidth = frame->width;
    ctx->barSaidEdge = e;
    LOGI("bar: picture's bottom edge at %.2f %.2f %.2f, %.2f m off; bar at %.2f %.2f %.2f, "
         "%.2f m off, %.2f m wide, %s",
         e.x, e.y, e.z, sqrtf(vecDot(e, e)), b.x, b.y, b.z, sqrtf(vecDot(b, b)),
         frame->width * BAR_WIDTH_FRAC, frame->curved ? "on the curved picture" : "on the picture");
}

// Player panels, back to front.
static void addUiPanelLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    static const int order[UI_PANEL_COUNT] = {
        UI_PANEL_LOADING, UI_PANEL_SUBS, UI_PANEL_BACK, UI_PANEL_RAIL, UI_PANEL_TOP,
        UI_PANEL_CONTROLS, UI_PANEL_SHEET, UI_PANEL_DIALOG
    };
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        int id = order[i];
        float level = ctx->ui[id].level;
        if (level <= 0.0f || !ctx->ui[id].ready || ctx->ui[id].texW <= 0) {
            continue;
        }
        float w, h;
        XrPosef pose = uiPanelPose(ctx, id, &w, &h);
        quadLayer(&layers->uiPanel[id], fadeNext(ctx, layers, FADE_SLOT_UI + id, 1, level),
                  XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, ctx->ui[id].chain,
                  ctx->ui[id].texW, ctx->ui[id].texH, view->space, pose, w, h);
        pushLayer(ctx, layers, &layers->uiPanel[id]);
    }
}

// The controller models' projection layer, alpha blended over the picture and panels
// since a controller is nearer. World locked, with the poses it was drawn from.
static void addModelLayer(XrCtx* ctx, FrameLayers* layers) {
    if (!ctx->modelsShowing || !ctx->modelRendered || ctx->passthrough) {
        return;
    }
    XrCompositionLayerProjection* models = &layers->models;
    memset(models, 0, sizeof(*models));
    memset(layers->modelViews, 0, sizeof(layers->modelViews));
    models->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    models->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    models->space = ctx->localSpace;
    models->viewCount = MODEL_EYES;
    models->views = layers->modelViews;
    for (int eye = 0; eye < MODEL_EYES; eye++) {
        XrCompositionLayerProjectionView* projView = &layers->modelViews[eye];
        projView->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projView->pose = ctx->modelViews[eye].pose;
        projView->fov = ctx->modelViews[eye].fov;
        projView->subImage.swapchain = ctx->modelSwapchain;
        projView->subImage.imageRect.offset.x = eye * ctx->modelEyeWidth;
        projView->subImage.imageRect.offset.y = 0;
        projView->subImage.imageRect.extent.width = ctx->modelEyeWidth;
        projView->subImage.imageRect.extent.height = ctx->modelEyeHeight;
        projView->subImage.imageArrayIndex = 0;
    }
    pushLayer(ctx, layers, models);
}

// The laser and its cursor.
static void addPointerLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // Unfocused, the system UI has input, so no pointer is drawn.
    if (ctx->sessionState != XR_SESSION_STATE_FOCUSED) {
        return;
    }
    // The beam (hands included) hides when the ray is
    // switched off, unless a panel is up. The dot stays.
    int beamShown = rayDrawn(ctx->raySetting, ctx->rayFlipped, 0);
    if (beamShown != ctx->rayDrawnSaid) {
        if (ctx->rayDrawnSaid >= 0 || !beamShown) {
            LOGI("ray %s", beamShown ? (raySwitchOn(ctx->raySetting, ctx->rayFlipped)
                                        ? "drawn" : "drawn while a panel is up")
                                     : "hidden, the dot stays");
        }
        ctx->rayDrawnSaid = beamShown;
    }

    // When the pointer sleeps the ray fades out in place, so it reads
    // as put away. Without colour scale support it goes at once.
    int drawn = ctx->beamVisible && !ctx->beamGaze && ctx->pointerArtReady;
    float level = 1.0f;
    if (drawn) {
        ctx->rayFading = 0;
        ctx->rayFadeStart = (Vec3){ ctx->beamStart.x, ctx->beamStart.y, ctx->beamStart.z };
        ctx->rayFadeEnd = (Vec3){ ctx->beamEnd.x, ctx->beamEnd.y, ctx->beamEnd.z };
        ctx->rayFadeFree = ctx->beamFree;
    }
    else if (ctx->rayWasDrawn && !ctx->pointerAwake && ctx->colorScaleSupported) {
        ctx->rayFading = 1;
        ctx->rayFadeFrom = ctx->predictedDisplayTime;
    }
    ctx->rayWasDrawn = drawn;
    if (!drawn && ctx->rayFading) {
        level = 1.0f - (float)(ctx->predictedDisplayTime - ctx->rayFadeFrom)
                / (float)POINTER_FADE_NS;
        if (level <= 0.0f || ctx->pointerAwake || !ctx->pointerArtReady) {
            ctx->rayFading = 0;
        }
    }
    const void* fade = fadeNext(ctx, layers, FADE_SLOT_POINTER, 0, level);

    // Submitted last so they sit over the picture; the compositor places the art.
    if (drawn || ctx->rayFading) {
        Vec3 start = ctx->rayFadeStart;
        Vec3 end = ctx->rayFadeEnd;
        Vec3 head = { ctx->headPos.x, ctx->headPos.y, ctx->headPos.z };
        Vec3 along = vecSub(end, start);
        float length = sqrtf(along.x * along.x + along.y * along.y + along.z * along.z);

        Vec3 mid = { (start.x + end.x) * 0.5f, (start.y + end.y) * 0.5f,
                     (start.z + end.z) * 0.5f };
        Vec3 beamY = vecNorm(along);
        Vec3 toHead = vecNorm(vecSub(head, mid));
        Vec3 beamX = vecCross(beamY, toHead);
        float sideLen = sqrtf(beamX.x * beamX.x + beamX.y * beamX.y + beamX.z * beamX.z);

        // The ribbon is turned to face the head. Aimed nearly along the line
        // of sight there is no such direction, and any perpendicular will do.
        if (sideLen < 0.15f) {
            Vec3 up = { 0.0f, 1.0f, 0.0f };
            beamX = vecCross(beamY, up);
            sideLen = sqrtf(beamX.x * beamX.x + beamX.y * beamX.y + beamX.z * beamX.z);
            if (sideLen < 0.15f) {
                Vec3 side = { 1.0f, 0.0f, 0.0f };
                beamX = vecCross(beamY, side);
            }
        }

        if (length > 0.10f && beamShown) {
            beamX = vecNorm(beamX);
            Vec3 beamZ = vecCross(beamX, beamY);
            XrPosef beamPose = { quatFromBasis(beamX, beamY, beamZ),
                                 { mid.x, mid.y, mid.z } };

            quadLayer(&layers->beam, fade, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->pointerSwapchain, PTR_TEX_W, PTR_BEAM_H, view->space, beamPose,
                      ctx->beamWidth, length);
            pushLayer(ctx, layers, &layers->beam);
        }

        // The cursor sits just off the surface facing the viewer. Independent of
        // the ribbon: gaze has a cursor and no ray; a ray at nothing has no cursor.
        if (!ctx->rayFadeFree) {
            // Scaled with distance so it looks the same size near or far.
            Vec3 toHead = vecSub(head, end);
            float away = sqrtf(vecDot(toHead, toHead));
            float dotSize = pointerDotSize(away);
            if (fabsf(dotSize - ctx->dotSizeSaid) > 0.2f * ctx->dotSizeSaid) {
                LOGI("cursor dot %.1f cm across at %.2f m", dotSize * 100.0f, away);
                ctx->dotSizeSaid = dotSize;
            }
            Vec3 dotZ = vecNorm(toHead);
            Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
            Vec3 dotX = vecNorm(vecCross(worldUp, dotZ));
            Vec3 dotY = vecCross(dotZ, dotX);
            XrPosef dotPose = { quatFromBasis(dotX, dotY, dotZ),
                                { end.x + dotZ.x * 0.012f, end.y + dotZ.y * 0.012f,
                                  end.z + dotZ.z * 0.012f } };

            quadLayer(&layers->dot, fade, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->pointerSwapchain, PTR_TEX_W, PTR_DOT_H, view->space, dotPose,
                      dotSize, dotSize);
            // The dot is the strip under the beam in the shared swapchain.
            layers->dot.subImage.imageRect.offset.y = PTR_BEAM_H;
            pushLayer(ctx, layers, &layers->dot);
        }
    }
}

// What the splash still waits on: the first depth map, if wanted. A model that gave
// up is not waited on.
static int splashWaiting(XrCtx* ctx) {
    int waiting = 0;
    if (ctx->stereoMode == DEPTH_MODE_MODEL
            && atomic_load_explicit(&ctx->depthMapsStaged, memory_order_relaxed) == 0
            && !atomic_load_explicit(&ctx->depthGaveUp, memory_order_relaxed)) {
        waiting |= SPLASH_WAIT_DEPTH;
    }
    return waiting;
}

// Logs when the splash lifted and why: the floor, all ready, or the ceiling.
static void logSplashLift(XrCtx* ctx) {
    static const char* const NAMES[SPLASH_PARTS] = { "panels", "depth model" };
    long upMs = (long)((ctx->splash.liftNs - ctx->splash.firstNs) / 1000000L);
    char parts[160];
    parts[0] = '\0';
    for (int i = 0; i < SPLASH_PARTS; i++) {
        int wanted = i == 0 || ctx->stereoMode == DEPTH_MODE_MODEL;
        if (!wanted) {
            continue;
        }
        size_t used = strlen(parts);
        if (ctx->splashReadyMs[i] >= 0) {
            snprintf(parts + used, sizeof(parts) - used, "%s%s at %d ms", used > 0 ? ", " : "",
                     NAMES[i], ctx->splashReadyMs[i]);
        }
        else {
            snprintf(parts + used, sizeof(parts) - used, "%s%s not ready", used > 0 ? ", " : "",
                     NAMES[i]);
        }
    }
    const char* why = ctx->splash.waitingAtLift != 0 ? "the ceiling"
            : upMs <= SPLASH_FLOOR_NS / 1000000L + 50 ? "the floor" : "ready";
    LOGEV("splash lifted after %ld ms (%s): %s, %s", upMs, why, parts,
          ctx->colorScaleSupported ? "fading" : "cut");
}

// Records what became ready this frame and decides whether the splash lifts.
static void stepSplash(XrCtx* ctx, int64_t now) {
    if (ctx->splash.phase == SPLASH_GONE) {
        return;
    }
    int waiting = splashWaiting(ctx);
    if (ctx->splash.firstNs != 0) {
        int bits[SPLASH_PARTS] = { SPLASH_WAIT_PANELS, SPLASH_WAIT_DEPTH };
        for (int i = 0; i < SPLASH_PARTS; i++) {
            if (ctx->splashReadyMs[i] < 0 && !(waiting & bits[i])) {
                ctx->splashReadyMs[i] = (int)((now - ctx->splash.firstNs) / 1000000L);
            }
        }
    }
    int64_t fadeNs = ctx->colorScaleSupported ? 2 * ctx->fadeNs : 0;
    int phase = ctx->splash.phase;
    if (splashStep(&ctx->splash, now, waiting, fadeNs)) {
        logSplashLift(ctx);
    }
    if (phase == SPLASH_FADING && ctx->splash.phase == SPLASH_GONE) {
        LOGI("splash gone, faded over %ld ms", (long)((now - ctx->splash.liftNs) / 1000000L));
    }
}

// The splash, head locked in front of everything: a ground quad wider than any view,
// and the sheet row for the wedges open now. While fully up it is the only content.
static void addSplashLayers(XrCtx* ctx, FrameLayers* layers, int64_t now) {
    if (ctx->splash.phase == SPLASH_GONE || !ctx->splashArtReady) {
        return;
    }
    float level = splashLevel(&ctx->splash, now, 2 * ctx->fadeNs);
    const void* next = fadeNext(ctx, layers, FADE_SLOT_SPLASH, 0, level);
    XrPosef pose;
    memset(&pose, 0, sizeof(pose));
    pose.orientation.w = 1.0f;

    pose.position.z = -SPLASH_GROUND_DISTANCE_M;
    quadLayer(&layers->splashGround, next, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
              ctx->splashSwapchain, SPLASH_TEX_W, SPLASH_TEX_H, ctx->viewSpace, pose,
              SPLASH_GROUND_M, SPLASH_GROUND_M);
    // The ground's square is the bottom left of the image.
    XrRect2Di* ground = &layers->splashGround.subImage.imageRect;
    ground->offset.x = SPLASH_GROUND_INSET;
    ground->offset.y = SPLASH_GROUND_INSET;
    ground->extent.width = SPLASH_GROUND_PX - 2 * SPLASH_GROUND_INSET;
    ground->extent.height = SPLASH_GROUND_PX - 2 * SPLASH_GROUND_INSET;
    pushLayer(ctx, layers, &layers->splashGround);

    // Rows are drawn top down and arrive bottom up.
    int row = splashWedges(&ctx->splash, now);
    if (row != ctx->splashWedgesShown) {
        ctx->splashWedgesShown = row;
        LOGI("splash wedges %d at %ld ms", row, (long)((now - ctx->splash.firstNs) / 1000000L));
    }
    pose.position.z = -SPLASH_SHEET_DISTANCE_M;
    quadLayer(&layers->splashSheet, next, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
              ctx->splashSwapchain, SPLASH_TEX_W, SPLASH_ROW_H, ctx->viewSpace, pose,
              SPLASH_SHEET_W_M, SPLASH_SHEET_W_M * SPLASH_ROW_H / SPLASH_TEX_W);
    layers->splashSheet.subImage.imageRect.offset.y = SPLASH_TEX_H - (row + 1) * SPLASH_ROW_H;
    pushLayer(ctx, layers, &layers->splashSheet);
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeEndFrame(JNIEnv* env, jobject thiz, jlong handle,
                                                           jboolean newFrame, jfloatArray texMatrixArr,
                                                           jfloat distance, jfloat quadWidth,
                                                           jfloat curvature, jboolean headLocked,
                                                           jfloat separation, jboolean eyeSwap,
                                                           jboolean passthrough) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }

    ctx->passthrough = passthrough;
    ctx->prefCurvature = curvature;
    pollCaptureRequest(ctx);
    propFlag(PROP_PASSTHROUGH, &ctx->passthrough);
    // Panel value first, then the debug property, which always wins.
    if (ctx->panelSeparation >= 0.0f) {
        separation = ctx->panelSeparation;
    }
    if (ctx->separationOverride >= 0.0f) {
        separation = ctx->separationOverride;
    }
    // Read back by the panel's thumb.
    ctx->separationCurrent = separation;
    if (ctx->distanceOverride > 0.0f) {
        distance = ctx->distanceOverride;
    }
    if (ctx->screenOverride > 0.0f) {
        quadWidth = ctx->screenOverride;
    }

    // 3D back on and waiting flat: a new map landed or the wait
    // expired. Redraw at once so a paused picture picks up depth.
    if (ctx->stereoWaiting
            && (atomic_load_explicit(&ctx->depthStagedIndex, memory_order_acquire)
                    != ctx->stereoWaitIndex
                || nowNs() - ctx->stereoWaitNs > STEREO_WAIT_NS)) {
        ctx->stereoWaiting = 0;
        ctx->warpRedraw = 1;
        LOGI("3d warping again after %.0f ms", (nowNs() - ctx->stereoWaitNs) / 1e6);
    }

    // Before anything reads the frame.
    applyVideoColour(ctx);
    applyColourMode(ctx);
    applyBoundary(ctx);

    updateControllerModels(ctx);

    // A 3D switch redraws the latched frame, since the decoder sends nothing while paused;
    // never the last film's frame on a new film's screen.
    if (newFrame) {
        ctx->videoStale = 0;
    }
    int redraw = ctx->warpRedraw && ctx->everRendered && !ctx->videoStale;
    if ((newFrame || redraw) && ctx->shouldRender) {
        int64_t startNs = nowNs();

        float texMatrix[16];
        (*env)->GetFloatArrayRegion(env, texMatrixArr, 0, 16, texMatrix);
        renderVideoFrame(ctx, texMatrix, separation);

        int64_t elapsed = nowNs() - startNs;
        ctx->statFrames++;
        ctx->statTotalNs += elapsed;
        if (elapsed > ctx->statMaxNs) ctx->statMaxNs = elapsed;
        logWarpStats(ctx);
    }
    else if (ctx->shouldRender && ctx->everRendered && glowStale(ctx)) {
        // Glow level or switch changed on a paused picture.
        float texMatrix[16];
        (*env)->GetFloatArrayRegion(env, texMatrixArr, 0, 16, texMatrix);
        redrawGlow(ctx, texMatrix);
        LOGI("glow redrawn on a still picture");
    }
    // Every frame, since controllers move independently of the
    // video. After the warp, so the picture never waits on them.
    if (ctx->shouldRender && ctx->everRendered) {
        renderControllerModels(ctx);
    }

    FrameView view;
    view.aspect = screenAspect(ctx);
    view.stereo = ctx->stereoMode != DEPTH_MODE_OFF;
    view.headLocked = headLocked;
    view.eyeSwap = eyeSwap;
    view.space = headLocked ? ctx->viewSpace : ctx->localSpace;

    if (!ctx->pointerArtReady && ctx->pointerSwapchain != XR_NULL_HANDLE && ctx->shouldRender) {
        uploadPointerArt(ctx);
    }

    // The panel's curve, so a reseed keeps it rather than reverting to the preference.
    view.curve = effectiveCurvature(ctx);
    updatePlacement(ctx, distance, quadWidth, view.curve);
    view.screenPose = ctx->screenPose;
    view.screenWidth = ctx->screenWidth;
    view.screenHeight = view.screenWidth * view.aspect;
    // Same test as the picture's own layer, so furniture sits on the surface actually shown.
    view.screenCurved = view.curve > 0.01f && ctx->cylinderSupported;
    view.bar = barFrame(ctx);
    logBarPlacement(ctx, &view.bar);
    int64_t frameNs = nowNs();
    stepSplash(ctx, frameNs);
    int splashUp = ctx->splash.phase == SPLASH_UP;
    view.barArea = ctx->hoverKind == HOVER_BAR || ctx->backHover;

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = ctx->predictedDisplayTime;
    // Some runtimes only start the cameras on a blend mode change seen after focus;
    // alpha blend from the first frame leaves them off. So the first focused frame
    // is opaque, and blending also waits for the splash to start going.
    int wantPassthrough = ctx->passthrough && ctx->alphaBlendSupported;
    int blendNow = wantPassthrough && ctx->focusedFrames > 0 && !splashUp;
    // The player shows the room through its own dimmable,
    // tintable passthrough layer, with an opaque frame.
    relightStep(ctx, frameNs);
    int roomLayer = blendNow && relightLayerReady(ctx);
    if (roomLayer) {
        blendNow = 0;
    }
    endInfo.environmentBlendMode = blendNow
            ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (blendNow && !ctx->passthroughBlendSaid) {
        ctx->passthroughBlendSaid = 1;
        LOGI("passthrough blend enabled after %d focused frames", ctx->focusedFrames);
        LOGEV("passthrough blend enabled after %d focused frames", ctx->focusedFrames);
    }
    if (ctx->sessionState == XR_SESSION_STATE_FOCUSED) {
        ctx->focusedFrames++;
    }

    FrameLayers layers;
    layers.count = 0;
    setLayerSettings(ctx, &layers);
    if (roomLayer) {
        addPassthroughLayer(ctx, &layers);
    }

    // Nothing else is visible while the splash is fully up.
    if (!splashUp && ctx->everRendered && ctx->shouldRender) {
        // No film: panels without the screen. Handles
        // stay so the panels can be moved and sized.
        if (!ctx->playerScreenHidden && ctx->videoReady) {
            // On a cylinder the glow's clear middle does not line up with the picture,
            // so the glow goes under it and shows through the picture's soft edge.
            int glowUnder = view.screenCurved;
            if (glowUnder) {
                addGlowLayer(ctx, &view, &layers);
            }
            addVideoLayers(ctx, &view, &layers);
            // Otherwise the player's glow goes over the picture, whose edge
            // fades out under it, matching the frame shader's light split.
            if (!glowUnder) {
                addGlowLayer(ctx, &view, &layers);
            }
        }
        addHandleLayer(ctx, &view, &layers);
        stepUiPanels(ctx, frameNs);
        addUiPanelLayers(ctx, &view, &layers);
        addModelLayer(ctx, &layers);
        addPointerLayers(ctx, &view, &layers);
    }
    addSplashLayers(ctx, &layers, frameNs);

    // Over the runtime's limit the frame is refused whole, so shed the controller
    // models first: the beam and dot still point.
    if (layers.count > (uint32_t)ctx->maxLayerCount) {
        dropLayer(&layers, &layers.models);
    }

    // Warn once; a frame that crowds the limit usually repeats. Nothing
    // else is dropped, so the count in the log identifies the culprit.
    if (layers.count >= (uint32_t)ctx->maxLayerCount && !ctx->layerLimitWarned) {
        ctx->layerLimitWarned = 1;
        LOGW("submitted %u composition layers against a limit of %d",
             layers.count, ctx->maxLayerCount);
    }

    endInfo.layerCount = layers.count;
    endInfo.layers = layers.order;
    checkXr(xrEndFrame(ctx->session, &endInfo), "xrEndFrame");
    displayFrameEnded(ctx);
}
