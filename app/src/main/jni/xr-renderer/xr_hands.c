// Hand outlines: the runtime's mesh of each hand, skinned to the tracked joints and
// drawn as a glowing rim (with a faint fill in the dark) in the controller models'
// layer, so the hands read as solid shapes in front of the picture.
#include "xr_renderer.h"
#include "xr_shaders.h"

static void loadHandMesh(XrCtx* ctx, int h) {
    XrHandTrackingMeshFB mesh = { XR_TYPE_HAND_TRACKING_MESH_FB };
    if (XR_FAILED(ctx->pfnGetHandMesh(ctx->handTrackers[h], &mesh))
            || mesh.jointCountOutput != XR_HAND_JOINT_COUNT_EXT || mesh.vertexCountOutput == 0
            || mesh.indexCountOutput == 0) {
        LOGW("hand %d mesh unavailable (%u joints)", h, mesh.jointCountOutput);
        return;
    }
    uint32_t nv = mesh.vertexCountOutput;
    uint32_t ni = mesh.indexCountOutput;
    float radii[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointEXT parents[XR_HAND_JOINT_COUNT_EXT];
    XrVector3f* positions = malloc(nv * sizeof(XrVector3f));
    XrVector3f* normals = malloc(nv * sizeof(XrVector3f));
    XrVector2f* uvs = malloc(nv * sizeof(XrVector2f));
    XrVector4sFB* joints = malloc(nv * sizeof(XrVector4sFB));
    XrVector4f* weights = malloc(nv * sizeof(XrVector4f));
    int16_t* indices = malloc(ni * sizeof(int16_t));
    float* vertices = malloc((size_t)nv * HAND_VERTEX_FLOATS * sizeof(float));
    int ok = positions && normals && uvs && joints && weights && indices && vertices;
    if (ok) {
        mesh.jointCapacityInput = XR_HAND_JOINT_COUNT_EXT;
        mesh.jointBindPoses = ctx->handMesh[h].bind;
        mesh.jointRadii = radii;
        mesh.jointParents = parents;
        mesh.vertexCapacityInput = nv;
        mesh.vertexPositions = positions;
        mesh.vertexNormals = normals;
        mesh.vertexUVs = uvs;
        mesh.vertexBlendIndices = joints;
        mesh.vertexBlendWeights = weights;
        mesh.indexCapacityInput = ni;
        mesh.indices = indices;
        ok = XR_SUCCEEDED(ctx->pfnGetHandMesh(ctx->handTrackers[h], &mesh));
    }
    if (ok) {
        for (uint32_t i = 0; i < nv; i++) {
            float* v = vertices + (size_t)i * HAND_VERTEX_FLOATS;
            v[0] = positions[i].x;
            v[1] = positions[i].y;
            v[2] = positions[i].z;
            v[3] = normals[i].x;
            v[4] = normals[i].y;
            v[5] = normals[i].z;
            v[6] = joints[i].x;
            v[7] = joints[i].y;
            v[8] = joints[i].z;
            v[9] = joints[i].w;
            v[10] = weights[i].x;
            v[11] = weights[i].y;
            v[12] = weights[i].z;
            v[13] = weights[i].w;
        }
        ctx->handMesh[h].vertices = vertices;
        ctx->handMesh[h].indices = (uint16_t*)indices;
        ctx->handMesh[h].vertexCount = (int)nv;
        ctx->handMesh[h].indexCount = (int)ni;
        LOGI("hand %d mesh: %u vertices, %u triangles", h, nv, ni / 3);
    }
    else {
        LOGW("hand %d mesh could not be read", h);
        free(vertices);
        free(indices);
    }
    free(positions);
    free(normals);
    free(uvs);
    free(joints);
    free(weights);
}

void loadHandMeshes(XrCtx* ctx) {
    if (!ctx->handMeshOffered
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrGetHandMeshFB",
                                               (PFN_xrVoidFunction*)&ctx->pfnGetHandMesh))
            || ctx->pfnGetHandMesh == NULL) {
        return;
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->handTrackers[h] != XR_NULL_HANDLE && ctx->handMesh[h].vertexCount == 0) {
            loadHandMesh(ctx, h);
        }
    }
}

// Which hands to draw this frame, and how strongly: tracked well, or just lost and
// held at their last good pose while fading
void updateHands(XrCtx* ctx) {
    int64_t now = nowNs();
    float dt = ctx->handStepNs > 0 ? (float)(now - ctx->handStepNs) / 1e9f : 0.0f;
    ctx->handStepNs = now;
    if (dt > 0.1f) {
        dt = 0.1f;
    }
    int live = ctx->shouldRender && ctx->sessionState == XR_SESSION_STATE_FOCUSED
            && ctx->splash.phase == SPLASH_GONE && !ctx->handPassFailed;
    int said = 0;
    ctx->handsShowing = 0;
    for (int h = 0; h < HAND_COUNT; h++) {
        int tracked = ctx->handMesh[h].vertexCount > 0 && ctx->handJointsOk[h]
                && ctx->profileKind[h] != PROFILE_CONTROLLER;
        if (tracked) {
            memcpy(ctx->handJointsLast[h], ctx->handJoints[h], sizeof(ctx->handJointsLast[h]));
            for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
                ctx->handJointsLast[h][j].position.x += ctx->pokePush[h].x;
                ctx->handJointsLast[h][j].position.y += ctx->pokePush[h].y;
                ctx->handJointsLast[h][j].position.z += ctx->pokePush[h].z;
            }
            ctx->handLastValid[h] = 1;
            ctx->handLostFor[h] = 0.0f;
        }
        else {
            ctx->handLostFor[h] += dt;
        }
        // Picked up a controller: no hold
        if (ctx->profileKind[h] == PROFILE_CONTROLLER) {
            ctx->handLevel[h] = 0.0f;
        }
        ctx->handLevel[h] = handFadeStep(ctx->handLevel[h], tracked, ctx->handLostFor[h], dt);
        ctx->handShown[h] = live && ctx->handLastValid[h] && ctx->handLevel[h] > 0.0f;
        ctx->handsShowing |= ctx->handShown[h];
        said |= ctx->handShown[h] << h;
    }
    if (said != ctx->handsSaid) {
        ctx->handsSaid = said;
        LOGI("hand outlines: left %s, right %s", (said & 1) ? "drawn" : "not drawn",
             (said & 2) ? "drawn" : "not drawn");
    }
}

