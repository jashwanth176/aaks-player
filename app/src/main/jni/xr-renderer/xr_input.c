// OpenXR input: controller and hand actions, the pointer ray, and the per-frame
// pass that turns it into hover, grab, panel presses and mouse events.
#include "xr_renderer.h"
#include "xr_handles.h"

// Pinch comes from the runtime's aim flags where offered, otherwise from the
// thumb-to-index joint distance (see xr_pinch.h).

static void initJointTracking(XrCtx* ctx) {
    if (!ctx->handTracking) {
        return;
    }
    if (XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrCreateHandTrackerEXT",
                                        (PFN_xrVoidFunction*)&ctx->pfnCreateHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrDestroyHandTrackerEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnDestroyHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrLocateHandJointsEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnLocateHandJoints))
            || ctx->pfnCreateHandTracker == NULL || ctx->pfnLocateHandJoints == NULL) {
        LOGW("hand joint entry points missing");
        ctx->jointTracking = 0;
        return;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrHandTrackerCreateInfoEXT info = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
        info.hand = h == HAND_LEFT ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (!checkXr(ctx->pfnCreateHandTracker(ctx->session, &info, &ctx->handTrackers[h]),
                     "create hand tracker")) {
            ctx->handTrackers[h] = XR_NULL_HANDLE;
            return;
        }
    }
    ctx->jointTracking = 1;
    ctx->handAim = ctx->handAimOffered;
    loadHandMeshes(ctx);
    LOGI("reading hand joints for pinch%s",
         ctx->handAim ? ", with the runtime's pinch flags beside them" : "");
}

// Records whether each hand is a controller or a tracked hand; hands wake the
// pointer by pinch rather than by the movement gate.
void refreshInputSource(XrCtx* ctx) {
    if (ctx->session == XR_NULL_HANDLE || !ctx->inputReady) {
        return;
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_FAILED(xrGetCurrentInteractionProfile(ctx->session, ctx->handPaths[h], &state))) {
            continue;
        }
        XrPath profile = state.interactionProfile;
        int kind = profileKind(profile != XR_NULL_PATH,
                               profile == ctx->handProfile || profile == ctx->msftHandProfile);
        // Without a bound pinch, hands fall back to the movement gate.
        ctx->usingHands[h] = ctx->handClickOk && kind == PROFILE_HANDS;
        ctx->onExtHands[h] = kind == PROFILE_HANDS && profile == ctx->handProfile;
        ctx->onMsftHands[h] = kind == PROFILE_HANDS && profile == ctx->msftHandProfile;
        if (kind != ctx->profileKind[h]) {
            ctx->profileKind[h] = kind;
            controllerClockReset(&ctx->aimClock[h]);
            LOGI("hand %d profile: %s", h, kind == PROFILE_CONTROLLER ? "controller"
                                           : (kind == PROFILE_HANDS ? "hands" : "none"));
        }
    }
}

static XrPath toPath(XrCtx* ctx, const char* str) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(ctx->instance, str, &path);
    return path;
}

static XrAction makeAction(XrCtx* ctx, XrActionType type, const char* name, const char* label) {
    XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
    info.actionType = type;
    strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.countSubactionPaths = HAND_COUNT;
    info.subactionPaths = ctx->handPaths;

    XrAction action = XR_NULL_HANDLE;
    if (!checkXr(xrCreateAction(ctx->actionSet, &info, &action), name)) {
        return XR_NULL_HANDLE;
    }
    return action;
}

// One unsupported path rejects a whole profile, so on rejection this retries with aim
// and trigger only.
static void suggestBindings(XrCtx* ctx, const char* profile, int full) {
    XrActionSuggestedBinding b[24];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };
    // x and y on the left controller, a and b on the right
    static const char* rightClick[HAND_COUNT] = { "input/x/click", "input/a/click" };
    static const char* middleClick[HAND_COUNT] = { "input/y/click", "input/b/click" };
    int simple = strstr(profile, "/khr/") != NULL;

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/input/aim/pose", hands[h]);
        b[n].action = ctx->aimAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h],
                 simple ? "input/select/click" : "input/trigger/value");
        b[n].action = ctx->triggerAction;
        b[n++].binding = toPath(ctx, path);

        // Grip pose places the controller model; omitted from the fallback set.
        if (full && ctx->gripAction != XR_NULL_HANDLE) {
            snprintf(path, sizeof(path), "%s/input/grip/pose", hands[h]);
            b[n].action = ctx->gripAction;
            b[n++].binding = toPath(ctx, path);
        }

        if (!full || simple) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", hands[h], rightClick[h]);
        b[n].action = ctx->rightClickAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h], middleClick[h]);
        b[n].action = ctx->middleClickAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick", hands[h]);
        b[n].action = ctx->scrollAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick/click", hands[h]);
        b[n].action = ctx->toggleAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/squeeze/value", hands[h]);
        b[n].action = ctx->grabAction;
        b[n++].binding = toPath(ctx, path);
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    XrResult res = xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
    if (XR_SUCCEEDED(res)) {
        LOGI("bindings accepted for %s (%s)", profile, full ? "full" : "reduced");
    }
    else if (full) {
        LOGW("full bindings rejected for %s (%d), trying aim and trigger only", profile, res);
        suggestBindings(ctx, profile, 0);
    }
    else {
        LOGW("bindings rejected for %s (%d)", profile, res);
    }
}

// Hands use their own actions: some runtimes cannot serve one action from both
// a controller profile and a hand profile.
static XrResult trySuggestHands(XrCtx* ctx, const char* profile, const char* aim,
                                const char* click, const char* grasp) {
    XrActionSuggestedBinding b[6];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };

    if (ctx->handAimAction == XR_NULL_HANDLE
            || (click != NULL && ctx->handPinchAction == XR_NULL_HANDLE)
            || (grasp != NULL && ctx->handGraspAction == XR_NULL_HANDLE)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/%s", hands[h], aim);
        b[n].action = ctx->handAimAction;
        b[n++].binding = toPath(ctx, path);

        if (click != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], click);
            b[n].action = ctx->handPinchAction;
            b[n++].binding = toPath(ctx, path);
        }

        if (grasp != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], grasp);
            b[n].action = ctx->handGraspAction;
            b[n++].binding = toPath(ctx, path);
        }
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    return xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
}

// Not every runtime implements every hand input, and one unsupported path
// rejects the whole suggestion, so sets are tried in falling order of
// usefulness. Returns whether a pinch was bound.
static int suggestHandBindings(XrCtx* ctx, const char* profile, const char* aim,
                               const char* const* clicks, int clickCount,
                               const char* grasp) {
    XrResult res = XR_SUCCESS;
    for (int c = 0; c < clickCount; c++) {
        if (grasp != NULL) {
            res = trySuggestHands(ctx, profile, aim, clicks[c], grasp);
            if (XR_SUCCEEDED(res)) {
                LOGEV("hand bindings accepted for %s (%s and grasp)", profile, clicks[c]);
                return 1;
            }
        }
        res = trySuggestHands(ctx, profile, aim, clicks[c], NULL);
        if (XR_SUCCEEDED(res)) {
            LOGEV("hand bindings accepted for %s (%s)", profile, clicks[c]);
            return 1;
        }
    }
    res = trySuggestHands(ctx, profile, aim, NULL, NULL);
    if (XR_SUCCEEDED(res)) {
        LOGW("only the aim pose bound for %s, so hands cannot click", profile);
        return 0;
    }
    LOGW("hand bindings rejected for %s, even the aim pose alone (%d)", profile, res);
    return 0;
}

int initXrInput(XrCtx* ctx) {
    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(setInfo.actionSetName, "aaks", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(setInfo.localizedActionSetName, "AaksVR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!checkXr(xrCreateActionSet(ctx->instance, &setInfo, &ctx->actionSet), "create action set")) {
        return 0;
    }

    ctx->handPaths[HAND_LEFT] = toPath(ctx, "/user/hand/left");
    ctx->handPaths[HAND_RIGHT] = toPath(ctx, "/user/hand/right");

    ctx->aimAction = makeAction(ctx, XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer");
    ctx->triggerAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Left click");
    ctx->rightClickAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "rightclick", "Right click");
    ctx->middleClickAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "middleclick", "Middle click");
    ctx->scrollAction = makeAction(ctx, XR_ACTION_TYPE_VECTOR2F_INPUT, "scroll", "Scroll");
    ctx->grabAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "grab", "Move the screen");
    ctx->toggleAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "pointertoggle", "Pointer on or off");
    // Hand actions, see trySuggestHands.
    ctx->handAimAction = makeAction(ctx, XR_ACTION_TYPE_POSE_INPUT, "handaim", "Hand pointer");
    ctx->handPinchAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "handpinch", "Pinch to click");
    ctx->handGraspAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "handgrasp",
                                      "Grasp to move the screen");
    // Controller model placement only; hands never bind it.
    ctx->gripAction = makeAction(ctx, XR_ACTION_TYPE_POSE_INPUT, "grip", "Controller");

    if (ctx->aimAction == XR_NULL_HANDLE || ctx->triggerAction == XR_NULL_HANDLE) {
        return 0;
    }

    // The simple controller has no stick or face buttons: aim, trigger and grip only.
    suggestBindings(ctx, "/interaction_profiles/khr/simple_controller", 1);
    suggestBindings(ctx, "/interaction_profiles/oculus/touch_controller", 1);
    if (ctx->picoInteraction) {
        // One extension covers both; the Neo 3 needs its own suggestion or it
        // falls back to the simple controller.
        suggestBindings(ctx, "/interaction_profiles/bytedance/pico4_controller", 1);
        suggestBindings(ctx, "/interaction_profiles/bytedance/pico_neo3_controller", 1);
    }

    // Gaze is its own top-level path, so it needs its own action. It has no
    // click; the runtime's trigger (usually a pinch) does the clicking.
    if (ctx->eyeGaze) {
        XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strncpy(info.actionName, "gaze", XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(info.localizedActionName, "Gaze pointer",
                XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        if (checkXr(xrCreateAction(ctx->actionSet, &info, &ctx->gazeAction), "gaze action")) {
            XrActionSuggestedBinding b;
            b.action = ctx->gazeAction;
            b.binding = toPath(ctx, "/user/eyes_ext/input/gaze_ext/pose");

            XrInteractionProfileSuggestedBinding suggest = {
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
            };
            suggest.interactionProfile = toPath(ctx,
                    "/interaction_profiles/ext/eye_gaze_interaction");
            suggest.countSuggestedBindings = 1;
            suggest.suggestedBindings = &b;
            if (XR_FAILED(xrSuggestInteractionProfileBindings(ctx->instance, &suggest))) {
                LOGW("gaze bindings rejected");
                ctx->gazeAction = XR_NULL_HANDLE;
                ctx->eyeGaze = 0;
            }
        }
        else {
            ctx->gazeAction = XR_NULL_HANDLE;
            ctx->eyeGaze = 0;
        }
    }

    // EXT hand profile only when the Microsoft one is absent: Quest offers both,
    // pinch is tuned on the Microsoft one, and a runtime may switch to EXT if
    // both are suggested. Pico 4 Ultra offers only EXT.
    if (ctx->handInteraction && !ctx->msftHandInteraction) {
        // pinch_ext first (works on Pico 4 Ultra), then aim_activate_ext.
        // The aim pose has no _ext suffix.
        static const char* const clicks[] = {
            "input/pinch_ext/value", "input/aim_activate_ext/value"
        };
        const char* profile = "/interaction_profiles/ext/hand_interaction_ext";
        ctx->extHandClick = suggestHandBindings(ctx, profile, "input/aim/pose",
                                                clicks, 2, "input/grasp_ext/value");
        ctx->handClickOk |= ctx->extHandClick;
        ctx->handProfile = toPath(ctx, profile);
        LOGEV("hands on the EXT hand interaction profile, the Microsoft one not offered");
    }
    else if (ctx->handInteraction) {
        LOGEV("hands on the Microsoft hand interaction profile, the EXT one offered too "
              "and left unsuggested");
    }
    else if (ctx->msftHandInteraction) {
        LOGEV("hands on the Microsoft hand interaction profile, the only one offered");
    }
    else if (ctx->handsEnabled) {
        LOGEV("no hand interaction profile offered, hands pinch off the joints if at all");
    }
    // Microsoft hand profile, for runtimes that predate the EXT one.
    if (ctx->msftHandInteraction) {
        static const char* const clicks[] = { "input/select/value" };
        const char* profile = "/interaction_profiles/microsoft/hand_interaction";
        ctx->msftHandClick = suggestHandBindings(ctx, profile, "input/aim/pose",
                                                 clicks, 1, "input/squeeze/value");
        ctx->handClickOk |= ctx->msftHandClick;
        ctx->msftHandProfile = toPath(ctx, profile);
    }

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &ctx->actionSet;
    if (!checkXr(xrAttachSessionActionSets(ctx->session, &attach), "attach action sets")) {
        return 0;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->aimAction;
        spaceInfo.subactionPath = ctx->handPaths[h];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[h]),
                     "create aim space")) {
            return 0;
        }
    }

    // Optional: without a hand aim space the joint-built ray is used.
    for (int h = 0; h < HAND_COUNT && ctx->handAimAction != XR_NULL_HANDLE; h++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->handAimAction;
        spaceInfo.subactionPath = ctx->handPaths[h];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->handAimSpaces[h]),
                     "create hand aim space")) {
            ctx->handAimSpaces[h] = XR_NULL_HANDLE;
        }
    }

    // Optional: without a grip space only the controller model is lost.
    for (int h = 0; h < HAND_COUNT && ctx->gripAction != XR_NULL_HANDLE; h++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->gripAction;
        spaceInfo.subactionPath = ctx->handPaths[h];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->gripSpaces[h]),
                     "create grip space")) {
            ctx->gripSpaces[h] = XR_NULL_HANDLE;
        }
    }

    if (ctx->gazeAction != XR_NULL_HANDLE) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->gazeAction;
        spaceInfo.subactionPath = XR_NULL_PATH;
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[SRC_GAZE]),
                     "create gaze space")) {
            ctx->aimSpaces[SRC_GAZE] = XR_NULL_HANDLE;
        }
    }

    ctx->inputReady = 1;
    ctx->pointerOn = 1;
    initJointTracking(ctx);
    refreshInputSource(ctx);
    LOGI("controller input ready (pico bindings %s, hand pinch %s)",
         ctx->picoInteraction ? "offered" : "not offered by this runtime",
         ctx->handClickOk ? "bound" : (ctx->jointTracking ? "from joints" : "unavailable"));
    return 1;
}

