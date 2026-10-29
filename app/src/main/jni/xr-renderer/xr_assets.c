// Art swapchains and their uploads: pointer and handle art drawn here, panels from Java.
#include "xr_renderer.h"

/**
* Creates a swapchain for one piece of art and fetches its images. On failure the
* handle stays null so the layer is skipped.
*/
int createArtSwapchain(XrCtx* ctx, int width, int height, const char* what,
                       XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images,
                       uint32_t* count) {
    *chain = XR_NULL_HANDLE;
    *images = NULL;
    *count = 0;

    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = ctx->swapchainFormat;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;

    XrSwapchain created = XR_NULL_HANDLE;
    if (!checkXr(xrCreateSwapchain(ctx->session, &info, &created), what)) {
        LOGE("%s failed with %d swapchains alive", what, ctx->swapchainsAlive);
        return 0;
    }

    uint32_t n = 0;
    if (!checkXr(xrEnumerateSwapchainImages(created, 0, &n, NULL), what) || n == 0) {
        xrDestroySwapchain(created);
        return 0;
    }
    XrSwapchainImageOpenGLESKHR* fetched = calloc(n, sizeof(XrSwapchainImageOpenGLESKHR));
    if (fetched == NULL) {
        LOGE("%s: no memory for %u swapchain images", what, n);
        xrDestroySwapchain(created);
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        fetched[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    if (!checkXr(xrEnumerateSwapchainImages(created, n, &n,
                                            (XrSwapchainImageBaseHeader*)fetched), what)) {
        free(fetched);
        xrDestroySwapchain(created);
        return 0;
    }

    *chain = created;
    *images = fetched;
    *count = n;
    ctx->swapchainsAlive++;
    return 1;
}

// Safe on a chain that was never created
void destroyArtSwapchain(XrCtx* ctx, XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images) {
    if (*chain != XR_NULL_HANDLE) {
        xrDestroySwapchain(*chain);
        *chain = XR_NULL_HANDLE;
        ctx->swapchainsAlive--;
    }
    free(*images);
    *images = NULL;
}

// Creates all art swapchains. Only the pointer's is required; others just drop their
// layer on failure. Kept few: some runtimes cap a session at 32 (Pico 4 Ultra), so
// multi-sheet panels share one chain and the bar buttons share one atlas.
int createPointerSwapchain(XrCtx* ctx) {
    if (!createArtSwapchain(ctx, PTR_TEX_W, PTR_TEX_H, "create pointer swapchain",
                            &ctx->pointerSwapchain, &ctx->pointerImages,
                            &ctx->pointerImageCount)) {
        return 0;
    }

    // Handles get their own swapchain to avoid image rect origin ambiguity
    createArtSwapchain(ctx, BAR_TEX_W, BAR_TEX_H, "create bar swapchain",
                       &ctx->barSwapchain, &ctx->barImages, &ctx->barImageCount);

    createArtSwapchain(ctx, PICKER_TEX_W, PICKER_TEX_H, "create picker swapchain",
                       &ctx->pickerSwapchain, &ctx->pickerImages, &ctx->pickerImageCount);

    createArtSwapchain(ctx, COG_TEX_W, COG_TEX_H, "create cog panel swapchain",
                       &ctx->cogPanelSwapchain, &ctx->cogPanelImages,
                       &ctx->cogPanelImageCount);
    ctx->cogArtShown = -1;

    createArtSwapchain(ctx, COG_THUMB_TEX, COG_THUMB_TEX, "create cog thumb swapchain",
                       &ctx->cogThumbSwapchain, &ctx->cogThumbImages, &ctx->cogThumbImageCount);
    createArtSwapchain(ctx, COG_READOUT_TEX_W, COG_READOUT_TEX_H, "create cog readout swapchain",
                       &ctx->cogReadoutSwapchain, &ctx->cogReadoutImages,
                       &ctx->cogReadoutImageCount);
    createArtSwapchain(ctx, COG_MARKS_TEX_W, COG_MARKS_TEX_H, "create cog marks swapchain",
                       &ctx->cogMarksSwapchain, &ctx->cogMarksImages, &ctx->cogMarksImageCount);
    createArtSwapchain(ctx, COG_CLOCK_TEX_W, COG_CLOCK_TEX_H, "create cog clock swapchain",
                       &ctx->cogClockSwapchain, &ctx->cogClockImages, &ctx->cogClockImageCount);

    createArtSwapchain(ctx, KB_TEX_W, KB_TEX_H, "create keyboard swapchain",
                       &ctx->kbPanelSwapchain, &ctx->kbPanelImages, &ctx->kbPanelImageCount);
    ctx->kbStateShown = -1;

    createArtSwapchain(ctx, EXIT_TEX_W, EXIT_TEX_H, "create exit prompt swapchain",
                       &ctx->exitPromptSwapchain, &ctx->exitPromptImages,
                       &ctx->exitPromptImageCount);
    ctx->exitArtShown = -1;

    // Redrawn whenever its content changes
    createArtSwapchain(ctx, REPORT_TEX_W, REPORT_TEX_H, "create report sheet swapchain",
                       &ctx->reportSwapchain, &ctx->reportImages, &ctx->reportImageCount);

    // Bar button atlas; the 3D switch cells stay empty without stereo
    createArtSwapchain(ctx, BTN_ATLAS_W, BTN_ATLAS_H, "create bar button swapchain",
                       &ctx->buttonSwapchain, &ctx->buttonImages, &ctx->buttonImageCount);

    if (ctx->handsEnabled) {
        createArtSwapchain(ctx, HINT_TEX_W, HINT_TEX_H, "create hand lock hint swapchain",
                           &ctx->hintSwapchain, &ctx->hintImages, &ctx->hintImageCount);
    }

    createArtSwapchain(ctx, KOFI_TEX_W, KOFI_TEX_H, "create Ko-fi sheet swapchain",
                       &ctx->kofiSwapchain, &ctx->kofiImages, &ctx->kofiImageCount);

    createArtSwapchain(ctx, OUTLINE_TEX, OUTLINE_TEX, "create outline swapchain",
                       &ctx->outlineSwapchain, &ctx->outlineImages, &ctx->outlineImageCount);

    // Redrawn every frame from the picture
    createArtSwapchain(ctx, GLOW_TEX, GLOW_TEX, "create glow swapchain",
                       &ctx->glowSwapchain, &ctx->glowImages, &ctx->glowImageCount);

    createArtSwapchain(ctx, CORNER_TEX_W, CORNER_TEX_H, "create corner swapchain",
                       &ctx->cornerSwapchain, &ctx->cornerImages, &ctx->cornerImageCount);

    LOGEV("swapchains alive %d", ctx->swapchainsAlive);
    return 1;
}

// Uploads one CPU buffer into a swapchain image
static int uploadArt(XrCtx* ctx, XrSwapchain chain, XrSwapchainImageOpenGLESKHR* images,
                     const unsigned char* px, int width, int height) {
    if (chain == XR_NULL_HANDLE) {
        return 0;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(chain, &acquire, &index), "acquire art image")) {
        return 0;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(chain, &wait);

    glBindTexture(GL_TEXTURE_2D, images[index].image);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(chain, &release);
    return 1;
}

// Uploads with rows reversed (Java bitmaps are top down)
int uploadFlipped(XrCtx* ctx, XrSwapchain chain, XrSwapchainImageOpenGLESKHR* images,
                         const unsigned char* px, int width, int height) {
    size_t stride = (size_t)width * 4;
    unsigned char* flipped = malloc(stride * height);
    if (flipped == NULL) {
        return 0;
    }
    for (int y = 0; y < height; y++) {
        memcpy(flipped + stride * y, px + stride * (height - 1 - y), stride);
    }
    int ok = uploadArt(ctx, chain, images, flipped, width, height);
    free(flipped);
    return ok;
}

// Antialiased coverage at a distance from a shape edge, in pixels
static float edgeAlpha(float distance, float halfStroke) {
    float a = (halfStroke - distance) / 1.5f + 0.5f;
    if (a < 0.0f) return 0.0f;
    if (a > 1.0f) return 1.0f;
    return a;
}

static void buildHandleArt(XrCtx* ctx) {
    unsigned char* bar = calloc(BAR_TEX_W * BAR_TEX_H * 4, 1);
    unsigned char* corner = calloc(CORNER_TEX_W * CORNER_TEX_H * 4, 1);
    if (bar == NULL || corner == NULL) {
        free(bar);
        free(corner);
        return;
    }

    // Rounded bar; vertically symmetric
    float barR = BAR_TEX_H * 0.5f;
    for (int y = 0; y < BAR_TEX_H; y++) {
        for (int x = 0; x < BAR_TEX_W; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float cx = px;
            if (cx < barR) cx = barR;
            if (cx > BAR_TEX_W - barR) cx = BAR_TEX_W - barR;
            float dx = px - cx, dy = py - barR;
            float d = sqrtf(dx * dx + dy * dy);
            unsigned char* p = bar + ((y * BAR_TEX_W) + x) * 4;
            unsigned char a = (unsigned char)(edgeAlpha(d, barR - 1.0f) * 235.0f);
            p[0] = p[1] = p[2] = a;
            p[3] = a;
        }
    }

    // Rounded bracket with its outer corner at the tile centre and arms running right and
    // down, so the quad centred on a screen corner wraps it. Rows written bottom up.
    const float mid = CORNER_TEX_W * 0.5f;
    const float arcR = 10.0f;
    const float stroke = 3.0f;
    for (int y = 0; y < CORNER_TEX_H; y++) {
        for (int x = 0; x < CORNER_TEX_W; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float d;
            if (px < mid + arcR && py < mid + arcR) {
                float ax = px - (mid + arcR), ay = py - (mid + arcR);
                d = fabsf(sqrtf(ax * ax + ay * ay) - arcR);
            }
            else if (px >= mid + arcR) {
                d = fabsf(py - mid);
            }
            else {
                d = fabsf(px - mid);
            }
            unsigned char* p = corner + (((CORNER_TEX_H - 1 - y) * CORNER_TEX_W) + x) * 4;
            unsigned char a = (unsigned char)(edgeAlpha(d, stroke) * 235.0f);
            p[0] = p[1] = p[2] = a;
            p[3] = a;
        }
    }

    unsigned char* outline = calloc(OUTLINE_TEX * OUTLINE_TEX * 4, 1);
    if (outline != NULL) {
        // Rounded rectangle ring marking the hovered and selected picker cells
        const float radius = 16.0f;
        const float border = 2.5f;
        const float half = OUTLINE_TEX * 0.5f;
        for (int y = 0; y < OUTLINE_TEX; y++) {
            for (int x = 0; x < OUTLINE_TEX; x++) {
                // Signed distance to a rounded rectangle
                float qx = fabsf(x + 0.5f - half) - (half - radius);
                float qy = fabsf(y + 0.5f - half) - (half - radius);
                float mx = qx > 0.0f ? qx : 0.0f;
                float my = qy > 0.0f ? qy : 0.0f;
                float outside = sqrtf(mx * mx + my * my);
                float inside = (qx > qy ? qx : qy);
                if (inside > 0.0f) {
                    inside = 0.0f;
                }
                float dist = fabsf(outside + inside);

                unsigned char a = (unsigned char)(edgeAlpha(dist, border) * 255.0f);
                unsigned char* p = outline + ((y * OUTLINE_TEX) + x) * 4;
                p[0] = p[1] = p[2] = a;
                p[3] = a;
            }
        }
    }

    // Slider thumb; round, so orientation doesn't matter
    unsigned char* thumb = calloc(COG_THUMB_TEX * COG_THUMB_TEX * 4, 1);
    if (thumb != NULL) {
        const float thumbMid = COG_THUMB_TEX * 0.5f;
        const float thumbR = COG_THUMB_TEX * 0.42f;
        for (int y = 0; y < COG_THUMB_TEX; y++) {
            for (int x = 0; x < COG_THUMB_TEX; x++) {
                float dx = x + 0.5f - thumbMid, dy = y + 0.5f - thumbMid;
                float d = sqrtf(dx * dx + dy * dy);
                unsigned char* p = thumb + ((y * COG_THUMB_TEX) + x) * 4;
                unsigned char a = (unsigned char)(edgeAlpha(d, thumbR) * 235.0f);
                p[0] = p[1] = p[2] = a;
                p[3] = a;
            }
        }
    }

    int ok = uploadArt(ctx, ctx->barSwapchain, ctx->barImages, bar, BAR_TEX_W, BAR_TEX_H);
    if (thumb != NULL) {
        ctx->cogThumbReady = uploadArt(ctx, ctx->cogThumbSwapchain, ctx->cogThumbImages,
                                       thumb, COG_THUMB_TEX, COG_THUMB_TEX);
        free(thumb);
    }
    if (outline != NULL) {
        ctx->outlineReady = uploadArt(ctx, ctx->outlineSwapchain, ctx->outlineImages,
                                      outline, OUTLINE_TEX, OUTLINE_TEX);
        free(outline);
    }
    ok &= uploadArt(ctx, ctx->cornerSwapchain, ctx->cornerImages, corner,
                    CORNER_TEX_W, CORNER_TEX_H);
    ctx->handleArtReady = ok;

    free(bar);
    free(corner);
}

// Frame loop only, with the session running: waiting on a swapchain image before the
// session begins blocks forever.
int uploadPointerArt(XrCtx* ctx) {
    unsigned char* px = calloc(PTR_TEX_W * PTR_TEX_H * 4, 1);
    if (px == NULL) {
        return 0;
    }

    const float half = PTR_TEX_W * 0.5f;
    for (int y = 0; y < PTR_BEAM_H; y++) {
        // Fades at both ends; symmetric because the runtime's image orientation isn't fixed
        float along = (y + 0.5f) / PTR_BEAM_H;
        float edge = along < 0.5f ? along : 1.0f - along;
        float lengthFade = edge < 0.12f ? edge / 0.12f : 1.0f;
        for (int x = 0; x < PTR_TEX_W; x++) {
            float r = fabsf((x + 0.5f) - half) / half;
            float t = r * 3.2f;
            float a = expf(-t * t) * lengthFade;
            unsigned char* p = px + ((y * PTR_TEX_W) + x) * 4;
            unsigned char lit = (unsigned char)(a * 255.0f + 0.5f);
            p[0] = lit;
            p[1] = lit;
            p[2] = lit;
            p[3] = lit;
        }
    }

    for (int y = 0; y < PTR_DOT_H; y++) {
        for (int x = 0; x < PTR_TEX_W; x++) {
            float dx = ((x + 0.5f) - half) / half;
            float dy = ((y + 0.5f) - PTR_DOT_H * 0.5f) / (PTR_DOT_H * 0.5f);
            float r = sqrtf(dx * dx + dy * dy);
            // Solid core with a darker rim, visible against a bright picture
            float a = r < 0.45f ? 1.0f : (r < 0.75f ? (0.75f - r) / 0.30f : 0.0f);
            float shade = r < 0.35f ? 1.0f : 0.25f;
            unsigned char* p = px + (((PTR_BEAM_H + y) * PTR_TEX_W) + x) * 4;
            unsigned char lit = (unsigned char)(a * 255.0f * shade + 0.5f);
            p[0] = lit;
            p[1] = lit;
            p[2] = lit;
            p[3] = (unsigned char)(a * 255.0f + 0.5f);
        }
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (checkXr(xrAcquireSwapchainImage(ctx->pointerSwapchain, &acquire, &index),
                "acquire pointer image")) {
        XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(ctx->pointerSwapchain, &wait);

        glBindTexture(GL_TEXTURE_2D, ctx->pointerImages[index].image);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PTR_TEX_W, PTR_TEX_H,
                        GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindTexture(GL_TEXTURE_2D, 0);

        XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(ctx->pointerSwapchain, &release);
        // Static art, uploaded once
        ctx->pointerArtReady = 1;
    }

    free(px);
    if (ctx->pointerArtReady) {
        buildHandleArt(ctx);
    }
    return ctx->pointerArtReady;
}

// Whether a direct buffer holds a full width x height RGBA image
static int artBufferFits(JNIEnv* env, jobject buffer, const void* px, int width, int height) {
    jlong need = (jlong)width * height * 4;
    jlong have = (*env)->GetDirectBufferCapacity(env, buffer);
    if (px == NULL || have < need) {
        LOGW("art of %dx%d refused: its buffer holds %lld of the %lld bytes", width, height,
             (long long)have, (long long)need);
        return 0;
    }
    return 1;
}

// Uploads one Java-drawn sheet (direct buffer, RGBA, top down). A missing sheet leaves
// the swapchain and ready flag unchanged.
static void uploadSheet(JNIEnv* env, XrCtx* ctx, jobject buffer, XrSwapchain chain,
                        XrSwapchainImageOpenGLESKHR* images, int width, int height,
                        int* ready) {
    if (buffer == NULL) {
        return;
    }
    const unsigned char* px = (*env)->GetDirectBufferAddress(env, buffer);
    if (artBufferFits(env, buffer, px, width, height)) {
        *ready = uploadFlipped(ctx, chain, images, px, width, height);
    }
}

// Stores a Java sheet in memory, rows flipped for upload, allocating the slot on first
// use. Returns 0 and leaves the slot unchanged if missing or too small.
static int keepSheet(JNIEnv* env, jobject buffer, unsigned char** slot, int width, int height) {
    if (buffer == NULL) {
        return 0;
    }
    const unsigned char* px = (*env)->GetDirectBufferAddress(env, buffer);
    if (!artBufferFits(env, buffer, px, width, height)) {
        return 0;
    }
    size_t stride = (size_t)width * 4;
    if (*slot == NULL) {
        *slot = malloc(stride * height);
        if (*slot == NULL) {
            LOGE("no memory to keep a sheet of %dx%d", width, height);
            return 0;
        }
    }
    for (int y = 0; y < height; y++) {
        memcpy(*slot + stride * y, px + stride * (height - 1 - y), stride);
    }
    return 1;
}

// Uploads a stored sheet into a multi-sheet chain unless already shown. Always a whole
// sheet, since any image may reach the compositor. On failure the sheet is marked not
// ready so its layer is skipped.
static int showSheet(XrCtx* ctx, XrSwapchain chain, XrSwapchainImageOpenGLESKHR* images,
                     unsigned char* const* sheets, int* ready, int which, int* shown,
                     int width, int height) {
    if (!ready[which] || sheets[which] == NULL) {
        return 0;
    }
    if (*shown == which) {
        return 1;
    }
    if (!uploadArt(ctx, chain, images, sheets[which], width, height)) {
        ready[which] = 0;
        *shown = -1;
        return 0;
    }
    *shown = which;
    return 1;
}

// Frame loop only, with the GL context current
int showCogArt(XrCtx* ctx, int art) {
    if (art < 0 || art >= COG_ART_COUNT) {
        return 0;
    }
    return showSheet(ctx, ctx->cogPanelSwapchain, ctx->cogPanelImages, ctx->cogPanelPixels,
                     ctx->cogPanelReady, art, &ctx->cogArtShown, COG_TEX_W, COG_TEX_H);
}

int showKbSheet(XrCtx* ctx, int state) {
    if (state < 0 || state >= KB_STATE_COUNT) {
        return 0;
    }
    return showSheet(ctx, ctx->kbPanelSwapchain, ctx->kbPanelImages, ctx->kbPanelPixels,
                     ctx->kbPanelReady, state, &ctx->kbStateShown, KB_TEX_W, KB_TEX_H);
}

int showExitSheet(XrCtx* ctx, int zone) {
    if (zone < 0 || zone >= EXIT_ART_COUNT) {
        return 0;
    }
    return showSheet(ctx, ctx->exitPromptSwapchain, ctx->exitPromptImages,
                     ctx->exitPromptPixels, ctx->exitPromptReady, zone, &ctx->exitArtShown,
                     EXIT_TEX_W, EXIT_TEX_H);
}

// Writes one bar button face into its atlas cell, allocating the atlas on first use
static int putButtonFace(JNIEnv* env, XrCtx* ctx, jobject buffer, int cell) {
    if (buffer == NULL || ctx->buttonSwapchain == XR_NULL_HANDLE) {
        return 0;
    }
    const unsigned char* px = (*env)->GetDirectBufferAddress(env, buffer);
    if (!artBufferFits(env, buffer, px, BUTTON_TEX, BUTTON_TEX)) {
        return 0;
    }
    if (ctx->buttonAtlas == NULL) {
        ctx->buttonAtlas = calloc((size_t)BTN_ATLAS_W * BTN_ATLAS_H * 4, 1);
        if (ctx->buttonAtlas == NULL) {
            LOGE("no memory for the bar's buttons");
            return 0;
        }
    }
    return buttonCellPut(ctx->buttonAtlas, cell, px);
}

// Re-uploads the whole button atlas
static int uploadButtons(XrCtx* ctx) {
    return ctx->buttonAtlas != NULL
            && uploadArt(ctx, ctx->buttonSwapchain, ctx->buttonImages, ctx->buttonAtlas,
                         BTN_ATLAS_W, BTN_ATLAS_H);
}

// Single-face button; ready flag unchanged if no face arrived
static void uploadButton(JNIEnv* env, XrCtx* ctx, jobject buffer, int cell, int* ready) {
    if (putButtonFace(env, ctx, buffer, cell)) {
        *ready = uploadButtons(ctx);
    }
}

// Two-face switch: off in cell, on in cell + 1; both or neither
static int uploadSwitch(JNIEnv* env, XrCtx* ctx, jobject off, jobject on, int cell) {
    int put = putButtonFace(env, ctx, off, cell);
    put = putButtonFace(env, ctx, on, cell + 1) && put;
    return put && uploadButtons(ctx);
}

// Frees stored sheets and the button atlas at session end
void freeArtSheets(XrCtx* ctx) {
    for (int art = 0; art < COG_ART_COUNT; art++) {
        free(ctx->cogPanelPixels[art]);
        ctx->cogPanelPixels[art] = NULL;
    }
    for (int state = 0; state < KB_STATE_COUNT; state++) {
        free(ctx->kbPanelPixels[state]);
        ctx->kbPanelPixels[state] = NULL;
    }
    for (int sheet = 0; sheet < EXIT_ART_COUNT; sheet++) {
        free(ctx->exitPromptPixels[sheet]);
        ctx->exitPromptPixels[sheet] = NULL;
    }
    free(ctx->buttonAtlas);
    ctx->buttonAtlas = NULL;
}

// Environment picker grid and its button. Java reports how many cells are filled; the
// rest are blank.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadPicker(JNIEnv* env, jobject thiz,
                                                               jlong handle, jobject grid,
                                                               jobject button, jint cells) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->pickerCells = cells < 0 ? 0 : (cells > PICKER_CELLS ? PICKER_CELLS : cells);
    uploadSheet(env, ctx, grid, ctx->pickerSwapchain, ctx->pickerImages,
                PICKER_TEX_W, PICKER_TEX_H, &ctx->pickerReady);
    uploadButton(env, ctx, button, BTN_CELL_ENV, &ctx->envButtonReady);
    LOGI("picker art %s, button %s, %d of %d cells", ctx->pickerReady ? "ready" : "missing",
         ctx->envButtonReady ? "ready" : "missing", ctx->pickerCells, PICKER_CELLS);
}

// Settings panel sheets (all of them, in COG_ART_ order, stored for later tab switches)
// and the cog button
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadCog(JNIEnv* env, jobject thiz,
                                                            jlong handle, jobjectArray sheets,
                                                            jobject button) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    int count = sheets != NULL ? (*env)->GetArrayLength(env, sheets) : 0;
    int ready = 0;
    int shownAgain = 0;
    for (int art = 0; art < COG_ART_COUNT && art < count; art++) {
        jobject sheet = (*env)->GetObjectArrayElement(env, sheets, art);
        if (keepSheet(env, sheet, &ctx->cogPanelPixels[art], COG_TEX_W, COG_TEX_H)) {
            ctx->cogPanelReady[art] = ctx->cogPanelSwapchain != XR_NULL_HANDLE;
            shownAgain |= art == ctx->cogArtShown;
        }
        if (sheet != NULL) {
            (*env)->DeleteLocalRef(env, sheet);
        }
        ready += ctx->cogPanelReady[art] ? 1 : 0;
    }
    // Re-upload the sheet currently shown; the rest go up when shown
    if (shownAgain) {
        int art = ctx->cogArtShown;
        ctx->cogArtShown = -1;
        showCogArt(ctx, art);
    }
    uploadButton(env, ctx, button, BTN_CELL_COG, &ctx->cogButtonReady);
    LOGI("cog sheets %d of %d ready, button %s", ready, COG_ART_COUNT,
         ctx->cogButtonReady ? "ready" : "missing");
}

