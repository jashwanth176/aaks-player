// Frosted backdrops: the picture is blurred into a small texture each video frame,
// and each display frame the panels over it get a backdrop layer of the part they
// cover, seen from the eye, masked to the panel's own shape.
#include "xr_renderer.h"
#include "xr_shaders.h"

// Dim, veil alpha beyond the picture, lift: the sheet keeps a dark veil over the room
// for legibility; the controls are plain glass there
static const float FROST_LOOK[FROST_PANELS][4] = {
    { 0.8f, 0.3f, 0.03f, 0.0f },
    { 0.85f, 0.0f, 0.03f, 0.0f },
};

int frostSlot(int id) {
    return id == UI_PANEL_SHEET ? 0 : id == UI_PANEL_CONTROLS ? 1 : -1;
}

int initFrost(XrCtx* ctx) {
    ctx->frostReady = 0;
    if (!linkProgram(&ctx->frostProgram, FROST_FRAGMENT_SRC, "frosted backdrop")) {
        return 0;
    }
    ctx->frostRectUniform = glGetUniformLocation(ctx->frostProgram, "u_rect");
    ctx->frostTexelUniform = glGetUniformLocation(ctx->frostProgram, "u_texel");
    ctx->frostFilmOnUniform = glGetUniformLocation(ctx->frostProgram, "u_filmOn");
    ctx->frostLookUniform = glGetUniformLocation(ctx->frostProgram, "u_look");
    glUseProgram(ctx->frostProgram);
    glUniform1i(glGetUniformLocation(ctx->frostProgram, "u_film"), 0);
    glUniform1i(glGetUniformLocation(ctx->frostProgram, "u_mask"), 1);

    glGenTextures(1, &ctx->frostTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->frostTexture);
    glTexStorage2D(GL_TEXTURE_2D, 3, GL_RGBA8, FROST_TEX_W, FROST_TEX_H);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &ctx->frostFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->frostFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->frostTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("frost framebuffer incomplete: 0x%x", status);
        return 0;
    }
    glGenFramebuffers(1, &ctx->frostDrawFbo);
    ctx->frostReady = 1;
    return 1;
}

// The picture, one eye with the bars cropped and graded like the screen, through the
// colour sample program; its box filter and the mips do the blurring
void runFrostSample(XrCtx* ctx, const float* texMatrix) {
    if (!ctx->frostReady) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->frostFbo);
    glViewport(0, 0, FROST_TEX_W, FROST_TEX_H);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }
    glUseProgram(ctx->ambiProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->ambiTexMatrixUniform, 1, GL_FALSE, texMatrix);
    glUniform4fv(ctx->ambiCropUniform, 1, ctx->ambiCrop);
    setGradeUniforms(ctx, ctx->ambiGradeOnUniform, ctx->ambiGradeUniform, ctx->gradeOn);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, ctx->frostTexture);
    glGenerateMipmap(GL_TEXTURE_2D);
    ctx->frostFilmValid = 1;
}

// The panel's shape at a quarter size, rows bottom up: a cell is backdrop only when
// every pixel in it is, so the backdrop never shows past the panel's edge
void frostMaskFromPanel(XrCtx* ctx, int id, const unsigned char* px, int width, int height) {
    int slot = frostSlot(id);
    if (slot < 0 || !ctx->frostReady) {
        return;
    }
    int mw = (width + FROST_MASK_DIV - 1) / FROST_MASK_DIV;
    int mh = (height + FROST_MASK_DIV - 1) / FROST_MASK_DIV;
    unsigned char* mask = malloc((size_t)mw * mh);
    if (mask == NULL) {
        return;
    }
    for (int by = 0; by < mh; by++) {
        int y1 = by * FROST_MASK_DIV + FROST_MASK_DIV;
        if (y1 > height) y1 = height;
        for (int bx = 0; bx < mw; bx++) {
            int x1 = bx * FROST_MASK_DIV + FROST_MASK_DIV;
            if (x1 > width) x1 = width;
            unsigned char v = 255;
            for (int y = by * FROST_MASK_DIV; y < y1 && v; y++) {
                const unsigned char* row = px + ((size_t)y * width) * 4;
                for (int x = bx * FROST_MASK_DIV; x < x1; x++) {
                    if (row[x * 4 + 3] < FROST_MASK_ALPHA) {
                        v = 0;
                        break;
                    }
                }
            }
            mask[(size_t)(mh - 1 - by) * mw + bx] = v;
        }
    }

    if (ctx->frost[slot].chain == XR_NULL_HANDLE || ctx->frost[slot].w != mw
            || ctx->frost[slot].h != mh) {
        destroyArtSwapchain(ctx, &ctx->frost[slot].chain, &ctx->frost[slot].images);
        ctx->frost[slot].ready = 0;
        if (!createArtSwapchain(ctx, mw, mh, "create frost swapchain", &ctx->frost[slot].chain,
                                &ctx->frost[slot].images, &ctx->frost[slot].imageCount)) {
            ctx->frost[slot].w = 0;
            ctx->frost[slot].h = 0;
            free(mask);
            return;
        }
        ctx->frost[slot].w = mw;
        ctx->frost[slot].h = mh;
    }
    if (ctx->frost[slot].mask == 0) {
        glGenTextures(1, &ctx->frost[slot].mask);
    }
    glBindTexture(GL_TEXTURE_2D, ctx->frost[slot].mask);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, mw, mh, 0, GL_RED, GL_UNSIGNED_BYTE, mask);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(mask);
    ctx->frost[slot].ready = 1;
}

