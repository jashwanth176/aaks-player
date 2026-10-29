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

// Binding levels: aim and trigger only; everything the pointer uses; plus the
// left menu button.
#define BIND_REDUCED 0
#define BIND_FULL 1
#define BIND_MENU 2

// One unsupported path rejects a whole profile, so on rejection this retries
// without haptics, then without the menu button, then with aim and trigger only.
static void suggestBindings(XrCtx* ctx, const char* profile, int level, int haptic) {
    XrActionSuggestedBinding b[24];
    uint32_t n = 0;
    int full = level >= BIND_FULL;
    int menu = 0;
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

        if (haptic && ctx->hapticAction != XR_NULL_HANDLE) {
            snprintf(path, sizeof(path), "%s/output/haptic", hands[h]);
            b[n].action = ctx->hapticAction;
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

        // Left only: the right menu button is reserved by the system.
        if (level >= BIND_MENU && h == HAND_LEFT && ctx->menuAction != XR_NULL_HANDLE) {
            snprintf(path, sizeof(path), "%s/input/menu/click", hands[h]);
            b[n].action = ctx->menuAction;
            b[n++].binding = toPath(ctx, path);
            menu = 1;
        }
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    int vibration = haptic && ctx->hapticAction != XR_NULL_HANDLE;
    XrResult res = xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
    if (XR_SUCCEEDED(res)) {
        LOGI("bindings accepted for %s (%s%s)", profile,
             menu ? "full, with the menu button" : (full ? "full" : "reduced"),
             vibration ? ", with the vibration" : "");
        ctx->menuBound |= menu;
        ctx->hapticBound |= vibration;
    }
    else if (vibration) {
        LOGW("bindings with the vibration rejected for %s (%d), trying without it", profile, res);
        suggestBindings(ctx, profile, level, 0);
    }
    else if (menu) {
        LOGW("full bindings with the menu button rejected for %s (%d), trying without it",
             profile, res);
        suggestBindings(ctx, profile, BIND_FULL, 0);
    }
    else if (full) {
        LOGW("full bindings rejected for %s (%d), trying aim and trigger only", profile, res);
        suggestBindings(ctx, profile, BIND_REDUCED, 0);
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
    ctx->menuAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu",
                                 "Start, or with the grip the gamepad switch");
    // Driven only by gamepad-mode rumble.
    ctx->hapticAction = makeAction(ctx, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble",
                                   "Rumble from the game");

    if (ctx->aimAction == XR_NULL_HANDLE || ctx->triggerAction == XR_NULL_HANDLE) {
        return 0;
    }

    // The simple controller has no stick or face buttons: aim, trigger, grip
    // and haptics only.
    suggestBindings(ctx, "/interaction_profiles/khr/simple_controller", BIND_FULL, 1);
    suggestBindings(ctx, "/interaction_profiles/oculus/touch_controller", BIND_MENU, 1);
    if (ctx->picoInteraction) {
        // One extension covers both; the Neo 3 needs its own suggestion or it
        // falls back to the simple controller.
        suggestBindings(ctx, "/interaction_profiles/bytedance/pico4_controller", BIND_MENU, 1);
        suggestBindings(ctx, "/interaction_profiles/bytedance/pico_neo3_controller", BIND_MENU, 1);
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
    padRumbleReset(&ctx->rumble);
    LOGI("controller input ready (pico bindings %s, hand pinch %s, menu button %s, "
         "vibration %s)",
         ctx->picoInteraction ? "offered" : "not offered by this runtime",
         ctx->handClickOk ? "bound" : (ctx->jointTracking ? "from joints" : "unavailable"),
         ctx->menuBound ? "bound" : "not bound, so gamepad mode has no Start or switch",
         ctx->hapticBound ? "bound" : "not bound, so gamepad mode has no rumble");
    LOGEV("hand lock: three pinches on one hand, the third within %ld ms of the first, the "
          "third held back from the host", TRIPLE_PINCH_WINDOW_NS / 1000000L);
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

// Inputs as the pointer sees them, after the gamepad chord filter may hold one
// back or replay it as a tap. A stick reads centred while its click is held
// back. Hands give pinch and grasp in place of trigger and grip.
static float seenTrigger(XrCtx* ctx, int h) {
    XrAction action = onHandActions(ctx, h) ? ctx->handPinchAction : ctx->triggerAction;
    return padChordSeen(&ctx->padChord, h == HAND_LEFT ? PAD_PART_LT : PAD_PART_RT,
                        actionFloat(ctx, action, h));
}

static float seenGrip(XrCtx* ctx, int h) {
    XrAction action = onHandActions(ctx, h) ? ctx->handGraspAction : ctx->grabAction;
    return padChordSeen(&ctx->padChord, h == HAND_LEFT ? PAD_PART_LG : PAD_PART_RG,
                        actionFloat(ctx, action, h));
}

static int seenStickClick(XrCtx* ctx, int h) {
    int raw = actionBool(ctx, ctx->toggleAction, h);
    return padChordSeen(&ctx->padChord, h == HAND_LEFT ? PAD_PART_LS : PAD_PART_RS,
                        raw ? 1.0f : 0.0f) > 0.5f;
}

static XrVector2f seenStick(XrCtx* ctx, int h) {
    XrVector2f stick = actionVec2(ctx, ctx->scrollAction, h);
    if (ctx->padChord.held & (h == HAND_LEFT ? PAD_PART_LS : PAD_PART_RS)) {
        stick.x = 0.0f;
        stick.y = 0.0f;
    }
    return stick;
}

static int chordHasStickClick(XrCtx* ctx) {
    return ((ctx->padChord.held | ctx->padChord.replay) & (PAD_PART_LS | PAD_PART_RS)) != 0;
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

    XrHandJointsLocateInfoEXT locate = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
    locate.baseSpace = ctx->localSpace;
    locate.time = ctx->predictedDisplayTime;
    int located = XR_SUCCEEDED(ctx->pfnLocateHandJoints(ctx->handTrackers[hand], &locate,
                                                         &locations))
            && locations.isActive;
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
    if (!sane || trust < HAND_TRUST_MIN) {
        ctx->aimPinch[hand] = 0;
        ctx->pinchPointValid[hand] = 0;
        ctx->handRayValid[hand] = 0;
        return;
    }
    ctx->handTrust[hand] = trust;

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
        ctx->screenWidth = ctx->playerMode && ctx->playerScreenHidden
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
// While a room holds the screen, the saved placement is turned instead.
void recentreScreen(XrCtx* ctx) {
    ctx->grabMode = GRAB_NONE;
    if (!ctx->placementValid) {
        LOGI("recentred before the screen was placed");
        return;
    }
    int inRoom = ctx->roomHoldingScreen;
    XrPosef* pose = inRoom ? &ctx->savedScreenPose : &ctx->screenPose;
    XrPosef was = *pose;
    *pose = poseRecentred(was);
    if (inRoom) {
        // Written back when the room releases the screen.
        ctx->recentredInRoom = 1;
    }
    else {
        ctx->poseDirty = 1;
    }
    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 fwd = quatRotate(was.orientation, back);
    XrVector3f p = pose->position;
    LOGEV("recentred%s: screen from %.2f %.2f %.2f, turned %.1f deg, to %.2f %.2f %.2f;"
          " kept distance %.2f m, width %.2f m, radius %.2f m, curve %.2f, head lock %d",
          inRoom ? " behind the room" : "", was.position.x, was.position.y, was.position.z,
          atan2f(fwd.x, fwd.z) * 180.0f / (float)M_PI, p.x, p.y, p.z,
          sqrtf(p.x * p.x + p.y * p.y + p.z * p.z),
          inRoom ? ctx->savedScreenWidth : ctx->screenWidth,
          inRoom ? ctx->savedScreenRadius : ctx->screenRadius, effectiveCurvature(ctx),
          ctx->headLockedPref);
}

// Output only when dirty (end of a grab), so preferences are written once per move.
static void writeInputPose(XrCtx* ctx, float* out) {
    if (!ctx->poseDirty) {
        return;
    }
    ctx->poseDirty = 0;
    out[IN_POSE_DIRTY] = 1.0f;
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

// Move and resize start only from a handle under the ray; gripping the picture
// itself does nothing, to avoid accidental drags.
static void applyGrab(XrCtx* ctx, XrPosef* aims, const int* valid, const float* grab, int hand,
                      int hover, int corner, int offPicture, float height, int curved) {
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
            ctx->grabMode = GRAB_NONE;
            ctx->grabByGaze = 0;
            ctx->poseDirty = 1;
            if (ctx->playerMode) {
                startSpringIfOut(ctx);
            }
            return;
        }
    }

    if (ctx->grabMode == GRAB_NONE && ctx->playerMode) {
        stepSpring(ctx);
    }

    if (ctx->grabMode == GRAB_NONE) {
        // A room forces the screen pose each frame, so no move there; only a
        // resizable room's corners can be grabbed.
        int roomStyle = roomEffective(ctx);
        if (roomStyle > 0 && (hover != HOVER_CORNER || !roomResizable(roomStyle))) {
            return;
        }
        if (hand < 0 || (hover != HOVER_BAR && hover != HOVER_CORNER)) {
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

        if (hover == HOVER_BAR) {
            ctx->grabMode = GRAB_MOVE;
            if (ctx->playerMode) {
                ctx->grabHandDist = distFromHead(ctx, aims[mover].position);
                ctx->grabScreenDist = distFromHead(ctx, ctx->screenPose.position);
                ctx->grabSizeRatio = handleSizeRatio(ctx->grabScreenDist, ctx->screenWidth);
                Vec3 back = { 0.0f, 0.0f, 1.0f };
                Vec3 fwd = quatRotate(ctx->screenPose.orientation, back);
                ctx->playerYaw = atan2f(fwd.x, fwd.z);
                ctx->playerPitch = screenPitch(ctx);
                ctx->playerTurnNs = ctx->lastInputNs;
            }
            // Captured once: re-extracting each frame lets rounding drift the tilt.
            ctx->grabPitch = screenPitch(ctx);
            ctx->grabRoll = screenRoll(ctx);
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
        // A gaze drag starts from the corner itself (reach 1).
        ctx->grabRoomPercent = roomStyle > 0 ? roomScreenPercent(ctx, roomStyle) : 0;
        ctx->grabRoomReach = ctx->grabByGaze ? 1.0f : diagonalReach(ctx, u, v);
        ctx->grabMode = GRAB_RESIZE;
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

            if (ctx->playerMode) {
                // Direction from the rigid attach; distance reeled by the hand
                // within elastic limits; width keeps its place in the range.
                float dist = handleElasticDistance(handleReelDistance(
                        ctx->grabScreenDist, ctx->grabHandDist,
                        distFromHead(ctx, aims[h].position)));
                placeAlongView(ctx, dist, handleWidthFor(dist, ctx->grabSizeRatio));
            }
        }

        // Orientation does not follow the wrist: the screen keeps its tilt and
        // roll and yaws to face the viewer.
        float hx = ctx->headPos.x - ctx->screenPose.position.x;
        float hz = ctx->headPos.z - ctx->screenPose.position.z;
        if (ctx->playerMode) {
            // Player mode eases yaw and pitch toward the eyes, never rolls.
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
        // Directly above or below the head yaw is undefined; keep last frame's.
        if (hx * hx + hz * hz > 0.0025f) {
            ctx->screenPose.orientation = screenOrient(atan2f(hx, hz), ctx->grabPitch,
                                                       ctx->grabRoll);
        }
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

    int roomStyle = roomEffective(ctx);
    if (roomStyle > 0) {
        // In a room the drag sets the picture's share of the room screen
        // (25-100%, whole percent); saved on release.
        int percent = roomResizePercent(ctx->grabRoomPercent, ctx->grabRoomReach, reach);
        if (percent != ctx->roomScreen[roomStyle]) {
            ctx->roomScreen[roomStyle] = percent;
            ctx->roomScreenUnsaved = roomStyle;
        }
        // Applied now so the ray and bracket move with the picture this frame.
        applyRoomPlacement(ctx, roomStyle, screenAspect(ctx), 0);
        return;
    }

    float width = ctx->grabWidth * scale;
    if (ctx->playerMode) {
        // Elastic past the distance's width range; springs back on release.
        width = handleElasticWidth(distFromHead(ctx, ctx->grabScreen.position), width);
    }
    else {
        if (width < SCREEN_MIN_WIDTH) width = SCREEN_MIN_WIDTH;
        if (width > SCREEN_MAX_WIDTH) width = SCREEN_MAX_WIDTH;
    }

    // Radius scales with width so the arc keeps its shape.
    ctx->screenRadius = ctx->grabRadius * (width / ctx->grabWidth);
    ctx->screenWidth = width;

    // Grows about its centre.
    ctx->screenPose.position = ctx->grabScreen.position;
    ctx->screenPose.orientation = ctx->grabScreen.orientation;
}

// Marks a press as consumed by an overlay. For gaze, the pinching hands' presses
// are consumed too.
static void swallowTrigger(XrCtx* ctx, int src) {
    if (src < 0 || src >= SRC_COUNT) {
        return;
    }
    ctx->triggerSwallowed[src] = 1;
    if (src == SRC_GAZE) {
        for (int h = 0; h < HAND_COUNT; h++) {
            if (ctx->triggerDown[h]) {
                ctx->triggerSwallowed[h] = 1;
            }
        }
    }
}

// Whether a hover target lies on the bar's frame rather than the picture.
static int onFurniture(int hover) {
    return hover == HOVER_BAR || hover == HOVER_ENVBUTTON || hover == HOVER_COGBUTTON
            || hover == HOVER_KBBUTTON || hover == HOVER_EXITBUTTON
            || hover == HOVER_STEREOBUTTON || hover == HOVER_RAYBUTTON
            || hover == HOVER_AIMBUTTON || hover == HOVER_PADBUTTON;
}

// World point for a hit on (u, v), on whichever surface the hover belongs to.
static Vec3 furniturePoint(XrCtx* ctx, int hover, float u, float v, XrPosef screenPose,
                           float height, float radius, int curved, const BarFrame* bar) {
    if (onFurniture(hover)) {
        return screenPoint(u, v, bar->pose, bar->width, bar->height, bar->radius, bar->curved);
    }
    if (hover == HOVER_PICKER) {
        float pickW, pickH;
        XrPosef pose = pickerPose(ctx, &pickW, &pickH);
        return screenPoint(u, v, pose, pickW, pickH, 0.0f, 0);
    }
    if (hover == HOVER_COGPANEL) {
        return screenPoint(u, v, ctx->cogPose, ctx->cogW, ctx->cogH, 0.0f, 0);
    }
    if (hover == HOVER_KBPANEL) {
        return screenPoint(u, v, ctx->kbPose, ctx->kbW, ctx->kbH, 0.0f, 0);
    }
    if (hover == HOVER_EXITPROMPT) {
        return screenPoint(u, v, ctx->exitPose, ctx->exitW, ctx->exitH, 0.0f, 0);
    }
    if (hover == HOVER_REPORT) {
        return screenPoint(u, v, ctx->reportPose, ctx->reportW, ctx->reportH, 0.0f, 0);
    }
    if (hover == HOVER_HINT) {
        return screenPoint(u, v, ctx->hintPose, ctx->hintW, ctx->hintH, 0.0f, 0);
    }
    if (hover == HOVER_KOFI) {
        return screenPoint(u, v, ctx->kofiPose, ctx->kofiW, ctx->kofiH, 0.0f, 0);
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
    int roomOn;
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

// In gamepad mode controllers drive the pad, not the pointer; tracked hands still point.
static int padHand(XrCtx* ctx, int h) {
    return ctx->padMode && ctx->profileKind[h] == PROFILE_CONTROLLER;
}

// Clears a pad controller's pointer state and filters, so its ray does not
// sweep in from a stale pose on return.
static void padHandAway(XrCtx* ctx, int h) {
    ctx->triggerDown[h] = 0;
    ctx->triggerEdge[h] = 0;
    ctx->triggerValue[h] = 0.0f;
    ctx->pinchDown[h] = 0;
    ctx->aimTracked[h] = 0;
    ctx->aimFilterPos[h][0].valid = 0;
    ctx->aimFilterPos[h][1].valid = 0;
    ctx->aimFilterPos[h][2].valid = 0;
    ctx->aimFilterRot[h].valid = 0;
    ctx->poseSeen[h] = 0;
}

// Whether a gaze-started drag is in progress.
static int gazeHoldingPress(XrCtx* ctx) {
    return (ctx->grabMode != GRAB_NONE && (ctx->grabByGaze || ctx->grabHand == SRC_GAZE))
            || (ctx->cogDragSlider >= 0
                && (ctx->cogDragByGaze || ctx->cogDragHand == SRC_GAZE));
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
            || ctx->grabMode != GRAB_NONE || ctx->cogDragSlider >= 0;
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
static void releaseInput(XrCtx* ctx, float* out) {
    ctx->buttonsDown = 0;
    ctx->beamVisible = 0;
    ctx->aimTracked[HAND_LEFT] = 0;
    ctx->aimTracked[HAND_RIGHT] = 0;
    // Not asked for, which must not count as gaze lost.
    gazeBridgeTrack(&ctx->gazeBridge, 0, 0, 0);
    if (ctx->grabMode != 0) {
        // End the grab, or the stale anchor makes the screen jump on refocus.
        ctx->grabMode = 0;
        ctx->grabByGaze = 0;
        ctx->poseDirty = 1;
    }
    if (ctx->cogDragSlider >= 0) {
        // Still writes the value; out is flushed on return.
        cogDragEnded(ctx, out);
    }
}

// Refines a hover into a bar button. u and v are on the bar's frame.
static int furnitureHover(XrCtx* ctx, int hover, float u, float v, const BarFrame* bar) {
    // These two overhang the bar's zone, so they are tested before a hand is picked.
    if ((hover == HOVER_NONE || hover == HOVER_BAR)
            && barButtonHit(ctx, BAR_SLOT_ENV, u, v, bar)) {
        hover = HOVER_ENVBUTTON;
    }
    if ((hover == HOVER_NONE || hover == HOVER_BAR)
            && barButtonHit(ctx, BAR_SLOT_COG, u, v, bar)) {
        hover = HOVER_COGBUTTON;
    }
    // The outer buttons sit in the halo past the bar's ends and must claim it back.
    static const int OUTER[6] = {
        BAR_SLOT_KB, BAR_SLOT_EXIT, BAR_SLOT_PAD, BAR_SLOT_AIM, BAR_SLOT_RAY, BAR_SLOT_STEREO
    };
    static const int HOVERS[6] = {
        HOVER_KBBUTTON, HOVER_EXITBUTTON, HOVER_PADBUTTON, HOVER_AIMBUTTON, HOVER_RAYBUTTON,
        HOVER_STEREOBUTTON
    };
    for (int i = 0; i < 6; i++) {
        if ((hover == HOVER_NONE || hover == HOVER_BAR || hover == HOVER_HALO)
                && barButtonHit(ctx, OUTER[i], u, v, bar)) {
            hover = HOVERS[i];
        }
    }
    return hover;
}

// Room hover priority: picture, bar buttons, corner brackets, bar zone, then
// margin. Buttons beat brackets because on a small picture they sit under the
// bottom corners. The hit is left in the claiming surface's coordinates.
static void roomHover(XrCtx* ctx, InputFrame* f, int h) {
    float pu = 0.0f, pv = 0.0f;
    int picture = HOVER_NONE;
    if (screenProject(f->aimPoses[h], f->screenPose, ctx->screenWidth, f->height,
                      f->radius, f->curved, &pu, &pv)) {
        picture = hoverTest(pu, pv, ctx->screenWidth, f->height, f->cornerSide,
                            &f->corners[h]);
    }
    if (picture == HOVER_SCREEN) {
        f->hovers[h] = picture;
        f->hitU[h] = pu;
        f->hitV[h] = pv;
        return;
    }

    float su, sv;
    const BarFrame* bar = &f->bar;
    int row = HOVER_NONE;
    if (screenProject(f->aimPoses[h], bar->pose, bar->width, bar->height, bar->radius,
                      bar->curved, &su, &sv)) {
        int unused;
        row = furnitureHover(ctx, hoverTest(su, sv, bar->width, bar->height, 0.0f, &unused),
                             su, sv, bar);
    }
    if (onFurniture(row) && row != HOVER_BAR) {
        f->hovers[h] = row;
        f->hitU[h] = su;
        f->hitV[h] = sv;
        return;
    }
    if (picture == HOVER_CORNER) {
        f->hovers[h] = picture;
        f->hitU[h] = pu;
        f->hitV[h] = pv;
        return;
    }
    if (row == HOVER_BAR) {
        f->hovers[h] = row;
        f->hitU[h] = su;
        f->hitV[h] = sv;
        return;
    }

    // In a room the picture's own bar zone is unused.
    f->hovers[h] = picture == HOVER_BAR ? HOVER_HALO : picture;
    f->hitU[h] = pu;
    f->hitV[h] = pv;
}

// Finds what a source's ray is over.
static void hoverSource(XrCtx* ctx, InputFrame* f, int h) {
    // Corner brackets first: the library page fills its window to the edges.
    if (ctx->playerMode && !f->roomOn && f->cornerSide > 0.0f) {
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

    // The keyboard is not modal but occludes what is behind it.
    float kbU, kbV;
    int onKeyboard = ctx->kbOpen
            && screenProject(f->aimPoses[h], ctx->kbPose, ctx->kbW, ctx->kbH, 0.0f, 0,
                             &kbU, &kbV)
            && kbU >= 0.0f && kbU <= 1.0f && kbV >= 0.0f && kbV <= 1.0f;
    if (onKeyboard) {
        f->hovers[h] = HOVER_KBPANEL;
        f->hitU[h] = kbU;
        f->hitV[h] = kbV;
    }
    else if (!f->roomOn) {
        if (screenProject(f->aimPoses[h], f->screenPose, ctx->screenWidth, f->height,
                          f->radius, f->curved, &f->hitU[h], &f->hitV[h])) {
            int hover = hoverTest(f->hitU[h], f->hitV[h], ctx->screenWidth, f->height,
                                  f->cornerSide, &f->corners[h]);
            f->hovers[h] = furnitureHover(ctx, hover, f->hitU[h], f->hitV[h], &f->bar);
        }
    }
    else {
        roomHover(ctx, f, h);
    }
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
    for (int h = 0; h < HAND_COUNT; h++) {
        if (padHand(ctx, h)) {
            padHandAway(ctx, h);
        }
    }
    // In gaze mode the eyes point and the hands only pinch.
    int gazeMode = gazePointing(ctx) && gazeSpace;
    // Gaze is still read while bridged, but does not point.
    int gazeWatch = gazeWanted(ctx) && gazeSpace;
    for (int h = 0; h < SRC_COUNT; h++) {
        if (h < HAND_COUNT && padHand(ctx, h)) {
            continue;
        }
        if (h < HAND_COUNT) {
            f->grab[h] = seenGrip(ctx, h);
            f->stick[h] = seenStick(ctx, h);
            int wasDown = ctx->triggerDown[h];
            float value = seenTrigger(ctx, h);
            readJoints(ctx, h, &f->xform, &f->headPose, f->headValid);
            if (ctx->profileKind[h] != PROFILE_CONTROLLER) {
                int state = ctx->handTrust[h] >= 0.7f ? 1 : (ctx->handTrust[h] >= HAND_TRUST_MIN ? 2 : 3);
                if (ctx->trustSaid[h] != state) {
                    LOGI("hand %d tracking: %s (%.0f percent of joints)", h,
                         state == 1 ? "good" : (state == 2 ? "weak" : "lost"), ctx->handTrust[h] * 100.0f);
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

// Closes the hand lock hint; forGood tells Java not to show it again.
static void closeHandHint(XrCtx* ctx, InputFrame* f, int forGood, const char* by) {
    ctx->hintOpen = 0;
    ctx->hintHoverZone = HINT_ZONE_NONE;
    if (forGood) {
        f->out[IN_HINT] = 1.0f;
    }
    LOGEV("hand lock hint put away by %s, %s", by,
          forGood ? "not shown again" : "back next session");
}

static void setHandsLocked(XrCtx* ctx, int locked, const char* from) {
    ctx->handsLocked = locked;
    LOGEV("hands %s by %s", locked ? "locked" : "unlocked", from);
    if (locked) {
        // Release everything so nothing stays grabbed or pressed.
        ctx->pointerAwake = 0;
        ctx->buttonsDown = 0;
        ctx->stillFor = 0.0f;
        ctx->movingFor = 0.0f;
        if (ctx->grabMode != GRAB_NONE) {
            ctx->grabMode = GRAB_NONE;
            ctx->grabByGaze = 0;
            ctx->poseDirty = 1;
        }
    }
}

// Triple pinch toggles the hand lock. Runs before applyHandLock so locked hands
// can unlock. The first two pinches pass through; the third is held back until
// released.
static void updateLockGesture(XrCtx* ctx, InputFrame* f) {
    for (int h = 0; h < HAND_COUNT; h++) {
        if (!ctx->handsEnabled || ctx->profileKind[h] == PROFILE_CONTROLLER) {
            triplePinchReset(&ctx->triplePinch[h]);
            continue;
        }
        if (triplePinchStep(&ctx->triplePinch[h], ctx->triggerDown[h], f->now)) {
            if (ctx->hintOpen) {
                closeHandHint(ctx, f, 0, "the triple pinch");
            }
            setHandsLocked(ctx, !ctx->handsLocked, "the triple pinch");
            // Toast, since nothing else visibly changes.
            noticePush(&ctx->notices, ctx->handsLocked ? TOAST_HANDS_LOCKED
                                                       : TOAST_HANDS_UNLOCKED, 0);
        }
        if (triplePinchHeld(&ctx->triplePinch[h])) {
            ctx->triggerDown[h] = 0;
            ctx->triggerEdge[h] = 0;
        }
    }
}

// Locked hands (any non-controller source) get no aim, press or movement.
// Then gaze takes the hands' pinch.
static void applyHandLock(XrCtx* ctx, InputFrame* f) {
    int locked = 0;
    for (int h = 0; h < HAND_COUNT; h++) {
        if (!ctx->handsLocked || ctx->profileKind[h] == PROFILE_CONTROLLER) {
            continue;
        }
        f->hovers[h] = HOVER_NONE;
        f->aimValid[h] = 0;
        f->handMoved[h] = 0;
        ctx->triggerDown[h] = 0;
        ctx->triggerEdge[h] = 0;
        locked = 1;
    }
    if (locked) {
        f->moved = f->handMoved[HAND_LEFT] || f->handMoved[HAND_RIGHT];
    }

    gazeTrigger(ctx, f);
}

// Whether any button is down on this controller.
static int controllerPressed(XrCtx* ctx, const InputFrame* f, int h) {
    return ctx->triggerDown[h] || f->grab[h] > PRESS_ON || stickPushed(f->stick[h])
            || actionBool(ctx, ctx->rightClickAction, h)
            || actionBool(ctx, ctx->middleClickAction, h)
            || seenStickClick(ctx, h);
}

// Per-controller wake/sleep clocks, so a controller put down retires even while
// the other hand or gaze keeps the shared pointer awake.
static void updateControllerClocks(XrCtx* ctx, InputFrame* f) {
    int eyesPoint = ctx->eyeGaze && ctx->gazeEnabled;
    int swallowed = 0;
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->profileKind[h] != PROFILE_CONTROLLER || padHand(ctx, h)) {
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
    // A pinch while the pointer is asleep is swallowed. Only a gaze pinch
    // wakes it; hands wake it with an open palm (below).
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->usingHands[h]) {
            ctx->pinchSwallowed[h] = 0;
            continue;
        }
        if (ctx->triggerDown[h]) {
            f->pinching = 1;
            if (!ctx->pointerAwake) {
                if (h == SRC_GAZE) {
                    ctx->pointerAwake = 1;
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
    int handBusy = f->pinching;
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

    // Modal sheets wait on a press, so the pointer stays awake under them.
    if (ctx->hintOpen || ctx->kofiOpen) {
        ctx->pointerAwake = 1;
        ctx->stillFor = 0.0f;
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

    f->hand = -1;
    for (int i = 0; i < SRC_COUNT; i++) {
        int h = order[i];
        if (f->hovers[h] == HOVER_SCREEN && ctx->triggerDown[h]) {
            f->hand = h;
            break;
        }
    }
    // A hit on something beats a hit in the halo.
    for (int pass = 0; pass < 2 && f->hand < 0; pass++) {
        for (int i = 0; i < SRC_COUNT && f->hand < 0; i++) {
            int h = order[i];
            if (f->hovers[h] == HOVER_NONE || (pass == 0 && f->hovers[h] == HOVER_HALO)) {
                continue;
            }
            f->hand = h;
        }
    }

    if (!ctx->pointerAwake && ctx->grabMode == GRAB_NONE) {
        f->hand = -1;
        for (int h = 0; h < SRC_COUNT; h++) {
            f->hovers[h] = HOVER_NONE;
        }
    }

    f->hover = f->hand >= 0 ? f->hovers[f->hand] : HOVER_NONE;
    if (f->hover == HOVER_CORNER) {
        ctx->hoverCorner = f->corners[f->hand];
    }
}

// Clears all hover highlights before this frame sets them.
static void clearHotState(XrCtx* ctx) {
    ctx->pickerHover = -1;
    ctx->envButtonHot = 0;
    ctx->cogButtonHot = 0;
    ctx->cogHoverSlider = -1;
    ctx->cogHoverCell = -1;
    ctx->cogHoverStep = 0;
    ctx->pickerPick = -1;
    ctx->kbButtonHot = 0;
    ctx->kbHoverKey = -1;
    ctx->kbKeyDown = 0;
    ctx->exitButtonHot = 0;
    ctx->exitHoverZone = EXIT_ZONE_NONE;
    ctx->stereoButtonHot = 0;
    ctx->rayButtonHot = 0;
    ctx->aimButtonHot = 0;
    ctx->padButtonHot = 0;
    ctx->reportHoverZone = REPORT_ZONE_NONE;
    ctx->cogReportHot = 0;
    ctx->cogKofiHot = 0;
}

// The picker is modal.
static void updatePicker(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_PICKER;
    // Earlier hits are in screen coordinates, not grid ones.
    f->hand = -1;
    float pickW, pickH;
    XrPosef pose = pickerPose(ctx, &pickW, &pickH);
    for (int h = 0; h < SRC_COUNT; h++) {
        float pu, pv;
        if (!canPoint(ctx, f, h)) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], pose, pickW, pickH, 0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        // Each band is a header strip over a row of cells; the header is not a target.
        int band = (int)(pv * PICKER_ROWS);
        if (band >= PICKER_ROWS) band = PICKER_ROWS - 1;
        float inBand = pv * PICKER_TEX_H - band * PICKER_BAND_PX;
        if (inBand < PICKER_HEADER_PX) {
            continue;
        }
        int col = (int)(pu * PICKER_COLS);
        if (col >= PICKER_COLS) col = PICKER_COLS - 1;
        // Cells past the last real one are blank tiles.
        int cell = band * PICKER_COLS + col;
        if (cell >= ctx->pickerCells) {
            continue;
        }
        ctx->pickerHover = cell;
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;

        if (ctx->triggerEdge[h]) {
            ctx->pickerPick = ctx->pickerHover;
            ctx->pickerChoice = ctx->pickerHover;
            ctx->pickerOpen = 0;
            ctx->clickPending = 1;
            swallowTrigger(ctx, h);
        }
        break;
    }

    // A press on no cell closes the picker.
    if (ctx->pickerOpen && ctx->pickerHover < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                ctx->pickerOpen = 0;
                swallowTrigger(ctx, h);
            }
        }
    }
}

// Replaces the settings panel with the report sheet and opens the keyboard.
// The sheet shows once Java has drawn it.
static void openReport(XrCtx* ctx, InputFrame* f) {
    ctx->cogOpen = 0;
    ctx->reportOpen = 1;
    ctx->reportReady = 0;
    ctx->reportSendReady = 0;
    ctx->reportHoverZone = REPORT_ZONE_NONE;
    ctx->reportPose = reportSheetPose(ctx, &ctx->reportW, &ctx->reportH);
    ctx->kbOpen = 1;
    ctx->kbState = KB_STATE_LOWER;
    ctx->kbMods = 0;
    ctx->kbPose = kbPanelPose(ctx, &ctx->kbW, &ctx->kbH);
    f->out[IN_REPORT] = (float)REPORT_OPENED;
    LOGEV("report sheet open");
}

// Handles one key press (at most one per frame). Modifiers are ignored when
// typing into the report sheet.
static void pressKey(XrCtx* ctx, InputFrame* f, int key, int intoReport) {
    int code = ctx->kbCodes[ctx->kbState][key];
    KbPress press = kbPress(ctx->kbState, intoReport ? 0 : ctx->kbMods, code);
    if (intoReport) {
        press.mods = 0;
        press.codeMods = 0;
    }
    if (press.code > 0) {
        f->out[IN_KEY] = (float)press.code;
        f->out[IN_KEY_MODS] = (float)press.codeMods;
    }
    if (press.mods != ctx->kbMods) {
        LOGI("keyboard modifiers %d -> %d%s", ctx->kbMods, press.mods,
             press.code > 0 ? ", let go after the key" : "");
    }
    ctx->kbState = press.sheet;
    ctx->kbMods = press.mods;
    if (press.hide) {
        ctx->kbOpen = 0;
        LOGI("keyboard closed");
    }
}

// Report sheet press. A field takes focus and reopens the keyboard; Send works
// only once Java marks the input valid; Send and Cancel close the sheet.
static void pressReport(XrCtx* ctx, InputFrame* f, int zone) {
    if (zone == REPORT_ZONE_NONE || (zone == REPORT_ZONE_SEND && !ctx->reportSendReady)) {
        if (zone == REPORT_ZONE_SEND) {
            LOGI("report send pressed before the note will do");
        }
        return;
    }
    ctx->clickPending = 1;
    f->out[IN_REPORT] = (float)zone;
    if (zone == REPORT_ZONE_NOTE || zone == REPORT_ZONE_EMAIL) {
        if (!ctx->kbOpen) {
            ctx->kbOpen = 1;
            ctx->kbState = KB_STATE_LOWER;
            ctx->kbPose = kbPanelPose(ctx, &ctx->kbW, &ctx->kbH);
        }
        LOGI("report field %s takes the keys", zone == REPORT_ZONE_NOTE ? "note" : "address");
        return;
    }
    ctx->reportOpen = 0;
    ctx->kbOpen = 0;
    LOGEV("report sheet %s", zone == REPORT_ZONE_SEND ? "sent" : "cancelled");
}

// Ignored until the sheet's texture is ready; an invisible modal would eat presses.
static void openKofi(XrCtx* ctx) {
    if (!ctx->kofiReady) {
        LOGI("Ko-fi pressed on the About tab before its sheet's art arrived");
        return;
    }
    ctx->cogOpen = 0;
    ctx->kofiOpen = 1;
    ctx->kofiHoverZone = KOFI_ZONE_NONE;
    ctx->kofiPose = kofiSheetPose(ctx, &ctx->kofiW, &ctx->kofiH);
    LOGEV("Ko-fi sheet open from the About tab");
}

// Starts a slider drag. With gaze, the pinching hand carries the drag.
static void cogStartDrag(XrCtx* ctx, InputFrame* f, int h, int face, int row, float pu,
                         float pv) {
    ctx->cogDragByGaze = 0;
    ctx->cogDragHand = h;
    if (h == SRC_GAZE) {
        Vec3 target = screenPoint(pu, pv, ctx->cogPose, ctx->cogW, ctx->cogH, 0.0f, 0);
        int hand = gazeDragHand(ctx, f->aimPoses, f->aimValid, target,
                                &ctx->cogDragHandStart, &ctx->cogDragScale);
        if (hand < 0) {
            // No hand: set the value here; the drag ends (and saves) next frame.
            ctx->cogDragHand = -1;
            ctx->cogDragSlider = row;
            ctx->cogDragFace = face;
            cogApplySlider(ctx, face, row, pu);
            return;
        }
        ctx->cogDragByGaze = 1;
        ctx->cogDragHand = hand;
        ctx->cogDragStartU = pu;
        dragRampStart(&ctx->cogDragRamp, f->now);
        LOGI("gaze drag: slider %d by hand %d, scale %.2f", row, hand, ctx->cogDragScale);
    }
    ctx->cogDragSlider = row;
    ctx->cogDragFace = face;
    // Jump to the press point.
    cogApplySlider(ctx, face, row, pu);
}

// Settings panel, modal, at the pose frozen when it opened.
static void updateCogPanel(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_COGPANEL;
    f->hand = -1;
    int face = cogFace(ctx);

    // A drag keeps its hand even when the ray leaves the panel.
    if (ctx->cogDragSlider >= 0 && ctx->cogDragFace != face) {
        // The tab layout changed mid drag (debug property only).
        cogDragEnded(ctx, f->out);
    }
    else if (ctx->cogDragSlider >= 0 && cogRowIsTrack(face, ctx->cogDragSlider)) {
        int h = ctx->cogDragHand;
        float pu = 0.0f, pv = 0.0f;
        int held = h >= 0 && ctx->triggerDown[h]
                && (f->aimValid[h] || (ctx->cogDragByGaze && pinchTracked(ctx, h)));
        if (held && ctx->cogDragByGaze) {
            // Hand travel along the panel's x axis, with a dead zone so a
            // shaky pinch at high gearing does not slide the thumb.
            Vec3 travel = vecSub(gazeHandPos(ctx, f->aimPoses, h), ctx->cogDragHandStart);
            Vec3 carry = gazeDragCarry(ctx, &ctx->cogDragRamp,
                                       dragDeadZone(travel, GAZE_DRAG_DEAD_M), f->now);
            Vec3 xAxis = { 1.0f, 0.0f, 0.0f };
            Vec3 right = quatRotate(ctx->cogPose.orientation, xAxis);
            pu = ctx->cogDragStartU + vecDot(carry, right) * ctx->cogDragScale / ctx->cogW;
            if (pu < 0.0f) pu = 0.0f;
            if (pu > 1.0f) pu = 1.0f;
            pv = cogRowV(face, ctx->cogDragSlider);
        }
        else if (held) {
            held = screenProject(f->aimPoses[h], ctx->cogPose, ctx->cogW, ctx->cogH,
                                 0.0f, 0, &pu, &pv);
        }
        if (held) {
            // Attribute a gaze drag to gaze, so no hand ray is drawn.
            f->hand = ctx->cogDragByGaze ? SRC_GAZE : h;
            f->hitU[f->hand] = pu;
            f->hitV[f->hand] = pv;
            ctx->cogHoverSlider = ctx->cogDragSlider;
            cogApplySlider(ctx, face, ctx->cogDragSlider, pu);
        }
        else {
            cogDragEnded(ctx, f->out);
        }
    }

    for (int h = 0; f->hand < 0 && h < SRC_COUNT; h++) {
        float pu, pv;
        if (!canPoint(ctx, f, h)) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], ctx->cogPose, ctx->cogW, ctx->cogH,
                           0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;

        // Tab bar across the top, one even slot per tab.
        if (pv < COG_TAB_BAR_B) {
            if (ctx->triggerEdge[h]) {
                int t = (int)(pu * COG_TAB_COUNT);
                if (t >= COG_TAB_COUNT) t = COG_TAB_COUNT - 1;
                ctx->cogTab = t;
                ctx->clickPending = 1;
                ctx->cogDragSlider = -1;
                ctx->cogDragHand = -1;
                ctx->cogDragFace = -1;
                ctx->cogDragByGaze = 0;
            }
            break;
        }

        // Greyed-out rows are skipped.
        int rowCount = cogTabRowCount(face);
        int row = -1;
        for (int s = 0; s < rowCount; s++) {
            if (!cogRowLive(ctx, face, s)) {
                continue;
            }
            if (fabsf(pv - cogRowV(face, s)) < cogRowHalf(face)) {
                row = s;
                break;
            }
        }

        if (row >= 0 && !cogRowIsTrack(face, row)) {
            int cell = cogCellAt(pu, cogRowCells(face, row));
            ctx->cogHoverSlider = cell >= 0 ? row : -1;
            ctx->cogHoverCell = cell;
            if (cell >= 0 && ctx->triggerEdge[h]) {
                cogApplyCell(ctx, face, row, cell, f->out);
                ctx->clickPending = 1;
            }
            break;
        }

        // Sliders: step buttons at each end, the track between them.
        int part = row >= 0 ? cogTrackPart(pu) : TRACK_PART_NONE;
        if (part == TRACK_PART_NONE) {
            row = -1;
        }
        ctx->cogHoverSlider = row;
        if (part == TRACK_PART_DOWN || part == TRACK_PART_UP) {
            int dir = part == TRACK_PART_UP ? 1 : -1;
            ctx->cogHoverStep = dir;
            if (ctx->triggerEdge[h]) {
                cogStepTrack(ctx, face, row, dir, f->out);
                ctx->clickPending = 1;
            }
            break;
        }

        // The About tab has only two buttons, each opening a sheet.
        if (face == COG_TAB_ABOUT) {
            ctx->cogReportHot = cogReportButtonAt(pu, pv);
            ctx->cogKofiHot = cogKofiButtonAt(pu, pv);
            if (ctx->cogReportHot && ctx->triggerEdge[h]) {
                ctx->clickPending = 1;
                openReport(ctx, f);
                swallowTrigger(ctx, h);
            }
            else if (ctx->cogKofiHot && ctx->triggerEdge[h]) {
                ctx->clickPending = 1;
                openKofi(ctx);
                swallowTrigger(ctx, h);
            }
            break;
        }

        // Reset button on the Screen, 3D and Picture tabs.
        int onReset = (face == COG_TAB_SCREEN || face == COG_TAB_3D || face == COG_TAB_PICTURE)
                && pu >= COG_RESET_L && pu <= COG_RESET_R
                && pv >= COG_RESET_T && pv <= COG_RESET_B;
        if (onReset && ctx->triggerEdge[h]) {
            ctx->clickPending = 1;
        }
        if (onReset && ctx->triggerEdge[h] && face == COG_TAB_3D) {
            // The running model's defaults, set at session start. Allowed with
            // stereo off too.
            ctx->panelSeparation = ctx->defaultSeparation;
            ctx->separationCurrent = ctx->defaultSeparation;
            ctx->convergence = ctx->defaultConvergence;
            f->out[IN_SETTING] = (float)SETTING_RESET_3D;
            f->out[IN_SETTING_VALUE] = 0.0f;
            LOGEV("3d settings reset from the panel to separation %d, convergence %d",
                  separationUnits(ctx->defaultSeparation),
                  (int)roundf(ctx->defaultConvergence * 100.0f));
        }
        else if (onReset && ctx->triggerEdge[h] && face == COG_TAB_PICTURE) {
            pictureReset(ctx);
            f->out[IN_SETTING] = (float)SETTING_RESET_PICTURE;
            f->out[IN_SETTING_VALUE] = 0.0f;
        }
        else if (onReset && ctx->triggerEdge[h]) {
            // updatePlacement reseeds from preferences next frame and marks
            // the pose dirty. The panel stays open so the jump is visible.
            ctx->panelCurve = -1.0f;
            ctx->placementValid = 0;
            LOGI("screen placement reset from the panel");
        }
        else if (row >= 0 && ctx->triggerEdge[h]) {
            cogStartDrag(ctx, f, h, face, row, pu, pv);
            ctx->clickPending = 1;
        }
    }

    // A press off the panel closes it (also how the cog button toggles it).
    if (f->hand < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                ctx->cogOpen = 0;
                swallowTrigger(ctx, h);
            }
        }
    }
}

// Exit confirmation, modal, at the pose frozen when it opened.
static void updateExitPrompt(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_EXITPROMPT;
    f->hand = -1;

    for (int h = 0; h < SRC_COUNT; h++) {
        float pu, pv;
        if (!canPoint(ctx, f, h)) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], ctx->exitPose, ctx->exitW, ctx->exitH,
                           0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;
        ctx->exitHoverZone = exitPromptZone(pu, pv);

        if (ctx->triggerEdge[h]) {
            // Any press closes it; only the buttons click.
            if (ctx->exitHoverZone != EXIT_ZONE_NONE) {
                ctx->clickPending = 1;
            }
            if (ctx->exitHoverZone == EXIT_ZONE_EXIT) {
                // Java handles the exit; the prompt closes regardless.
                f->out[IN_EXIT] = 1.0f;
                LOGI("exit confirmed from the prompt");
            }
            ctx->exitConfirmOpen = 0;
            swallowTrigger(ctx, h);
        }
        break;
    }

    // A press off the sheet closes it.
    if (f->hand < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                ctx->exitConfirmOpen = 0;
                swallowTrigger(ctx, h);
            }
        }
    }
}

// Report sheet, modal except for the keyboard. Java routes keys into the
// focused field. Only Cancel and Send close it, so a stray press cannot lose
// the typed note.
static void updateReport(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_REPORT;
    f->hand = -1;

    for (int h = 0; h < SRC_COUNT; h++) {
        float pu, pv;
        if (!canPoint(ctx, f, h)) {
            continue;
        }
        if (screenProject(f->aimPoses[h], ctx->reportPose, ctx->reportW, ctx->reportH,
                          0.0f, 0, &pu, &pv)
                && pu >= 0.0f && pu <= 1.0f && pv >= 0.0f && pv <= 1.0f) {
            f->hand = h;
            f->hitU[h] = pu;
            f->hitV[h] = pv;
            ctx->reportHoverZone = reportZone(pu, pv);
            if (ctx->triggerEdge[h]) {
                pressReport(ctx, f, ctx->reportHoverZone);
                swallowTrigger(ctx, h);
            }
            break;
        }
        if (ctx->kbOpen && f->hovers[h] == HOVER_KBPANEL) {
            f->hand = h;
            f->hover = HOVER_KBPANEL;
            int key = kbKeyAt(ctx, f->hitU[h], f->hitV[h]);
            ctx->kbHoverKey = key;
            ctx->kbKeyDown = key >= 0 && ctx->triggerDown[h];
            if (key >= 0 && ctx->triggerEdge[h]) {
                ctx->clickPending = 1;
                pressKey(ctx, f, key, 1);
                swallowTrigger(ctx, h);
            }
            break;
        }
    }

    // Presses elsewhere are swallowed.
    if (f->hand < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                swallowTrigger(ctx, h);
            }
        }
    }
}

// Opens the hand lock hint once per session, after a hand has pointed for
// HINT_POINTING_NS with nothing else open or held, and only once its texture is
// ready.
static void updateHintOpening(XrCtx* ctx, InputFrame* f) {
    if (!ctx->hintWanted || ctx->hintShown || !ctx->hintReady) {
        return;
    }
    int hand = -1;
    for (int h = 0; h < HAND_COUNT && hand < 0; h++) {
        if (ctx->usingHands[h] && f->aimValid[h] && !f->pinchOnly[h]) {
            hand = h;
        }
    }
    int clear = !panelUp(ctx) && !ctx->panelFadingOut && !ctx->handsLocked
            && !ctx->controllerAwake && !pressHeld(ctx);
    if (hand < 0 || !clear) {
        ctx->hintPointingNs = 0;
        return;
    }
    if (ctx->hintPointingNs == 0) {
        ctx->hintPointingNs = f->now;
    }
    if (f->now - ctx->hintPointingNs < HINT_POINTING_NS) {
        return;
    }
    ctx->hintOpen = 1;
    ctx->hintShown = 1;
    ctx->hintHoverZone = HINT_ZONE_NONE;
    ctx->hintPose = handHintPose(ctx, &ctx->hintW, &ctx->hintH);
    ctx->pointerAwake = 1;
    ctx->stillFor = 0.0f;
    LOGEV("hand lock hint up: hand %d is pointing", hand);
}

// Hand lock hint, modal. Any press closes it; "Don't show this again" closes it
// for good.
static void updateHandHint(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_HINT;
    f->hand = -1;
    int zone = HINT_ZONE_NONE;
    for (int h = 0; h < SRC_COUNT; h++) {
        float pu, pv;
        if (!canPoint(ctx, f, h)) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], ctx->hintPose, ctx->hintW, ctx->hintH,
                           0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;
        zone = handHintZone(pu, pv);
        break;
    }
    ctx->hintHoverZone = zone;

    // Prefer the source on the sheet, so a gaze pinch on a button counts as on it.
    int pressed = f->hand >= 0 && ctx->triggerEdge[f->hand] ? f->hand : -1;
    for (int h = 0; h < SRC_COUNT && pressed < 0; h++) {
        if (ctx->triggerEdge[h]) {
            pressed = h;
        }
    }
    if (pressed < 0) {
        return;
    }
    int on = pressed == f->hand ? zone : HINT_ZONE_NONE;
    if (on != HINT_ZONE_NONE) {
        ctx->clickPending = 1;
    }
    closeHandHint(ctx, f, on == HINT_ZONE_NEVER,
                  on == HINT_ZONE_NEVER ? "\"Don't show this again\""
                  : on == HINT_ZONE_OK ? "OK" : "a press off its buttons");
    for (int h = 0; h < SRC_COUNT; h++) {
        if (ctx->triggerEdge[h]) {
            swallowTrigger(ctx, h);
        }
    }
}

// Modal sheet; any press closes it and is swallowed.
static void updateKofiSheet(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_KOFI;
    f->hand = -1;
    int zone = KOFI_ZONE_NONE;
    for (int h = 0; h < SRC_COUNT; h++) {
        float pu, pv;
        if (!canPoint(ctx, f, h)) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], ctx->kofiPose, ctx->kofiW, ctx->kofiH,
                           0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;
        zone = kofiSheetZone(pu, pv);
        break;
    }
    ctx->kofiHoverZone = zone;

    int pressed = -1;
    for (int h = 0; h < SRC_COUNT && pressed < 0; h++) {
        if (ctx->triggerEdge[h]) {
            pressed = h;
        }
    }
    if (pressed < 0) {
        return;
    }
    int onClose = pressed == f->hand && zone == KOFI_ZONE_CLOSE;
    if (onClose) {
        ctx->clickPending = 1;
    }
    ctx->kofiOpen = 0;
    ctx->kofiHoverZone = KOFI_ZONE_NONE;
    int byHand = pressed == SRC_GAZE
            || (pressed < HAND_COUNT && ctx->profileKind[pressed] != PROFILE_CONTROLLER);
    LOGEV("Ko-fi sheet put away by %s", onClose ? "Close" : byHand ? "a pinch" : "a press");
    for (int h = 0; h < SRC_COUNT; h++) {
        if (ctx->triggerEdge[h]) {
            swallowTrigger(ctx, h);
        }
    }
}

// Highlights the bar button or key under the ray and handles a press on it.
static void updateFurniture(XrCtx* ctx, InputFrame* f) {
    int pressed = f->hand >= 0 && ctx->triggerEdge[f->hand];
    if (pressed && (f->hover == HOVER_ENVBUTTON || f->hover == HOVER_COGBUTTON
            || f->hover == HOVER_KBBUTTON || f->hover == HOVER_EXITBUTTON
            || f->hover == HOVER_STEREOBUTTON || f->hover == HOVER_RAYBUTTON
            || f->hover == HOVER_AIMBUTTON || f->hover == HOVER_PADBUTTON
            || (f->hover == HOVER_KBPANEL
                && kbKeyAt(ctx, f->hitU[f->hand], f->hitV[f->hand]) >= 0))) {
        ctx->clickPending = 1;
    }
    if (f->hover == HOVER_ENVBUTTON) {
        ctx->envButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->pickerOpen = 1;
        }
    }
    else if (f->hover == HOVER_COGBUTTON) {
        ctx->cogButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->cogOpen = !ctx->cogOpen;
            if (ctx->cogOpen) {
                ctx->cogTab = COG_TAB_SCREEN;
                ctx->cogPose = cogPanelPose(ctx, &ctx->cogW, &ctx->cogH);
            }
        }
    }
    else if (f->hover == HOVER_KBBUTTON) {
        ctx->kbButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->kbOpen = !ctx->kbOpen;
            if (ctx->kbOpen) {
                ctx->kbState = KB_STATE_LOWER;
                ctx->kbPose = kbPanelPose(ctx, &ctx->kbW, &ctx->kbH);
            }
            LOGI("keyboard %s", ctx->kbOpen ? "open" : "closed");
        }
    }
    else if (f->hover == HOVER_EXITBUTTON) {
        ctx->exitButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->exitConfirmOpen = 1;
            ctx->exitHoverZone = EXIT_ZONE_NONE;
            ctx->exitPose = exitPromptPose(ctx, &ctx->exitW, &ctx->exitH);
            swallowTrigger(ctx, f->hand);
            LOGI("exit prompt open");
        }
    }
    else if (f->hover == HOVER_STEREOBUTTON) {
        ctx->stereoButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            setStereoLive(ctx, !ctx->stereoLive, "the bar button");
            // Toast: the change is easy to miss otherwise.
            noticePush(&ctx->notices, ctx->stereoLive ? TOAST_3D_ON : TOAST_3D_OFF, 0);
        }
    }
    else if (f->hover == HOVER_RAYBUTTON) {
        ctx->rayButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            setRayOn(ctx, !raySwitchOn(ctx->raySetting, ctx->rayFlipped), "the bar button");
        }
    }
    else if (f->hover == HOVER_AIMBUTTON) {
        ctx->aimButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            int on = !headAimSwitchOn(ctx->headAimSetting, ctx->headAimFlipped);
            setHeadAimOn(ctx, on, "the bar button");
            // Toast: nothing visible changes until the head turns.
            noticePush(&ctx->notices, on ? TOAST_HEAD_AIM_ON : TOAST_HEAD_AIM_OFF, 0);
        }
    }
    else if (f->hover == HOVER_PADBUTTON) {
        ctx->padButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            setPadMode(ctx, !ctx->padMode, "the bar button");
        }
    }
    else if (f->hover == HOVER_KBPANEL) {
        int key = kbKeyAt(ctx, f->hitU[f->hand], f->hitV[f->hand]);
        ctx->kbHoverKey = key;
        ctx->kbKeyDown = key >= 0 && ctx->triggerDown[f->hand];
        if (key >= 0 && ctx->triggerEdge[f->hand]) {
            pressKey(ctx, f, key, 0);
        }
    }
}