// Readout strip and the values it was drawn with; shown only while those still match.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadCogReadout(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject strip,
                                                                   jintArray values) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || strip == NULL || values == NULL
            || (*env)->GetArrayLength(env, values) < READOUT_VALUES) {
        return;
    }
    int drawn[READOUT_VALUES];
    (*env)->GetIntArrayRegion(env, values, 0, READOUT_VALUES, drawn);
    uploadSheet(env, ctx, strip, ctx->cogReadoutSwapchain, ctx->cogReadoutImages,
                COG_READOUT_TEX_W, COG_READOUT_TEX_H, &ctx->cogReadoutReady);
    memcpy(ctx->cogReadoutDrawn, drawn, sizeof(drawn));
}

// Display tab selection marks, redrawn in the frame a mark moved
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadCogMarks(JNIEnv* env, jobject thiz,
                                                                 jlong handle, jobject strip) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || strip == NULL) {
        return;
    }
    uploadSheet(env, ctx, strip, ctx->cogMarksSwapchain, ctx->cogMarksImages, COG_MARKS_TEX_W,
                COG_MARKS_TEX_H, &ctx->cogMarksReady);
}

// Clock and battery strip above the settings panel
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadCogClock(JNIEnv* env, jobject thiz,
                                                                 jlong handle, jobject strip) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || strip == NULL) {
        return;
    }
    uploadSheet(env, ctx, strip, ctx->cogClockSwapchain, ctx->cogClockImages, COG_CLOCK_TEX_W,
                COG_CLOCK_TEX_H, &ctx->cogClockReady);
}