static int initHandPass(XrCtx* ctx) {
    if (ctx->handProgram != 0) {
        return 1;
    }
    if (ctx->handPassFailed) {
        return 0;
    }
    ctx->handPassFailed = 1;
    GLuint vs = compileShader(GL_VERTEX_SHADER, HAND_VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, HAND_FRAGMENT_SRC);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_normal");
    glBindAttribLocation(program, 2, "a_joints");
    glBindAttribLocation(program, 3, "a_weights");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOGE("hand outline program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    ctx->handProgram = program;
    ctx->handViewProjUniform = glGetUniformLocation(program, "u_viewproj");
    ctx->handJointsUniform = glGetUniformLocation(program, "u_joints");
    ctx->handEyeUniform = glGetUniformLocation(program, "u_eye");
    ctx->handLookUniform = glGetUniformLocation(program, "u_look");
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->handMesh[h].vertexCount == 0) {
            continue;
        }
        glGenBuffers(1, &ctx->handMesh[h].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, ctx->handMesh[h].vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)ctx->handMesh[h].vertexCount * HAND_VERTEX_FLOATS * sizeof(float),
                     ctx->handMesh[h].vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &ctx->handMesh[h].ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->handMesh[h].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)ctx->handMesh[h].indexCount * sizeof(uint16_t),
                     ctx->handMesh[h].indices, GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    ctx->handPassFailed = 0;
    return 1;
}

// Into the bound controller model framebuffer, both eyes, after the controllers.
// Leaves attributes 2 and 3 off and no buffers bound.
void drawHandOutlines(XrCtx* ctx) {
    if (!ctx->handsShowing || !initHandPass(ctx)) {
        return;
    }
    float skins[HAND_COUNT][XR_HAND_JOINT_COUNT_EXT * 16];
    for (int h = 0; h < HAND_COUNT; h++) {
        if (!ctx->handShown[h]) {
            continue;
        }
        for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
            float size = ctx->handScale[h] > 0.0f ? ctx->handScale[h] : 1.0f;
            handSkinMatrix(&skins[h][j * 16], ctx->handJointsLast[h][j], ctx->handMesh[h].bind[j],
                           size);
        }
    }
    glUseProgram(ctx->handProgram);
    float fill = ctx->passthrough ? 0.0f : HAND_FILL_DARK;
    GLsizei stride = HAND_VERTEX_FLOATS * sizeof(float);
    for (int eye = 0; eye < MODEL_EYES; eye++) {
        glViewport(eye * ctx->modelEyeWidth, 0, ctx->modelEyeWidth, ctx->modelEyeHeight);
        float proj[16];
        float view[16];
        float viewProj[16];
        projectionFromFov(proj, ctx->modelViews[eye].fov, MODEL_NEAR_M, MODEL_FAR_M);
        viewFromPose(view, ctx->modelViews[eye].pose);
        matMul(viewProj, proj, view);
        glUniformMatrix4fv(ctx->handViewProjUniform, 1, GL_FALSE, viewProj);
        XrVector3f eyeAt = ctx->modelViews[eye].pose.position;
        glUniform3f(ctx->handEyeUniform, eyeAt.x, eyeAt.y, eyeAt.z);
        for (int h = 0; h < HAND_COUNT; h++) {
            if (!ctx->handShown[h]) {
                continue;
            }
            glUniformMatrix4fv(ctx->handJointsUniform, XR_HAND_JOINT_COUNT_EXT, GL_FALSE, skins[h]);
            glUniform2f(ctx->handLookUniform, fill * ctx->handLevel[h], HAND_RIM * ctx->handLevel[h]);
            glBindBuffer(GL_ARRAY_BUFFER, ctx->handMesh[h].vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->handMesh[h].ibo);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(10 * sizeof(float)));
            glEnableVertexAttribArray(3);
            glDrawElements(GL_TRIANGLES, (GLsizei)ctx->handMesh[h].indexCount, GL_UNSIGNED_SHORT,
                           (const void*)0);
        }
    }
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void destroyHands(XrCtx* ctx) {
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->handMesh[h].vbo != 0) {
            glDeleteBuffers(1, &ctx->handMesh[h].vbo);
            glDeleteBuffers(1, &ctx->handMesh[h].ibo);
        }
        free(ctx->handMesh[h].vertices);
        free(ctx->handMesh[h].indices);
        memset(&ctx->handMesh[h], 0, sizeof(ctx->handMesh[h]));
        ctx->handShown[h] = 0;
        ctx->handLastValid[h] = 0;
        ctx->handLevel[h] = 0.0f;
    }
    if (ctx->handProgram != 0) {
        glDeleteProgram(ctx->handProgram);
    }
    ctx->handProgram = 0;
    ctx->handsShowing = 0;
}