// A press on empty space (from any source) closes the keyboard.
static void dismissKeyboard(XrCtx* ctx, InputFrame* f) {
    if (ctx->kbOpen && !ctx->pickerOpen && !ctx->cogOpen && !ctx->exitConfirmOpen
            && !ctx->reportOpen && !ctx->hintOpen && !ctx->kofiOpen) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (!canPoint(ctx, f, h) || !ctx->triggerEdge[h]) {
                continue;
            }
            // The halo counts as empty space.
            if (f->hovers[h] == HOVER_NONE || f->hovers[h] == HOVER_HALO) {
                ctx->kbOpen = 0;
                swallowTrigger(ctx, h);
                LOGI("keyboard closed");
                break;
            }
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
    if (ctx->headAimActive) {
        // Head aim drives relative mouse motion, so the ray adds a relative
        // nudge instead of an absolute position.
        int dx, dy;
        pointerNudge(&ctx->headAimNudge, f->hand, u, v, ctx->inputFrames, ctx->videoWidth,
                     ctx->videoHeight, &dx, &dy);
        f->out[IN_MOUSE_DX] += (float)dx;
        f->out[IN_MOUSE_DY] += (float)dy;
    }
    else {
        f->out[IN_HIT] = 1.0f;
        f->out[IN_U] = u;
        f->out[IN_V] = v;
    }

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
    // Controller only; owned by the pad in gamepad mode.
    if (!ctx->padMode && actionBool(ctx, ctx->rightClickAction, -1)) {
        mask |= VR_BUTTON_RIGHT;
    }
    if (!ctx->padMode && actionBool(ctx, ctx->middleClickAction, -1)) {
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
    // No scroll in gamepad mode or while a stick click is part of the chord.
    XrVector2f stick = { 0.0f, 0.0f };
    if (!ctx->padMode && !chordHasStickClick(ctx)) {
        stick = actionVec2(ctx, ctx->scrollAction, -1);
    }
    int offPicture = !ctx->pointerSleepOn
            && (f->hover == HOVER_NONE || f->hover == HOVER_HALO);
    int onPanel = ctx->playerMode && f->hover == HOVER_UIPANEL;
    if ((hit || offPicture || onPanel) && fabsf(stick.y) > SCROLL_DEADZONE) {
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

// Head yaw relative to the screen, for virtual surround. 0 when head locked
// (outside a room); keeps the last value if the head is not located.
static void updateAudioYaw(XrCtx* ctx, int headLocked) {
    if (headLocked && roomEffective(ctx) <= 0) {
        ctx->audioYaw = 0.0f;
        return;
    }
    if (!ctx->sessionRunning || !ctx->placementValid) {
        return;
    }
    XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, ctx->localSpace,
                                   ctx->predictedDisplayTime, &head))
            && (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
        ctx->audioYaw = yawBetween(head.pose.orientation, ctx->screenPose.orientation);
    }
}

// Collapses head aim step results to a log state; dropped or dead-zone frames count as on.
static int headAimDoing(int what) {
    return what == HEAD_AIM_OFF || what == HEAD_AIM_PAUSED || what == HEAD_AIM_LOST
            ? what : HEAD_AIM_SENT;
}

// Head aim: while head locked, the head's turn since last frame is output as
// relative mouse motion (IN_MOUSE_DX/DY), measured in local space. Runs before
// the input pass's early returns; paused while a panel is up, unfocused, or
// during the splash.
static void updateHeadAim(XrCtx* ctx, int headLocked, float* out) {
    int active = headAimSwitchOn(ctx->headAimSetting, ctx->headAimFlipped) && headLocked
            && roomEffective(ctx) <= 0;
    int paused = !ctx->sessionRunning || ctx->sessionState != XR_SESSION_STATE_FOCUSED
            || ctx->splash.phase != SPLASH_GONE || !ctx->placementValid || panelUp(ctx);
    int tracked = 0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    if (active && !paused) {
        // Require tracked: snapping back from a held orientation would read as a turn.
        const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
                | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
        tracked = XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, ctx->localSpace,
                                             ctx->predictedDisplayTime, &head))
                && (head.locationFlags & needed) == needed;
        if (tracked) {
            headAimAngles(head.pose.orientation, &yaw, &pitch);
        }
    }
    // Drop frames until the recentre has reached the display, even with head aim off.
    int recentred = ctx->headAimRecentring;
    if (recentred && ctx->predictedDisplayTime >= ctx->headAimRecentreAt) {
        ctx->headAimRecentring = 0;
    }

    int dx = 0;
    int dy = 0;
    int what = headAimStep(&ctx->headAim, headAimBlocked(active, paused, tracked, recentred),
                           yaw, pitch, ctx->predictedDisplayTime,
                           (float)ctx->headAimSensitivity, (float)ctx->headAimDeadZone,
                           &dx, &dy);
    ctx->headAimActive = active;
    out[IN_HEAD_AIM] = active ? 1.0f : 0.0f;
    out[IN_MOUSE_DX] = (float)dx;
    out[IN_MOUSE_DY] = (float)dy;

    // On/off to the event log; pauses (every panel opening) to logcat only.
    int doing = headAimDoing(what);
    if (doing != ctx->headAimSaid) {
        int wasOn = ctx->headAimSaid >= 0 && ctx->headAimSaid != HEAD_AIM_OFF;
        if (doing == HEAD_AIM_OFF) {
            if (wasOn) {
                LOGEV("head aim off");
            }
        }
        else {
            if (!wasOn) {
                LOGEV("head aim on: %d px a degree, dead zone %d deg/s",
                      ctx->headAimSensitivity, ctx->headAimDeadZone);
            }
            LOGI("head aim %s", doing == HEAD_AIM_SENT ? "measuring the head"
                                : doing == HEAD_AIM_PAUSED
                                ? "paused while a panel is up or the session is not focused"
                                : "waiting for the head to be tracked");
        }
        ctx->headAimSaid = doing;
        ctx->headAimSentX = 0;
        ctx->headAimSentY = 0;
        ctx->headAimCountNs = ctx->predictedDisplayTime;
    }
    if (what == HEAD_AIM_JUMPED) {
        LOGI("head aim dropped a frame turning faster than a head can");
    }
    if (doing == HEAD_AIM_SENT) {
        ctx->headAimSentX += dx;
        ctx->headAimSentY += dy;
        if (ctx->predictedDisplayTime - ctx->headAimCountNs >= HEAD_AIM_COUNT_NS) {
            if (ctx->headAimSentX != 0 || ctx->headAimSentY != 0) {
                LOGI("head aim moved the mouse %ld, %ld px in %.0f s", ctx->headAimSentX,
                     ctx->headAimSentY,
                     (ctx->predictedDisplayTime - ctx->headAimCountNs) / 1e9);
            }
            ctx->headAimSentX = 0;
            ctx->headAimSentY = 0;
            ctx->headAimCountNs = ctx->predictedDisplayTime;
        }
    }
}

