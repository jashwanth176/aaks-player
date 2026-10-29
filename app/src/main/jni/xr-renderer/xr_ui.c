// Layout and hit testing around the picture: handles, bar buttons, panels, and the
// settings panel's rows and cells.
#include "xr_renderer.h"

// In a room, panels are placed and sized off the stand-in screen rather than the
// picture. The bar is the exception (see barFrame).
int furnitureOnStandIn(XrCtx* ctx) {
    return roomEffective(ctx) > 0;
}

// Picture pose outside a room, stand-in pose inside one
XrPosef furniturePose(XrCtx* ctx) {
    return furnitureOnStandIn(ctx) ? standInPose() : ctx->screenPose;
}

float furnitureWidth(XrCtx* ctx) {
    return furnitureOnStandIn(ctx) ? STAND_IN_WIDTH_M : ctx->screenWidth;
}

// Same aspect as the picture
float furnitureHeight(XrCtx* ctx) {
    return furnitureWidth(ctx) * screenAspect(ctx);
}

// Frame the bar row is laid out in. In a room: under the room's picture at the
// stand-in's bar size. Otherwise the picture as drawn (narrower on a cylinder).
BarFrame barFrame(XrCtx* ctx) {
    float aspect = screenAspect(ctx);
    if (roomEffective(ctx) > 0) {
        return roomBarFrame(ctx->screenPose, ctx->screenWidth * aspect, aspect);
    }
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

// Whether a bar slot is hittable: fixed buttons always, switches once their art is
// in, head aim only while it can act
static int barSlotShown(XrCtx* ctx, int slot) {
    switch (slot) {
        case BAR_SLOT_PAD:
            return ctx->padButtonReady;
        case BAR_SLOT_AIM:
            return ctx->aimButtonReady && headAimCanAct(ctx);
        case BAR_SLOT_RAY:
            return ctx->rayButtonReady;
        case BAR_SLOT_STEREO:
            return ctx->stereoButtonReady;
        default:
            return 1;
    }
}

int barButtonHit(XrCtx* ctx, int slot, float u, float v, const BarFrame* frame) {
    // No bar buttons in player mode
    if (ctx->playerMode) {
        return 0;
    }
    return barSlotShown(ctx, slot) && barSlotHit(slot, u, v, frame->width, frame->height);
}

// Corner bracket size in metres, 0 for none. In a room only resizable rooms have
// corners, sized by distance so they look the same at any picture size.
float cornerSide(XrCtx* ctx) {
    int style = roomEffective(ctx);
    if (style > 0) {
        if (!roomResizable(style)) {
            return 0.0f;
        }
        XrVector3f p = ctx->screenPose.position;
        return roomCornerSide(sqrtf(p.x * p.x + p.y * p.y + p.z * p.z));
    }
    return CORNER_FRAC * ctx->screenWidth;
}

// Panel curvature once touched, else the preference; reset clears the override
float effectiveCurvature(XrCtx* ctx) {
    // Library with no video: keep flat, or a curved screen behind the page would catch
    // the ray at the sides before the corners
    if (ctx->playerMode && ctx->playerScreenHidden) {
        return 0.0f;
    }
    return ctx->panelCurve >= 0.0f ? ctx->panelCurve : ctx->prefCurvature;
}

// In a room the screen tab is replaced by the Room tab. Input must agree with this.
int cogFace(XrCtx* ctx) {
    return ctx->cogTab == COG_TAB_SCREEN && roomEffective(ctx) > 0 ? COG_FACE_ROOM
                                                                    : ctx->cogTab;
}

// Sheet for the current tab: room variants in a room, head-aim-greyed variants for a
// screen that isn't head-locked.
int cogArt(XrCtx* ctx) {
    int style = roomEffective(ctx);
    if (style <= 0) {
        if (!ctx->headLockedPref && ctx->cogTab == COG_TAB_SCREEN) {
            return COG_ART_SCREEN_WORLD;
        }
        if (!ctx->headLockedPref && ctx->cogTab == COG_TAB_DISPLAY) {
            return COG_ART_DISPLAY_WORLD;
        }
        return ctx->cogTab;
    }
    if (ctx->cogTab == COG_TAB_DISPLAY) {
        return COG_ART_ROOM_DISPLAY;
    }
    if (ctx->cogTab == COG_TAB_3D) {
        return COG_ART_ROOM_3D;
    }
    if (ctx->cogTab == COG_TAB_PICTURE) {
        return COG_ART_ROOM_PICTURE;
    }
    if (ctx->cogTab == COG_TAB_ABOUT) {
        return COG_ART_ROOM_ABOUT;
    }
    return roomResizable(style) ? COG_ART_ROOM : COG_ART_ROOM_FIXED;
}

// Room style the Room tab edits, or 0
static int roomFaceStyle(XrCtx* ctx) {
    int style = roomEffective(ctx);
    return style >= ROOM_STYLE_FIRST && style <= ROOM_STYLE_LAST ? style : 0;
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

// Roll about the facing axis in radians, positive raises the right edge. Undoes yaw
// and pitch, so it round-trips through screenOrient at any pitch.
float screenRoll(XrCtx* ctx) {
    XrQuaternionf q = ctx->screenPose.orientation;
    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 fwd = quatRotate(q, back);
    float yaw = atan2f(fwd.x, fwd.z);
    XrQuaternionf level = screenOrient(yaw, screenPitch(ctx), 0.0f);
    XrQuaternionf twist = quatMul(quatConj(level), q);
    // q and -q are the same rotation; normalise to avoid a result a full turn out
    if (twist.w < 0.0f) {
        twist.z = -twist.z;
        twist.w = -twist.w;
    }
    return 2.0f * atan2f(twist.z, twist.w);
}

// Centred just in front of the screen
XrPosef pickerPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float width = furnitureWidth(ctx) * PICKER_WIDTH_FRAC;
    *outWidth = width;
    *outHeight = width * (float)PICKER_TEX_H / (float)PICKER_TEX_W;

    XrPosef pose = furniturePose(ctx);
    Vec3 local = { 0.0f, 0.0f, 0.06f };
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Settings panel sits above the cog button. The caller freezes the pose while it is
// open, so the distance slider can't move it out from under the ray mid drag.
XrPosef cogPanelPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float frameWidth = furnitureWidth(ctx);
    float width = frameWidth * COG_WIDTH_FRAC;
    float height = width * (float)COG_TEX_H / (float)COG_TEX_W;
    *outWidth = width;
    *outHeight = height;

    // Placed off the cog button; in a room, where the button would be on the stand-in
    Vec3 button;
    float side;
    barSlotPlacement(BAR_SLOT_COG, frameWidth, furnitureHeight(ctx), &button, &side);

    Vec3 local;
    local.x = button.x;
    local.y = button.y + side * 0.5f + frameWidth * ENV_GAP_FRAC + height * 0.5f;
    local.z = 0.05f;

    XrPosef pose = furniturePose(ctx);
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Keyboard below the screen, centred, in the bar's band. Frozen while open, like the
// settings panel.
XrPosef kbPanelPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float frameWidth = furnitureWidth(ctx);
    float width = frameWidth * KB_WIDTH_FRAC;
    float height = width * (float)KB_TEX_H / (float)KB_TEX_W;
    *outWidth = width;
    *outHeight = height;

    Vec3 local;
    local.x = 0.0f;
    // Top edge at the bar's gap below the picture
    local.y = -(furnitureHeight(ctx) * 0.5f + frameWidth * BAR_GAP_FRAC + height * 0.5f);
    local.z = 0.05f;

    XrPosef pose = furniturePose(ctx);
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Key index at a point on the keyboard, or -1. Blank keys on the current sheet don't
// count.
int kbKeyAt(XrCtx* ctx, float u, float v) {
    int found = -1;
    for (int i = 0; i < ctx->kbKeyCount; i++) {
        if (ctx->kbCodes[ctx->kbState][i] == 0) {
            continue;
        }
        const float* r = &ctx->kbKeyRects[i * 4];
        if (u >= r[0] && u <= r[2] && v >= r[1] && v <= r[3]) {
            found = i;
        }
    }
    return found;
}

// Head aim needs a head-locked screen and no room
int headAimCanAct(XrCtx* ctx) {
    return ctx->headLockedPref && roomEffective(ctx) <= 0;
}

// Session-only toggle from the bar or the Display tab; not stored.
void setHeadAimOn(XrCtx* ctx, int on, const char* from) {
    on = on ? 1 : 0;
    if (headAimSwitchOn(ctx->headAimSetting, ctx->headAimFlipped) == on) {
        return;
    }
    ctx->headAimFlipped = headAimFlipFor(ctx->headAimSetting, on);
    LOGEV("head aim %s from %s", on ? "on" : "off", from);
}

// Session-only gamepad toggle (shortcut, bar or Display tab), announced by toast.
// Turning on puts panels away; turning off swallows a trigger still held so the first
// pointer frame doesn't click.
void setPadMode(XrCtx* ctx, int on, const char* from) {
    on = on ? 1 : 0;
    if (ctx->padMode == on) {
        return;
    }
    ctx->padMode = on;
    if (on) {
        ctx->padPutAway = 1;
    }
    else {
        for (int h = 0; h < HAND_COUNT; h++) {
            if (ctx->profileKind[h] == PROFILE_CONTROLLER && ctx->padRead[h].trigger >= PRESS_ON) {
                ctx->triggerDown[h] = 1;
                ctx->triggerSwallowed[h] = 1;
            }
        }
    }
    LOGEV("controllers: %s from %s", on ? "gamepad mode" : "pointer mode", from);
    noticePush(&ctx->notices, on ? TOAST_GAMEPAD_MODE : TOAST_POINTER_MODE, 0);
}

// Session-only toggle from the bar or the Display tab; not stored.
void setRayOn(XrCtx* ctx, int on, const char* from) {
    on = on ? 1 : 0;
    if (raySwitchOn(ctx->raySetting, ctx->rayFlipped) == on) {
        return;
    }
    ctx->rayFlipped = rayFlipFor(ctx->raySetting, on);
    LOGEV("ray %s from %s%s", on ? "on" : "off", from,
          on ? "" : ", the dot stays and presses land as before");
}

// Whether any panel is up; the ray is shown for panels even when switched off.
int panelUp(XrCtx* ctx) {
    return ctx->cogOpen || ctx->pickerOpen || ctx->kbOpen || ctx->exitConfirmOpen
            || ctx->reportOpen || ctx->hintOpen || ctx->kofiOpen;
}

// Session-only 3D toggle from the bar or 3D tab; forces a redraw. Turning back on with
// a model running waits for a fresh depth map.
void setStereoLive(XrCtx* ctx, int on, const char* from) {
    on = on ? 1 : 0;
    if (ctx->stereoMode == DEPTH_MODE_OFF || ctx->stereoLive == on) {
        return;
    }
    ctx->stereoLive = on;
    ctx->warpRedraw = 1;
    ctx->stereoWaiting = on && ctx->stereoMode == DEPTH_MODE_MODEL;
    ctx->stereoWaitIndex = atomic_load_explicit(&ctx->depthStagedIndex, memory_order_acquire);
    ctx->stereoWaitNs = nowNs();
    // Re-run the shift test in the new state
    ctx->barTestFramesLogged = 0;
    LOGEV("3d %s from %s", on ? "on" : "off", from);
}

// Exit prompt sits above the exit button; frozen while up, like the settings panel.
XrPosef exitPromptPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float frameWidth = furnitureWidth(ctx);
    float width = frameWidth * EXIT_WIDTH_FRAC;
    float height = width * (float)EXIT_TEX_H / (float)EXIT_TEX_W;
    *outWidth = width;
    *outHeight = height;

    Vec3 button;
    float side;
    barSlotPlacement(BAR_SLOT_EXIT, frameWidth, furnitureHeight(ctx), &button, &side);

    Vec3 local;
    local.x = button.x;
    local.y = button.y + side * 0.5f + frameWidth * ENV_GAP_FRAC + height * 0.5f;
    local.z = 0.05f;

    XrPosef pose = furniturePose(ctx);
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Report sheet just above the picture's bottom edge, so the keyboard sits directly
// below it. Frozen while up.
XrPosef reportSheetPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float frameWidth = furnitureWidth(ctx);
    float width = frameWidth * REPORT_WIDTH_FRAC;
    float height = width * (float)REPORT_TEX_H / (float)REPORT_TEX_W;
    *outWidth = width;
    *outHeight = height;

    Vec3 local;
    local.x = 0.0f;
    local.y = -furnitureHeight(ctx) * 0.5f + height * 0.5f + frameWidth * 0.01f;
    local.z = 0.06f;

    XrPosef pose = furniturePose(ctx);
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Hand-lock hint centred just in front of the picture. Frozen while up.
XrPosef handHintPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float width = furnitureWidth(ctx) * HINT_WIDTH_FRAC;
    float height = width * (float)HINT_TEX_H / (float)HINT_TEX_W;
    *outWidth = width;
    *outHeight = height;

    Vec3 local = { 0.0f, 0.0f, 0.06f };
    XrPosef pose = furniturePose(ctx);
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// About sheet, placed like the hint. Frozen while up.
XrPosef kofiSheetPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float width = furnitureWidth(ctx) * KOFI_WIDTH_FRAC;
    float height = width * (float)KOFI_TEX_H / (float)KOFI_TEX_W;
    *outWidth = width;
    *outHeight = height;

    Vec3 local = { 0.0f, 0.0f, 0.06f };
    XrPosef pose = furniturePose(ctx);
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Exit prompt button at a point in sheet coordinates
int exitPromptZone(float u, float v) {
    if (v < EXIT_BTN_T || v > EXIT_BTN_B) {
        return EXIT_ZONE_NONE;
    }
    if (u >= EXIT_EXIT_L && u <= EXIT_EXIT_R) {
        return EXIT_ZONE_EXIT;
    }
    if (u >= EXIT_CANCEL_L && u <= EXIT_CANCEL_R) {
        return EXIT_ZONE_CANCEL;
    }
    return EXIT_ZONE_NONE;
}

// Row count for a face
int cogTabRowCount(int face) {
    if (face == COG_TAB_SCREEN) {
        return COG_SCREEN_ROW_COUNT;
    }
    if (face == COG_TAB_3D) {
        return COG_ROW3D_COUNT;
    }
    if (face == COG_FACE_ROOM) {
        return COG_ROOM_ROW_COUNT;
    }
    if (face == COG_TAB_PICTURE) {
        return PICTURE_VALUES;
    }
    if (face == COG_TAB_ABOUT) {
        return 0;
    }
    // Option rows plus the glow level track
    return COG_DISPLAY_SLIDER_ROW + 1;
}

// Whether a row is a track rather than cells
int cogRowIsTrack(int face, int row) {
    if (face == COG_TAB_DISPLAY) {
        return row == COG_DISPLAY_SLIDER_ROW;
    }
    if (face == COG_FACE_ROOM) {
        return row != COG_ROOM_ROW_GLOW && row != COG_ROOM_ROW_LIGHT;
    }
    if (face == COG_TAB_3D) {
        return row == COG_ROW3D_SEPARATION || row == COG_ROW3D_CONVERGENCE;
    }
    return 1;
}

int cogRowCells(int face, int row) {
    if (face == COG_FACE_ROOM) {
        return COG_ROOM_SWITCH_CELLS;
    }
    if (face == COG_TAB_3D) {
        return row == COG_ROW3D_PRESET ? COG_PRESET_CELLS : COG_STEREO_CELLS;
    }
    return cogOptionCells(row);
}

// Dead rows are greyed, have no thumb and take no press.
int cogRowLive(XrCtx* ctx, int face, int row) {
    // Curvature needs cylinder layers
    if (face == COG_TAB_SCREEN && row == COG_SLIDER_CURVE) {
        return ctx->cylinderSupported;
    }
    // Head aim needs a head-locked screen (this face is never shown in a room)
    if (face == COG_TAB_SCREEN
            && (row == COG_SLIDER_AIM_SENSITIVITY || row == COG_SLIDER_AIM_DEADZONE)) {
        return ctx->headLockedPref;
    }
    if (face == COG_TAB_DISPLAY && row == COG_OPTION_HEAD_AIM) {
        return headAimCanAct(ctx);
    }
    // No stereo: all 3D rows dead
    if (face == COG_TAB_3D) {
        return ctx->stereoMode != DEPTH_MODE_OFF;
    }
    // Fixed-size rooms
    if (face == COG_FACE_ROOM && row == COG_ROOM_ROW_SIZE) {
        return roomResizable(roomFaceStyle(ctx));
    }
    return 1;
}

// Thumb position 0..1, read back from the controlled value so it never goes stale.
float cogSliderValue(XrCtx* ctx, int face, int slider) {
    XrVector3f p = ctx->screenPose.position;
    float t = 0.0f;

    if (face == COG_TAB_DISPLAY) {
        // The tab's only track
        t = ctx->ambiIntensity;
    }
    else if (face == COG_FACE_ROOM) {
        // Values of the room currently shown
        int style = roomFaceStyle(ctx);
        if (style == 0) {
            return 0.0f;
        }
        if (slider == COG_ROOM_ROW_BRIGHTNESS) {
            t = lanePlace(ctx->roomBrightness[style], ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX);
        }
        else if (slider == COG_ROOM_ROW_LIGHT_LEVEL) {
            t = lanePlace(ctx->roomLightLevel[style], ROOM_LIGHT_MIN, ROOM_LIGHT_MAX);
        }
        else if (slider == COG_ROOM_ROW_SIZE) {
            t = lanePlace(roomScreenPercent(ctx, style), ROOM_SCREEN_MIN, ROOM_SCREEN_MAX);
        }
    }
    else if (face == COG_TAB_3D) {
        if (slider == COG_ROW3D_SEPARATION) {
            t = ctx->separationCurrent / COG_SEP_MAX;
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            t = ctx->convergence;
        }
    }
    else if (face == COG_TAB_PICTURE) {
        if (slider >= 0 && slider < PICTURE_VALUES) {
            t = lanePlace(ctx->pictureUnits[slider], pictureMin(slider), pictureMax(slider));
        }
    }
    else if (slider == COG_SLIDER_DISTANCE) {
        float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        t = (d - COG_DIST_MIN) / (COG_DIST_MAX - COG_DIST_MIN);
    }
    else if (slider == COG_SLIDER_HEIGHT) {
        t = (p.y - COG_HEIGHT_MIN) / (COG_HEIGHT_MAX - COG_HEIGHT_MIN);
    }
    else if (slider == COG_SLIDER_TILT) {
        // Level at mid track
        t = (screenPitch(ctx) + COG_TILT_MAX) / (2.0f * COG_TILT_MAX);
    }
    else if (slider == COG_SLIDER_ROTATE) {
        // Reversed: the right end rolls clockwise, i.e. negative screenRoll
        t = (COG_ROLL_MAX - screenRoll(ctx)) / (2.0f * COG_ROLL_MAX);
    }
    else if (slider == COG_SLIDER_CURVE) {
        t = effectiveCurvature(ctx);
    }
    else if (slider == COG_SLIDER_SIZE) {
        t = (ctx->screenWidth - SCREEN_MIN_WIDTH) / (SCREEN_MAX_WIDTH - SCREEN_MIN_WIDTH);
    }
    else if (slider == COG_SLIDER_AIM_SENSITIVITY) {
        t = lanePlace(ctx->headAimSensitivity, HEAD_AIM_SENSITIVITY_MIN,
                      HEAD_AIM_SENSITIVITY_MAX);
    }
    else if (slider == COG_SLIDER_AIM_DEADZONE) {
        t = lanePlace(ctx->headAimDeadZone, HEAD_AIM_DEADZONE_MIN, HEAD_AIM_DEADZONE_MAX);
    }

    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// Applies a track position to the row's value. The thumb's run is inset from the
// track ends, where the step buttons are.
void cogApplySlider(XrCtx* ctx, int face, int slider, float pu) {
    float t = cogRunPlace(pu);

    if (face == COG_FACE_ROOM) {
        // Whole stored units so the thumb matches what's saved; read live every frame
        int style = roomFaceStyle(ctx);
        if (style == 0) {
            return;
        }
        if (slider == COG_ROOM_ROW_BRIGHTNESS) {
            ctx->roomBrightness[style] = laneUnits(t, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX);
        }
        else if (slider == COG_ROOM_ROW_LIGHT_LEVEL) {
            ctx->roomLightLevel[style] = laneUnits(t, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX);
        }
        else if (slider == COG_ROOM_ROW_SIZE && roomResizable(style)) {
            // About the picture centre
            ctx->roomScreen[style] = laneUnits(t, ROOM_SCREEN_MIN, ROOM_SCREEN_MAX);
        }
        return;
    }

    if (face == COG_TAB_DISPLAY) {
        // 5% steps, matching the stored value
        int units = (int)roundf(t * 20.0f) * 5;
        ctx->ambiIntensity = units / 100.0f;
        return;
    }

    if (face == COG_TAB_PICTURE) {
        // Whole stored units; the grade is read live every frame
        if (slider >= 0 && slider < PICTURE_VALUES) {
            pictureSet(ctx, slider, laneUnits(t, pictureMin(slider), pictureMax(slider)));
        }
        return;
    }

    if (face == COG_TAB_3D) {
        // Grabbing either track turns 3D back on
        setStereoLive(ctx, 1, "the 3D tab's tracks");
        if (slider == COG_ROW3D_SEPARATION) {
            // Snap to stored units
            ctx->panelSeparation = separationOf(laneUnits(t, 0, COG_SEP_STEPS));
            ctx->separationCurrent = ctx->panelSeparation;
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            // Whole percent
            int units = (int)roundf(t * 100.0f);
            ctx->convergence = units / 100.0f;
        }
        return;
    }

    // Head aim, whole stored units, read live every frame
    if (slider == COG_SLIDER_AIM_SENSITIVITY) {
        ctx->headAimSensitivity = laneUnits(t, HEAD_AIM_SENSITIVITY_MIN,
                                            HEAD_AIM_SENSITIVITY_MAX);
        return;
    }
    if (slider == COG_SLIDER_AIM_DEADZONE) {
        ctx->headAimDeadZone = laneUnits(t, HEAD_AIM_DEADZONE_MIN, HEAD_AIM_DEADZONE_MAX);
        return;
    }

    if (slider == COG_SLIDER_DISTANCE) {
        XrVector3f p = ctx->screenPose.position;
        float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        float wanted = COG_DIST_MIN + t * (COG_DIST_MAX - COG_DIST_MIN);
        if (d > 0.01f) {
            // Along the existing line, so only distance changes
            float scale = wanted / d;
            ctx->screenPose.position.x = p.x * scale;
            ctx->screenPose.position.y = p.y * scale;
            ctx->screenPose.position.z = p.z * scale;
            // Keep the arc shape, as in resize
            ctx->screenRadius *= scale;
        }
        else {
            // At the viewer: no line to follow, use straight ahead
            ctx->screenPose.position.x = 0.0f;
            ctx->screenPose.position.y = 0.0f;
            ctx->screenPose.position.z = -wanted;
        }
    }
    else if (slider == COG_SLIDER_HEIGHT) {
        // Vertical only, not an arc about the viewer
        ctx->screenPose.position.y = COG_HEIGHT_MIN + t * (COG_HEIGHT_MAX - COG_HEIGHT_MIN);
    }
    else if (slider == COG_SLIDER_TILT) {
        float target = -COG_TILT_MAX + t * (2.0f * COG_TILT_MAX);
        // Snap to level
        if (fabsf(target) < COG_TILT_SNAP) {
            target = 0.0f;
        }
        // Rebuild from yaw/pitch/roll rather than rotating about local x, which isn't
        // horizontal once the picture is rolled
        Vec3 back = { 0.0f, 0.0f, 1.0f };
        Vec3 fwd = quatRotate(ctx->screenPose.orientation, back);
        float yaw = atan2f(fwd.x, fwd.z);
        float roll = screenRoll(ctx);
        // Position unchanged: tilts about its own centre
        ctx->screenPose.orientation = screenOrient(yaw, target, roll);
    }
    else if (slider == COG_SLIDER_ROTATE) {
        // Dragging right rolls clockwise as seen; forward points at the viewer, so the track
        // runs +max to -max
        float target = COG_ROLL_MAX - t * (2.0f * COG_ROLL_MAX);
        // Snap the middle of the track to exactly level
        if (fabsf(target) < COG_ROLL_SNAP) {
            target = 0.0f;
        }
        float delta = target - screenRoll(ctx);
        Vec3 fwd = { 0.0f, 0.0f, 1.0f };
        Vec3 axis = quatRotate(ctx->screenPose.orientation, fwd);
        XrQuaternionf turn = axisAngleQuat(axis, delta);
        // Rotating about the facing axis leaves pitch and yaw unchanged
        ctx->screenPose.orientation = quatNorm(quatMul(turn, ctx->screenPose.orientation));
    }
    else if (slider == COG_SLIDER_CURVE) {
        ctx->panelCurve = t;
        // Same radius updatePlacement would choose, so a later reseed agrees
        XrVector3f p = ctx->screenPose.position;
        float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        ctx->screenRadius = d * (1.0f + 3.0f * (1.0f - t));
    }
    else if (slider == COG_SLIDER_SIZE) {
        float wanted = SCREEN_MIN_WIDTH + t * (SCREEN_MAX_WIDTH - SCREEN_MIN_WIDTH);
        if (ctx->screenWidth > 0.01f) {
            // Keep the arc shape as it grows
            ctx->screenRadius *= wanted / ctx->screenWidth;
        }
        // Grows about the centre
        ctx->screenWidth = wanted;
    }
}

// Cell count for a display tab option row
int cogOptionCells(int option) {
    if (option == COG_OPTION_SHARPEN) {
        return COG_SHARPEN_CELLS;
    }
    if (option == COG_OPTION_SUPERSAMPLE) {
        return COG_SUPERSAMPLE_CELLS;
    }
    if (option == COG_OPTION_HEAD_LOCK) {
        return COG_HEAD_LOCK_CELLS;
    }
    if (option == COG_OPTION_HEAD_AIM) {
        return COG_HEAD_AIM_CELLS;
    }
    if (option == COG_OPTION_GAMEPAD) {
        return COG_GAMEPAD_CELLS;
    }
    if (option == COG_OPTION_POINTER_SLEEP) {
        return COG_POINTER_SLEEP_CELLS;
    }
    if (option == COG_OPTION_RAY) {
        return COG_RAY_CELLS;
    }
    if (option == COG_OPTION_CONTROLLERS) {
        return COG_CONTROLLERS_CELLS;
    }
    if (option == COG_OPTION_CLICK_SOUND) {
        return COG_CLICK_SOUND_CELLS;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        return COG_AMBI_CELLS;
    }
    if (option == COG_OPTION_ROOM_LIGHT) {
        return COG_ROOM_LIGHT_CELLS;
    }
    return COG_STATS_CELLS;
}

// Selected cell for an option. Head lock comes from the frame (headLocked) so the mark
// stays correct if changed elsewhere.
int cogOptionValue(XrCtx* ctx, int option, int headLocked) {
    if (option == COG_OPTION_SHARPEN) {
        return ctx->sharpenMode;
    }
    if (option == COG_OPTION_SUPERSAMPLE) {
        return ctx->supersampleMode;
    }
    if (option == COG_OPTION_HEAD_LOCK) {
        return headLocked ? 1 : 0;
    }
    if (option == COG_OPTION_HEAD_AIM) {
        // The switch state, even when it can't act
        return headAimSwitchOn(ctx->headAimSetting, ctx->headAimFlipped);
    }
    if (option == COG_OPTION_GAMEPAD) {
        return ctx->padMode ? 1 : 0;
    }
    if (option == COG_OPTION_POINTER_SLEEP) {
        return ctx->pointerSleepOn ? 1 : 0;
    }
    if (option == COG_OPTION_RAY) {
        // The switch state, regardless of panels forcing the ray on
        return raySwitchOn(ctx->raySetting, ctx->rayFlipped);
    }
    if (option == COG_OPTION_CONTROLLERS) {
        return ctx->modelOn ? 1 : 0;
    }
    if (option == COG_OPTION_CLICK_SOUND) {
        return ctx->clickSoundOn ? 1 : 0;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        // In a room, the room's own glow switch
        return roomGlowOn(ctx, roomEffective(ctx)) ? 1 : 0;
    }
    if (option == COG_OPTION_ROOM_LIGHT) {
        return ctx->roomLightOn ? 1 : 0;
    }
    return ctx->overlayVisible ? 1 : 0;
}

// Applies an option now and returns the setting id for Java to store, or -1. Values
// are read live, so nothing restarts.
int cogApplyOption(XrCtx* ctx, int option, int cell) {
    if (option == COG_OPTION_SHARPEN) {
        ctx->sharpenMode = cell;
        return SETTING_SHARPEN;
    }
    if (option == COG_OPTION_SUPERSAMPLE) {
        ctx->supersampleMode = cell;
        return SETTING_SUPERSAMPLE;
    }
    if (option == COG_OPTION_STATS) {
        ctx->overlayVisible = cell != 0;
        return SETTING_STATS;
    }
    if (option == COG_OPTION_HEAD_LOCK) {
        // Java writes the preference it passes down each frame. Left live in a room, where
        // the wall placement wins anyway.
        LOGEV("head lock %s from the panel", cell != 0 ? "on" : "off");
        return SETTING_HEAD_LOCK;
    }
    if (option == COG_OPTION_HEAD_AIM) {
        // Session only, not stored
        setHeadAimOn(ctx, cell != 0, "the Display tab");
        return -1;
    }
    if (option == COG_OPTION_GAMEPAD) {
        // Session only; puts the panel away
        setPadMode(ctx, cell != 0, "the Display tab");
        return -1;
    }
    if (option == COG_OPTION_POINTER_SLEEP) {
        // Set locally too so the mark moves on the press
        ctx->pointerSleepOn = cell != 0;
        LOGEV("pointer sleep %s from the panel", cell != 0 ? "on" : "off");
        return SETTING_POINTER_SLEEP;
    }
    if (option == COG_OPTION_RAY) {
        // Session only, not stored
        setRayOn(ctx, cell != 0, "the Display tab");
        return -1;
    }
    if (option == COG_OPTION_CONTROLLERS) {
        ctx->modelOn = cell != 0;
        LOGEV("controller model %s from the panel", cell != 0 ? "on" : "off");
        return SETTING_CONTROLLER_MODEL;
    }
    if (option == COG_OPTION_CLICK_SOUND) {
        // Java plays the sound
        ctx->clickSoundOn = cell != 0;
        LOGEV("click sound %s from the panel", cell != 0 ? "on" : "off");
        return SETTING_CLICK_SOUND;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        // In a room, the same switch as the Room tab's glow row
        int style = roomFaceStyle(ctx);
        if (style != 0) {
            ctx->roomGlow[style] = cell != 0;
            LOGEV("room %d glow %s from the panel", style, cell != 0 ? "on" : "off");
            return SETTING_ROOM_GLOW;
        }
        ctx->ambilightOn = cell != 0;
        LOGEV("ambilight %s from the panel", ctx->ambilightOn ? "on" : "off");
        return SETTING_AMBILIGHT;
    }
    if (option == COG_OPTION_ROOM_LIGHT) {
        ctx->roomLightOn = cell != 0;
        LOGEV("room light %s from the panel", ctx->roomLightOn ? "on" : "off");
        return SETTING_ROOM_LIGHT;
    }
    return -1;
}

// Room tab cells: the room's glow, and the global screen light
int cogRoomCellValue(XrCtx* ctx, int row) {
    if (row == COG_ROOM_ROW_GLOW) {
        return roomGlowOn(ctx, roomEffective(ctx)) ? 1 : 0;
    }
    return ctx->roomLightOn ? 1 : 0;
}

// Applies now and reports to Java; glow is stored under IN_SETTING_ROOM's room.
void cogApplyRoomCell(XrCtx* ctx, int row, int cell, float* out) {
    int id = row == COG_ROOM_ROW_GLOW ? cogApplyOption(ctx, COG_OPTION_AMBILIGHT, cell)
                                      : cogApplyOption(ctx, COG_OPTION_ROOM_LIGHT, cell);
    out[IN_SETTING] = (float)id;
    out[IN_SETTING_VALUE] = (float)cell;
}

// Selected cell on the Room or 3D tab, or -1 (e.g. a separation between presets)
int cogCellInForce(XrCtx* ctx, int face, int row) {
    if (face == COG_FACE_ROOM) {
        return cogRoomCellValue(ctx, row);
    }
    if (face == COG_TAB_3D && row == COG_ROW3D_PRESET) {
        return cogPresetAt(separationUnits(ctx->separationCurrent), ctx->presetUnits);
    }
    if (face == COG_TAB_3D && row == COG_ROW3D_SWITCH) {
        return ctx->stereoLive ? 1 : 0;
    }
    return -1;
}

// The switch is session only. A preset is stored like a separation drag and turns 3D
// back on.
static void cogApply3dCell(XrCtx* ctx, int row, int cell, float* out) {
    static const char* const PRESET_NAMES[COG_PRESET_CELLS] = {
        "Comfort", "Balanced", "Strong"
    };
    if (row == COG_ROW3D_SWITCH) {
        setStereoLive(ctx, cell != 0, "the 3D tab");
        return;
    }
    if (row != COG_ROW3D_PRESET || cell < 0 || cell >= COG_PRESET_CELLS) {
        return;
    }
    int units = ctx->presetUnits[cell];
    ctx->panelSeparation = separationOf(units);
    ctx->separationCurrent = ctx->panelSeparation;
    out[IN_SETTING] = (float)SETTING_SEPARATION;
    out[IN_SETTING_VALUE] = (float)units;
    LOGEV("3d preset %s from the panel, separation %d", PRESET_NAMES[cell], units);
    setStereoLive(ctx, 1, "a preset");
}

// Applies a cell press now and reports it to Java
void cogApplyCell(XrCtx* ctx, int face, int row, int cell, float* out) {
    if (face == COG_FACE_ROOM) {
        cogApplyRoomCell(ctx, row, cell, out);
        return;
    }
    if (face == COG_TAB_3D) {
        cogApply3dCell(ctx, row, cell, out);
        return;
    }
    int id = cogApplyOption(ctx, row, cell);
    if (id >= 0) {
        out[IN_SETTING] = (float)id;
        out[IN_SETTING_VALUE] = (float)cell;
    }
}

// Picture setting ids and names, PICTURE_ order
static const int PICTURE_SETTINGS[PICTURE_VALUES] = {
    SETTING_PICTURE_BRIGHTNESS, SETTING_PICTURE_CONTRAST, SETTING_PICTURE_GAMMA,
    SETTING_PICTURE_SATURATION
};
static const char* const PICTURE_NAMES[PICTURE_VALUES] = {
    "brightness", "contrast", "gamma", "saturation"
};

// Drag ended (release or focus loss): report the final value to Java for storage.
void cogDragEnded(XrCtx* ctx, float* out) {
    int slider = ctx->cogDragSlider;
    int face = ctx->cogDragFace;
    ctx->cogDragSlider = -1;
    ctx->cogDragHand = -1;
    ctx->cogDragFace = -1;
    ctx->cogDragByGaze = 0;

    if (slider < 0) {
        return;
    }
    if (face == COG_TAB_SCREEN && slider == COG_SLIDER_AIM_SENSITIVITY) {
        out[IN_SETTING] = (float)SETTING_HEAD_AIM_SENSITIVITY;
        out[IN_SETTING_VALUE] = (float)ctx->headAimSensitivity;
        LOGEV("head aim %d px a degree from the panel", ctx->headAimSensitivity);
    }
    else if (face == COG_TAB_SCREEN && slider == COG_SLIDER_AIM_DEADZONE) {
        out[IN_SETTING] = (float)SETTING_HEAD_AIM_DEADZONE;
        out[IN_SETTING_VALUE] = (float)ctx->headAimDeadZone;
        LOGEV("head aim dead zone %d deg/s from the panel", ctx->headAimDeadZone);
    }
    else if (face == COG_TAB_SCREEN) {
        // Placement is saved from the pose returned to Java
        ctx->poseDirty = 1;
    }
    else if (face == COG_FACE_ROOM) {
        // Stored under the room shown; nothing if the room went away mid drag
        int style = roomFaceStyle(ctx);
        if (style == 0) {
            return;
        }
        int setting = -1;
        int value = 0;
        const char* what = "";
        if (slider == COG_ROOM_ROW_BRIGHTNESS) {
            setting = SETTING_ROOM_BRIGHTNESS;
            value = ctx->roomBrightness[style];
            what = "brightness";
        }
        else if (slider == COG_ROOM_ROW_LIGHT_LEVEL) {
            setting = SETTING_ROOM_LIGHT_LEVEL;
            value = ctx->roomLightLevel[style];
            what = "light level";
        }
        else if (slider == COG_ROOM_ROW_SIZE) {
            setting = SETTING_ROOM_SCREEN;
            value = roomScreenPercent(ctx, style);
            what = "screen percent";
        }
        if (setting >= 0) {
            out[IN_SETTING] = (float)setting;
            out[IN_SETTING_VALUE] = (float)value;
            LOGEV("room %d %s %d from the panel", style, what, value);
        }
    }
    else if (face == COG_TAB_3D) {
        if (slider == COG_ROW3D_SEPARATION) {
            out[IN_SETTING] = (float)SETTING_SEPARATION;
            // Tenths of a percent of frame width
            out[IN_SETTING_VALUE] = (float)separationUnits(ctx->separationCurrent);
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            out[IN_SETTING] = (float)SETTING_CONVERGENCE;
            out[IN_SETTING_VALUE] = roundf(ctx->convergence * 100.0f);
        }
    }
    else if (face == COG_TAB_DISPLAY) {
        out[IN_SETTING] = (float)SETTING_AMBI_LEVEL;
        // Whole percent
        out[IN_SETTING_VALUE] = roundf(ctx->ambiIntensity * 100.0f);
    }
    else if (face == COG_TAB_PICTURE && slider < PICTURE_VALUES) {
        out[IN_SETTING] = (float)PICTURE_SETTINGS[slider];
        out[IN_SETTING_VALUE] = (float)ctx->pictureUnits[slider];
        LOGEV("picture %s %d from the panel, grade %s", PICTURE_NAMES[slider],
              ctx->pictureUnits[slider], ctx->gradeOn ? "on" : "off");
    }
}

// Step button press: one step, applied and stored at once. No auto-repeat.
void cogStepTrack(XrCtx* ctx, int face, int row, int dir, float* out) {
    int steps = cogTrackSteps(face, row);
    if (steps <= 0) {
        return;
    }
    float before = cogSliderValue(ctx, face, row);
    int step = cogStepIndex(before, steps, dir);
    cogApplySlider(ctx, face, row, cogRunU((float)step / (float)steps));
    float after = cogSliderValue(ctx, face, row);
    LOGEV("track step %s: face %d row %d, step %.2f to %.2f of %d", dir > 0 ? "up" : "down",
          face, row, before * steps, after * steps, steps);
    ctx->cogDragSlider = row;
    ctx->cogDragFace = face;
    ctx->cogDragHand = -1;
    cogDragEnded(ctx, out);
}

// Cell under the ray, or -1 past the ends
int cogCellAt(float pu, int cells) {
    if (pu < COG_TRACK_L || pu > COG_TRACK_R) {
        return -1;
    }
    int cell = (int)((pu - COG_TRACK_L) / (COG_TRACK_R - COG_TRACK_L) * cells);
    if (cell < 0) cell = 0;
    if (cell >= cells) cell = cells - 1;
    return cell;
}

// Readout strip values in IN_READOUT order: tab (or -1), then one per row, -1 past the
// rows. Room size is -1 in fixed-size rooms; Screen is empty while head aim is dead.
void cogReadouts(XrCtx* ctx, int* values) {
    for (int i = 0; i < READOUT_VALUES; i++) {
        values[i] = -1;
    }
    // Kept during the fade-out
    int showing = ctx->cogOpen || ctx->panelFades[FADE_COG].level > 0.0f;
    if (!showing) {
        return;
    }
    int face = cogFace(ctx);
    if (face == COG_TAB_PICTURE) {
        values[0] = READOUT_PICTURE;
        for (int row = 0; row < PICTURE_VALUES; row++) {
            values[1 + row] = ctx->pictureUnits[row];
        }
        return;
    }
    // Head aim values only while live
    if (face == COG_TAB_SCREEN) {
        if (cogRowLive(ctx, face, COG_SLIDER_AIM_SENSITIVITY)) {
            values[0] = READOUT_SCREEN;
            values[1] = ctx->headAimSensitivity;
            values[2] = ctx->headAimDeadZone;
        }
        return;
    }
    int style = roomFaceStyle(ctx);
    if (face != COG_FACE_ROOM || style == 0) {
        return;
    }
    values[0] = READOUT_ROOM;
    values[1] = lanePercent(ctx->roomBrightness[style], ROOM_BRIGHTNESS_MIN,
                            ROOM_BRIGHTNESS_MAX);
    values[2] = lanePercent(ctx->roomLightLevel[style], ROOM_LIGHT_MIN, ROOM_LIGHT_MAX);
    values[3] = roomResizable(style) ? roomScreenPercent(ctx, style) : -1;
}

// Clamp to the separation track
static int onSeparationTrack(int units) {
    return units < 0 ? 0 : (units > COG_SEP_STEPS ? COG_SEP_STEPS : units);
}

// Model's default separation/convergence and preset separations, in stored units.
// Before the first frame. The 3D tab's reset returns to the defaults.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetDepthDefaults(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jint separation,
                                                                   jint convergence,
                                                                   jintArray presets) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    int units = onSeparationTrack(separation);
    int percent = convergence < 0 ? 0 : (convergence > 100 ? 100 : convergence);
    ctx->defaultSeparation = separationOf(units);
    ctx->defaultConvergence = percent / 100.0f;
    if (presets != NULL && (*env)->GetArrayLength(env, presets) >= COG_PRESET_CELLS) {
        int values[COG_PRESET_CELLS];
        (*env)->GetIntArrayRegion(env, presets, 0, COG_PRESET_CELLS, values);
        for (int i = 0; i < COG_PRESET_CELLS; i++) {
            ctx->presetUnits[i] = onSeparationTrack(values[i]);
        }
    }
    LOGEV("3d defaults: separation %d, convergence %d, presets %d %d %d", units, percent,
          ctx->presetUnits[COG_PRESET_COMFORT], ctx->presetUnits[COG_PRESET_BALANCED],
          ctx->presetUnits[COG_PRESET_STRONG]);
}

// Click sound setting, for the display tab's mark. Before the first frame.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetClickSound(JNIEnv* env, jobject thiz,
                                                                jlong handle, jboolean on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx != NULL) {
        ctx->clickSoundOn = on;
    }
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