static float actionFloat(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0.0f;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(ctx->session, &get, &state)) || !state.isActive) {
        return 0.0f;
    }
    return state.currentState;
}

static int actionBool(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(ctx->session, &get, &state)) || !state.isActive) {
        return 0;
    }
    return state.currentState != 0;
}

static XrVector2f actionVec2(XrCtx* ctx, XrAction action, int hand) {
    XrVector2f zero = { 0.0f, 0.0f };
    if (action == XR_NULL_HANDLE) {
        return zero;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(ctx->session, &get, &state)) || !state.isActive) {
        return zero;
    }
    return state.currentState;
}

// Inactive when the controller is off, asleep or absent.
static int actionPoseActive(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = ctx->handPaths[hand];

    XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
    return XR_SUCCEEDED(xrGetActionStatePose(ctx->session, &get, &state)) && state.isActive;
}

static int stickPushed(XrVector2f stick) {
    return fabsf(stick.x) > SCROLL_DEADZONE || fabsf(stick.y) > SCROLL_DEADZONE;
}

// Whether a hand reads the hand actions rather than the controller ones.
static int onHandActions(XrCtx* ctx, int h) {
    return h < HAND_COUNT && ctx->profileKind[h] == PROFILE_HANDS;
}

// Inputs as the pointer sees them. Hands pinch in place of the trigger and never
// grip: a relaxed hand reads as half a grasp, which would grab what it points at.
static float seenTrigger(XrCtx* ctx, int h) {
    XrAction action = onHandActions(ctx, h) ? ctx->handPinchAction : ctx->triggerAction;
    return actionFloat(ctx, action, h);
}

static float seenGrip(XrCtx* ctx, int h) {
    if (onHandActions(ctx, h)) {
        return 0.0f;
    }
    return actionFloat(ctx, ctx->grabAction, h);
}

static XrVector2f seenStick(XrCtx* ctx, int h) {
    return actionVec2(ctx, ctx->scrollAction, h);
}

// Pointer ray from the joints, for runtimes without a hand aim pose. Cast from
// an estimated shoulder through the index knuckle: steadier than a finger ray.
static void buildHandRay(XrCtx* ctx, int hand, const XrPosef* head,
                         const XrHandJointLocationEXT* joints) {
    const XrHandJointLocationEXT* knuckle = &joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
    if (!(knuckle->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ctx->handRayValid[hand] = 0;
        return;
    }

    Vec3 offset = { hand == HAND_RIGHT ? 0.17f : -0.17f, -0.20f, 0.05f };
    Vec3 shoulder = quatRotate(head->orientation, offset);
    shoulder.x += head->position.x;
    shoulder.y += head->position.y;
    shoulder.z += head->position.z;

    Vec3 origin = { knuckle->pose.position.x, knuckle->pose.position.y,
                    knuckle->pose.position.z };
    Vec3 dir = vecSub(origin, shoulder);
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 0.05f) {
        ctx->handRayValid[hand] = 0;
        return;
    }
    dir = vecNorm(dir);

    // A pose points down its -Z.
    Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
    Vec3 rayZ = { -dir.x, -dir.y, -dir.z };
    Vec3 rayX = vecCross(worldUp, rayZ);
    float side = sqrtf(rayX.x * rayX.x + rayX.y * rayX.y + rayX.z * rayX.z);
    if (side < 0.01f) {
        Vec3 fallback = { 1.0f, 0.0f, 0.0f };
        rayX = vecCross(fallback, rayZ);
    }
    rayX = vecNorm(rayX);
    Vec3 rayY = vecCross(rayZ, rayX);

    ctx->handRay[hand].orientation = quatFromBasis(rayX, rayY, rayZ);
    ctx->handRay[hand].position = knuckle->pose.position;
    ctx->handRayValid[hand] = 1;
}

// Moves a local-space pose into the screen's frame (the head's, when head
// locked). Done here rather than by locating against the view space, which
// lost the pointer on Pico 4 Ultra (#20). Fails if the head is not located.
typedef struct {
    int toHead;
    int ok;
    XrPosef head;
} FrameXform;

static int intoFrame(const FrameXform* x, XrPosef* p) {
    if (!x->toHead) {
        return 1;
    }
    if (!x->ok) {
        return 0;
    }
    *p = poseInFrame(x->head, *p);
    return 1;
}

// Minimum fraction of tracked joints for a hand to be used.
#define HAND_TRUST_MIN 0.35f
// How long the last good aim, and a held pinch, survive a tracking gap.
#define POINTER_HOLD_NS 250000000LL
#define PINCH_HOLD_NS 300000000LL

