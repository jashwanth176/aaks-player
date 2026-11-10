// Hand outlines: skinning maths.
#include <math.h>
#include <stddef.h>

#include "xr_handmesh.h"

float handFadeStep(float level, int tracked, float lostFor, float dt) {
    if (tracked) {
        return fminf(1.0f, level + dt / HAND_FADE_IN_SEC);
    }
    if (lostFor < HAND_HOLD_SEC) {
        return level;
    }
    return fmaxf(0.0f, level - dt / HAND_FADE_OUT_SEC);
}

void matFromPose(float* m, XrPosef pose) {
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;
    m[0] = 1.0f - 2.0f * (y * y + z * z);
    m[1] = 2.0f * (x * y + w * z);
    m[2] = 2.0f * (x * z - w * y);
    m[3] = 0.0f;
    m[4] = 2.0f * (x * y - w * z);
    m[5] = 1.0f - 2.0f * (x * x + z * z);
    m[6] = 2.0f * (y * z + w * x);
    m[7] = 0.0f;
    m[8] = 2.0f * (x * z + w * y);
    m[9] = 2.0f * (y * z - w * x);
    m[10] = 1.0f - 2.0f * (x * x + y * y);
    m[11] = 0.0f;
    m[12] = pose.position.x;
    m[13] = pose.position.y;
    m[14] = pose.position.z;
    m[15] = 1.0f;
}

void handSkinMatrix(float* out, XrPosef joint, XrPosef bind, float scale) {
    float now[16];
    float unbind[16];
    float moved[16];
    matFromPose(now, joint);
    // The mesh grows about its origin, the bind pose with it; then the inverse of the
    // bind pose, as a view matrix is of a camera's
    bind.position.x *= scale;
    bind.position.y *= scale;
    bind.position.z *= scale;
    viewFromPose(unbind, bind);
    matMul(moved, now, unbind);
    float grow[16] = { scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, 1 };
    matMul(out, moved, grow);
}

void handJointReach(float* reach, int jointCount, const float* vertices, int vertexCount,
                    const XrPosef* bind) {
    for (int j = 0; j < jointCount; j++) {
        reach[j] = 0.0f;
    }
    for (int i = 0; i < vertexCount; i++) {
        const float* v = vertices + (size_t)i * HAND_VERTEX_FLOATS;
        for (int k = 0; k < 4; k++) {
            int j = (int)v[6 + k];
            if (v[10 + k] <= 0.0f || j < 0 || j >= jointCount) {
                continue;
            }
            float dx = v[0] - bind[j].position.x;
            float dy = v[1] - bind[j].position.y;
            float dz = v[2] - bind[j].position.z;
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > reach[j]) {
                reach[j] = d;
            }
        }
    }
}

void viewBoxClear(ViewBox* box) {
    box->left = box->right = box->down = box->up = 0.0f;
    box->width = box->height = 0;
    box->count = 0;
    box->depthSum = 0.0f;
}

void viewBoxAdd(ViewBox* box, XrPosef eye, XrVector3f centre, float radius) {
    Vec3 d = { centre.x - eye.position.x, centre.y - eye.position.y,
               centre.z - eye.position.z };
    Vec3 p = quatRotate(quatConj(eye.orientation), d);
    float depth = -p.z;
    if (depth + radius < VIEW_BOX_NEAR_M) {
        return;
    }
    // The sphere's widest tangents: its near side for an edge going outward, its far
    // side for one still short of the axis
    float nearZ = fmaxf(depth - radius, VIEW_BOX_NEAR_M);
    float farZ = fmaxf(depth + radius, VIEW_BOX_NEAR_M);
    float l = (p.x - radius) / (p.x - radius < 0.0f ? nearZ : farZ);
    float r = (p.x + radius) / (p.x + radius > 0.0f ? nearZ : farZ);
    float dn = (p.y - radius) / (p.y - radius < 0.0f ? nearZ : farZ);
    float u = (p.y + radius) / (p.y + radius > 0.0f ? nearZ : farZ);
    if (box->count == 0 || l < box->left) box->left = l;
    if (box->count == 0 || r > box->right) box->right = r;
    if (box->count == 0 || dn < box->down) box->down = dn;
    if (box->count == 0 || u > box->up) box->up = u;
    box->count++;
    box->depthSum += fmaxf(depth, VIEW_BOX_NEAR_M);
}

int viewBoxFit(ViewBox* box, XrFovf fov, int eyeWidth, int eyeHeight, int maxSide) {
    float fl = tanf(fov.angleLeft);
    float fr = tanf(fov.angleRight);
    float fd = tanf(fov.angleDown);
    float fu = tanf(fov.angleUp);
    if (box->count == 0 || fr <= fl || fu <= fd || eyeWidth <= 0 || eyeHeight <= 0) {
        return 0;
    }
    float l = fmaxf(box->left, fl);
    float r = fminf(box->right, fr);
    float d = fmaxf(box->down, fd);
    float u = fminf(box->up, fu);
    if (l >= r || d >= u) {
        return 0;
    }
    // Pixels per unit of tangent in the eye's own image
    float perX = (float)eyeWidth / (fr - fl);
    float perY = (float)eyeHeight / (fu - fd);
    float stepX = (float)VIEW_BOX_SNAP_PX / perX;
    float stepY = (float)VIEW_BOX_SNAP_PX / perY;
    box->left = floorf(l / stepX) * stepX;
    box->right = ceilf(r / stepX) * stepX;
    box->down = floorf(d / stepY) * stepY;
    box->up = ceilf(u / stepY) * stepY;
    int w = (int)lroundf((box->right - box->left) * perX);
    int h = (int)lroundf((box->up - box->down) * perY);
    box->width = w > maxSide ? maxSide : w;
    box->height = h > maxSide ? maxSide : h;
    return 1;
}

XrFovf viewBoxFov(const ViewBox* box) {
    XrFovf fov;
    fov.angleLeft = atanf(box->left);
    fov.angleRight = atanf(box->right);
    fov.angleDown = atanf(box->down);
    fov.angleUp = atanf(box->up);
    return fov;
}

XrPosef viewBoxQuad(const ViewBox* box, XrPosef eye, float* outWidth, float* outHeight) {
    float q = box->count > 0 ? box->depthSum / (float)box->count : 1.0f;
    Vec3 local = { 0.5f * (box->left + box->right) * q, 0.5f * (box->down + box->up) * q, -q };
    Vec3 offset = quatRotate(eye.orientation, local);
    XrPosef pose;
    pose.orientation = eye.orientation;
    pose.position.x = eye.position.x + offset.x;
    pose.position.y = eye.position.y + offset.y;
    pose.position.z = eye.position.z + offset.z;
    *outWidth = (box->right - box->left) * q;
    *outHeight = (box->up - box->down) * q;
    return pose;
}