// Keyboard sheets (KB_STATE_ order), its button, and the layout: key rects plus a code
// table per state. Java owns drawing and layout; native keeps only rects and codes.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadKeyboard(JNIEnv* env, jobject thiz,
                                                                  jlong handle, jobjectArray sheets,
                                                                  jobject buttonIcon,
                                                                  jfloatArray keyRects,
                                                                  jobjectArray codes) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }

    int sheetCount = sheets != NULL ? (*env)->GetArrayLength(env, sheets) : 0;
    int shownAgain = 0;
    for (int state = 0; state < KB_STATE_COUNT && state < sheetCount; state++) {
        jobject sheet = (*env)->GetObjectArrayElement(env, sheets, state);
        if (keepSheet(env, sheet, &ctx->kbPanelPixels[state], KB_TEX_W, KB_TEX_H)) {
            ctx->kbPanelReady[state] = ctx->kbPanelSwapchain != XR_NULL_HANDLE;
            shownAgain |= state == ctx->kbStateShown;
        }
        if (sheet != NULL) {
            (*env)->DeleteLocalRef(env, sheet);
        }
    }
    if (shownAgain) {
        int state = ctx->kbStateShown;
        ctx->kbStateShown = -1;
        showKbSheet(ctx, state);
    }
    uploadButton(env, ctx, buttonIcon, BTN_CELL_KB, &ctx->kbButtonReady);

    if (keyRects != NULL && codes != NULL
            && (*env)->GetArrayLength(env, codes) >= KB_STATE_COUNT) {
        jintArray tables[KB_STATE_COUNT];
        int count = (*env)->GetArrayLength(env, keyRects) / 4;
        int ok = 1;
        for (int state = 0; state < KB_STATE_COUNT; state++) {
            tables[state] = (jintArray)(*env)->GetObjectArrayElement(env, codes, state);
            if (tables[state] == NULL) {
                ok = 0;
                continue;
            }
            int length = (*env)->GetArrayLength(env, tables[state]);
            if (length < count) {
                count = length;
            }
        }
        if (count > KB_MAX_KEYS) {
            LOGW("keyboard layout has %d keys, keeping the first %d", count, KB_MAX_KEYS);
            count = KB_MAX_KEYS;
        }
        if (ok) {
            (*env)->GetFloatArrayRegion(env, keyRects, 0, count * 4, ctx->kbKeyRects);
            for (int state = 0; state < KB_STATE_COUNT; state++) {
                (*env)->GetIntArrayRegion(env, tables[state], 0, count, ctx->kbCodes[state]);
            }
            ctx->kbKeyCount = count;
        }
        for (int state = 0; state < KB_STATE_COUNT; state++) {
            if (tables[state] != NULL) {
                (*env)->DeleteLocalRef(env, tables[state]);
            }
        }
    }

    int ready = 0;
    for (int state = 0; state < KB_STATE_COUNT; state++) {
        ready += ctx->kbPanelReady[state] ? 1 : 0;
    }
    LOGI("keyboard art %d of %d sheets ready, button %s, %d keys", ready, KB_STATE_COUNT,
         ctx->kbButtonReady ? "ready" : "missing", ctx->kbKeyCount);
}

