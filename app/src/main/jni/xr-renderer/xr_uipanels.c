// Player UI panels rendered in Java (top bar, side rail, playback controls,
// settings sheet), anchored to the screen. Uploaded on the frame loop, faded
// and hit tested here, drawn by addUiPanelLayers in xr_layers.c.
#include "xr_renderer.h"

// Full fade duration
#define UI_FADE_NS 150000000LL
// The spinner shows once a film's first frame is this late, and turns once a second
#define UI_LOADING_DELAY_NS 400000000LL
#define UI_LOADING_TURN_NS 1000000000LL
// The back button stays this long after the bar or the button was last hovered
#define UI_BACK_HOLD_NS 700000000LL

static int validId(int id) {
    return id >= 0 && id < UI_PANEL_COUNT;
}

// Metres per pixel of a fixed panel: a fixed angle at the screen's distance, so
// a resized screen or library window keeps its menus the same size
float uiMetresPerPx(const XrCtx* ctx) {
    XrVector3f p = ctx->screenPose.position;
    float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
    return UI_PX_ANGLE * fmaxf(d, 0.5f);
}

XrPosef uiPanelPose(XrCtx* ctx, int id, float* outWidth, float* outHeight) {
    float screenW = ctx->screenWidth;
    float screenH = screenW * screenAspect(ctx);
    float w, gap, xOff;
    if (ctx->ui[id].fixed) {
        float perPx = uiMetresPerPx(ctx);
        w = (float)ctx->ui[id].texW * perPx;
        gap = ctx->ui[id].gapFrac * perPx;
        xOff = ctx->ui[id].xOff * perPx;
    }
    else {
        w = ctx->ui[id].widthFrac * screenW;
        gap = ctx->ui[id].gapFrac * screenW;
        xOff = ctx->ui[id].xOff * screenW;
    }
    float h = ctx->ui[id].texW > 0 ? w * (float)ctx->ui[id].texH / (float)ctx->ui[id].texW : 0.0f;
    *outWidth = w;
    *outHeight = h;

    // Slightly in front of the picture so overlapping panels are never behind it
    Vec3 local = { 0.0f, 0.0f, 0.005f };
    switch (ctx->ui[id].anchor) {
        case UI_ANCHOR_ABOVE:
            local.y = screenH * 0.5f + gap + h * 0.5f;
            break;
        case UI_ANCHOR_LEFT:
            local.x = -(screenW * 0.5f + gap + w * 0.5f);
            break;
        case UI_ANCHOR_BELOW:
            local.y = -(screenH * 0.5f + gap + h * 0.5f);
            break;
        case UI_ANCHOR_FRONT:
        default:
            local.z = gap;
            break;
    }
    local.x += xOff;
    Vec3 offset = quatRotate(ctx->screenPose.orientation, local);
    XrPosef pose;
    pose.orientation = ctx->screenPose.orientation;
    if (id == UI_PANEL_LOADING) {
        Vec3 axis = { 0.0f, 0.0f, 1.0f };
        float turn = (float)(nowNs() % UI_LOADING_TURN_NS) / (float)UI_LOADING_TURN_NS;
        pose.orientation = quatMul(pose.orientation, axisAngleQuat(axis, -turn * 6.2831853f));
    }
    pose.position.x = ctx->screenPose.position.x + offset.x;
    pose.position.y = ctx->screenPose.position.y + offset.y;
    pose.position.z = ctx->screenPose.position.z + offset.z;

    // Float toward the eyes and scale down by the same factor: same angular
    // size, nearer stereo depth
    float k = ctx->ui[id].depthK;
    if (k > 0.0f && k < 0.9f) {
        float f = 1.0f - k;
        XrVector3f eye = ctx->headPos;
        pose.position.x = eye.x + (pose.position.x - eye.x) * f;
        pose.position.y = eye.y + (pose.position.y - eye.y) * f;
        pose.position.z = eye.z + (pose.position.z - eye.z) * f;
        *outWidth *= f;
        *outHeight *= f;
    }
    return pose;
}

// Front-most panel under the ray, or -1. Only panels at least half faded in.
int uiPanelAt(XrCtx* ctx, XrPosef aim, float* outU, float* outV) {
    if (!ctx->playerMode) {
        return -1;
    }
    // Subtitles are not hit tested
    static const int order[6] = {
        UI_PANEL_DIALOG, UI_PANEL_SHEET, UI_PANEL_CONTROLS, UI_PANEL_TOP, UI_PANEL_RAIL,
        UI_PANEL_BACK
    };
    for (int i = 0; i < 6; i++) {
        int id = order[i];
        if (!ctx->ui[id].ready || !ctx->ui[id].visible || ctx->ui[id].level < 0.5f) {
            continue;
        }
        float w, h, u, v;
        XrPosef pose = uiPanelPose(ctx, id, &w, &h);
        if (screenProject(aim, pose, w, h, 0.0f, 0, &u, &v)
                && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            *outU = u;
            *outV = v;
            return id;
        }
    }
    return -1;
}