// Reads one hand's joints: ray, thumb-index gap, pinch point, palm, and the
// runtime's pinch flag where offered.
static void readJoints(XrCtx* ctx, int hand, const FrameXform* xform, const XrPosef* head,
                       int headValid) {
    ctx->tipsValid[hand] = 0;
    ctx->indexTipValid[hand] = 0;
    ctx->handJointsOk[hand] = 0;
    ctx->handLostWhy[hand] = 1;
    ctx->aimPinch[hand] = 0;
    ctx->handTrust[hand] = 0.0f;
    ctx->palmShown[hand] = 0;
    if (!ctx->jointTracking || ctx->handTrackers[hand] == XR_NULL_HANDLE) {
        ctx->pinchPointValid[hand] = 0;
        ctx->handRayValid[hand] = 0;
        return;
    }

    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
    locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    locations.jointLocations = joints;
    XrHandTrackingAimStateFB aim = { XR_TYPE_HAND_TRACKING_AIM_STATE_FB };
    if (ctx->handAim) {
        locations.next = &aim;
    }
    // The hand's size, for the outline
    XrHandTrackingScaleFB scale = { XR_TYPE_HAND_TRACKING_SCALE_FB };
    scale.overrideHandScale = XR_FALSE;
    if (ctx->handMeshOffered) {
        scale.next = locations.next;
        locations.next = &scale;
    }

    XrHandJointsLocateInfoEXT locate = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
    locate.baseSpace = ctx->localSpace;
    locate.time = ctx->predictedDisplayTime;
    int located = XR_SUCCEEDED(ctx->pfnLocateHandJoints(ctx->handTrackers[hand], &locate,
                                                         &locations))
            && locations.isActive;
    if (located && ctx->handMeshOffered && scale.currentOutput > 0.5f && scale.currentOutput < 2.0f) {
        float was = ctx->handScale[hand];
        ctx->handScale[hand] = was > 0.0f ? was + (scale.currentOutput - was) * HAND_SCALE_FOLLOW
                                          : scale.currentOutput;
        if (fabsf(ctx->handScale[hand] - ctx->handScaleSaid[hand]) > 0.03f) {
            LOGI("hand %d is %.2f times the mesh's size", hand, ctx->handScale[hand]);
            ctx->handScaleSaid[hand] = ctx->handScale[hand];
        }
    }
    // During the system gesture the hand neither points nor pinches, but its
    // joints are still read for the palm check.
    int systemGesture = 0;
    if (located && ctx->handAim) {
        ctx->aimPinch[hand] = (aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB)
                && (aim.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB);
        systemGesture = (aim.status & (XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB
                                       | XR_HAND_TRACKING_AIM_MENU_PRESSED_BIT_FB)) != 0;
        if (systemGesture) {
            ctx->aimPinch[hand] = 0;
        }
    }
    // Kept in local space for the outline, before the joints move into the screen's frame
    int whole = located;
    const XrSpaceLocationFlags posed = XR_SPACE_LOCATION_POSITION_VALID_BIT
            | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    for (int j = 0; located && j < XR_HAND_JOINT_COUNT_EXT; j++) {
        ctx->handJoints[hand][j] = joints[j].pose;
        whole = whole && (joints[j].locationFlags & posed) == posed;
    }
    for (int j = 0; located && j < XR_HAND_JOINT_COUNT_EXT; j++) {
        located = intoFrame(xform, &joints[j].pose);
    }
    if (!located) {
        ctx->pinchPointValid[hand] = 0;
        ctx->handRayValid[hand] = 0;
        return;
    }

    // Runtimes keep reporting joints for a lost or barely seen hand, so require
    // enough tracked joints and a plausible hand size.
    int tracked = 0;
    int valid = 0;
    for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
        if (joints[j].locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) {
            tracked++;
        }
        if (joints[j].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
            valid++;
        }
    }
    if (tracked > 0) {
        ctx->jointsReportTracked = 1;
    }
    float trust = (ctx->jointsReportTracked ? tracked : valid) / (float)XR_HAND_JOINT_COUNT_EXT;
    const XrHandJointLocationEXT* wrist = &joints[XR_HAND_JOINT_WRIST_EXT];
    const XrHandJointLocationEXT* middle = &joints[XR_HAND_JOINT_MIDDLE_TIP_EXT];
    float sx = wrist->pose.position.x - middle->pose.position.x;
    float sy = wrist->pose.position.y - middle->pose.position.y;
    float sz = wrist->pose.position.z - middle->pose.position.z;
    float span = sqrtf(sx * sx + sy * sy + sz * sz);
    int sane = (wrist->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            && (middle->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            && span > 0.06f && span < 0.30f;
    // A still or half-seen hand keeps valid joints with few marked tracked: too doubtful
    // to point or pinch with, but still drawn, as the system draws it
    ctx->handJointsOk[hand] = whole && sane;
    if (!sane || trust < HAND_TRUST_MIN) {
        ctx->handLostWhy[hand] = sane ? 2 : 3;
        ctx->aimPinch[hand] = 0;
        ctx->pinchPointValid[hand] = 0;
        ctx->handRayValid[hand] = 0;
        return;
    }
    ctx->handTrust[hand] = trust;
    ctx->handLostWhy[hand] = 0;

    if (headValid) {
        PalmHand palm;
        palm.palm = joints[XR_HAND_JOINT_PALM_EXT].pose;
        static const int tips[4] = { XR_HAND_JOINT_INDEX_TIP_EXT, XR_HAND_JOINT_MIDDLE_TIP_EXT,
                                     XR_HAND_JOINT_RING_TIP_EXT, XR_HAND_JOINT_LITTLE_TIP_EXT };
        for (int i = 0; i < 4; i++) {
            XrVector3f t = joints[tips[i]].pose.position;
            palm.tips[i].x = t.x;
            palm.tips[i].y = t.y;
            palm.tips[i].z = t.z;
        }
        XrVector3f thumb = joints[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position;
        palm.thumbTip.x = thumb.x;
        palm.thumbTip.y = thumb.y;
        palm.thumbTip.z = thumb.z;
        ctx->palmShown[hand] = palmShown(&palm, *head,
                systemGesture && (aim.status & XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB));
    }
    if (systemGesture) {
        ctx->pinchPointValid[hand] = 0;
        ctx->handRayValid[hand] = 0;
        return;
    }

    const XrHandJointLocationEXT* fingertip = &joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    ctx->indexTipValid[hand] = (fingertip->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    ctx->indexTip[hand] = fingertip->pose.position;
    ctx->indexTipRadius[hand] = fingertip->radius;

    if (headValid) {
        buildHandRay(ctx, hand, head, joints);
    }
    else {
        ctx->handRayValid[hand] = 0;
    }

    const XrHandJointLocationEXT* thumb = &joints[XR_HAND_JOINT_THUMB_TIP_EXT];
    const XrHandJointLocationEXT* index = &joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    if (!(thumb->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            || !(index->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ctx->pinchPointValid[hand] = 0;
        return;
    }

    float dx = thumb->pose.position.x - index->pose.position.x;
    float dy = thumb->pose.position.y - index->pose.position.y;
    float dz = thumb->pose.position.z - index->pose.position.z;
    ctx->tipGap[hand] = sqrtf(dx * dx + dy * dy + dz * dz);
    ctx->tipsValid[hand] = 1;

    // Midpoint of the tips; drags follow it.
    ctx->pinchPoint[hand].x = (thumb->pose.position.x + index->pose.position.x) * 0.5f;
    ctx->pinchPoint[hand].y = (thumb->pose.position.y + index->pose.position.y) * 0.5f;
    ctx->pinchPoint[hand].z = (thumb->pose.position.z + index->pose.position.z) * 0.5f;
    ctx->pinchPointValid[hand] = 1;
}

// Reseeds the screen placement from the sliders on first call or when a slider
// moves. Returns whether it reseeded.
int updatePlacement(XrCtx* ctx, float distance, float quadWidth, float curvature) {
    int sliderMoved = ctx->sliderSeen
            && (fabsf(distance - ctx->lastDistance) > 1e-4f
                || fabsf(quadWidth - ctx->lastQuadWidth) > 1e-4f);
    int reseeded = !ctx->placementValid || sliderMoved;

    if (reseeded) {
        memset(&ctx->screenPose, 0, sizeof(ctx->screenPose));
        ctx->screenPose.orientation.w = 1.0f;
        ctx->screenPose.position.z = -distance;
        // The library window starts narrower than a video screen.
        ctx->screenWidth = ctx->playerScreenHidden
                ? quadWidth * PLAYER_BROWSE_START_WIDTH : quadWidth;
        // Radius goes from 4x distance at curvature 0 to 1x distance at 1.
        ctx->screenRadius = distance * (1.0f + 3.0f * (1.0f - curvature));
        ctx->placementValid = 1;
        ctx->grabMode = GRAB_NONE;
        ctx->poseDirty = 1;
    }

    ctx->lastDistance = distance;
    ctx->lastQuadWidth = quadWidth;
    ctx->sliderSeen = 1;
    return reseeded;
}

// Turns the screen to face the new forward, keeping distance, size and curve.
void recentreScreen(XrCtx* ctx) {
    ctx->grabMode = GRAB_NONE;
    if (!ctx->placementValid) {
        LOGI("recentred before the screen was placed");
        return;
    }
    XrPosef was = ctx->screenPose;
    ctx->screenPose = poseRecentred(was);
    ctx->poseDirty = 1;
    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 fwd = quatRotate(was.orientation, back);
    XrVector3f p = ctx->screenPose.position;
    LOGEV("recentred: screen from %.2f %.2f %.2f, turned %.1f deg, to %.2f %.2f %.2f;"
          " kept distance %.2f m, width %.2f m, radius %.2f m, curve %.2f, head lock %d",
          was.position.x, was.position.y, was.position.z,
          atan2f(fwd.x, fwd.z) * 180.0f / (float)M_PI, p.x, p.y, p.z,
          sqrtf(p.x * p.x + p.y * p.y + p.z * p.z), ctx->screenWidth, ctx->screenRadius,
          effectiveCurvature(ctx), ctx->headLockedPref);
}

// The pose every frame, for a save at a film switch; the dirty flag once per
// grab, so preferences are written once per move.
static void writeInputPose(XrCtx* ctx, float* out) {
    out[IN_POSE_DIRTY] = ctx->poseDirty ? 1.0f : 0.0f;
    ctx->poseDirty = 0;
    out[IN_POSE + 0] = ctx->screenPose.position.x;
    out[IN_POSE + 1] = ctx->screenPose.position.y;
    out[IN_POSE + 2] = ctx->screenPose.position.z;
    out[IN_POSE + 3] = ctx->screenPose.orientation.x;
    out[IN_POSE + 4] = ctx->screenPose.orientation.y;
    out[IN_POSE + 5] = ctx->screenPose.orientation.z;
    out[IN_POSE + 6] = ctx->screenPose.orientation.w;
    out[IN_POSE + 7] = ctx->screenWidth;
    out[IN_POSE + 8] = ctx->screenRadius;
    out[IN_POSE + 9] = ctx->panelCurve;
}

// Projection of a point onto the centre-to-held-corner half diagonal, as at
// grab start: 0 at the centre, 1 at the corner. Keeps the corner under the ray.
static float diagonalReachAt(XrCtx* ctx, float px, float py) {
    float halfX = -ctx->grabOppX;
    float halfY = -ctx->grabOppY;
    float halfLen = halfX * halfX + halfY * halfY;
    return halfLen > 0.0f ? (px * halfX + py * halfY) / halfLen : 1.0f;
}

static float diagonalReach(XrCtx* ctx, float u, float v) {
    return diagonalReachAt(ctx, (u - 0.5f) * ctx->grabWidth, (0.5f - v) * ctx->grabHeight);
}

// The hand that pinched for a gaze click, right first, or -1.
static int pinchingHand(XrCtx* ctx) {
    if (ctx->triggerEdge[HAND_RIGHT]) {
        return HAND_RIGHT;
    }
    if (ctx->triggerEdge[HAND_LEFT]) {
        return HAND_LEFT;
    }
    if (ctx->triggerDown[HAND_RIGHT]) {
        return HAND_RIGHT;
    }
    if (ctx->triggerDown[HAND_LEFT]) {
        return HAND_LEFT;
    }
    return -1;
}

// Whether a pinch point is still being followed. With a runtime pinch, the
// runtime decides the pinch and the joints only give the point.
static int pinchTracked(XrCtx* ctx, int hand) {
    if (hand < 0 || hand >= HAND_COUNT || !ctx->pinchPointValid[hand]) {
        return 0;
    }
    if (ctx->pinchSrc[hand] != PINCH_SRC_JOINTS) {
        return ctx->triggerDown[hand];
    }
    return ctx->pinchDown[hand];
}

// Drag hand position: the pinch point, else the aim pose.
static Vec3 gazeHandPos(XrCtx* ctx, const XrPosef* aims, int hand) {
    if (pinchTracked(ctx, hand)) {
        return ctx->pinchPoint[hand];
    }
    Vec3 p = { aims[hand].position.x, aims[hand].position.y, aims[hand].position.z };
    return p;
}

// Gaze picks the target, the pinching hand moves it. Returns that hand with its
// start position and drag gearing, or -1 if nothing is pinching.
static int gazeDragHand(XrCtx* ctx, const XrPosef* aims, const int* valid, Vec3 target,
                        Vec3* outStart, float* outScale) {
    int hand = pinchingHand(ctx);
    if (hand < 0 || (!valid[hand] && !pinchTracked(ctx, hand))) {
        return -1;
    }
    Vec3 handPos = gazeHandPos(ctx, aims, hand);
    Vec3 head = { ctx->headPos.x, ctx->headPos.y, ctx->headPos.z };
    *outStart = handPos;
    *outScale = dragScale(head, target, handPos);
    return hand;
}

// Hand travel for a gaze drag, eased in from the pinch and held while the head turns.
static Vec3 gazeDragCarry(XrCtx* ctx, DragRamp* ramp, Vec3 travel, int64_t nowNs) {
    int held = 0;
    Vec3 carry = dragRampStep(ramp, travel, ctx->headTurnRate, nowNs, &held);
    if (held && !ctx->dragHeldByHead) {
        LOGI("drag held, head turning %.0f deg/s", ctx->headTurnRate);
    }
    ctx->dragHeldByHead = held;
    return carry;
}

// ---- player screen handles (xr_handles.h) ---------------------------------

static float distFromHead(const XrCtx* ctx, XrVector3f p) {
    float dx = p.x - ctx->headPos.x;
    float dy = p.y - ctx->headPos.y;
    float dz = p.z - ctx->headPos.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Sets distance along the current view line and width; radius scales with width.
static void placeAlongView(XrCtx* ctx, float dist, float width) {
    Vec3 head = { ctx->headPos.x, ctx->headPos.y, ctx->headPos.z };
    Vec3 at = { ctx->screenPose.position.x, ctx->screenPose.position.y,
                ctx->screenPose.position.z };
    Vec3 dir = vecNorm(vecSub(at, head));
    ctx->screenPose.position.x = head.x + dir.x * dist;
    ctx->screenPose.position.y = head.y + dir.y * dist;
    ctx->screenPose.position.z = head.z + dir.z * dist;
    if (ctx->screenWidth > 0.0f) {
        ctx->screenRadius *= width / ctx->screenWidth;
    }
    ctx->screenWidth = width;
}

// On release, springs the screen back if it was stretched past a limit.
static void startSpringIfOut(XrCtx* ctx) {
    float dist = distFromHead(ctx, ctx->screenPose.position);
    float toDist = dist < HANDLE_DIST_MIN ? HANDLE_DIST_MIN
                 : dist > HANDLE_DIST_MAX ? HANDLE_DIST_MAX : dist;
    float lo, hi;
    handleWidthRange(toDist, &lo, &hi);
    float width = ctx->screenWidth;
    float toWidth = width < lo ? lo : width > hi ? hi : width;
    if (toDist == dist && toWidth == width) {
        return;
    }
    ctx->springActive = 1;
    ctx->springStartNs = ctx->lastInputNs;
    ctx->springFromDist = dist;
    ctx->springToDist = toDist;
    ctx->springFromWidth = width;
    ctx->springToWidth = toWidth;
}

static void stepSpring(XrCtx* ctx) {
    if (!ctx->springActive) {
        return;
    }
    float e = handleSpringEase(ctx->springStartNs, ctx->lastInputNs);
    placeAlongView(ctx, ctx->springFromDist + (ctx->springToDist - ctx->springFromDist) * e,
                   ctx->springFromWidth + (ctx->springToWidth - ctx->springFromWidth) * e);
    if (e >= 1.0f) {
        ctx->springActive = 0;
        ctx->poseDirty = 1;
    }
}

// Move and resize start from a handle under the ray, or in the player from a grip
// on the picture itself; the trigger on the picture is a click, never a drag.
static void applyGrab(XrCtx* ctx, XrPosef* aims, const int* valid, const float* grab, int hand,
                      int hover, int corner, int offPicture, float height, int curved,
                      const XrVector2f* sticks, float dt) {
    for (int h = 0; h < HAND_COUNT; h++) {
        int wasDown = ctx->grabDown[h];
        ctx->grabDown[h] = grab[h] > (wasDown ? PRESS_OFF : PRESS_ON);
        ctx->gripEdge[h] = ctx->grabDown[h] && !wasDown;
    }

    if (ctx->grabMode != GRAB_NONE) {
        int stillHeld = ctx->grabByTrigger ? ctx->triggerDown[ctx->grabHand]
                                           : ctx->grabDown[ctx->grabHand];
        // A gaze grab survives losing the hand ray while the pinch is tracked.
        int stillThere = valid[ctx->grabHand]
                || (ctx->grabByGaze && pinchTracked(ctx, ctx->grabHand));
        if (!stillHeld || !stillThere) {
            LOGI("grab ended: %s", stillHeld ? "the hand was lost" : "let go");
            ctx->grabMode = GRAB_NONE;
            ctx->grabByGaze = 0;
            ctx->grabNear = 0;
            ctx->poseDirty = 1;
            startSpringIfOut(ctx);
            return;
        }
    }

    if (ctx->grabMode == GRAB_NONE) {
        stepSpring(ctx);
    }

    if (ctx->grabMode == GRAB_NONE) {
        // The picture or any panel on it, so the library window moves the same way
        int gripPicture = hand >= 0 && hand != SRC_GAZE && ctx->gripEdge[hand]
                && (hover == HOVER_SCREEN || hover == HOVER_HALO || hover == HOVER_UIPANEL);
        if (hand < 0 || (hover != HOVER_BAR && hover != HOVER_CORNER && !gripPicture)) {
            return;
        }

        // Grip or trigger both grab; the trigger only off the picture, where it
        // is not needed as a click.
        int byGrip = ctx->gripEdge[hand];
        int byTrigger = ctx->triggerEdge[hand] && offPicture;
        if (!byGrip && !byTrigger) {
            return;
        }
        ctx->grabByTrigger = !byGrip;

        // Gaze picks the handle, a hand carries it; driving it off the gaze
        // ray would make the screen follow the eyes.
        int mover = hand;
        ctx->grabByGaze = hand == SRC_GAZE;
        if (ctx->grabByGaze) {
            float gu, gv;
            if (!screenProject(aims[hand], ctx->screenPose, ctx->screenWidth, height,
                               ctx->screenRadius, curved, &gu, &gv)) {
                ctx->grabByGaze = 0;
                return;
            }
            Vec3 target = screenPoint(gu, gv, ctx->screenPose, ctx->screenWidth, height,
                                      ctx->screenRadius, curved);
            mover = gazeDragHand(ctx, aims, valid, target, &ctx->grabHandStart,
                                 &ctx->grabScale);
            if (mover < 0) {
                ctx->grabByGaze = 0;
                return;
            }
            dragRampStart(&ctx->grabRamp, ctx->lastInputNs);
            LOGI("gaze grab: %s by hand %d, scale %.2f", hover == HOVER_BAR ? "move" : "resize",
                 mover, ctx->grabScale);
        }

        ctx->grabHand = mover;
        ctx->grabAim = aims[mover];
        ctx->grabScreen = ctx->screenPose;
        ctx->grabWidth = ctx->screenWidth;
        ctx->grabHeight = height;
        ctx->grabRadius = ctx->screenRadius;
        ctx->springActive = 0;

        if (hover == HOVER_BAR || gripPicture) {
            ctx->grabMode = GRAB_MOVE;
            ctx->grabReel = 1.0f;
            ctx->grabNear = mover < HAND_COUNT && ctx->nearBar[mover];
            LOGI("move grab by %s %d, %s%s", ctx->grabByTrigger ? "trigger or pinch" : "grip", mover,
                 hover == HOVER_BAR ? "on the bar" : "on the picture",
                 ctx->grabNear ? ", in hand" : "");
            ctx->grabHandDist = distFromHead(ctx, aims[mover].position);
            ctx->grabScreenDist = distFromHead(ctx, ctx->screenPose.position);
            ctx->grabSizeRatio = handleSizeRatio(ctx->grabScreenDist, ctx->screenWidth);
            Vec3 back = { 0.0f, 0.0f, 1.0f };
            Vec3 fwd = quatRotate(ctx->screenPose.orientation, back);
            ctx->playerYaw = atan2f(fwd.x, fwd.z);
            ctx->playerPitch = screenPitch(ctx);
            ctx->playerTurnNs = ctx->lastInputNs;
            return;
        }

        // A ray that misses the plane has nothing to measure the drag against.
        float u, v;
        if (!screenProject(aims[hand], ctx->grabScreen, ctx->screenWidth, height,
                           ctx->screenRadius, curved, &u, &v)) {
            return;
        }

        // Anchored at the centre; the drag runs along the half diagonal to the corner.
        int right = (corner == 1 || corner == 3);
        int bottom = (corner >= 2);
        ctx->grabOppX = (right ? -0.5f : 0.5f) * ctx->grabWidth;
        ctx->grabOppY = (bottom ? 0.5f : -0.5f) * ctx->grabHeight;
        ctx->grabMode = GRAB_RESIZE;
        LOGI("resize grab by %s %d", ctx->grabByTrigger ? "trigger or pinch" : "grip", mover);
        return;
    }

    int h = ctx->grabHand;
    if (ctx->grabMode == GRAB_MOVE) {
        if (ctx->grabByGaze) {
            // Gaze grab: translate by the hand's travel.
            Vec3 travel = vecSub(gazeHandPos(ctx, aims, h), ctx->grabHandStart);
            Vec3 carry = gazeDragCarry(ctx, &ctx->grabRamp, travel, ctx->lastInputNs);
            ctx->screenPose.position.x = ctx->grabScreen.position.x + carry.x * ctx->grabScale;
            ctx->screenPose.position.y = ctx->grabScreen.position.y + carry.y * ctx->grabScale;
            ctx->screenPose.position.z = ctx->grabScreen.position.z + carry.z * ctx->grabScale;
        }
        else {
            // Rigid attach: the offset from the hand turns with the hand.
            XrQuaternionf turn = quatMul(aims[h].orientation,
                                         quatConj(ctx->grabAim.orientation));
            Vec3 offset = { ctx->grabScreen.position.x - ctx->grabAim.position.x,
                            ctx->grabScreen.position.y - ctx->grabAim.position.y,
                            ctx->grabScreen.position.z - ctx->grabAim.position.z };
            Vec3 moved = quatRotate(turn, offset);

            ctx->screenPose.position.x = aims[h].position.x + moved.x;
            ctx->screenPose.position.y = aims[h].position.y + moved.y;
            ctx->screenPose.position.z = aims[h].position.z + moved.z;

            // Direction from the rigid attach; distance reeled by the hand and
            // the stick within elastic limits; width keeps its place in the range.
            float push = h >= 0 && h < HAND_COUNT ? sticks[h].y : 0.0f;
            if (fabsf(push) > SCROLL_DEADZONE) {
                float past = (fabsf(push) - SCROLL_DEADZONE) / (1.0f - SCROLL_DEADZONE);
                ctx->grabReel *= expf(copysignf(past * STICK_REEL_RATE * dt, push));
            }
            // Held in the hand it stays where the hand puts it; from afar the pull is
            // amplified, so a far window comes within reach in one move
            float reel = ctx->grabNear ? distFromHead(ctx, ctx->screenPose.position)
                    : handleReelDistance(ctx->grabScreenDist, ctx->grabHandDist,
                                         distFromHead(ctx, aims[h].position));
            float want = ctx->grabReel * reel;
            // The stick stops at the limits rather than winding past them
            if (want > HANDLE_DIST_MAX) {
                ctx->grabReel = HANDLE_DIST_MAX / reel;
            }
            else if (want < HANDLE_DIST_MIN) {
                ctx->grabReel = HANDLE_DIST_MIN / reel;
            }
            float dist = handleElasticDistance(want);
            placeAlongView(ctx, dist, handleWidthFor(dist, ctx->grabSizeRatio));
        }

        // Orientation does not follow the wrist: the screen keeps its tilt and
        // roll and yaws to face the viewer.
        float hx = ctx->headPos.x - ctx->screenPose.position.x;
        float hz = ctx->headPos.z - ctx->screenPose.position.z;
        // Eases yaw and pitch toward the eyes, never rolls.
        float hy = ctx->headPos.y - ctx->screenPose.position.y;
        float flat = sqrtf(hx * hx + hz * hz);
        float dt = (float)(ctx->lastInputNs - ctx->playerTurnNs) / 1e9f;
        ctx->playerTurnNs = ctx->lastInputNs;
        if (flat > 0.05f) {
            ctx->playerYaw = handleTurnToward(ctx->playerYaw, atan2f(hx, hz), dt);
        }
        ctx->playerPitch = handleTurnToward(ctx->playerPitch,
                                            handleFacePitch(atan2f(hy, flat)), dt);
        ctx->screenPose.orientation = screenOrient(ctx->playerYaw, ctx->playerPitch, 0.0f);
        return;
    }

    // Resize, measured against the grab-start pose so growth cannot feed back.
    float reach;
    if (ctx->grabByGaze) {
        // Hand travel projected onto the screen's axes.
        Vec3 travel = vecSub(gazeHandPos(ctx, aims, h), ctx->grabHandStart);
        Vec3 carry = gazeDragCarry(ctx, &ctx->grabRamp, travel, ctx->lastInputNs);
        Vec3 xAxis = { 1.0f, 0.0f, 0.0f };
        Vec3 yAxis = { 0.0f, 1.0f, 0.0f };
        Vec3 right = quatRotate(ctx->grabScreen.orientation, xAxis);
        Vec3 up = quatRotate(ctx->grabScreen.orientation, yAxis);
        reach = diagonalReachAt(ctx, -ctx->grabOppX + vecDot(carry, right) * ctx->grabScale,
                                -ctx->grabOppY + vecDot(carry, up) * ctx->grabScale);
    }
    else {
        float u, v;
        if (!screenProject(aims[h], ctx->grabScreen, ctx->grabWidth, ctx->grabHeight,
                           ctx->grabRadius, curved, &u, &v)) {
            return;
        }
        reach = diagonalReach(ctx, u, v);
    }
    float scale = reach < 0.05f ? 0.05f : reach;

    float width = ctx->grabWidth * scale;
    // Elastic past the distance's width range; springs back on release.
    width = handleElasticWidth(distFromHead(ctx, ctx->grabScreen.position), width);

    // Radius scales with width so the arc keeps its shape.
    ctx->screenRadius = ctx->grabRadius * (width / ctx->grabWidth);
    ctx->screenWidth = width;

    // Grows about its centre.
    ctx->screenPose.position = ctx->grabScreen.position;
    ctx->screenPose.orientation = ctx->grabScreen.orientation;
}

// Whether a hover target lies on the bar's frame rather than the picture.
static int onFurniture(int hover) {
    return hover == HOVER_BAR;
}

// World point for a hit on (u, v), on whichever surface the hover belongs to.
static Vec3 furniturePoint(XrCtx* ctx, int hover, float u, float v, XrPosef screenPose,
                           float height, float radius, int curved, const BarFrame* bar) {
    if (onFurniture(hover)) {
        return screenPoint(u, v, bar->pose, bar->width, bar->height, bar->radius, bar->curved);
    }
    return screenPoint(u, v, screenPose, ctx->screenWidth, height, radius, curved);
}

void destroyXrInput(XrCtx* ctx) {
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->handTrackers[h] != XR_NULL_HANDLE && ctx->pfnDestroyHandTracker != NULL) {
            ctx->pfnDestroyHandTracker(ctx->handTrackers[h]);
            ctx->handTrackers[h] = XR_NULL_HANDLE;
        }
    }
    for (int h = 0; h < SRC_COUNT; h++) {
        if (ctx->aimSpaces[h] != XR_NULL_HANDLE) {
            xrDestroySpace(ctx->aimSpaces[h]);
            ctx->aimSpaces[h] = XR_NULL_HANDLE;
        }
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->handAimSpaces[h] != XR_NULL_HANDLE) {
            xrDestroySpace(ctx->handAimSpaces[h]);
            ctx->handAimSpaces[h] = XR_NULL_HANDLE;
        }
        if (ctx->gripSpaces[h] != XR_NULL_HANDLE) {
            xrDestroySpace(ctx->gripSpaces[h]);
            ctx->gripSpaces[h] = XR_NULL_HANDLE;
        }
    }
    if (ctx->actionSet != XR_NULL_HANDLE) {
        // Also destroys its actions.
        xrDestroyActionSet(ctx->actionSet);
        ctx->actionSet = XR_NULL_HANDLE;
    }
    ctx->inputReady = 0;
}

// Per-frame input state shared between the stages below.
typedef struct {
    // IN_ slots returned to Java.
    float* out;
    int64_t now;
    float dt;
    // Into the screen's frame (head when head locked, else local).
    FrameXform xform;
    int curved;
    float height;
    float radius;
    // Corner bracket size, 0 if none.
    float cornerSide;
    XrPosef screenPose;
    BarFrame bar;
    // Head pose in the screen's frame.
    XrPosef headPose;
    int headValid;
    XrPosef aimPoses[SRC_COUNT];
    int aimValid[SRC_COUNT];
    float hitU[SRC_COUNT];
    float hitV[SRC_COUNT];
    int hovers[SRC_COUNT];
    int corners[SRC_COUNT];
    int moved;
    int handMoved[HAND_COUNT];
    int pinching;
    // Hand that only pinches for gaze: no ray, no hover.
    int pinchOnly[HAND_COUNT];
    float grab[HAND_COUNT];
    XrVector2f stick[HAND_COUNT];
    int gazeAsked;
    int gazeUsable;
    // Pointing source (-1 if none) and its hover.
    int hand;
    int hover;
    // A fingertip is touching or about to touch something, and whose.
    int poking;
    int touch[HAND_COUNT];
} InputFrame;

// Gaze points unless a controller is awake or the hands are bridging for lost eyes.
static int gazePointing(XrCtx* ctx) {
    return ctx->eyeGaze && ctx->gazeEnabled && !ctx->controllerAwake
            && !ctx->gazeBridge.bridged;
}

// Gaze is still read while bridged, so the bridge can see the eyes return.
static int gazeWanted(XrCtx* ctx) {
    return ctx->eyeGaze && ctx->gazeEnabled && !ctx->controllerAwake;
}

static int handInUse(XrCtx* ctx, int h) {
    return controllerInUse(ctx->profileKind[h], ctx->aimTracked[h], ctx->aimClock[h].awake);
}

// Whether a gaze-started drag is in progress.
static int gazeHoldingPress(XrCtx* ctx) {
    return ctx->grabMode != GRAB_NONE && (ctx->grabByGaze || ctx->grabHand == SRC_GAZE);
}

// Settled once per frame from last frame's clocks, so the frame agrees on it.
static void updateControllerAwake(XrCtx* ctx) {
    int awake = handInUse(ctx, HAND_LEFT) || handInUse(ctx, HAND_RIGHT);
    // Don't take the pointer from gaze mid drag.
    if (awake && !ctx->controllerAwake && gazeHoldingPress(ctx)) {
        awake = 0;
    }
    if (awake == ctx->controllerAwake) {
        return;
    }
    ctx->controllerAwake = awake;
    if (ctx->eyeGaze && ctx->gazeEnabled) {
        LOGEV("pointer: %s", awake ? "controller" : ctx->gazeBridge.bridged ? "hands" : "eyes");
    }
}

// The bridge switches only when nothing is held, so a press ends on its own source.
static int pressHeld(XrCtx* ctx) {
    return ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT]
            || ctx->grabMode != GRAB_NONE;
}

// Hands take over after gaze is lost for GAZE_BRIDGE_SEC, and hand back as soon
// as it returns. Runs after updateControllerAwake, off last frame's gaze.
static void updateGazeBridge(XrCtx* ctx) {
    float sec = 0.0f;
    int event = gazeBridgeUpdate(&ctx->gazeBridge, ctx->eyeGaze && ctx->gazeEnabled,
                                 ctx->controllerAwake,
                                 ctx->sessionState == XR_SESSION_STATE_FOCUSED,
                                 pressHeld(ctx), nowNs(), GAZE_BRIDGE_SEC, &sec);
    if (event == BRIDGE_ON) {
        LOGEV("gaze bridge on after %.1f s, the hands point until the eyes are back", sec);
    }
    else if (event == BRIDGE_OFF) {
        LOGEV("gaze bridge off after %.1f s", sec);
    }
}

// Whether a source may be picked to point: not pinch-only, not asleep, and not
// an idle controller while gaze points.
static int canPoint(XrCtx* ctx, const InputFrame* f, int h) {
    if (!f->aimValid[h] || !ctx->pointerAwake) {
        return 0;
    }
    if (h >= HAND_COUNT) {
        return 1;
    }
    return !f->pinchOnly[h] && !(gazePointing(ctx) && !handInUse(ctx, h));
}

// Gaze has no button: either hand's pinch clicks.
static void gazeTrigger(XrCtx* ctx, const InputFrame* f) {
    if (f->aimValid[SRC_GAZE]) {
        ctx->triggerDown[SRC_GAZE] = ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT];
        ctx->triggerEdge[SRC_GAZE] = ctx->triggerEdge[HAND_LEFT] || ctx->triggerEdge[HAND_RIGHT];
        ctx->usingHands[SRC_GAZE] = 1;
    }
    else {
        ctx->triggerDown[SRC_GAZE] = 0;
        ctx->triggerEdge[SRC_GAZE] = 0;
        ctx->usingHands[SRC_GAZE] = 0;
    }
}

// Releases everything held when input stops being read (e.g. focus lost).
static void releaseInput(XrCtx* ctx) {
    ctx->buttonsDown = 0;
    ctx->beamVisible = 0;
    ctx->aimTracked[HAND_LEFT] = 0;
    ctx->aimTracked[HAND_RIGHT] = 0;
    // No touch, hold or press outlives it either
    Vec3 none = { 0.0f, 0.0f, 0.0f };
    for (int h = 0; h < HAND_COUNT; h++) {
        ctx->poke[h].state = POKE_IDLE;
        ctx->poke[h].target = -1;
        ctx->pokePush[h] = none;
        ctx->pinchHold[h] = HOLD_NONE;
        ctx->nearBar[h] = 0;
    }
    memset(ctx->uiPress, 0, sizeof(ctx->uiPress));
    // Not asked for, which must not count as gaze lost.
    gazeBridgeTrack(&ctx->gazeBridge, 0, 0, 0);
    if (ctx->grabMode != 0) {
        // End the grab, or the stale anchor makes the screen jump on refocus.
        ctx->grabMode = 0;
        ctx->grabByGaze = 0;
        ctx->grabNear = 0;
        ctx->poseDirty = 1;
    }
}

// Finds what a source's ray is over.
static void hoverSource(XrCtx* ctx, InputFrame* f, int h) {
    // Corner brackets first: the library page fills its window to the edges.
    if (f->cornerSide > 0.0f) {
        float cu, cv;
        int corner;
        if (screenProject(f->aimPoses[h], f->screenPose, ctx->screenWidth, f->height,
                          f->radius, f->curved, &cu, &cv)
                && hoverTest(cu, cv, ctx->screenWidth, f->height, f->cornerSide, &corner)
                   == HOVER_CORNER) {
            ctx->uiHover[h] = -1;
            f->corners[h] = corner;
            f->hovers[h] = HOVER_CORNER;
            f->hitU[h] = cu;
            f->hitV[h] = cv;
            return;
        }
    }

    // UI panels occlude everything behind them.
    float uiU, uiV;
    int ui = uiPanelAt(ctx, f->aimPoses[h], &uiU, &uiV);
    ctx->uiHover[h] = ui;
    if (ui >= 0) {
        f->hovers[h] = HOVER_UIPANEL;
        f->hitU[h] = uiU;
        f->hitV[h] = uiV;
        return;
    }

    if (screenProject(f->aimPoses[h], f->screenPose, ctx->screenWidth, f->height,
                      f->radius, f->curved, &f->hitU[h], &f->hitV[h])) {
        f->hovers[h] = hoverTest(f->hitU[h], f->hitV[h], ctx->screenWidth, f->height,
                                 f->cornerSide, &f->corners[h]);
    }
}

typedef struct {
    int target;
    float depth;
    int over;
    XrPosef ray;
} PokeHit;

// A fingertip measured against one surface, and the touch ray: from just in front
// of the surface under the fingertip, straight into it.
static int pokeSurface(XrCtx* ctx, const InputFrame* f, int id, int h, Vec3 tip,
                       PokeHit* hit) {
    XrPosef pose;
    float w, ht;
    float radius = 0.0f;
    int curved = 0;
    if (id == POKE_SCREEN) {
        // Without a film the library is a panel in the screen's place
        if (ctx->playerScreenHidden) {
            return 0;
        }
        pose = f->screenPose;
        w = ctx->screenWidth;
        ht = f->height;
        radius = f->radius;
        curved = f->curved;
    }
    else {
        if (id == UI_PANEL_SUBS || id == UI_PANEL_LOADING || !ctx->ui[id].ready
                || !ctx->ui[id].visible || ctx->ui[id].level < 0.5f) {
            return 0;
        }
        pose = uiPanelPose(ctx, id, &w, &ht);
    }
    Vec3 at = { pose.position.x, pose.position.y, pose.position.z };
    Vec3 local = quatRotate(quatConj(pose.orientation), vecSub(tip, at));
    PokeLocal pl;
    pokeLocal(local, w, ht, radius, curved, &pl);
    hit->target = id;
    hit->depth = pl.depth - ctx->indexTipRadius[h];
    hit->over = pl.over;

    Vec3 normal = { -sinf(pl.yaw), 0.0f, cosf(pl.yaw) };
    float back = POKE_RAY_BACK_M - pl.depth;
    Vec3 start = { local.x + normal.x * back, local.y, local.z + normal.z * back };
    Vec3 offset = quatRotate(pose.orientation, start);
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    hit->ray.orientation = quatMul(pose.orientation, axisAngleQuat(up, -pl.yaw));
    hit->ray.position.x = at.x + offset.x;
    hit->ray.position.y = at.y + offset.y;
    hit->ray.position.z = at.z + offset.z;
    return 1;
}

// Fingers at the white bar take hold of it there: the hand's ray becomes a short one
// into the bar under the pinch, and while held it keeps its turn and its offset from the
// pinch point, so the window follows the hand one to one. Beside the bar the ray goes
// into the Back button instead, which a pinch there presses. Returns whether it applies.
static int nearBarHand(XrCtx* ctx, InputFrame* f, int h) {
    int holding = ctx->grabMode == GRAB_MOVE && ctx->grabHand == h && ctx->grabNear;
    ctx->nearBar[h] = 0;
    XrPosef ray;
    if (holding) {
        // A pinch point lost for a moment stays where it was; lost for longer, the
        // grab ends where the window is, never on the far ray
        if (ctx->pinchPointValid[h]) {
            ctx->nearLast[h] = ctx->pinchPoint[h];
            ctx->nearLastNs[h] = f->now;
        }
        else if (f->now - ctx->nearLastNs[h] >= POINTER_HOLD_NS) {
            return 1;
        }
        ray.orientation = ctx->nearRayTurn[h];
        ray.position.x = ctx->nearLast[h].x + ctx->nearOffset[h].x;
        ray.position.y = ctx->nearLast[h].y + ctx->nearOffset[h].y;
        ray.position.z = ctx->nearLast[h].z + ctx->nearOffset[h].z;
        ctx->nearBar[h] = 1;
        f->aimPoses[h] = ray;
        f->aimValid[h] = 1;
        hoverSource(ctx, f, h);
        return 1;
    }
    // A pinch holding something on the far ray keeps it
    if (ctx->grabMode != GRAB_NONE || ctx->pinchHold[h] == HOLD_RAY) {
        return 0;
    }

    // The bar hangs centred under the frame, where the curve is flat
    const BarFrame* frame = &f->bar;
    Vec3 below = { 0.0f, -(frame->height * 0.5f + BAR_DROP_FRAC * frame->width), 0.0f };
    Vec3 drop = quatRotate(frame->pose.orientation, below);
    Vec3 at = { frame->pose.position.x + drop.x, frame->pose.position.y + drop.y,
                frame->pose.position.z + drop.z };
    NearBarRow row;
    row.halfW = 0.5f * BAR_WIDTH_FRAC * frame->width;
    row.halfH = 0.5f * BAR_HEIGHT_FRAC * frame->width;
    row.hasBack = ctx->backWanted && ctx->ui[UI_PANEL_BACK].ready;
    row.backX = 0.0f;
    row.backHalfW = 0.0f;
    if (row.hasBack) {
        float bw, bh;
        XrPosef back = uiPanelPose(ctx, UI_PANEL_BACK, &bw, &bh);
        Vec3 backAt = { back.position.x, back.position.y, back.position.z };
        row.backX = quatRotate(quatConj(frame->pose.orientation), vecSub(backAt, at)).x;
        row.backHalfW = 0.5f * bw;
    }

    // The pinch point, or with the fingers apart the fingertip, which only hovers
    Vec3 probe = { 0.0f, 0.0f, 0.0f };
    float aimX = 0.0f;
    int zone = NEAR_AWAY;
    if (ctx->pinchPointValid[h]) {
        probe = ctx->pinchPoint[h];
        zone = nearBarZone(quatRotate(quatConj(frame->pose.orientation), vecSub(probe, at)),
                           &row, &aimX);
    }
    if (zone == NEAR_AWAY && ctx->indexTipValid[h] && !ctx->triggerDown[h]) {
        Vec3 tip = { ctx->indexTip[h].x, ctx->indexTip[h].y, ctx->indexTip[h].z };
        probe = tip;
        zone = nearBarZone(quatRotate(quatConj(frame->pose.orientation), vecSub(probe, at)),
                           &row, &aimX);
    }
    if (zone == NEAR_AWAY) {
        return 0;
    }
    Vec3 start = { aimX, 0.0f, POKE_RAY_BACK_M };
    Vec3 offset = quatRotate(frame->pose.orientation, start);
    ctx->nearRayTurn[h] = frame->pose.orientation;
    ctx->nearOffset[h].x = at.x + offset.x - probe.x;
    ctx->nearOffset[h].y = at.y + offset.y - probe.y;
    ctx->nearOffset[h].z = at.z + offset.z - probe.z;
    ctx->nearLast[h] = probe;
    ctx->nearLastNs[h] = f->now;
    ray.orientation = frame->pose.orientation;
    ray.position.x = at.x + offset.x;
    ray.position.y = at.y + offset.y;
    ray.position.z = at.z + offset.z;
    ctx->nearBar[h] = 1;
    f->aimPoses[h] = ray;
    f->aimValid[h] = 1;
    // Hands at the bar are not hands at rest
    ctx->pointerAwake = 1;
    hoverSource(ctx, f, h);
    // The button's place is the button's: until it shows, a pinch there is not the bar's
    if (zone == NEAR_BACK && f->hovers[h] != HOVER_UIPANEL) {
        ctx->triggerEdge[h] = 0;
    }
    return 1;
}

// Direct touch: a fingertip near a menu or the picture points straight into it and
// presses by touching it, in place of the far ray and the pinch. Returns whether the
// hand is touching or about to.
static int pokeHand(XrCtx* ctx, InputFrame* f, int h, int wasDown) {
    Poke* p = &ctx->poke[h];
    Vec3 none = { 0.0f, 0.0f, 0.0f };
    ctx->pokePush[h] = none;
    PokeHit best;
    best.target = -1;
    best.depth = 0.0f;
    best.over = 0;
    // A pinch holding something keeps it: no touch starts under it
    int free = p->state != POKE_IDLE || ctx->pinchHold[h] == HOLD_NONE;
    if (ctx->indexTipValid[h] && ctx->grabMode == GRAB_NONE && free) {
        Vec3 tip = { ctx->indexTip[h].x, ctx->indexTip[h].y, ctx->indexTip[h].z };
        int held = p->state == POKE_PRESSED ? p->target : -1;
        for (int id = 0; id <= POKE_SCREEN; id++) {
            PokeHit hit;
            if (!pokeSurface(ctx, f, id, h, tip, &hit)) {
                continue;
            }
            if (held >= 0) {
                if (id == held) {
                    best = hit;
                }
                continue;
            }
            // The front-most surface under the fingertip
            if (!hit.over || hit.depth >= POKE_HOVER_M || hit.depth <= -POKE_THROUGH_M) {
                continue;
            }
            if (best.target < 0 || hit.depth < best.depth) {
                best = hit;
            }
        }
    }
    int state = pokeStep(p, best.target, best.depth, best.over);
    if (state != ctx->pokeSaid[h]) {
        LOGI("touch: hand %d %s %s %d", h,
             state == POKE_PRESSED ? "pressing" : state == POKE_HOVER ? "near" : "away from",
             best.target == POKE_SCREEN ? "picture" : "panel", best.target);
        ctx->pokeSaid[h] = state;
    }
    if (state == POKE_IDLE) {
        return 0;
    }
    if (state == POKE_PRESSED && best.depth < 0.0f) {
        // Back out along the surface's normal, in local space like the drawn joints
        Vec3 out = { 0.0f, 0.0f, 1.0f };
        Vec3 n = quatRotate(best.ray.orientation, out);
        if (f->xform.toHead) {
            n = quatRotate(f->xform.head.orientation, n);
        }
        float push = fminf(-best.depth, POKE_THROUGH_M);
        ctx->pokePush[h].x = n.x * push;
        ctx->pokePush[h].y = n.y * push;
        ctx->pokePush[h].z = n.z * push;
    }
    f->aimPoses[h] = best.ray;
    f->aimValid[h] = 1;
    f->poking = 1;
    ctx->triggerDown[h] = state == POKE_PRESSED;
    ctx->triggerEdge[h] = ctx->triggerDown[h] && !wasDown;
    ctx->pointerAwake = 1;
    hoverSource(ctx, f, h);
    // A touch on the picture is a click, never a resize from its corner
    if (f->hovers[h] == HOVER_CORNER) {
        f->hovers[h] = HOVER_SCREEN;
    }
    return 1;
}

// World-space head turn rate, for the gaze drag guard. World frame even when
// head locked.
static void updateHeadTurn(XrCtx* ctx, InputFrame* f) {
    if (!f->xform.ok) {
        ctx->headTurnLastValid = 0;
        ctx->headTurnRate = 0.0f;
        return;
    }
    ctx->headTurnRate = ctx->headTurnLastValid
            ? turnRateDegS(f->xform.head.orientation, ctx->headTurnLast, f->dt) : 0.0f;
    ctx->headTurnLast = f->xform.head.orientation;
    ctx->headTurnLastValid = 1;
}

// Logs each pinch source once per session, the first time a hand uses it.
static void sayPinchSource(XrCtx* ctx, int h) {
    int src = ctx->pinchSrc[h];
    int seen = ctx->profileKind[h] == PROFILE_HANDS
            || (ctx->profileKind[h] == PROFILE_NONE && ctx->tipsValid[h]);
    if (!seen || (ctx->pinchSrcSaid & (1u << src))) {
        return;
    }
    ctx->pinchSrcSaid |= 1u << src;
    switch (src) {
        case PINCH_SRC_VALUE: {
            float on, off;
            pinchValuePair(ctx->onExtHands[h], &on, &off);
            LOGEV("pinch: the runtime's pinch value (%s), on %.2f, off %.2f", ctx->onExtHands[h]
                  ? "hand_interaction_ext" : "microsoft hand_interaction select", on, off);
            break;
        }
        case PINCH_SRC_AIM:
            LOGEV("pinch: the runtime's pinching flag (XR_FB_hand_tracking_aim), no hand "
                  "profile with a pinch on hand %d", h);
            break;
        case PINCH_SRC_JOINTS:
            LOGEV("pinch: thumb to index tip off the joints, on %.0f mm, off %.0f mm, the "
                  "runtime giving no pinch on hand %d", PINCH_ON_M * 1000.0f,
                  PINCH_OFF_M * 1000.0f, h);
            break;
        default:
            LOGEV("pinch: nothing to read one from on hand %d", h);
            break;
    }
}

// Reads triggers, aim poses and ray hits for every source.
static void readSources(XrCtx* ctx, InputFrame* f) {
    int gazeSpace = ctx->aimSpaces[SRC_GAZE] != XR_NULL_HANDLE;
    // In gaze mode the eyes point and the hands only pinch.
    int gazeMode = gazePointing(ctx) && gazeSpace;
    // Gaze is still read while bridged, but does not point.
    int gazeWatch = gazeWanted(ctx) && gazeSpace;
    for (int h = 0; h < SRC_COUNT; h++) {
        if (h < HAND_COUNT) {
            f->grab[h] = seenGrip(ctx, h);
            f->stick[h] = seenStick(ctx, h);
            int wasDown = ctx->triggerDown[h];
            float value = seenTrigger(ctx, h);
            readJoints(ctx, h, &f->xform, &f->headPose, f->headValid);
            if (ctx->profileKind[h] != PROFILE_CONTROLLER) {
                int state = ctx->handTrust[h] >= 0.7f ? 1 : (ctx->handTrust[h] >= HAND_TRUST_MIN ? 2 : 3);
                if (ctx->trustSaid[h] != state) {
                    static const char* const why[4] = { "", ", not active", ", low confidence",
                                                        ", not hand-sized" };
                    LOGI("hand %d tracking: %s (%.0f percent of joints%s)%s", h,
                         state == 1 ? "good" : (state == 2 ? "weak" : "lost"), ctx->handTrust[h] * 100.0f,
                         state == 3 ? why[ctx->handLostWhy[h] & 3] : "",
                         state == 3 && ctx->handJointsOk[h] ? ", still drawn" : "");
                    ctx->trustSaid[h] = state;
                }
            }
            if (ctx->profileKind[h] == PROFILE_CONTROLLER) {
                ctx->pinchDown[h] = 0;
                ctx->triggerDown[h] = pressHysteresis(value, wasDown, PRESS_ON, PRESS_OFF);
            }
            else {
                // One pinch source per hand, the runtime's own where offered.
                int bound = (ctx->extHandClick && ctx->onExtHands[h])
                        || (ctx->msftHandClick && ctx->onMsftHands[h]);
                ctx->pinchSrc[h] = pinchSource(bound, ctx->handAim, ctx->jointTracking);
                int pinch = pinchStep(ctx->pinchSrc[h], ctx->onExtHands[h],
                                      ctx->pinchDown[h], value, ctx->aimPinch[h],
                                      ctx->tipsValid[h], ctx->tipGap[h]);
                // Poorly tracked hands flicker: don't start a joint pinch from
                // untrusted joints, and hold a pinch across short tracking gaps.
                int trusted = ctx->handTrust[h] >= HAND_TRUST_MIN;
                int seen = trusted || ctx->aimTracked[h];
                if (pinch && !wasDown && ctx->pinchSrc[h] == PINCH_SRC_JOINTS && !trusted) {
                    pinch = 0;
                }
                if (wasDown && !pinch && !seen && f->now - ctx->pinchGoodNs[h] < PINCH_HOLD_NS) {
                    pinch = 1;
                }
                if (pinch && seen) {
                    ctx->pinchGoodNs[h] = f->now;
                }
                ctx->pinchDown[h] = pinch;
                ctx->triggerDown[h] = ctx->pinchDown[h];
                sayPinchSource(ctx, h);
            }
            ctx->triggerEdge[h] = ctx->triggerDown[h] && !wasDown;

            // Diagnostics only; reads 0 for a held pinch.
            ctx->triggerValue[h] = value;
            if (ctx->triggerEdge[h]) {
                ctx->triggerHoldMin[h] = value;
                ctx->triggerDipFrames[h] = 0;
                ctx->triggerRetaps[h] = 0;
                ctx->triggerDipping[h] = 0;
            }
            else if (ctx->triggerDown[h]) {
                if (value < ctx->triggerHoldMin[h]) {
                    ctx->triggerHoldMin[h] = value;
                }
                if (value < PRESS_ON) {
                    ctx->triggerDipFrames[h]++;
                    ctx->triggerDipping[h] = 1;
                }
                else if (ctx->triggerDipping[h]) {
                    ctx->triggerRetaps[h]++;
                    ctx->triggerDipping[h] = 0;
                }
            }
            if (ctx->profileKind[h] != PROFILE_CONTROLLER) {
                // Touch first, so the Back button beside the bar can be touched; then
                // fingers at the bar; then the far ray
                ctx->pinchHold[h] = pinchHoldStep(ctx->pinchHold[h], ctx->pinchDown[h], 0, 0);
                f->touch[h] = pokeHand(ctx, f, h, wasDown) ? ctx->poke[h].state : TOUCH_NONE;
                ctx->pinchIgnored[h] = pinchAfterTouch(ctx->pinchIgnored[h], ctx->pinchDown[h],
                                                       f->touch[h] != TOUCH_NONE);
                if (f->touch[h] != TOUCH_NONE) {
                    ctx->nearBar[h] = 0;
                    ctx->aimTracked[h] = 0;
                    continue;
                }
                if (ctx->pinchIgnored[h]) {
                    ctx->triggerDown[h] = 0;
                    ctx->triggerEdge[h] = 0;
                }
                if (nearBarHand(ctx, f, h)) {
                    ctx->aimTracked[h] = 0;
                    continue;
                }
            }
            else {
                Vec3 none = { 0.0f, 0.0f, 0.0f };
                ctx->poke[h].state = POKE_IDLE;
                ctx->poke[h].target = -1;
                ctx->pokePush[h] = none;
                ctx->pinchHold[h] = HOLD_NONE;
                ctx->pinchIgnored[h] = 0;
                ctx->nearBar[h] = 0;
            }
        }
        else if (!gazeWatch) {
            continue;
        }

        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT
                | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        // A hand must not read the controller aim: it can keep reporting a
        // valid pose from where the controller was put down.
        int handActions = onHandActions(ctx, h);
        XrSpace space = handActions ? ctx->handAimSpaces[h] : ctx->aimSpaces[h];
        int ok = space != XR_NULL_HANDLE
                && XR_SUCCEEDED(xrLocateSpace(space, ctx->localSpace,
                                              ctx->predictedDisplayTime, &loc))
                && intoFrame(&f->xform, &loc.pose);
        int located = ok && (loc.locationFlags & needed) == needed;
        if (h < HAND_COUNT && located && ctx->profileKind[h] != PROFILE_CONTROLLER) {
            // Require tracked, not just valid: a lost hand keeps a valid pose,
            // often at the face, where a real hand never aims from.
            unsigned fl = (unsigned)loc.locationFlags;
            if (aimFullyTracked(fl)) {
                ctx->aimReportsTracked[h] = 1;
            }
            int trackedOk = !ctx->aimReportsTracked[h] || aimFullyTracked(fl);
            int atFace = 0;
            if (f->headValid) {
                float dx = loc.pose.position.x - f->headPose.position.x;
                float dy = loc.pose.position.y - f->headPose.position.y;
                float dz = loc.pose.position.z - f->headPose.position.z;
                atFace = sqrtf(dx * dx + dy * dy + dz * dz) < 0.12f;
            }
            if (!trackedOk || atFace) {
                located = 0;
            }
        }
        if (h == SRC_GAZE) {
            // Gaze must be tracked as well as valid.
            located = ok && gazeUsable((unsigned)loc.locationFlags);
            f->gazeAsked = 1;
            f->gazeUsable = located;
        }
        else {
            // Require fully tracked and active: a lost hand's last known pose
            // must not count as a controller in use.
            ctx->aimTracked[h] = located && aimFullyTracked((unsigned)loc.locationFlags)
                    && actionPoseActive(ctx, handActions ? ctx->handAimAction : ctx->aimAction,
                                        h);
        }
        int fromJoints = 0;
        int held = 0;
        if (!located) {
            // No runtime aim pose: fall back to the joint-built ray.
            if (h < HAND_COUNT && ctx->handRayValid[h]) {
                loc.pose = ctx->handRay[h];
                fromJoints = 1;
            }
            else if (h < HAND_COUNT && ctx->profileKind[h] != PROFILE_CONTROLLER
                     && ctx->lastGoodValid[h] && f->now - ctx->lastGoodNs[h] < POINTER_HOLD_NS) {
                // Short tracking gap: keep the last good aim.
                loc.pose = ctx->lastGoodAim[h];
                held = 1;
            }
            else {
                // Reset filters so the ray does not sweep in from the last pose.
                if (h < HAND_COUNT) {
                    ctx->aimFilterPos[h][0].valid = 0;
                    ctx->aimFilterPos[h][1].valid = 0;
                    ctx->aimFilterPos[h][2].valid = 0;
                    ctx->aimFilterRot[h].valid = 0;
                }
                continue;
            }
        }
        // Bridged: gaze is read but does not point.
        if (h == SRC_GAZE && !gazeMode) {
            continue;
        }
        // Gaze is not filtered: eyes move in saccades and the cursor is
        // smoothed downstream.
        if (h < HAND_COUNT) {
            // Smooth poorly tracked hands harder.
            float cutoff = ctx->aimMinCutoff;
            if (ctx->profileKind[h] != PROFILE_CONTROLLER && ctx->handTrust[h] > 0.0f
                    && ctx->handTrust[h] < 0.7f) {
                cutoff *= 0.5f;
            }
            XrPosef filtered = loc.pose;
            filtered.position.x = euroFilter(&ctx->aimFilterPos[h][0], filtered.position.x,
                                             f->dt, cutoff, ctx->aimBeta);
            filtered.position.y = euroFilter(&ctx->aimFilterPos[h][1], filtered.position.y,
                                             f->dt, cutoff, ctx->aimBeta);
            filtered.position.z = euroFilter(&ctx->aimFilterPos[h][2], filtered.position.z,
                                             f->dt, cutoff, ctx->aimBeta);
            filtered.orientation = euroFilterQuat(&ctx->aimFilterRot[h], filtered.orientation,
                                                  f->dt, cutoff, ctx->aimBeta);
            f->aimPoses[h] = filtered;
        }
        else {
            f->aimPoses[h] = loc.pose;
        }
        f->aimValid[h] = 1;
        if (!held && h < HAND_COUNT) {
            ctx->lastGoodAim[h] = f->aimPoses[h];
            ctx->lastGoodNs[h] = f->now;
            ctx->lastGoodValid[h] = 1;
        }

        if (ctx->poseSeen[h] && f->dt > 0.0f) {
            Vec3 now3 = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
            Vec3 was3 = { ctx->lastAim[h].position.x, ctx->lastAim[h].position.y,
                          ctx->lastAim[h].position.z };
            Vec3 step = vecSub(now3, was3);
            float speed = sqrtf(step.x * step.x + step.y * step.y + step.z * step.z) / f->dt;

            // Rotation angle = 2 * acos(|q1 . q2|).
            XrQuaternionf a = loc.pose.orientation, b = ctx->lastAim[h].orientation;
            float dot = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
            if (dot > 1.0f) {
                dot = 1.0f;
            }
            float turn = 2.0f * acosf(dot) / f->dt;

            // Hands and eyes are never still, so their motion does not count.
            if (!ctx->usingHands[h] && h != SRC_GAZE
                    && (speed > POINTER_MOVE_SPEED || turn > POINTER_TURN_SPEED)) {
                f->moved = 1;
                f->handMoved[h] = 1;
            }
        }
        ctx->lastAim[h] = loc.pose;
        ctx->poseSeen[h] = 1;

        // In gaze mode hands only pinch; their rays would steal gaze presses.
        if (gazeMode && h < HAND_COUNT && (ctx->usingHands[h] || fromJoints)) {
            f->pinchOnly[h] = 1;
            continue;
        }
        hoverSource(ctx, f, h);
    }
}

// Whether any button is down on this controller.
static int controllerPressed(XrCtx* ctx, const InputFrame* f, int h) {
    return ctx->triggerDown[h] || f->grab[h] > PRESS_ON || stickPushed(f->stick[h])
            || actionBool(ctx, ctx->rightClickAction, h)
            || actionBool(ctx, ctx->middleClickAction, h)
            || actionBool(ctx, ctx->toggleAction, h);
}

// Per-controller wake/sleep clocks, so a controller put down retires even while
// the other hand or gaze keeps the shared pointer awake.
static void updateControllerClocks(XrCtx* ctx, InputFrame* f) {
    int eyesPoint = ctx->eyeGaze && ctx->gazeEnabled;
    int swallowed = 0;
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->profileKind[h] != PROFILE_CONTROLLER) {
            controllerClockReset(&ctx->aimClock[h]);
            continue;
        }
        // With gaze, only a press wakes a controller; movement alone would let
        // a nudged idle controller steal the pointer.
        int pressWakes = eyesPoint;
        int pressed = pressWakes && controllerPressed(ctx, f, h);
        int holding = ctx->triggerDown[h] || stickPushed(f->stick[h]);
        int swallow = 0;
        int event = controllerClockStep(&ctx->aimClock[h], f->dt, f->handMoved[h], pressed,
                                        holding, ctx->triggerDown[h], pressWakes,
                                        ctx->pointerSleepOn, ctx->pointerWake,
                                        ctx->pointerSleep, &swallow);
        if (event == CLOCK_PRESSED || event == CLOCK_PICKED_UP) {
            LOGI("controller %d %s, its ray is back", h,
                 event == CLOCK_PRESSED ? "pressed" : "picked up");
            ctx->pointerAwake = 1;
            ctx->stillFor = 0.0f;
        }
        else if (event == CLOCK_PUT_DOWN) {
            LOGI("controller %d put down, its ray is gone", h);
        }
        // The waking press is swallowed until released; it was not aimed.
        if (swallow) {
            ctx->triggerDown[h] = 0;
            ctx->triggerEdge[h] = 0;
            swallowed = 1;
        }
    }
    // Recompute gaze's trigger without the swallowed press.
    if (swallowed && ctx->usingHands[SRC_GAZE]) {
        gazeTrigger(ctx, f);
    }
}