// One controller's pad inputs, zero for a hand without one. Read in pointer mode
// too, for the shortcuts.
static void readPadHand(XrCtx* ctx, int h, PadHand* p) {
    memset(p, 0, sizeof(*p));
    if (ctx->profileKind[h] != PROFILE_CONTROLLER) {
        return;
    }
    p->grip = actionFloat(ctx, ctx->grabAction, h);
    p->menu = h == HAND_LEFT && actionBool(ctx, ctx->menuAction, h);
    p->trigger = actionFloat(ctx, ctx->triggerAction, h);
    XrVector2f stick = actionVec2(ctx, ctx->scrollAction, h);
    p->stickX = stick.x;
    p->stickY = stick.y;
    p->stickClick = actionBool(ctx, ctx->toggleAction, h);
    p->lower = actionBool(ctx, ctx->rightClickAction, h);
    p->upper = actionBool(ctx, ctx->middleClickAction, h);
}

// The virtual pad is attached in gamepad mode while either hand holds a controller.
static int padPlugged(XrCtx* ctx) {
    return ctx->padMode && (ctx->profileKind[HAND_LEFT] == PROFILE_CONTROLLER
                            || ctx->profileKind[HAND_RIGHT] == PROFILE_CONTROLLER);
}

// Final pad state for Java: at rest unless live; inputs held from before it went
// live stay off until released.
static void settlePad(XrCtx* ctx, int plugged, int live, PadState* s, const char* why) {
    if (!plugged || !live) {
        padRest(s);
        if (plugged && !ctx->padResting) {
            LOGI("gamepad resting: %s", why);
        }
        ctx->padResting = 1;
    }
    else if (ctx->padResting) {
        ctx->padHeld = padHeldIn(s);
        ctx->padResting = 0;
        LOGI("gamepad live%s", ctx->padHeld != 0 ? ", holding back what is still down" : "");
    }
    padHoldBack(&ctx->padHeld, s);

    if (plugged != ctx->padAttached) {
        if (plugged) {
            LOGEV("gamepad plugged in: the controllers are one Xbox pad on the host");
        }
        else {
            LOGEV("gamepad unplugged: %s", ctx->padMode ? "no controller in either hand"
                                                        : "pointer mode");
        }
        ctx->padAttached = plugged;
    }
    // Buttons only; sticks and triggers change every frame.
    if (s->buttons != ctx->padButtonsSaid) {
        LOGI("gamepad buttons 0x%04x, triggers %d %d", s->buttons, s->leftTrigger,
             s->rightTrigger);
        ctx->padButtonsSaid = s->buttons;
    }
    ctx->pad = *s;
}