// Head aim start state, px/degree and dead zone deg/s, clamped. Before the first frame.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetHeadAim(JNIEnv* env, jobject thiz,
                                                             jlong handle, jboolean on,
                                                             jint sensitivity, jint deadZone) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->headAimSetting = on ? 1 : 0;
    ctx->headAimFlipped = 0;
    ctx->headAimSensitivity = headAimSensitivityClamp(sensitivity);
    ctx->headAimDeadZone = headAimDeadZoneClamp(deadZone);
    LOGEV("head aim %s at the start of the session, %d px a degree, dead zone %d deg/s",
          on ? "on" : "off", ctx->headAimSensitivity, ctx->headAimDeadZone);
}

// Gamepad shortcut name for the toast
const char* padShortcutName(int shortcut) {
    switch (shortcut) {
        case PAD_SHORTCUT_STICKS: return "both thumbsticks";
        case PAD_SHORTCUT_TRIGGERS_GRIPS: return "both triggers and both grips";
        default: return "the left menu button and grip";
    }
}

// Gamepad shortcut and stick dead zone (percent). Before the first frame. Sessions
// always start in pointer mode.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetGamepad(JNIEnv* env, jobject thiz,
                                                             jlong handle, jint shortcut,
                                                             jint deadzonePercent) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->padMode = 0;
    ctx->padShortcut = shortcut == PAD_SHORTCUT_STICKS || shortcut == PAD_SHORTCUT_TRIGGERS_GRIPS
            ? shortcut : PAD_SHORTCUT_MENU_GRIP;
    ctx->padDeadzone = padDeadzoneFromPercent(deadzonePercent);
    LOGEV("controllers start as the pointer, gamepad mode shortcut %s, stick dead zone %.0f%%",
          padShortcutName(ctx->padShortcut), ctx->padDeadzone * 100.0f);
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

// Picture tab reset: all defaults, grade off
void pictureReset(XrCtx* ctx) {
    for (int row = 0; row < PICTURE_VALUES; row++) {
        pictureSet(ctx, row, pictureDefault(row));
    }
    LOGEV("picture reset from the panel, grade %s", ctx->gradeOn ? "on" : "off");
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