// Wakes the pointer on a pinch, open palm or deliberate move; sleeps it when idle.
static void updatePointerWake(XrCtx* ctx, InputFrame* f) {
    // A pinch while the pointer is asleep is swallowed. A gaze pinch wakes it, and so
    // does a hand's new pinch aimed at the picture or its panels; hands at rest wake it
    // only with an open palm (below).
    int asleep = !ctx->pointerAwake;
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->usingHands[h]) {
            ctx->pinchSwallowed[h] = 0;
            continue;
        }
        if (ctx->triggerDown[h]) {
            f->pinching = 1;
            if (asleep) {
                int aimed = f->hovers[h] != HOVER_NONE && f->hovers[h] != HOVER_HALO;
                if (h == SRC_GAZE) {
                    ctx->pointerAwake = 1;
                }
                else if (pinchWakes(!ctx->pinchSwallowed[h], aimed, ctx->handTrust[h])) {
                    LOGI("hand %d pinched at the picture, the pointer is back", h);
                    ctx->pointerAwake = 1;
                    ctx->handIdleFor = 0.0f;
                }
                ctx->pinchSwallowed[h] = 1;
            }
        }
        else {
            ctx->pinchSwallowed[h] = 0;
        }
        if (ctx->pinchSwallowed[h]) {
            ctx->triggerDown[h] = 0;
            ctx->triggerEdge[h] = 0;
        }
    }

    // A swallowed press stays swallowed until released.
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->triggerDown[h]) {
            ctx->triggerSwallowed[h] = 0;
        }
    }

    // A stick push wakes the pointer and keeps it awake, so scrolling with a
    // still controller works.
    int stick = stickPushed(f->stick[HAND_LEFT]) || stickPushed(f->stick[HAND_RIGHT]);
    if (stick) {
        ctx->pointerAwake = 1;
    }

    if (f->pinching || stick) {
        ctx->stillFor = 0.0f;
        ctx->movingFor = 0.0f;
    }
    else if (f->moved) {
        ctx->movingFor += f->dt;
        ctx->stillFor = 0.0f;
        if (ctx->movingFor >= ctx->pointerWake) {
            ctx->pointerAwake = 1;
        }
    }
    else {
        ctx->stillFor += f->dt;
        ctx->movingFor = 0.0f;
        if (ctx->stillFor >= ctx->pointerSleep) {
            ctx->pointerAwake = 0;
        }
    }

    // Hands: the pointer sleeps once a hand stops pinching or resting on a
    // control, and wakes on an open palm, which also grants extra time to aim.
    int handsPoint = ctx->usingHands[HAND_LEFT] || ctx->usingHands[HAND_RIGHT];
    int handBusy = f->pinching || f->poking;
    for (int h = 0; h < HAND_COUNT; h++) {
        if (!ctx->usingHands[h]) {
            palmHoldStep(&ctx->palmHold[h], 0, f->dt);
            continue;
        }
        if (palmHoldStep(&ctx->palmHold[h], ctx->palmShown[h], f->dt) && !ctx->pointerAwake) {
            LOGI("hand %d showed its palm, the pointer is back", h);
            ctx->pointerAwake = 1;
            ctx->handIdleFor = -HAND_POINTER_SLEEP_SEC;
        }
        int onControl = f->hovers[h] != HOVER_NONE && f->hovers[h] != HOVER_SCREEN
                && f->hovers[h] != HOVER_HALO;
        handBusy |= ctx->palmShown[h] || onControl;
    }
    if (handsPoint && !atomic_load_explicit(&ctx->handAutoHide, memory_order_relaxed)) {
        // Auto-hide off in settings.
        ctx->pointerAwake = 1;
        ctx->stillFor = 0.0f;
        ctx->handIdleFor = 0.0f;
    }
    else if (handsPoint) {
        if (handBusy) {
            ctx->handIdleFor = fminf(ctx->handIdleFor, 0.0f);
        }
        ctx->handIdleFor += f->dt;
        if (handBusy) {
            ctx->stillFor = 0.0f;
        }
        if (ctx->pointerAwake && ctx->handIdleFor >= HAND_POINTER_SLEEP_SEC) {
            LOGI("hands idle, the pointer is gone until a palm is shown");
            ctx->pointerAwake = 0;
        }
    }
    else {
        ctx->handIdleFor = 0.0f;
    }

    // Pointer sleep off: a controller keeps the pointer awake. Hands keep their rule.
    if (!ctx->pointerSleepOn && (ctx->profileKind[HAND_LEFT] == PROFILE_CONTROLLER
                                 || ctx->profileKind[HAND_RIGHT] == PROFILE_CONTROLLER)) {
        ctx->pointerAwake = 1;
        ctx->stillFor = 0.0f;
    }

    // A menu waits on a press, so the pointer stays awake over it, and idle hands
    // do not count down to putting it away
    if (ctx->ui[UI_PANEL_SHEET].visible) {
        ctx->pointerAwake = 1;
        ctx->stillFor = 0.0f;
        ctx->handIdleFor = 0.0f;
    }

    updateControllerClocks(ctx, f);
}