// Screen-local point: x right, y up, z toward the viewer
static Vec3 screenLocal(const XrCtx* ctx, XrVector3f p) {
    Vec3 d = { p.x - ctx->screenPose.position.x, p.y - ctx->screenPose.position.y,
               p.z - ctx->screenPose.position.z };
    return quatRotate(quatConj(ctx->screenPose.orientation), d);
}

// Draws a panel's backdrop: the picture under the panel as the eye sees it, blurred.
// Frame loop. Returns 0 when there is nothing to draw.
int renderFrostBackdrop(XrCtx* ctx, int slot, XrPosef pose, float width, float height) {
    if (!ctx->frostReady || !ctx->frost[slot].ready || ctx->screenWidth <= 0.0f) {
        return 0;
    }
    // The panel is parallel to the screen, so its footprint from the eye is a rectangle
    Vec3 eye = screenLocal(ctx, ctx->headPos);
    Vec3 c = screenLocal(ctx, pose.position);
    float s = eye.z > c.z + 0.01f ? eye.z / (eye.z - c.z) : 1.0f;
    float screenW = ctx->screenWidth;
    float screenH = screenW * screenAspect(ctx);
    float cx = eye.x + (c.x - eye.x) * s;
    float cy = eye.y + (c.y - eye.y) * s;
    float rect[4] = {
        (cx - 0.5f * width * s) / screenW + 0.5f,
        (cy - 0.5f * height * s) / screenH + 0.5f,
        width * s / screenW,
        height * s / screenH,
    };
    int filmOn = ctx->frostFilmValid && !ctx->playerScreenHidden && ctx->videoReady;

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->frost[slot].chain, &acquire, &index),
                 "acquire frost image")) {
        return 0;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->frost[slot].chain, &wait);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->frostDrawFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->frost[slot].images[index].image, 0);
    glViewport(0, 0, ctx->frost[slot].w, ctx->frost[slot].h);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }
    glUseProgram(ctx->frostProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->frostTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->frost[slot].mask);
    glActiveTexture(GL_TEXTURE0);
    glUniform4fv(ctx->frostRectUniform, 1, rect);
    glUniform2f(ctx->frostTexelUniform, 1.0f / FROST_TEX_W, 1.0f / FROST_TEX_H);
    glUniform1f(ctx->frostFilmOnUniform, filmOn ? 1.0f : 0.0f);
    glUniform4fv(ctx->frostLookUniform, 1, FROST_LOOK[slot]);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->frost[slot].chain, &release);
    return 1;
}

void destroyFrost(XrCtx* ctx) {
    for (int i = 0; i < FROST_PANELS; i++) {
        destroyArtSwapchain(ctx, &ctx->frost[i].chain, &ctx->frost[i].images);
        if (ctx->frost[i].mask != 0) {
            glDeleteTextures(1, &ctx->frost[i].mask);
        }
        ctx->frost[i].mask = 0;
        ctx->frost[i].w = 0;
        ctx->frost[i].h = 0;
        ctx->frost[i].ready = 0;
    }
    if (ctx->frostFbo != 0) {
        glDeleteFramebuffers(1, &ctx->frostFbo);
        glDeleteFramebuffers(1, &ctx->frostDrawFbo);
        glDeleteTextures(1, &ctx->frostTexture);
    }
    if (ctx->frostProgram != 0) {
        glDeleteProgram(ctx->frostProgram);
    }
    ctx->frostFbo = 0;
    ctx->frostDrawFbo = 0;
    ctx->frostTexture = 0;
    ctx->frostProgram = 0;
    ctx->frostReady = 0;
    ctx->frostFilmValid = 0;
}