// Redraw of the visible keyboard sheet with current modifiers lit; uploaded at once.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadKeyboardSheet(JNIEnv* env, jobject thiz,
                                                                       jlong handle, jint state,
                                                                       jobject sheet) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || sheet == NULL || state < 0 || state >= KB_STATE_COUNT) {
        return;
    }
    if (keepSheet(env, sheet, &ctx->kbPanelPixels[state], KB_TEX_W, KB_TEX_H)) {
        ctx->kbPanelReady[state] = ctx->kbPanelSwapchain != XR_NULL_HANDLE;
        if (state == ctx->kbStateShown) {
            ctx->kbStateShown = -1;
            showKbSheet(ctx, state);
        }
    }
}

// Exit button and prompt sheets, one per highlighted zone, stored for cheap switching
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadExit(JNIEnv* env, jobject thiz,
                                                              jlong handle, jobject button,
                                                              jobject promptPlain,
                                                              jobject promptExitHot,
                                                              jobject promptCancelHot) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    uploadButton(env, ctx, button, BTN_CELL_EXIT, &ctx->exitButtonReady);

    jobject sheets[EXIT_ART_COUNT] = { promptPlain, promptExitHot, promptCancelHot };
    int shownAgain = 0;
    for (int sheet = 0; sheet < EXIT_ART_COUNT; sheet++) {
        if (keepSheet(env, sheets[sheet], &ctx->exitPromptPixels[sheet], EXIT_TEX_W,
                      EXIT_TEX_H)) {
            ctx->exitPromptReady[sheet] = ctx->exitPromptSwapchain != XR_NULL_HANDLE;
            shownAgain |= sheet == ctx->exitArtShown;
        }
    }
    if (shownAgain) {
        int sheet = ctx->exitArtShown;
        ctx->exitArtShown = -1;
        showExitSheet(ctx, sheet);
    }

    // Last panel art to arrive; the splash stops waiting here
    ctx->panelArtArrived = 1;
    LOGI("exit button %s, prompt %s, %s and %s",
         ctx->exitButtonReady ? "ready" : "missing",
         ctx->exitPromptReady[EXIT_ZONE_NONE] ? "ready" : "missing",
         ctx->exitPromptReady[EXIT_ZONE_EXIT] ? "ready" : "missing",
         ctx->exitPromptReady[EXIT_ZONE_CANCEL] ? "ready" : "missing");
}