// Chooses the pointing source and its hover.
static void pickPointingSource(XrCtx* ctx, InputFrame* f) {
    // A source holding the trigger on the screen wins; otherwise right first.
    static const int order[SRC_COUNT] = { HAND_RIGHT, HAND_LEFT, SRC_GAZE };

    // While gaze points, idle controllers hover nothing.
    if (gazePointing(ctx)) {
        for (int h = 0; h < HAND_COUNT; h++) {
            if (!handInUse(ctx, h)) {
                f->hovers[h] = HOVER_NONE;
            }
        }
    }

    PointerOffer offers[SRC_COUNT];
    for (int h = 0; h < SRC_COUNT; h++) {
        offers[h].on = f->hovers[h] != HOVER_NONE && f->hovers[h] != HOVER_HALO;
        offers[h].halo = f->hovers[h] == HOVER_HALO;
        offers[h].pressing = ctx->triggerDown[h];
        offers[h].touch = TOUCH_NONE;
        if (h < HAND_COUNT) {
            // Fingers at the bar count as about to touch it
            offers[h].touch = f->touch[h] != TOUCH_NONE ? f->touch[h]
                    : ctx->nearBar[h] ? TOUCH_NEAR : TOUCH_NONE;
        }
    }
    f->hand = pickPointer(offers, order, SRC_COUNT, ctx->pointHand);

    if (!ctx->pointerAwake && ctx->grabMode == GRAB_NONE) {
        f->hand = -1;
        for (int h = 0; h < SRC_COUNT; h++) {
            f->hovers[h] = HOVER_NONE;
        }
    }
    ctx->pointHand = f->hand;

    f->hover = f->hand >= 0 ? f->hovers[f->hand] : HOVER_NONE;
    if (f->hover == HOVER_CORNER) {
        ctx->hoverCorner = f->corners[f->hand];
    }

    // A pinch pressing on something holds it until let go; a press that began on a
    // menu is the menu's, and never a click on what is under it
    for (int h = 0; h < SRC_COUNT; h++) {
        int here = h == f->hand && ctx->grabMode == GRAB_NONE;
        int touching = h < HAND_COUNT && f->touch[h] != TOUCH_NONE;
        if (h < HAND_COUNT) {
            ctx->pinchHold[h] = pinchHoldStep(ctx->pinchHold[h], ctx->pinchDown[h],
                                              here && !touching && ctx->triggerDown[h]
                                              && offers[h].on, ctx->nearBar[h]);
        }
        int was = ctx->uiPress[h];
        ctx->uiPress[h] = uiPressStep(was, ctx->triggerDown[h], ctx->triggerEdge[h],
                                      here && f->hover == HOVER_UIPANEL);
        if (ctx->uiPress[h] && !was && !touching) {
            ctx->triggerSwallowed[h] = 1;
        }
    }
}

