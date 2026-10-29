
// Vectors, quaternions, the one euro filter and projection maths. Uses only
// OpenXR value types; no GL.

#ifndef XR_MATH_H
#define XR_MATH_H

#include <math.h>
#include <openxr/openxr.h>

// One euro derivative cutoff, scalar and rotation versions
#define POINTER_D_CUTOFF 1.0f

typedef struct { float x, y, z; } Vec3;

// One euro filter: low pass whose cutoff rises with speed (smooth at rest,
// responsive when moving)
typedef struct {
    int valid;
    float x;
    float dx;
} EuroState;

// One euro on a rotation: angular speed drives the cutoff; nlerp blend is
// adequate at per-frame angles
typedef struct {
    int valid;
    XrQuaternionf q;
    float dAngle;
} EuroQuatState;

Vec3 vecSub(Vec3 a, Vec3 b);
Vec3 vecNorm(Vec3 v);
Vec3 vecCross(Vec3 a, Vec3 b);
float vecDot(Vec3 a, Vec3 b);

XrQuaternionf quatConj(XrQuaternionf q);
XrQuaternionf quatMul(XrQuaternionf a, XrQuaternionf b);
XrQuaternionf quatNorm(XrQuaternionf q);
XrQuaternionf axisAngleQuat(Vec3 axis, float angle);
Vec3 quatRotate(XrQuaternionf q, Vec3 v);
XrQuaternionf quatFromBasis(Vec3 x, Vec3 y, Vec3 z);

// pose expressed relative to frame (both in the same space)
XrPosef poseInFrame(XrPosef frame, XrPosef pose);

// Screen rotated about the vertical to straight ahead of the origin, keeping
// distance, height, tilt and roll
XrPosef poseRecentred(XrPosef screen);

// Head yaw relative to the screen's facing, radians, positive left, (-pi, pi]
float yawBetween(XrQuaternionf head, XrQuaternionf screen);

float euroFilter(EuroState* s, float x, float dt, float minCutoff, float beta);
XrQuaternionf euroFilterQuat(EuroQuatState* s, XrQuaternionf q, float dt,
                             float minCutoff, float beta);

void matMul(float* out, const float* a, const float* b);
void projectionFromFov(float* m, XrFovf fov, float nearZ, float farZ);
void viewFromPose(float* m, XrPosef pose);

// Max cylinder layer central angle. The spec requires < 2*pi and a runtime
// may reject the frame otherwise; wider pictures are scaled down to fit.
#define CYLINDER_MAX_ANGLE (2.0f * (float)M_PI - 0.01f)

// Central angle for a width at a radius, and the scale needed to fit it
// (1 unless it exceeds CYLINDER_MAX_ANGLE)
float cylinderAngle(float width, float radius);
float cylinderFit(float width, float radius);

// Ray/screen intersection to uv and uv back to world, for flat and curved
// screens (curved at cylinderFit's scale)
int screenProject(XrPosef aim, XrPosef screen, float width, float height,
                  float radius, int curved, float* outU, float* outV);
Vec3 screenPoint(float u, float v, XrPosef screen, float width, float height,
                 float radius, int curved);
void curveLocal(Vec3* local, float radius, int curved, float* outYaw);

#endif
