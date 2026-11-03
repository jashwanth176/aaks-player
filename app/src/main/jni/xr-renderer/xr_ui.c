// Layout and hit testing around the picture: handles, bar buttons, panels, and the
// settings panel's rows and cells.
#include "xr_renderer.h"

// Frame the bar row is laid out in: the picture as drawn (narrower on a cylinder)
BarFrame barFrame(XrCtx* ctx) {
    float aspect = screenAspect(ctx);
    BarFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.pose = ctx->screenPose;
    frame.curved = effectiveCurvature(ctx) > 0.01f && ctx->cylinderSupported;
    frame.radius = ctx->screenRadius;
    float fit = frame.curved ? cylinderFit(ctx->screenWidth, ctx->screenRadius) : 1.0f;
    frame.width = ctx->screenWidth * fit;
    frame.height = frame.width * aspect;
    return frame;
}

// Corner bracket size in metres
float cornerSide(XrCtx* ctx) {
    return CORNER_FRAC * ctx->screenWidth;
}

// Panel curvature once touched, else the preference; reset clears the override
float effectiveCurvature(XrCtx* ctx) {
    // Library with no video: keep flat, or a curved screen behind the page would catch
    // the ray at the sides before the corners
    if (ctx->playerScreenHidden) {
        return 0.0f;
    }
    return ctx->panelCurve >= 0.0f ? ctx->panelCurve : ctx->prefCurvature;
}

// Screen pitch in radians, positive facing up
float screenPitch(XrCtx* ctx) {
    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 fwd = quatRotate(ctx->screenPose.orientation, back);
    return atan2f(fwd.y, sqrtf(fwd.x * fwd.x + fwd.z * fwd.z));
}

// Orientation from yaw (world up), pitch, then roll about the screen's forward axis.
// Roll is innermost so pitch and yaw read back unchanged at any roll. Pitch is negated:
// +theta about local x tips the face down.
XrQuaternionf screenOrient(float yaw, float pitch, float roll) {
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    Vec3 fwd = { 0.0f, 0.0f, 1.0f };
    XrQuaternionf q = quatMul(axisAngleQuat(up, yaw), axisAngleQuat(right, -pitch));
    return quatNorm(quatMul(q, axisAngleQuat(fwd, roll)));
}

// Ray visibility at session start; the bar and Display tab toggle it per session.
// Before the first frame.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetShowRay(JNIEnv* env, jobject thiz,
                                                             jlong handle, jboolean on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->raySetting = on ? 1 : 0;
    ctx->rayFlipped = 0;
    LOGEV("ray %s at the start of the session", on ? "shown" : "hidden");
}

// Sets one grade value (clamped) and recomputes the grade; off when all are default.
void pictureSet(XrCtx* ctx, int row, int units) {
    if (row < 0 || row >= PICTURE_VALUES) {
        return;
    }
    ctx->pictureUnits[row] = pictureClamp(row, units);
    ctx->grade = pictureGradeFor(ctx->pictureUnits);
    ctx->gradeOn = !pictureNeutral(ctx->pictureUnits);
}

// Picture values from preferences, PICTURE_ order. Before the first frame.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetPicture(JNIEnv* env, jobject thiz,
                                                             jlong handle, jintArray values) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || values == NULL || (*env)->GetArrayLength(env, values) < PICTURE_VALUES) {
        return;
    }
    int units[PICTURE_VALUES];
    (*env)->GetIntArrayRegion(env, values, 0, PICTURE_VALUES, units);
    for (int row = 0; row < PICTURE_VALUES; row++) {
        pictureSet(ctx, row, units[row]);
    }
    LOGEV("picture: brightness %d, contrast %d, gamma %d, saturation %d, grade %s",
          ctx->pictureUnits[PICTURE_BRIGHTNESS], ctx->pictureUnits[PICTURE_CONTRAST],
          ctx->pictureUnits[PICTURE_GAMMA], ctx->pictureUnits[PICTURE_SATURATION],
          ctx->gradeOn ? "on" : "off");
}