// Report sheet redraw with current field contents; ignored unless the sheet is open.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadReport(JNIEnv* env, jobject thiz,
                                                               jlong handle, jobject sheet) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || sheet == NULL || !ctx->reportOpen) {
        return;
    }
    uploadSheet(env, ctx, sheet, ctx->reportSwapchain, ctx->reportImages, REPORT_TEX_W,
                REPORT_TEX_H, &ctx->reportReady);
}

// Whether the note and address are valid, enabling Send. Frame loop.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetReportSend(JNIEnv* env, jobject thiz,
                                                                jlong handle, jboolean ready) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx != NULL) {
        ctx->reportSendReady = ready;
    }
}

// Launch splash sheet; uploaded before the first frame.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadSplash(JNIEnv* env, jobject thiz,
                                                               jlong handle, jobject sheet) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    uploadSheet(env, ctx, sheet, ctx->splashSwapchain, ctx->splashImages, SPLASH_TEX_W,
                SPLASH_TEX_H, &ctx->splashArtReady);
    LOGI("splash art %s", ctx->splashArtReady ? "ready" : "missing");
}

// Toast text for the current notice; shown only while that notice is still up.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadToast(JNIEnv* env, jobject thiz,
                                                              jlong handle, jobject sheet,
                                                              jint kind, jint arg) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    uploadSheet(env, ctx, sheet, ctx->toastSwapchain, ctx->toastImages, TOAST_TEX_W,
                TOAST_TEX_H, &ctx->toastArtReady);
    ctx->toastDrawnKind = kind;
    ctx->toastDrawnArg = arg;
    LOGEV("toast up: kind %d, %d", kind, arg);
}