// Logs gaze/pinch/awake state when it changes.
static void logInputSnapshot(XrCtx* ctx, InputFrame* f) {
    int snapshot = (f->aimValid[SRC_GAZE] ? 1 : 0) | (f->hand == SRC_GAZE ? 2 : 0)
            | ((ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT]) ? 4 : 0)
            | (ctx->pointerAwake ? 8 : 0);
    if (snapshot != ctx->lastSnapshot) {
        ctx->lastSnapshot = snapshot;
        LOGI("input: gaze tracked %d, pointing by gaze %d, pinch %d, awake %d",
             (snapshot & 1) != 0, (snapshot & 2) != 0, (snapshot & 4) != 0,
             (snapshot & 8) != 0);
    }
}

// During a grab no mouse input is sent and the ray ends on the held handle.
static void beamToHandle(XrCtx* ctx, InputFrame* f) {
    ctx->buttonsDown = 0;
    ctx->scrollCarry = 0.0f;
    ctx->filterU.valid = 0;
    ctx->filterV.valid = 0;

    if (f->headValid) {
        Vec3 local;
        local.z = 0.0f;
        if (ctx->grabMode == GRAB_MOVE) {
            local.x = 0.0f;
            local.y = -(f->bar.height * 0.5f + BAR_DROP_FRAC * f->bar.width);
        }
        else {
            // Brackets sit half a bracket outside the corner.
            float side = cornerSide(ctx);
            local.x = (ctx->grabOppX > 0.0f ? -0.5f : 0.5f) * (ctx->screenWidth + side);
            local.y = (ctx->grabOppY > 0.0f ? -0.5f : 0.5f) * (f->height + side);
        }
        curveLocal(&local, f->radius, f->curved, NULL);
        Vec3 handle = quatRotate(f->screenPose.orientation, local);
        ctx->beamStart = f->aimPoses[ctx->grabHand].position;
        ctx->beamEnd.x = f->screenPose.position.x + handle.x;
        ctx->beamEnd.y = f->screenPose.position.y + handle.y;
        ctx->beamEnd.z = f->screenPose.position.z + handle.z;
        ctx->beamVisible = 1;
    }
}

