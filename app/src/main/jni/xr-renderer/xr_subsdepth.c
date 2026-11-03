// Subtitle depth on a 3D file: a small block matcher over the strip along the
// bottom of the picture, read back a frame late. The maths is in xr_depthmap.c.
#include "xr_renderer.h"
#include "xr_shaders.h"

int initStripMatch(XrCtx* ctx) {
    ctx->stripReady = 0;
    if (!linkProgram(&ctx->stripProgram, STRIP_MATCH_FRAGMENT_SRC, "subtitle strip match")) {
        return 0;
    }
    ctx->stripTexMatrixUniform = glGetUniformLocation(ctx->stripProgram, "u_texmatrix");
    ctx->stripLeftUniform = glGetUniformLocation(ctx->stripProgram, "u_left");
    ctx->stripRightUniform = glGetUniformLocation(ctx->stripProgram, "u_right");
    ctx->stripStripUniform = glGetUniformLocation(ctx->stripProgram, "u_strip");
    ctx->stripReachUniform = glGetUniformLocation(ctx->stripProgram, "u_reach");
    glUseProgram(ctx->stripProgram);
    glUniform1i(glGetUniformLocation(ctx->stripProgram, "u_texture"), 0);

    glGenTextures(1, &ctx->stripTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->stripTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, STRIP_CELLS_X, STRIP_CELLS_Y, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &ctx->stripFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->stripFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->stripTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("subtitle strip framebuffer incomplete: 0x%x", status);
        return 0;
    }
    glGenBuffers(2, ctx->stripPbo);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->stripPbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, STRIP_CELLS_X * STRIP_CELLS_Y * 4, NULL,
                     GL_STREAM_READ);
        ctx->stripPboFull[i] = 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    ctx->stripReady = 1;
    return 1;
}

// Queues this frame's match and reads the previous one's
void runStripMatch(XrCtx* ctx, const float* texMatrix) {
    if (!ctx->stripReady) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->stripFbo);
    glViewport(0, 0, STRIP_CELLS_X, STRIP_CELLS_Y);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }
    glUseProgram(ctx->stripProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->stripTexMatrixUniform, 1, GL_FALSE, texMatrix);
    float rect[4];
    contentRect(ctx, 0, rect);
    glUniform4fv(ctx->stripLeftUniform, 1, rect);
    contentRect(ctx, 1, rect);
    glUniform4fv(ctx->stripRightUniform, 1, rect);
    glUniform1f(ctx->stripStripUniform, SUBS_STRIP_SHARE);
    glUniform1f(ctx->stripReachUniform, STRIP_REACH);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    const int bytes = STRIP_CELLS_X * STRIP_CELLS_Y * 4;
    int slot = ctx->stripPboSlot;
    int other = 1 - slot;
    if (ctx->stripPboFull[other]) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->stripPbo[other]);
        const unsigned char* px = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (px != NULL) {
            ctx->subsDisparity = stripDisparity(px, STRIP_CELLS_X * STRIP_CELLS_Y, STRIP_REACH);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        ctx->stripPboFull[other] = 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->stripPbo[slot]);
    glReadPixels(0, 0, STRIP_CELLS_X, STRIP_CELLS_Y, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    ctx->stripPboFull[slot] = 1;
    ctx->stripPboSlot = other;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void destroyStripMatch(XrCtx* ctx) {
    if (ctx->stripFbo != 0) {
        glDeleteFramebuffers(1, &ctx->stripFbo);
        glDeleteTextures(1, &ctx->stripTexture);
        glDeleteBuffers(2, ctx->stripPbo);
    }
    if (ctx->stripProgram != 0) {
        glDeleteProgram(ctx->stripProgram);
    }
    ctx->stripFbo = 0;
    ctx->stripTexture = 0;
    ctx->stripProgram = 0;
    ctx->stripReady = 0;
}