// Releases all pad inputs and resets the shortcut state, but keeps the pad
// attached across a brief focus loss.
static void restPad(XrCtx* ctx, const char* why) {
    PadState s;
    padRest(&s);
    padToggleReset(&ctx->padToggle);
    padChordReset(&ctx->padChord, ctx->padChord.parts);
    settlePad(ctx, padPlugged(ctx), 0, &s, why);
}

// Applies a rumble pulse or stops it. Logs the first result per controller and
// any failure.
static void hapticOut(XrCtx* ctx, int h, int action, float amp) {
    XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
    info.action = ctx->hapticAction;
    info.subactionPath = ctx->handPaths[h];
    const char* side = h == HAND_LEFT ? "left" : "right";
    if (action == PAD_RUMBLE_APPLY) {
        XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
        vibration.duration = PAD_RUMBLE_PULSE_NS;
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        vibration.amplitude = amp;
        XrResult res = xrApplyHapticFeedback(ctx->session, &info,
                                             (const XrHapticBaseHeader*)&vibration);
        ctx->rumblePulses++;
        if (!ctx->rumbleResultSaid[h]) {
            ctx->rumbleResultSaid[h] = 1;
            LOGI("rumble on the %s controller at %.2f for %ld ms: runtime said %s", side, amp,
                 (long)(PAD_RUMBLE_PULSE_NS / 1000000LL), xrResultName(res));
        }
        else if (XR_FAILED(res)) {
            LOGW("rumble pulse on the %s controller refused: %s", side, xrResultName(res));
        }
        return;
    }
    XrResult res = xrStopHapticFeedback(ctx->session, &info);
    LOGI("rumble stopped on the %s controller: runtime said %s", side, xrResultName(res));
}