void stepUiPanels(XrCtx* ctx, int64_t nowNs) {
    float step = ctx->uiStepNs > 0 ? (float)(nowNs - ctx->uiStepNs) / (float)UI_FADE_NS : 1.0f;
    ctx->uiStepNs = nowNs;
    if (step < 0.0f) {
        step = 0.0f;
    }
    ctx->ui[UI_PANEL_LOADING].visible = ctx->playerMode && !ctx->playerScreenHidden
            && !ctx->videoReady && nowNs - ctx->videoWaitNs > UI_LOADING_DELAY_NS;
    if (ctx->hoverKind == HOVER_BAR || ctx->backHover) {
        ctx->backHoverNs = nowNs;
    }
    ctx->ui[UI_PANEL_BACK].visible = ctx->playerMode && ctx->backWanted
            && nowNs - ctx->backHoverNs < UI_BACK_HOLD_NS;
    for (int id = 0; id < UI_PANEL_COUNT; id++) {
        float target = ctx->ui[id].visible ? 1.0f : 0.0f;
        float level = ctx->ui[id].level;
        level += level < target ? step : -step;
        if ((target - ctx->ui[id].level) * (target - level) <= 0.0f) {
            level = target;
        }
        ctx->ui[id].level = level;
    }
}

void destroyUiPanels(XrCtx* ctx) {
    for (int id = 0; id < UI_PANEL_COUNT; id++) {
        destroyArtSwapchain(ctx, &ctx->ui[id].chain, &ctx->ui[id].images);
        ctx->ui[id].ready = 0;
        ctx->ui[id].texW = 0;
        ctx->ui[id].texH = 0;
    }
}

// Uploads a panel image: RGBA, top-down rows, premultiplied alpha. Frame loop
// thread only (needs the GL context).
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadUiPanel(JNIEnv* env, jobject thiz,
                                                                jlong handle, jint id,
                                                                jobject sheet, jint width,
                                                                jint height) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || !validId(id) || sheet == NULL || width <= 0 || height <= 0
            || width > UI_PANEL_TEX_MAX || height > UI_PANEL_TEX_MAX) {
        return;
    }
    if (ctx->ui[id].chain == XR_NULL_HANDLE || ctx->ui[id].texW != width
            || ctx->ui[id].texH != height) {
        destroyArtSwapchain(ctx, &ctx->ui[id].chain, &ctx->ui[id].images);
        ctx->ui[id].ready = 0;
        if (!createArtSwapchain(ctx, width, height, "create player panel swapchain",
                                &ctx->ui[id].chain, &ctx->ui[id].images,
                                &ctx->ui[id].imageCount)) {
            ctx->ui[id].texW = 0;
            ctx->ui[id].texH = 0;
            return;
        }
        ctx->ui[id].texW = width;
        ctx->ui[id].texH = height;
        LOGI("player panel %d chain %dx%d", id, width, height);
    }
    const unsigned char* px = (*env)->GetDirectBufferAddress(env, sheet);
    jlong capacity = (*env)->GetDirectBufferCapacity(env, sheet);
    if (px == NULL || capacity < (jlong)width * height * 4) {
        LOGE("player panel %d sheet is %lld bytes, needs %d", id, (long long)capacity,
             width * height * 4);
        return;
    }
    ctx->ui[id].ready = uploadFlipped(ctx, ctx->ui[id].chain, ctx->ui[id].images, px, width,
                                      height);
}

// Panel float depth toward the viewer, fraction of the distance (0 on the
// screen). Any thread.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetUiPanelDepth(JNIEnv* env, jobject thiz,
                                                                  jlong handle, jint id, jfloat k) {
    (void)env;
    (void)thiz;
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || !validId(id)) {
        return;
    }
    ctx->ui[id].depthK = k < 0.0f ? 0.0f : (k > 0.5f ? 0.5f : k);
}

// Panel placement and visibility. Any thread; the frame loop fades it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetUiPanel(JNIEnv* env, jobject thiz,
                                                             jlong handle, jint id,
                                                             jboolean visible, jint anchor,
                                                             jboolean fixed, jfloat widthFrac,
                                                             jfloat gapFrac, jfloat xOff) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || !validId(id)) {
        return;
    }
    // The renderer decides when the spinner and the back button show
    if (id == UI_PANEL_BACK) {
        ctx->backWanted = visible ? 1 : 0;
    }
    else if (id != UI_PANEL_LOADING) {
        ctx->ui[id].visible = visible ? 1 : 0;
    }
    ctx->ui[id].anchor = anchor;
    ctx->ui[id].fixed = fixed ? 1 : 0;
    ctx->ui[id].widthFrac = widthFrac;
    ctx->ui[id].gapFrac = gapFrac;
    ctx->ui[id].xOff = xOff;
}