static void beamToFurniture(XrCtx* ctx, InputFrame* f) {
    Vec3 end;
    int ui = f->hover == HOVER_UIPANEL ? ctx->uiHover[f->hand] : -1;
    if (ui >= 0) {
        float w, h;
        XrPosef pose = uiPanelPose(ctx, ui, &w, &h);
        end = screenPoint(f->hitU[f->hand], f->hitV[f->hand], pose, w, h, 0.0f, 0);
    }
    else {
        end = furniturePoint(ctx, f->hover, f->hitU[f->hand], f->hitV[f->hand],
                             f->screenPose, f->height, f->radius, f->curved, &f->bar);
    }
    ctx->beamStart = f->aimPoses[f->hand].position;
    ctx->beamEnd.x = end.x;
    ctx->beamEnd.y = end.y;
    ctx->beamEnd.z = end.z;
    ctx->beamVisible = f->headValid;
}

// Smooths the hit point, outputs it to Java and ends the ray on it.
static void sendPointer(XrCtx* ctx, InputFrame* f, int hit) {
    if (!hit) {
        return;
    }
    // Reset the filter after a gap or a source change, so the cursor does not slide in.
    if (f->hand != ctx->lastHand || f->now - ctx->lastHitNs > POINTER_RESET_NS) {
        ctx->filterU.valid = 0;
        ctx->filterV.valid = 0;
    }
    ctx->lastHand = f->hand;
    ctx->lastHitNs = f->now;

    float u = euroFilter(&ctx->filterU, f->hitU[f->hand], f->dt, ctx->pointerMinCutoff,
                         ctx->pointerBeta);
    float v = euroFilter(&ctx->filterV, f->hitV[f->hand], f->dt, ctx->pointerMinCutoff,
                         ctx->pointerBeta);
    f->out[IN_HIT] = 1.0f;
    f->out[IN_U] = u;
    f->out[IN_V] = v;

    Vec3 endPoint = screenPoint(u, v, f->screenPose, ctx->screenWidth, f->height, f->radius,
                                f->curved);
    ctx->beamStart = f->aimPoses[f->hand].position;
    ctx->beamEnd.x = endPoint.x;
    ctx->beamEnd.y = endPoint.y;
    ctx->beamEnd.z = endPoint.z;
    ctx->beamVisible = f->headValid;
}

// Computes the mouse button mask and logs trigger transitions.
static void updateButtons(XrCtx* ctx, InputFrame* f, int hit) {
    int mask = 0;
    for (int h = 0; h < HAND_COUNT; h++) {
        // Per hand, so one hand's swallowed press does not block the other.
        if (ctx->triggerDown[h] && !ctx->triggerSwallowed[h]) {
            mask |= VR_BUTTON_LEFT;
        }
    }
    // Controller only
    if (actionBool(ctx, ctx->rightClickAction, -1)) {
        mask |= VR_BUTTON_RIGHT;
    }
    if (actionBool(ctx, ctx->middleClickAction, -1)) {
        mask |= VR_BUTTON_MIDDLE;
    }
    // Presses need a hit; releases always apply, so leaving the screen mid drag releases.
    ctx->buttonsDown = (ctx->buttonsDown & mask) | (hit ? mask : 0);
    f->out[IN_BUTTONS] = (float)ctx->buttonsDown;

    // Click diagnostics, one line per transition. A release logs the minimum
    // analog value and frames below threshold, to spot merged double taps.
    int rawNow = ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT];
    int leftNow = (ctx->buttonsDown & VR_BUTTON_LEFT) ? 1 : 0;
    if (rawNow != ctx->trigLogRaw || leftNow != ctx->trigLogLeft
            || ctx->triggerDown[HAND_LEFT] != ctx->trigLogDown[HAND_LEFT]
            || ctx->triggerDown[HAND_RIGHT] != ctx->trigLogDown[HAND_RIGHT]) {
        char rel[80];
        rel[0] = '\0';
        for (int h = 0; h < HAND_COUNT; h++) {
            if (ctx->trigLogDown[h] && !ctx->triggerDown[h]) {
                snprintf(rel, sizeof(rel),
                         " up hand=%d holdMin=%.2f dipFrames=%d retaps=%d",
                         h, ctx->triggerHoldMin[h], ctx->triggerDipFrames[h],
                         ctx->triggerRetaps[h]);
            }
        }
        LOGI("trig: raw=%d%d val=%.2f,%.2f swal=%d%d edge=%d%d hit=%d awake=%d left=%d%s",
             ctx->triggerDown[HAND_LEFT], ctx->triggerDown[HAND_RIGHT],
             ctx->triggerValue[HAND_LEFT], ctx->triggerValue[HAND_RIGHT],
             ctx->triggerSwallowed[HAND_LEFT], ctx->triggerSwallowed[HAND_RIGHT],
             ctx->triggerEdge[HAND_LEFT], ctx->triggerEdge[HAND_RIGHT],
             hit ? 1 : 0, ctx->pointerAwake, leftNow, rel);

        ctx->trigLogRaw = rawNow;
        ctx->trigLogLeft = leftNow;
        ctx->trigLogDown[HAND_LEFT] = ctx->triggerDown[HAND_LEFT];
        ctx->trigLogDown[HAND_RIGHT] = ctx->triggerDown[HAND_RIGHT];
    }
}

// Turns the stick into scroll clicks while the pointer is on the picture, or
// off it with pointer sleep disabled, or on a player UI panel.
static void updateScroll(XrCtx* ctx, InputFrame* f, int hit) {
    XrVector2f stick = actionVec2(ctx, ctx->scrollAction, -1);
    int offPicture = !ctx->pointerSleepOn
            && (f->hover == HOVER_NONE || f->hover == HOVER_HALO);
    int onPanel = f->hover == HOVER_UIPANEL;
    // A held bar or grip takes the stick to reel the screen
    if (ctx->grabMode == GRAB_NONE && (hit || offPicture || onPanel)
            && fabsf(stick.y) > SCROLL_DEADZONE) {
        float past = (fabsf(stick.y) - SCROLL_DEADZONE) / (1.0f - SCROLL_DEADZONE);
        ctx->scrollCarry += copysignf(past * SCROLL_CLICKS_PER_SEC * f->dt, stick.y);
    }
    else {
        ctx->scrollCarry = 0.0f;
    }
    float clicks = truncf(ctx->scrollCarry);
    ctx->scrollCarry -= clicks;
    f->out[IN_SCROLL] = clicks;
}