// Drives pad rumble on the pad's controllers; live == 0 stops it. Cleared when
// the pad detaches.
static void updateRumble(XrCtx* ctx, int live) {
    if (ctx->hapticAction == XR_NULL_HANDLE || !ctx->hapticBound) {
        return;
    }
    int on[HAND_COUNT];
    int action[HAND_COUNT];
    float amp[HAND_COUNT];
    for (int h = 0; h < HAND_COUNT; h++) {
        on[h] = live && ctx->padAttached && ctx->profileKind[h] == PROFILE_CONTROLLER;
    }
    padRumbleStep(&ctx->rumble, on, nowNs(), action, amp);
    for (int h = 0; h < HAND_COUNT; h++) {
        if (action[h] != PAD_RUMBLE_KEEP) {
            hapticOut(ctx, h, action[h], amp[h]);
        }
    }
    if (!ctx->padAttached && ctx->rumbleOn) {
        LOGI("rumble from the host dropped after %ld pulses: the pad is out", ctx->rumblePulses);
        ctx->rumbleOn = 0;
        padRumbleReset(&ctx->rumble);
    }
}

static const char* chordPartNames(int parts, char* buf, size_t size) {
    static const char* names[PAD_PART_COUNT] = { "L3", "R3", "LT", "RT", "left grip",
                                                 "right grip" };
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < PAD_PART_COUNT && used < size; i++) {
        if (parts & (1 << i)) {
            used += snprintf(buf + used, size - used, "%s%s", used > 0 ? ", " : "", names[i]);
        }
    }
    return parts != 0 ? buf : "nothing";
}

