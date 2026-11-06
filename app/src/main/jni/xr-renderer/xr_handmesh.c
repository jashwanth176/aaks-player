// Hand outlines: skinning maths.
#include "xr_handmesh.h"

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

void handSkinMatrix(float* out, XrPosef joint, XrPosef bind) {
    float now[16];
    float unbind[16];
    matFromPose(now, joint);
    // The inverse of the bind pose, as a view matrix is of a camera's
    viewFromPose(unbind, bind);
    matMul(out, now, unbind);
}