// With no target, the ray extends FREE_BEAM_M into space rather than vanishing.
static void beamIntoRoom(XrCtx* ctx, InputFrame* f) {
    if (!ctx->beamVisible && ctx->pointerAwake && f->headValid && !ctx->beamGaze) {
        int free = f->hand;
        if (free < 0) {
            free = canPoint(ctx, f, HAND_RIGHT) ? HAND_RIGHT
                    : (canPoint(ctx, f, HAND_LEFT) ? HAND_LEFT : -1);
        }
        if (free >= 0) {
            Vec3 forward = { 0.0f, 0.0f, -1.0f };
            Vec3 d = quatRotate(f->aimPoses[free].orientation, forward);
            ctx->beamStart = f->aimPoses[free].position;
            ctx->beamEnd.x = ctx->beamStart.x + d.x * FREE_BEAM_M;
            ctx->beamEnd.y = ctx->beamStart.y + d.y * FREE_BEAM_M;
            ctx->beamEnd.z = ctx->beamStart.z + d.z * FREE_BEAM_M;
            ctx->beamVisible = 1;
            // No cursor dot without a target.
            ctx->beamFree = 1;
        }
    }
}

// Fills the state-derived output slots last, then copies everything to Java.
static void handBack(JNIEnv* env, XrCtx* ctx, float* out, jfloatArray outArr) {
    out[IN_STEREO] = 0.0f;
    out[IN_PLAYER_BUTTONS] = 0.0f;
    out[IN_PLAYER_STICK_X] = 0.0f;
    if (ctx != NULL) {
        int buttons = 0;
        float stickX = 0.0f;
        for (int h = 0; h < HAND_COUNT; h++) {
            if (ctx->profileKind[h] != PROFILE_CONTROLLER) {
                continue;
            }
            // Bits: A=1, B=2 (right), X=4, Y=8 (left).
            if (actionBool(ctx, ctx->rightClickAction, h)) {
                buttons |= h == HAND_RIGHT ? 1 : 4;
            }
            if (actionBool(ctx, ctx->middleClickAction, h)) {
                buttons |= h == HAND_RIGHT ? 2 : 8;
            }
            XrVector2f stick = actionVec2(ctx, ctx->scrollAction, h);
            if (fabsf(stick.x) > fabsf(stickX)) {
                stickX = stick.x;
            }
        }
        out[IN_PLAYER_BUTTONS] = (float)buttons;
        out[IN_PLAYER_STICK_X] = stickX;
        out[IN_STEREO] = ctx->stereoMode != DEPTH_MODE_OFF && ctx->stereoLive ? 1.0f : 0.0f;
    }
    (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
}

static void placeAtHead(XrCtx* ctx);

// Per-frame input pass. Fills the IN_ slots that Java turns into UI and mouse
// events.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUpdateInput(JNIEnv* env, jobject thiz,
                                                              jlong handle, jfloat distance,
                                                              jfloat quadWidth, jfloat curvature,
                                                              jboolean headLocked,
                                                              jboolean pointerEnabled,
                                                              jboolean gazeEnabled,
                                                              jboolean pointerSleep,
                                                              jfloatArray outArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    float out[IN_SLOTS];
    memset(out, 0, sizeof(out));
    if (ctx != NULL) {
        ctx->gazeEnabled = gazeEnabled;
        ctx->pointerSleepOn = pointerSleep;
        ctx->headLockedPref = headLocked;
        placeAtHead(ctx);
        // Uses last frame's clocks and gaze; must run before anything asks who points.
        updateControllerAwake(ctx);
        updateGazeBridge(ctx);
        ctx->prefCurvature = curvature;
    }

    // Release everything held when input is not read, including under the splash.
    if (ctx == NULL || !ctx->inputReady || !ctx->placementValid
            || ctx->sessionState != XR_SESSION_STATE_FOCUSED
            || ctx->splash.phase != SPLASH_GONE) {
        if (ctx != NULL) {
            releaseInput(ctx);
        }
        handBack(env, ctx, out, outArr);
        return;
    }

    XrActiveActionSet active;
    active.actionSet = ctx->actionSet;
    active.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(ctx->session, &sync))) {
        ctx->buttonsDown = 0;
        handBack(env, ctx, out, outArr);
        return;
    }
    // Controller models read their grip poses after this sync.
    ctx->actionsSynced = 1;

    if (!pointerEnabled) {
        releaseInput(ctx);
        handBack(env, ctx, out, outArr);
        return;
    }

    // Stick click toggles the pointer
    int toggle = actionBool(ctx, ctx->toggleAction, -1);
    if (toggle && !ctx->togglePrev) {
        ctx->pointerOn = !ctx->pointerOn;
        LOGI("pointer %s", ctx->pointerOn ? "on" : "off");
    }
    ctx->togglePrev = toggle;
    out[IN_POINTER] = ctx->pointerOn ? 1.0f : 0.0f;

    InputFrame f;
    memset(&f, 0, sizeof(f));
    f.out = out;
    out[IN_UI_PANEL] = -1.0f;
    out[IN_TOUCH] = TOUCH_NONE;
    // Must match what endFrame submits
    f.xform.toHead = headLocked;
    f.height = ctx->screenWidth * screenAspect(ctx);
    f.curved = effectiveCurvature(ctx) > 0.01f && ctx->cylinderSupported;
    f.radius = ctx->screenRadius;
    f.cornerSide = cornerSide(ctx);
    f.screenPose = ctx->screenPose;
    f.bar = barFrame(ctx);

    f.now = nowNs();
    f.dt = ctx->lastInputNs != 0 ? (f.now - ctx->lastInputNs) / 1e9f : 0.0f;
    ctx->lastInputNs = f.now;
    if (f.dt > 0.1f) {
        f.dt = 0.1f;
    }

    // Always located in local space, head locked or not (see FrameXform).
    XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
    int headOk = XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, ctx->localSpace,
                                            ctx->predictedDisplayTime, &head));
    const XrSpaceLocationFlags placed = XR_SPACE_LOCATION_POSITION_VALID_BIT
            | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    f.xform.ok = headOk && (head.locationFlags & placed) == placed;
    f.xform.head = head.pose;
    if (f.xform.toHead) {
        // In its own frame the head is the identity pose.
        memset(&f.headPose, 0, sizeof(f.headPose));
        f.headPose.orientation.w = 1.0f;
        f.headValid = f.xform.ok;
    }
    else {
        f.headPose = head.pose;
        f.headValid = headOk && (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    }
    if (f.headValid) {
        ctx->headPos = f.headPose.position;
    }
    updateHeadTurn(ctx, &f);

    readSources(ctx, &f);
    gazeBridgeTrack(&ctx->gazeBridge, f.gazeAsked, f.gazeUsable, f.now);
    gazeTrigger(ctx, &f);
    updatePointerWake(ctx, &f);
    pickPointingSource(ctx, &f);
    logInputSnapshot(ctx, &f);

    if (f.hand >= 0 && f.hand < HAND_COUNT) {
        out[IN_TOUCH] = (float)f.touch[f.hand];
    }

    // UI panel hit, which Java treats as a touch.
    if (f.hand >= 0 && f.hover == HOVER_UIPANEL && ctx->uiHover[f.hand] >= 0
            && ctx->grabMode == GRAB_NONE) {
        out[IN_UI_PANEL] = (float)ctx->uiHover[f.hand];
        out[IN_UI_U] = f.hitU[f.hand];
        out[IN_UI_V] = f.hitV[f.hand];
        // A fingertip presses what it touches; a ray press must have begun on a menu
        int touching = f.hand < HAND_COUNT && f.touch[f.hand] != TOUCH_NONE;
        int pressed = touching ? ctx->triggerDown[f.hand] : ctx->uiPress[f.hand];
        out[IN_UI_PRESS] = pressed ? 1.0f : 0.0f;
    }

    // Off the picture a trigger press cannot be a click, so it may grab.
    int offPicture = f.hand >= 0 && (f.hitU[f.hand] < 0.0f || f.hitU[f.hand] > 1.0f
                                     || f.hitV[f.hand] < 0.0f || f.hitV[f.hand] > 1.0f);
    applyGrab(ctx, f.aimPoses, f.aimValid, f.grab, f.hand, f.hover, ctx->hoverCorner,
              offPicture, f.height, f.curved, f.stick, f.dt);
    f.screenPose = ctx->screenPose;
    f.height = ctx->screenWidth * screenAspect(ctx);
    f.radius = ctx->screenRadius;
    f.bar = barFrame(ctx);

    // A dragged handle stays highlighted wherever the ray goes.
    if (ctx->grabMode == GRAB_MOVE) {
        ctx->hoverKind = HOVER_BAR;
    }
    else if (ctx->grabMode == GRAB_RESIZE) {
        ctx->hoverKind = HOVER_CORNER;
    }
    else {
        ctx->hoverKind = f.hover;
    }
    ctx->backHover = f.hand >= 0 && f.hover == HOVER_UIPANEL && ctx->uiHover[f.hand] == UI_PANEL_BACK;

    ctx->beamVisible = 0;
    ctx->beamFree = 0;
    // Gaze draws no ray or cursor; hover highlights are the feedback. A gaze
    // grab counts as gaze even though a hand carries it.
    ctx->beamGaze = ctx->grabMode != GRAB_NONE
            ? (ctx->grabByGaze || ctx->grabHand == SRC_GAZE) : f.hand == SRC_GAZE;

    if (ctx->grabMode != GRAB_NONE) {
        beamToHandle(ctx, &f);
        writeInputPose(ctx, out);
        handBack(env, ctx, out, outArr);
        return;
    }

    if (!ctx->pointerOn) {
        // With the pointer off, the ray still works on handles and panels.
        if (f.hand >= 0 && f.hover != HOVER_NONE && f.hover != HOVER_SCREEN) {
            beamToFurniture(ctx, &f);
        }
        ctx->buttonsDown = 0;
        writeInputPose(ctx, out);
        handBack(env, ctx, out, outArr);
        return;
    }

    // Only the picture itself moves the cursor.
    int hit = (f.hover == HOVER_SCREEN || f.hover == HOVER_CORNER) && f.hand != SRC_GAZE;
    if ((f.hover == HOVER_BAR || f.hover == HOVER_HALO || f.hover == HOVER_UIPANEL)
            && f.headValid && f.hand >= 0) {
        beamToFurniture(ctx, &f);
    }
    sendPointer(ctx, &f, hit);
    updateButtons(ctx, &f, hit);
    updateScroll(ctx, &f, hit);
    beamIntoRoom(ctx, &f);

    writeInputPose(ctx, out);
    handBack(env, ctx, out, outArr);
}

// Restores a saved placement. sliderSeen = 0 stops the first frame from
// reseeding over it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetScreenPose(JNIEnv* env, jobject thiz,
                                                                jlong handle, jfloatArray poseArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || poseArr == NULL) {
        return;
    }
    float p[POSE_VALUES];
    if ((*env)->GetArrayLength(env, poseArr) < POSE_VALUES) {
        return;
    }
    (*env)->GetFloatArrayRegion(env, poseArr, 0, POSE_VALUES, p);

    // The narrowest a screen can be made is at the near limit (a window pulled in to be
    // touched); a little under it allows for the save's rounding
    float narrowest = HANDLE_BASE_WIDTH * HANDLE_SCALE_MIN_NEAR * 0.95f;
    if (p[7] < narrowest || p[7] > 20.0f || p[8] <= 0.0f) {
        LOGW("stored screen placement out of range, ignoring it");
        return;
    }
    // -1 = no panel curve set; out-of-range values are discarded.
    ctx->panelCurve = (p[9] >= 0.0f && p[9] <= 1.0f) ? p[9] : -1.0f;

    ctx->screenPose.position.x = p[0];
    ctx->screenPose.position.y = p[1];
    ctx->screenPose.position.z = p[2];
    ctx->screenPose.orientation.x = p[3];
    ctx->screenPose.orientation.y = p[4];
    ctx->screenPose.orientation.z = p[5];
    ctx->screenPose.orientation.w = p[6];
    ctx->screenPose.orientation = quatNorm(ctx->screenPose.orientation);
    ctx->screenWidth = p[7];
    ctx->screenRadius = p[8];
    ctx->placementValid = 1;
    ctx->sliderSeen = 0;
    LOGI("restored screen placement %.2f %.2f %.2f, %.2f m wide",
         p[0], p[1], p[2], p[7]);
}

// Straight ahead at the restored distance, height and size: a placement saved under
// another session's origin would land anywhere otherwise. Any thread.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeRecentreScreen(JNIEnv* env, jobject thiz,
                                                                 jlong handle) {
    (void)env;
    (void)thiz;
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx != NULL) {
        ctx->placeAtHead = 1;
    }
}

// Turns a restored placement to face where the head looks now, around the head,
// keeping its distance, height, size and tilt. Once the head is tracked.
static void placeAtHead(XrCtx* ctx) {
    if (!ctx->placeAtHead || !ctx->placementValid || !ctx->sessionRunning) {
        return;
    }
    const XrSpaceLocationFlags placed = XR_SPACE_LOCATION_POSITION_VALID_BIT
            | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
            | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(ctx->viewSpace, ctx->localSpace, ctx->predictedDisplayTime,
                                &head))
            || (head.locationFlags & placed) != placed) {
        return;
    }
    ctx->placeAtHead = 0;
    XrPosef ahead = poseRecentred(ctx->screenPose);
    Vec3 back = { 0.0f, 0.0f, -1.0f };
    Vec3 look = quatRotate(head.pose.orientation, back);
    float yaw = atan2f(-look.x, -look.z);
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    XrQuaternionf turn = axisAngleQuat(up, yaw);
    Vec3 flat = { ahead.position.x, 0.0f, ahead.position.z };
    Vec3 out = quatRotate(turn, flat);
    ctx->screenPose.position.x = head.pose.position.x + out.x;
    ctx->screenPose.position.y = ahead.position.y;
    ctx->screenPose.position.z = head.pose.position.z + out.z;
    ctx->screenPose.orientation = quatNorm(quatMul(turn, ahead.orientation));
    ctx->grabMode = GRAB_NONE;
    LOGI("screen placed in front of the head: %.2f m ahead, turned %.0f degrees",
         sqrtf(out.x * out.x + out.z * out.z), yaw * 180.0f / (float)M_PI);
}