// Logs chord events, so a held-back press can be told from a missing one.
static void logChord(XrCtx* ctx, int event, const PadState* rawPad) {
    char held[64], tapped[64];
    const PadChord* c = &ctx->padChord;
    if (event == PAD_CHORD_FORMING) {
        LOGI("gamepad shortcut: holding back %s while it may be %s (%s mode)",
             chordPartNames(c->held, held, sizeof(held)), padShortcutName(ctx->padShortcut),
             ctx->padMode ? "gamepad" : "pointer");
    }
    else if (event == PAD_CHORD_ABANDONED) {
        LOGI("gamepad shortcut given up after %ld ms: %s goes through now, %s as a tap; "
             "the pad as read 0x%04x, triggers %d %d",
             (long)((nowNs() - c->startNs) / 1000000L),
             chordPartNames(c->stirred, held, sizeof(held)),
             chordPartNames(c->replay, tapped, sizeof(tapped)), rawPad->buttons,
             rawPad->leftTrigger, rawPad->rightTrigger);
    }
    else if (event == PAD_CHORD_FIRED) {
        LOGI("gamepad shortcut held %ld ms, %s kept back until let go",
             (long)(PAD_CHORD_HOLD_NS / 1000000L), chordPartNames(c->taken, held, sizeof(held)));
    }
}