// Notice whose text lives in Java. Frame loop only.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativePushNotice(JNIEnv* env, jobject thiz,
                                                             jlong handle, jint kind, jint arg) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx != NULL) {
        noticePush(&ctx->notices, kind, arg);
    }
}

// 3D switch faces. Refused without stereo, so the switch is never ready there.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadStereoButton(JNIEnv* env, jobject thiz,
                                                                     jlong handle, jobject off,
                                                                     jobject on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || off == NULL || on == NULL || ctx->stereoMode == DEPTH_MODE_OFF) {
        return;
    }
    ctx->stereoButtonReady = uploadSwitch(env, ctx, off, on, BTN_CELL_STEREO);
    LOGI("3d button art %s", ctx->stereoButtonReady ? "ready" : "missing");
}

// Ray switch faces
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRayButton(JNIEnv* env, jobject thiz,
                                                                  jlong handle, jobject off,
                                                                  jobject on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || off == NULL || on == NULL) {
        return;
    }
    ctx->rayButtonReady = uploadSwitch(env, ctx, off, on, BTN_CELL_RAY);
    LOGI("ray button art %s", ctx->rayButtonReady ? "ready" : "missing");
}

// Head aim switch faces
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadAimButton(JNIEnv* env, jobject thiz,
                                                                  jlong handle, jobject off,
                                                                  jobject on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || off == NULL || on == NULL) {
        return;
    }
    ctx->aimButtonReady = uploadSwitch(env, ctx, off, on, BTN_CELL_AIM);
    LOGI("head aim button art %s", ctx->aimButtonReady ? "ready" : "missing");
}

