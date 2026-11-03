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

    int ok = uploadArt(ctx, ctx->barSwapchain, ctx->barImages, bar, BAR_TEX_W, BAR_TEX_H);
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
    int was = ctx->roomStyle;
    ctx->roomStyle = roomStyleForCell(choice);
    if (ctx->roomStyle != was || !ctx->environmentSaid) {
        ctx->environmentSaid = 1;
        LOGEV("environment %d, room %d", choice, roomEffective(ctx));
    }
}