// Gamepad pass, after action sync and before pointing. The chosen shortcut
// (left menu + left grip, or a chord whose parts are held back while it forms)
// toggles gamepad mode. In gamepad mode both controllers form the pad, which
// rests while a panel is up.
static void updatePad(XrCtx* ctx) {
    PadHand* raw = ctx->padRead;
    for (int h = 0; h < HAND_COUNT; h++) {
        readPadHand(ctx, h, &raw[h]);
    }
    // A chord needs a controller in each hand.
    int parts = ctx->profileKind[HAND_LEFT] == PROFILE_CONTROLLER
            && ctx->profileKind[HAND_RIGHT] == PROFILE_CONTROLLER
            ? padChordParts(ctx->padShortcut) : 0;
    if (parts != ctx->padChord.parts) {
        padChordReset(&ctx->padChord, parts);
    }
    int64_t now = nowNs();
    int chord = padChordStep(&ctx->padChord, &raw[HAND_LEFT], &raw[HAND_RIGHT], now);
    PadHand seen[HAND_COUNT];
    padChordApply(&ctx->padChord, &raw[HAND_LEFT], &raw[HAND_RIGHT], &seen[HAND_LEFT],
                  &seen[HAND_RIGHT]);
    for (int h = 0; h < HAND_COUNT; h++) {
        ctx->padGrip[h] = padGripDown(seen[h].grip, ctx->padGrip[h]);
    }
    if (chord != PAD_CHORD_NOTHING) {
        PadState rawPad;
        int rawGrips[HAND_COUNT] = { raw[HAND_LEFT].grip >= PAD_GRIP_ON,
                                     raw[HAND_RIGHT].grip >= PAD_GRIP_ON };
        padMap(&raw[HAND_LEFT], &raw[HAND_RIGHT], rawGrips, 1, ctx->padDeadzone, &rawPad);
        logChord(ctx, chord, &rawPad);
    }

    // The menu button is Start unless the menu+grip shortcut claims it.
    int startOk = 1;
    int menuGrip = ctx->padShortcut == PAD_SHORTCUT_MENU_GRIP
            && padToggleStep(&ctx->padToggle, raw[HAND_LEFT].menu, ctx->padGrip[HAND_LEFT], now,
                             &startOk);
    if (menuGrip || chord == PAD_CHORD_FIRED) {
        setPadMode(ctx, !ctx->padMode, padShortcutName(ctx->padShortcut));
    }

    PadState s;
    padMap(&seen[HAND_LEFT], &seen[HAND_RIGHT], ctx->padGrip, startOk, ctx->padDeadzone, &s);
    int plugged = padPlugged(ctx);
    settlePad(ctx, plugged, plugged && !panelUp(ctx), &s, "a panel is up");
}

// Entering gamepad mode closes all panels and releases anything held.
static void padPutPanelsAway(XrCtx* ctx, float* out) {
    if (!ctx->padPutAway) {
        return;
    }
    ctx->padPutAway = 0;
    if (ctx->cogDragSlider >= 0) {
        cogDragEnded(ctx, out);
    }
    if (ctx->grabMode != GRAB_NONE) {
        ctx->grabMode = GRAB_NONE;
        ctx->grabByGaze = 0;
        ctx->poseDirty = 1;
    }
    if (panelUp(ctx)) {
        LOGI("gamepad mode put the panels away");
    }
    ctx->cogOpen = 0;
    ctx->pickerOpen = 0;
    ctx->kbOpen = 0;
    ctx->exitConfirmOpen = 0;
    ctx->reportOpen = 0;
    ctx->hintOpen = 0;
    ctx->kofiOpen = 0;
    ctx->buttonsDown = 0;
}

// Saves a corner-resized room screen size once the grab ends and the setting
// slot is free; dropped if the room changed meanwhile.
static void emitRoomScreen(XrCtx* ctx, float* out) {
    int style = ctx->roomScreenUnsaved;
    if (style == 0 || ctx->grabMode == GRAB_RESIZE || out[IN_SETTING] >= 0.0f) {
        return;
    }
    ctx->roomScreenUnsaved = 0;
    if (style != roomEffective(ctx)) {
        return;
    }
    out[IN_SETTING] = (float)SETTING_ROOM_SCREEN;
    out[IN_SETTING_VALUE] = (float)roomScreenPercent(ctx, style);
    LOGEV("room %d screen percent %d from a corner", style, roomScreenPercent(ctx, style));
}