// Gamepad switch faces (pointer, gamepad)
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadPadButton(JNIEnv* env, jobject thiz,
                                                                  jlong handle, jobject off,
                                                                  jobject on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || off == NULL || on == NULL) {
        return;
    }
    ctx->padButtonReady = uploadSwitch(env, ctx, off, on, BTN_CELL_PAD);
    LOGI("gamepad button art %s", ctx->padButtonReady ? "ready" : "missing");
}

// Hand-lock hint sheet. The hint waits for it, since an invisible modal would swallow
// presses.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadHandHint(JNIEnv* env, jobject thiz,
                                                                 jlong handle, jobject sheet) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    uploadSheet(env, ctx, sheet, ctx->hintSwapchain, ctx->hintImages, HINT_TEX_W, HINT_TEX_H,
                &ctx->hintReady);
    LOGI("hand lock hint art %s", ctx->hintReady ? "ready" : "missing");
}

// Room style for a picker cell; no cell maps to a room, so always 0.
int roomStyleForCell(int cell) {
    (void)cell;
    return 0;
}

// Picker cell for a room style, or -1
int roomCellForStyle(int style) {
    for (int cell = 0; cell < ENV_CELL_COUNT; cell++) {
        if (style > 0 && roomStyleForCell(cell) == style) {
            return cell;
        }
    }
    return -1;
}

// Persisted picker selection
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetEnvironment(JNIEnv* env, jobject thiz,
                                                                 jlong handle, jint choice) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->pickerChoice = choice;
    ctx->roomStyle = roomStyleForCell(choice);
    if (choice != ctx->loggedChoice) {
        ctx->loggedChoice = choice;
        LOGEV("environment %d, room %d", choice, roomEffective(ctx));
    }
}