// Fills the state-derived output slots last, then copies everything to Java.
static void handBack(JNIEnv* env, XrCtx* ctx, float* out, jfloatArray outArr) {
    int readouts[READOUT_VALUES];
    for (int i = 0; i < READOUT_VALUES; i++) {
        readouts[i] = -1;
    }
    out[IN_SETTING_ROOM] = -1.0f;
    out[IN_STEREO] = 0.0f;
    out[IN_TOAST] = -1.0f;
    out[IN_TOAST_ARG] = 0.0f;
    out[IN_CLICK] = 0.0f;
    out[IN_MARKS] = -1.0f;
    out[IN_COG_OPEN] = 0.0f;
    out[IN_KB_MODS] = 0.0f;
    out[IN_KB_SHEET] = -1.0f;
    out[IN_REPORT_ZONE] = -1.0f;
    out[IN_PLAYER_BUTTONS] = 0.0f;
    out[IN_PLAYER_STICK_X] = 0.0f;
    if (ctx != NULL && ctx->playerMode) {
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
    }
    if (ctx != NULL) {
        out[IN_PAD] = ctx->padAttached ? 1.0f : 0.0f;
        out[IN_PAD_BUTTONS] = (float)ctx->pad.buttons;
        out[IN_PAD_LT] = (float)ctx->pad.leftTrigger;
        out[IN_PAD_RT] = (float)ctx->pad.rightTrigger;
        out[IN_PAD_LX] = (float)ctx->pad.leftX;
        out[IN_PAD_LY] = (float)ctx->pad.leftY;
        out[IN_PAD_RX] = (float)ctx->pad.rightX;
        out[IN_PAD_RY] = (float)ctx->pad.rightY;
        out[IN_REPORT_ZONE] = ctx->reportOpen ? (float)ctx->reportHoverZone : -1.0f;
        // A closed keyboard holds no modifiers.
        if (!ctx->kbOpen && ctx->kbMods != 0) {
            LOGI("keyboard modifiers %d -> 0, keyboard closed", ctx->kbMods);
            ctx->kbMods = 0;
        }
        out[IN_KB_MODS] = (float)ctx->kbMods;
        out[IN_KB_SHEET] = ctx->kbOpen ? (float)ctx->kbState : -1.0f;
        out[IN_CLICK] = ctx->clickPending ? 1.0f : 0.0f;
        ctx->clickPending = 0;
        // Display tab selections, while the panel is visible (including fade-out).
        int showing = ctx->cogOpen || ctx->panelFades[FADE_COG].level > 0.0f;
        out[IN_COG_OPEN] = showing ? 1.0f : 0.0f;
        if (showing && cogFace(ctx) == COG_TAB_DISPLAY) {
            for (int m = 0; m < COG_OPTION_COUNT; m++) {
                out[IN_MARKS + m] = cogRowLive(ctx, COG_TAB_DISPLAY, m)
                        ? (float)cogOptionValue(ctx, m, ctx->headLockedPref) : -1.0f;
            }
        }
        // Next toast, held until the splash is gone.
        Notice up;
        if (noticeAdvance(&ctx->notices, nowNs(), ctx->splash.phase != SPLASH_GONE, &up)) {
            out[IN_TOAST] = (float)up.kind;
            out[IN_TOAST_ARG] = (float)up.arg;
        }
        emitRoomScreen(ctx, out);
        // Room settings are per room.
        out[IN_SETTING_ROOM] = (float)roomCellForStyle(roomEffective(ctx));
        cogReadouts(ctx, readouts);
        out[IN_STEREO] = ctx->stereoMode != DEPTH_MODE_OFF && ctx->stereoLive ? 1.0f : 0.0f;
    }
    for (int i = 0; i < READOUT_VALUES; i++) {
        out[IN_READOUT + i] = (float)readouts[i];
    }
    (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
}

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
        // Uses last frame's clocks and gaze; must run before anything asks who points.
        updateControllerAwake(ctx);
        updateGazeBridge(ctx);
        ctx->prefCurvature = curvature;
        // Before the early returns, so audio yaw and head aim run with the
        // pointer off or focus lost.
        updateAudioYaw(ctx, headLocked);
        out[IN_HEAD_YAW] = ctx->audioYaw;
        ctx->inputFrames++;
        updateHeadAim(ctx, headLocked, out);
    }
    // -1 means "none"; a zeroed slot would be read as a real pick, setting or key.
    out[IN_PICKER_PICK] = -1.0f;
    out[IN_SETTING] = -1.0f;
    out[IN_KEY] = -1.0f;
    out[IN_REPORT] = -1.0f;

    // Release everything held when input is not read, including under the splash.
    if (ctx == NULL || !ctx->inputReady || !ctx->placementValid
            || ctx->sessionState != XR_SESSION_STATE_FOCUSED
            || ctx->splash.phase != SPLASH_GONE) {
        if (ctx != NULL) {
            releaseInput(ctx, out);
            restPad(ctx, "the session is not focused");
            updateRumble(ctx, 0);
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
        restPad(ctx, "the controllers could not be read");
        updateRumble(ctx, 0);
        handBack(env, ctx, out, outArr);
        return;
    }
    // Controller models read their grip poses after this sync.
    ctx->actionsSynced = 1;

    // Before pointing, so a mode switch applies this frame.
    updatePad(ctx);
    updateRumble(ctx, 1);
    if (!pointerEnabled) {
        // Pointer disabled in settings: only the pad is read.
        releaseInput(ctx, out);
        padPutPanelsAway(ctx, out);
        handBack(env, ctx, out, outArr);
        return;
    }

    // Stick click toggles the pointer, read through the chord filter when the
    // chord uses it; ignored in gamepad mode.
    int toggle = chordHasStickClick(ctx)
            ? seenStickClick(ctx, HAND_LEFT) || seenStickClick(ctx, HAND_RIGHT)
            : actionBool(ctx, ctx->toggleAction, -1);
    if (toggle && !ctx->togglePrev && !ctx->padMode) {
        ctx->pointerOn = !ctx->pointerOn;
        LOGI("pointer %s", ctx->pointerOn ? "on" : "off");
    }
    ctx->togglePrev = toggle;
    out[IN_POINTER] = ctx->pointerOn ? 1.0f : 0.0f;

    InputFrame f;
    memset(&f, 0, sizeof(f));
    f.out = out;
    out[IN_UI_PANEL] = -1.0f;
    // Must match what endFrame submits. A room forces world-locked and flat.
    f.roomOn = roomEffective(ctx) > 0;
    f.xform.toHead = headLocked && !f.roomOn;
    f.height = ctx->screenWidth * screenAspect(ctx);
    f.curved = !f.roomOn && effectiveCurvature(ctx) > 0.01f && ctx->cylinderSupported;
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
    // No hand lock in player mode: three quick pinches are just three clicks.
    if (ctx->playerMode) {
        ctx->handsLocked = 0;
    }
    else {
        updateLockGesture(ctx, &f);
    }
    applyHandLock(ctx, &f);
    updatePointerWake(ctx, &f);
    pickPointingSource(ctx, &f);
    clearHotState(ctx);
    updateHintOpening(ctx, &f);
    if (ctx->pickerOpen) {
        updatePicker(ctx, &f);
    }
    else if (ctx->cogOpen) {
        updateCogPanel(ctx, &f);
    }
    else if (ctx->exitConfirmOpen) {
        updateExitPrompt(ctx, &f);
    }
    else if (ctx->reportOpen) {
        updateReport(ctx, &f);
    }
    else if (ctx->hintOpen) {
        updateHandHint(ctx, &f);
    }
    else if (ctx->kofiOpen) {
        updateKofiSheet(ctx, &f);
    }
    else {
        updateFurniture(ctx, &f);
    }
    dismissKeyboard(ctx, &f);
    padPutPanelsAway(ctx, out);
    logInputSnapshot(ctx, &f);

    // UI panel hit, which Java treats as a touch.
    if (f.hand >= 0 && f.hover == HOVER_UIPANEL && ctx->uiHover[f.hand] >= 0
            && ctx->grabMode == GRAB_NONE) {
        out[IN_UI_PANEL] = (float)ctx->uiHover[f.hand];
        out[IN_UI_U] = f.hitU[f.hand];
        out[IN_UI_V] = f.hitV[f.hand];
        out[IN_UI_PRESS] = ctx->triggerDown[f.hand] ? 1.0f : 0.0f;
    }

    // Off the picture a trigger press cannot be a click, so it may grab.
    int offPicture = f.hand >= 0 && (f.hitU[f.hand] < 0.0f || f.hitU[f.hand] > 1.0f
                                     || f.hitV[f.hand] < 0.0f || f.hitV[f.hand] > 1.0f);
    applyGrab(ctx, f.aimPoses, f.aimValid, f.grab, f.hand, f.hover, ctx->hoverCorner,
              offPicture, f.height, f.curved);
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

    ctx->screenOrientation = f.screenPose.orientation;
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
    if ((f.hover == HOVER_BAR || f.hover == HOVER_ENVBUTTON || f.hover == HOVER_PICKER
            || f.hover == HOVER_HINT || f.hover == HOVER_HALO || f.hover == HOVER_COGBUTTON
            || f.hover == HOVER_COGPANEL || f.hover == HOVER_KBBUTTON
            || f.hover == HOVER_KBPANEL || f.hover == HOVER_EXITBUTTON
            || f.hover == HOVER_EXITPROMPT || f.hover == HOVER_STEREOBUTTON
            || f.hover == HOVER_RAYBUTTON || f.hover == HOVER_AIMBUTTON
            || f.hover == HOVER_PADBUTTON || f.hover == HOVER_REPORT || f.hover == HOVER_KOFI
            || f.hover == HOVER_UIPANEL)
            && f.headValid && f.hand >= 0) {
        beamToFurniture(ctx, &f);
    }
    sendPointer(ctx, &f, hit);
    updateButtons(ctx, &f, hit);
    updateScroll(ctx, &f, hit);
    beamIntoRoom(ctx, &f);

    writeInputPose(ctx, out);
    out[IN_PICKER_PICK] = (float)ctx->pickerPick;
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

    if (p[7] < SCREEN_MIN_WIDTH || p[7] > 20.0f || p[8] <= 0.0f) {
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

// Player mode: disables the hand hint and bar detection, and switches the
// handles to player behaviour. Set once, before the first frame.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetPlayerMode(JNIEnv* env, jobject thiz,
                                                               jlong handle, jboolean on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->playerMode = on ? 1 : 0;
    if (ctx->playerMode) {
        ctx->hintWanted = 0;
        // The player detects letterbox bars itself.
        ctx->ambiBarDetect = 0;
    }
    LOGI("player mode %s", ctx->playerMode ? "on" : "off");
}

// Whether this session may show the hand lock hint. Set before the first frame.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetHandHint(JNIEnv* env, jobject thiz,
                                                              jlong handle, jboolean wanted) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->hintWanted = wanted && ctx->handsEnabled && !ctx->playerMode;
    LOGI("hand lock hint %s", ctx->hintWanted ? "shows the first time a hand points"
                              : ctx->handsEnabled ? "put away for good in an earlier session"
                                                  : "not wanted, hands are off");
}
