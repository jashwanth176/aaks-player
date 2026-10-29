// OpenXR instance, session and swapchains, the session state machine,
// and the JNI entry points that start and stop the renderer.
#include "xr_renderer.h"
#include "xr_relight.h"
#include "xr_depthmap.h"

#include <openxr/openxr_reflection.h>

const char* xrResultName(XrResult res) {
    switch (res) {
#define XR_RESULT_NAME_CASE(name, value) case name: return #name;
        XR_LIST_ENUM_XrResult(XR_RESULT_NAME_CASE)
#undef XR_RESULT_NAME_CASE
        default:
            return "unknown XrResult";
    }
}

// Last call checkXr saw fail on this thread, for the start failure report.
static __thread XrResult lastFailedResult;
static __thread const char* lastFailedCall;

static void forgetLastFailure(void) {
    lastFailedResult = XR_SUCCESS;
    lastFailedCall = NULL;
}

int checkXr(XrResult res, const char* what) {
    if (XR_FAILED(res)) {
        LOGE("%s failed: %d %s", what, res, xrResultName(res));
        lastFailedResult = res;
        lastFailedCall = what;
        return 0;
    }
    return 1;
}

// Runtimes offer about a hundred extensions; plenty of headroom.
#define START_EXTS_MAX 8192

// Where a failed start stopped, what the runtime answered and what
// it offered. Filled in during init and kept only on failure.
struct StartReport {
    char step[16];
    char call[64];
    XrResult result;
    char detail[192];
    char runtime[XR_MAX_RUNTIME_NAME_SIZE + 32];
    // Space separated.
    char offered[START_EXTS_MAX];
    char requested[1024];
};

// Last failed start, taken by Java after nativeInit returns. Locked,
// since a start Java gave up waiting for may still be finishing.
static struct StartReport failedStart;
static int failedStartSet;
static pthread_mutex_t failedStartLock = PTHREAD_MUTEX_INITIALIZER;

// Records where a start stopped. Only the first stop counts; later ones are unwinding.
static void startStopped(XrCtx* ctx, const char* step, const char* call, XrResult result) {
    struct StartReport* r = ctx->start;
    if (r == NULL || r->step[0] != '\0') {
        return;
    }
    snprintf(r->step, sizeof(r->step), "%s", step);
    snprintf(r->call, sizeof(r->call), "%s", call != NULL ? call : "");
    r->result = result;
}

// As startStopped, for the call checkXr last saw fail.
static void startStoppedAtLast(XrCtx* ctx, const char* step) {
    startStopped(ctx, step, lastFailedCall, lastFailedResult);
}

// Extra detail for the recorded stop, where the result alone is not enough.
static void startDetail(XrCtx* ctx, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void startDetail(XrCtx* ctx, const char* fmt, ...) {
    struct StartReport* r = ctx->start;
    if (r == NULL || r->detail[0] != '\0') {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(r->detail, sizeof(r->detail), fmt, args);
    va_end(args);
}

// Appends to a space separated list; skips the name if it does not fit.
static void appendName(char* list, size_t size, const char* name) {
    size_t used = strlen(list);
    size_t need = strlen(name) + (used > 0 ? 1 : 0);
    if (used + need >= size) {
        return;
    }
    snprintf(list + used, size - used, "%s%s", used > 0 ? " " : "", name);
}

// Publishes the failure report for Java to take (Java logs it in full).
static void publishStartFailure(XrCtx* ctx) {
    struct StartReport* r = ctx->start;
    if (r == NULL) {
        return;
    }
    if (r->step[0] == '\0') {
        startStoppedAtLast(ctx, "unknown");
    }
    snprintf(r->runtime, sizeof(r->runtime), "%s", ctx->runtimeLabel);
    LOGE("VR start stopped at %s: %s %s%s%s", r->step, r->call,
         r->result != XR_SUCCESS ? xrResultName(r->result) : "",
         r->detail[0] != '\0' ? ", " : "", r->detail);

    pthread_mutex_lock(&failedStartLock);
    failedStart = *r;
    failedStartSet = 1;
    pthread_mutex_unlock(&failedStartLock);
}

// Reports a failed EGL call; reads eglGetError before another call resets it.
static int eglFailed(XrCtx* ctx, const char* call, const char* why) {
    EGLint error = eglGetError();
    LOGE("%s failed: 0x%x%s%s", call, error, why != NULL ? ", " : "", why != NULL ? why : "");
    startStopped(ctx, "egl", call, XR_SUCCESS);
    if (why != NULL) {
        startDetail(ctx, "%s", why);
    }
    else {
        startDetail(ctx, "EGL error 0x%x", error);
    }
    return 0;
}

static int initEgl(XrCtx* ctx) {
    ctx->eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ctx->eglDisplay == EGL_NO_DISPLAY) {
        return eglFailed(ctx, "eglGetDisplay", NULL);
    }
    if (!eglInitialize(ctx->eglDisplay, NULL, NULL)) {
        return eglFailed(ctx, "eglInitialize", NULL);
    }

    const EGLint configAttribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLint numConfigs = 0;
    if (!eglChooseConfig(ctx->eglDisplay, configAttribs, &ctx->eglConfig, 1, &numConfigs)) {
        return eglFailed(ctx, "eglChooseConfig", NULL);
    }
    if (numConfigs < 1) {
        return eglFailed(ctx, "eglChooseConfig", "no RGBA8 ES3 pbuffer config");
    }

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ctx->eglContext = eglCreateContext(ctx->eglDisplay, ctx->eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (ctx->eglContext == EGL_NO_CONTEXT) {
        return eglFailed(ctx, "eglCreateContext", NULL);
    }

    // The context needs a current surface, but everything renders to FBOs.
    const EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    ctx->eglPbuffer = eglCreatePbufferSurface(ctx->eglDisplay, ctx->eglConfig, pbufferAttribs);
    if (ctx->eglPbuffer == EGL_NO_SURFACE) {
        return eglFailed(ctx, "eglCreatePbufferSurface", NULL);
    }

    if (!eglMakeCurrent(ctx->eglDisplay, ctx->eglPbuffer, ctx->eglPbuffer, ctx->eglContext)) {
        return eglFailed(ctx, "eglMakeCurrent", NULL);
    }

    return 1;
}

#define MAX_ENABLED_EXTS 24

static void enableExt(const char** list, uint32_t* count, const char* name) {
    if (*count >= MAX_ENABLED_EXTS) {
        LOGW("no room to enable %s, leaving it out", name);
        return;
    }
    list[(*count)++] = name;
}

// Plain-language meaning of the results most likely to show up in a user's log.
static const char* xrResultMeaning(XrResult res) {
    switch (res) {
        case XR_ERROR_RUNTIME_UNAVAILABLE:
            return "no OpenXR runtime answered the loader";
        case XR_ERROR_RUNTIME_FAILURE:
            return "the OpenXR runtime failed";
        case XR_ERROR_INITIALIZATION_FAILED:
            return "initialization failed";
        case XR_ERROR_API_VERSION_UNSUPPORTED:
            return "API version unsupported";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
            return "headset not available";
        default:
            return "see XrResult in openxr.h";
    }
}

static int initXrInstance(XrCtx* ctx) {
    forgetLastFailure();
    PFN_xrInitializeLoaderKHR initLoader = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction*)&initLoader);
    XrResult loaderResult = XR_SUCCESS;
    if (initLoader != NULL) {
        XrLoaderInitInfoAndroidKHR loaderInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        loaderInfo.applicationVM = ctx->vm;
        loaderInfo.applicationContext = ctx->activity;
        loaderResult = initLoader((XrLoaderInitInfoBaseHeaderKHR*)&loaderInfo);
        checkXr(loaderResult, "xrInitializeLoaderKHR");
    }

    // The most likely start failure: the loader finds the runtime
    // through the Android OpenXR runtime broker, and a headset
    // without one (or one that refuses this app) reports no runtime.
    uint32_t extCount = 0;
    XrResult listed = xrEnumerateInstanceExtensionProperties(NULL, 0, &extCount, NULL);
    if (XR_FAILED(listed)) {
        LOGE("xrEnumerateInstanceExtensionProperties failed: %d %s, %s", listed,
             xrResultName(listed), xrResultMeaning(listed));
        // A loader that could not initialise is the more likely cause.
        if (initLoader == NULL) {
            startStopped(ctx, "loader", "xrInitializeLoaderKHR", XR_SUCCESS);
            startDetail(ctx, "the loader has no xrInitializeLoaderKHR");
        }
        else if (XR_FAILED(loaderResult)) {
            startStopped(ctx, "loader", "xrInitializeLoaderKHR", loaderResult);
        }
        else {
            startStopped(ctx, "runtime", "xrEnumerateInstanceExtensionProperties", listed);
        }
        return 0;
    }
    XrExtensionProperties* exts = calloc(extCount, sizeof(XrExtensionProperties));
    if (extCount > 0 && exts == NULL) {
        LOGE("no memory for %u extension properties", extCount);
        return 0;
    }
    for (uint32_t i = 0; i < extCount; i++) {
        exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    xrEnumerateInstanceExtensionProperties(NULL, extCount, &extCount, exts);

    int haveGles = 0, haveAndroidCreate = 0;
    LOGEV("runtime offers %u OpenXR extensions", extCount);
    for (uint32_t i = 0; i < extCount; i++) {
        LOGI("  extension %s", exts[i].extensionName);
        if (ctx->start != NULL) {
            appendName(ctx->start->offered, sizeof(ctx->start->offered), exts[i].extensionName);
        }
        if (!strcmp(exts[i].extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) haveGles = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) haveAndroidCreate = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)) ctx->cylinderSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME)) ctx->picoInteraction = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_HAND_INTERACTION_EXTENSION_NAME)) ctx->handInteraction = 1;
        if (!strcmp(exts[i].extensionName, XR_MSFT_HAND_INTERACTION_EXTENSION_NAME)) ctx->msftHandInteraction = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME)) ctx->handTracking = 1;
        if (!strcmp(exts[i].extensionName, XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME)) ctx->handAimOffered = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) ctx->eyeGaze = 1;
        if (!strcmp(exts[i].extensionName, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME)) ctx->layerSettingsSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME)) ctx->virtualKeyboardSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) ctx->refreshRateSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME)) ctx->perfSettingsSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME)) ctx->colorScaleSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME)) ctx->fbPassthroughSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_FB_COLOR_SPACE_EXTENSION_NAME)) ctx->colorSpaceSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_META_BOUNDARY_VISIBILITY_EXTENSION_NAME)) ctx->boundarySupported = 1;
        if (!strcmp(exts[i].extensionName, XR_META_PASSTHROUGH_PREFERENCES_EXTENSION_NAME)) ctx->passthroughPrefsSupported = 1;
    }
    free(exts);

    // With hands disabled no hand extension is enabled, so the app only ever sees controllers.
    if (!ctx->handsEnabled) {
        ctx->handInteraction = 0;
        ctx->msftHandInteraction = 0;
        ctx->handTracking = 0;
        ctx->handAimOffered = 0;
        LOGI("hand tracking off by preference");
    }

    if (!haveGles || !haveAndroidCreate) {
        LOGE("required OpenXR extensions missing (gles=%d androidCreate=%d)", haveGles, haveAndroidCreate);
        if (extCount == 0) {
            // Answered with nothing: effectively no runtime.
            startStopped(ctx, "runtime", "xrEnumerateInstanceExtensionProperties", listed);
            startDetail(ctx, "the runtime offered no extensions");
        }
        else {
            startStopped(ctx, "extensions", "", XR_SUCCESS);
            startDetail(ctx, "missing%s%s",
                        haveGles ? "" : " " XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                        haveAndroidCreate ? "" : " " XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
        }
        return 0;
    }

    const char* enabledExts[MAX_ENABLED_EXTS];
    uint32_t enabledCount = 0;
    enableExt(enabledExts, &enabledCount, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
    enableExt(enabledExts, &enabledCount, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    if (ctx->cylinderSupported) {
        enableExt(enabledExts, &enabledCount, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }
    if (ctx->picoInteraction) {
        enableExt(enabledExts, &enabledCount, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
    }
    if (ctx->handInteraction) {
        enableExt(enabledExts, &enabledCount, XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    }
    // Some runtimes ignore the hand interaction profile unless hand tracking is also enabled.
    if (ctx->handTracking) {
        enableExt(enabledExts, &enabledCount, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    }
    // Pinch flag alongside the joints, for runtimes with no hand
    // profile. Extends the joint locate, so it needs hand tracking.
    ctx->handAimOffered = ctx->handAimOffered && ctx->handTracking;
    if (ctx->handAimOffered) {
        enableExt(enabledExts, &enabledCount, XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME);
    }
    if (ctx->eyeGaze) {
        enableExt(enabledExts, &enabledCount, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    }
    if (ctx->msftHandInteraction) {
        enableExt(enabledExts, &enabledCount, XR_MSFT_HAND_INTERACTION_EXTENSION_NAME);
    }
    if (ctx->layerSettingsSupported) {
        enableExt(enabledExts, &enabledCount, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    }
    if (ctx->refreshRateSupported) {
        enableExt(enabledExts, &enabledCount, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    if (ctx->perfSettingsSupported) {
        enableExt(enabledExts, &enabledCount, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    }
    if (ctx->colorScaleSupported) {
        enableExt(enabledExts, &enabledCount, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
    }
    if (ctx->fbPassthroughSupported) {
        enableExt(enabledExts, &enabledCount, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    }
    if (ctx->colorSpaceSupported) {
        enableExt(enabledExts, &enabledCount, XR_FB_COLOR_SPACE_EXTENSION_NAME);
    }
    if (ctx->boundarySupported) {
        enableExt(enabledExts, &enabledCount, XR_META_BOUNDARY_VISIBILITY_EXTENSION_NAME);
    }
    if (ctx->passthroughPrefsSupported) {
        enableExt(enabledExts, &enabledCount, XR_META_PASSTHROUGH_PREFERENCES_EXTENSION_NAME);
    }
    if (startFailKnob("instance")) {
        enableExt(enabledExts, &enabledCount, "XR_MOONLIGHT_no_such_extension");
    }
    if (ctx->start != NULL) {
        for (uint32_t i = 0; i < enabledCount; i++) {
            appendName(ctx->start->requested, sizeof(ctx->start->requested), enabledExts[i]);
        }
    }

    XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    androidInfo.applicationVM = ctx->vm;
    androidInfo.applicationActivity = ctx->activity;

    XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    createInfo.next = &androidInfo;
    strncpy(createInfo.applicationInfo.applicationName, "AaksVR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, "AaksVR", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = enabledCount;
    createInfo.enabledExtensionNames = enabledExts;

    XrResult created = xrCreateInstance(&createInfo, &ctx->instance);
    if (XR_FAILED(created)) {
        LOGE("xrCreateInstance failed: %d %s, %s", created, xrResultName(created),
             xrResultMeaning(created));
        startStopped(ctx, "instance", "xrCreateInstance", created);
        return 0;
    }
    probeDisplayExtensions(ctx);

    XrInstanceProperties instanceProps = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(ctx->instance, &instanceProps))) {
        snprintf(ctx->runtimeLabel, sizeof(ctx->runtimeLabel), "%s %u.%u.%u",
                 instanceProps.runtimeName,
                 (unsigned)XR_VERSION_MAJOR(instanceProps.runtimeVersion),
                 (unsigned)XR_VERSION_MINOR(instanceProps.runtimeVersion),
                 (unsigned)XR_VERSION_PATCH(instanceProps.runtimeVersion));
        LOGEV("runtime %s", ctx->runtimeLabel);
    }

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    // The fail knob asks for a handheld, which a headset runtime refuses.
    systemInfo.formFactor = startFailKnob("system") ? XR_FORM_FACTOR_HANDHELD_DISPLAY
                                                    : XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!checkXr(xrGetSystem(ctx->instance, &systemInfo, &ctx->systemId), "xrGetSystem")) {
        startStoppedAtLast(ctx, "system");
        return 0;
    }

    uint32_t blendModeCount = 0;
    xrEnumerateEnvironmentBlendModes(ctx->instance, ctx->systemId,
                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     0, &blendModeCount, NULL);
    XrEnvironmentBlendMode* modes = blendModeCount > 0
            ? calloc(blendModeCount, sizeof(XrEnvironmentBlendMode)) : NULL;
    if (modes != NULL) {
        xrEnumerateEnvironmentBlendModes(ctx->instance, ctx->systemId,
                                         XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                         blendModeCount, &blendModeCount, modes);
        for (uint32_t i = 0; i < blendModeCount; i++) {
            LOGEV("environment blend mode %u available", modes[i]);
            if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
                ctx->alphaBlendSupported = 1;
            }
        }
        free(modes);
    }
    LOGEV("passthrough %s", ctx->alphaBlendSupported ? "available" : "not offered by this runtime");
    LOGEV("compositor sharpening %s", ctx->layerSettingsSupported
          ? "available (XR_FB_composition_layer_settings)" : "not offered by this runtime");
    LOGEV("virtual keyboard extension %s", ctx->virtualKeyboardSupported
          ? "available (XR_META_virtual_keyboard)" : "not offered by this runtime");
    LOGEV("layer colour scale %s", ctx->colorScaleSupported
          ? "available (XR_KHR_composition_layer_color_scale_bias), the panels and the splash fade"
          : "not offered by this runtime, the panels and the splash cut");

    // Layers per frame; a runtime that does not say gets the spec minimum.
    ctx->maxLayerCount = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    XrSystemProperties layerProps = { XR_TYPE_SYSTEM_PROPERTIES };
    if (!XR_FAILED(xrGetSystemProperties(ctx->instance, ctx->systemId, &layerProps))) {
        if (layerProps.graphicsProperties.maxLayerCount > 0) {
            ctx->maxLayerCount = (int)layerProps.graphicsProperties.maxLayerCount;
        }
        // The Quest 3 reports 32, but its compositor drops any frame with more than 16
        // after merging. Capped at the spec's 16 (the Pico's own limit) everywhere.
        if (ctx->maxLayerCount > XR_MIN_COMPOSITION_LAYERS_SUPPORTED) {
            LOGI("runtime reports %d composition layers, %d used", ctx->maxLayerCount,
                 XR_MIN_COMPOSITION_LAYERS_SUPPORTED);
            ctx->maxLayerCount = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
        }
        // Identifies unknown headsets in user logs.
        LOGEV("system %s (vendor 0x%x)", layerProps.systemName, layerProps.vendorId);
    }

    // The runtime's recommended view size. Only the 3D room renders at it.
    uint32_t configViewCount = 0;
    xrEnumerateViewConfigurationViews(ctx->instance, ctx->systemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &configViewCount, NULL);
    XrViewConfigurationView* configViews = configViewCount > 0
            ? calloc(configViewCount, sizeof(XrViewConfigurationView)) : NULL;
    if (configViews != NULL) {
        for (uint32_t i = 0; i < configViewCount; i++) {
            configViews[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        }
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
                ctx->instance, ctx->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                configViewCount, &configViewCount, configViews))) {
            ctx->recommendedEyeWidth = (int)configViews[0].recommendedImageRectWidth;
            ctx->recommendedEyeHeight = (int)configViews[0].recommendedImageRectHeight;
            ctx->maxEyeWidth = (int)configViews[0].maxImageRectWidth;
            ctx->maxEyeHeight = (int)configViews[0].maxImageRectHeight;
            LOGI("recommended render size %dx%d per eye (max %dx%d), %u views",
                 ctx->recommendedEyeWidth, ctx->recommendedEyeHeight,
                 ctx->maxEyeWidth, ctx->maxEyeHeight, configViewCount);
        }
        free(configViews);
    }

    // Offering the extension does not mean the hardware has eye tracking, so ask the system.
    if (ctx->eyeGaze) {
        XrSystemEyeGazeInteractionPropertiesEXT gazeProps = {
            XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT
        };
        XrSystemProperties props = { XR_TYPE_SYSTEM_PROPERTIES };
        props.next = &gazeProps;
        if (XR_FAILED(xrGetSystemProperties(ctx->instance, ctx->systemId, &props))
                || !gazeProps.supportsEyeGazeInteraction) {
            ctx->eyeGaze = 0;
        }
        LOGEV("eye gaze %s", ctx->eyeGaze ? "available" : "offered but not supported by this system");
    }

    if (ctx->handTracking) {
        XrSystemHandTrackingPropertiesEXT handProps = {
            XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT
        };
        XrSystemProperties props = { XR_TYPE_SYSTEM_PROPERTIES };
        props.next = &handProps;
        if (XR_FAILED(xrGetSystemProperties(ctx->instance, ctx->systemId, &props))
                || !handProps.supportsHandTracking) {
            ctx->handTracking = 0;
        }
        LOGEV("hand joints %s", ctx->handTracking ? "available" : "not supported by this system");
    }

    XrResult found = xrGetInstanceProcAddr(ctx->instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                           (PFN_xrVoidFunction*)&ctx->pfnGetGlesReqs);
    if (ctx->pfnGetGlesReqs == NULL) {
        LOGE("xrGetOpenGLESGraphicsRequirementsKHR not found");
        startStopped(ctx, "graphics", "xrGetOpenGLESGraphicsRequirementsKHR", found);
        startDetail(ctx, "the entry point is missing");
        return 0;
    }

    return 1;
}

static int initXrSession(XrCtx* ctx) {
    forgetLastFailure();
    // Spec requires this call before session creation.
    XrGraphicsRequirementsOpenGLESKHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    if (!checkXr(ctx->pfnGetGlesReqs(ctx->instance, ctx->systemId, &reqs), "get gles requirements")) {
        startStoppedAtLast(ctx, "graphics");
        return 0;
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    binding.display = ctx->eglDisplay;
    binding.config = ctx->eglConfig;
    binding.context = ctx->eglContext;

    XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &binding;
    // The fail knob asks for a system that does not exist.
    sessionInfo.systemId = startFailKnob("session") ? XR_NULL_SYSTEM_ID : ctx->systemId;
    if (!checkXr(xrCreateSession(ctx->instance, &sessionInfo, &ctx->session), "xrCreateSession")) {
        startStoppedAtLast(ctx, "session");
        return 0;
    }

    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!checkXr(xrCreateReferenceSpace(ctx->session, &spaceInfo, &ctx->localSpace), "create local space")) {
        startStoppedAtLast(ctx, "space");
        return 0;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!checkXr(xrCreateReferenceSpace(ctx->session, &spaceInfo, &ctx->viewSpace), "create view space")) {
        startStoppedAtLast(ctx, "space");
        return 0;
    }

    readStartKnobs(ctx);
    startDisplay(ctx);
    startColorSpace(ctx);
    startBoundary(ctx);
    startPerfLevels(ctx);
    return 1;
}

static int initSwapchain(XrCtx* ctx) {
    forgetLastFailure();
    uint32_t formatCount = 0;
    XrResult listed = xrEnumerateSwapchainFormats(ctx->session, 0, &formatCount, NULL);
    int64_t* formats = calloc(formatCount, sizeof(int64_t));
    if (formatCount == 0 || formats == NULL) {
        LOGE("no swapchain formats to choose from");
        startStopped(ctx, "swapchain", "xrEnumerateSwapchainFormats", listed);
        startDetail(ctx, "no formats offered");
        free(formats);
        return 0;
    }
    xrEnumerateSwapchainFormats(ctx->session, formatCount, &formatCount, formats);

    ctx->swapchainFormat = 0;
    for (uint32_t i = 0; i < formatCount; i++) {
        if (formats[i] == GL_SRGB8_ALPHA8) {
            ctx->swapchainFormat = GL_SRGB8_ALPHA8;
            break;
        }
    }
    if (ctx->swapchainFormat == 0) {
        ctx->swapchainFormat = formats[0];
        LOGW("no SRGB8_ALPHA8 swapchain format, using %lld", (long long)ctx->swapchainFormat);
    }
    free(formats);

    // Stereo renders both eyes side by side in one swapchain.
    int chainWidth = ctx->stereoMode != DEPTH_MODE_OFF ? ctx->videoWidth * 2 : ctx->videoWidth;
    if (!createArtSwapchain(ctx, chainWidth, ctx->videoHeight, "xrCreateSwapchain",
                            &ctx->swapchain, &ctx->swapchainImages, &ctx->swapchainImageCount)) {
        startStoppedAtLast(ctx, "swapchain");
        startDetail(ctx, "%dx%d, format 0x%llx", chainWidth, ctx->videoHeight,
                    (unsigned long long)ctx->swapchainFormat);
        return 0;
    }

    LOGEV("swapchain %dx%d format %lld, %u images (stereo mode %d)", chainWidth, ctx->videoHeight,
         (long long)ctx->swapchainFormat, ctx->swapchainImageCount, ctx->stereoMode);

    // Optional: playback continues without the overlay, splash or toast.
    createArtSwapchain(ctx, OVERLAY_WIDTH, OVERLAY_HEIGHT, "create overlay swapchain",
                       &ctx->overlaySwapchain, &ctx->overlayImages, &ctx->overlayImageCount);
    createArtSwapchain(ctx, SPLASH_TEX_W, SPLASH_TEX_H, "create splash swapchain",
                       &ctx->splashSwapchain, &ctx->splashImages, &ctx->splashImageCount);
    createArtSwapchain(ctx, TOAST_TEX_W, TOAST_TEX_H, "create toast swapchain",
                       &ctx->toastSwapchain, &ctx->toastImages, &ctx->toastImageCount);

    return 1;
}

// Ends the frame loop for a runtime reason, which Java
// passes to the activity. The first reason is kept.
static void runtimeEnded(XrCtx* ctx, const char* what, XrResult res) {
    ctx->exitRequested = 1;
    if (ctx->exitReason[0] != '\0') {
        return;
    }
    if (res != XR_SUCCESS) {
        snprintf(ctx->exitReason, sizeof(ctx->exitReason), "%s, %s", what, xrResultName(res));
    }
    else {
        snprintf(ctx->exitReason, sizeof(ctx->exitReason), "%s", what);
    }
    LOGEV("frame loop ending: %s", ctx->exitReason);
}

static void handleSessionStateChange(XrCtx* ctx, XrSessionState newState) {
    LOGI("session state %d -> %d", ctx->sessionState, newState);
    ctx->sessionState = newState;

    switch (newState) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (checkXr(xrBeginSession(ctx->session, &beginInfo), "xrBeginSession")) {
                ctx->sessionRunning = 1;
                ctx->sessionBegins++;
                LOGEV("session begun (%d begun, %d ended)", ctx->sessionBegins, ctx->sessionEnds);
                displaySessionBegun(ctx);
            }
            break;
        }
        case XR_SESSION_STATE_FOCUSED:
            if (!ctx->everFocused) {
                LOGEV("session focused for the first time");
            }
            ctx->everFocused = 1;
            displayFocused(ctx);
            break;
        case XR_SESSION_STATE_STOPPING:
            checkXr(xrEndSession(ctx->session), "xrEndSession");
            ctx->sessionRunning = 0;
            ctx->sessionEnds++;
            LOGEV("session ended for the runtime (%d begun, %d ended)", ctx->sessionBegins,
                  ctx->sessionEnds);
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            ctx->sessionRunning = 0;
            runtimeEnded(ctx, newState == XR_SESSION_STATE_EXITING ? "session exiting"
                                                                   : "session loss pending",
                         XR_SUCCESS);
            break;
        default:
            break;
    }
}

static void pollEvents(XrCtx* ctx) {
    XrEventDataBuffer event;
    for (;;) {
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        event.next = NULL;
        XrResult res = xrPollEvent(ctx->instance, &event);
        if (res != XR_SUCCESS) {
            break;
        }
        // Not in the headers' event list, so handled before the switch.
        if ((int)event.type == (int)XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META) {
            boundaryChanged(ctx, ((XrEventDataBoundaryVisibilityChangedMETA*)&event)
                                    ->boundaryVisibility);
            continue;
        }
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                XrEventDataSessionStateChanged* sc = (XrEventDataSessionStateChanged*)&event;
                handleSessionStateChange(ctx, sc->state);
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                XrEventDataReferenceSpaceChangePending* change =
                        (XrEventDataReferenceSpaceChangePending*)&event;
                if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    recentreScreen(ctx);
                    // The head jumps when the change lands, which head
                    // aim must not read as a turn. changeTime can be 0
                    // or early, so also hold past the last frame.
                    XrTime hold = ctx->predictedDisplayTime + HEAD_AIM_RECENTRE_HOLD_NS;
                    ctx->headAimRecentring = 1;
                    ctx->headAimRecentreAt = change->changeTime > hold ? change->changeTime : hold;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                // Picking up or putting down a controller swaps that hand's profile.
                refreshInputSource(ctx);
                break;
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                XrEventDataDisplayRefreshRateChangedFB* rate =
                        (XrEventDataDisplayRefreshRateChangedFB*)&event;
                displayRateChanged(ctx, rate->fromDisplayRefreshRate, rate->toDisplayRefreshRate);
                break;
            }
            case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT:
                perfNotice(ctx, (XrEventDataPerfSettingsEXT*)&event);
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                runtimeEnded(ctx, "instance loss pending", XR_SUCCESS);
                break;
            default:
                break;
        }
    }
}

static void destroyCtx(JNIEnv* env, XrCtx* ctx) {
    destroyXrInput(ctx);

    // The depth threads are joined, but fences on slots published after the last
    // sample, or on captures the stage thread never took, still need deleting.
    for (int i = 0; i < DEPTH_TEX_COUNT; i++) {
        if (ctx->depthFences[i] != NULL) {
            glDeleteSync(ctx->depthFences[i]);
        }
    }
    for (int i = 0; i < DEPTH_PAIRS; i++) {
        if (ctx->captureFences[i] != NULL) {
            glDeleteSync(ctx->captureFences[i]);
        }
    }
    glDeleteBuffers(DEPTH_PAIRS, ctx->depthPbos);
    glDeleteBuffers(1, &ctx->ambiDetectPbo);
    relightDestroy(ctx);
    for (int i = 0; i < DEPTH_PAIRS; i++) {
        free(ctx->modelInput[i]);
        free(ctx->modelOutput[i]);
    }
    free(ctx->depthUploadBuf);
    free(ctx->depthNorm);
    free(ctx->depthTau);
    free(ctx->depthLow);
    free(ctx->depthScratch);
    free(ctx->depthColSums);

    // Context destruction would free these anyway;
    // released explicitly alongside the glow's swapchain.
    glDeleteFramebuffers(1, &ctx->ambiFbo);
    glDeleteFramebuffers(1, &ctx->ambiDetectFbo);
    glDeleteFramebuffers(1, &ctx->glowFbo);
    glDeleteFramebuffers(1, &ctx->glowEdgeFbo);
    glDeleteTextures(1, &ctx->ambiTexture);
    glDeleteTextures(1, &ctx->ambiDetectTexture);
    glDeleteTextures(1, &ctx->glowEdgeTexture);
    if (ctx->ambiProgram != 0) {
        glDeleteProgram(ctx->ambiProgram);
    }
    if (ctx->glowProgram != 0) {
        glDeleteProgram(ctx->glowProgram);
    }
    if (ctx->glowEdgeProgram != 0) {
        glDeleteProgram(ctx->glowEdgeProgram);
    }

    // Room resources exist only if a frame ran with a room.
    glDeleteFramebuffers(1, &ctx->roomFbo);
    glDeleteRenderbuffers(1, &ctx->roomDepthBuffer);
    glDeleteBuffers(1, &ctx->roomVertexBuffer);
    glDeleteBuffers(1, &ctx->roomIndexBuffer);
    glDeleteTextures(ROOM_MESH_ATLASES_MAX, ctx->roomTextures);
    glDeleteTextures(1, &ctx->roomWhiteTexture);
    if (ctx->roomProgram != 0) {
        glDeleteProgram(ctx->roomProgram);
    }
    // Controller model resources exist once a model has shown.
    glDeleteBuffers(1, &ctx->modelVertexBuffer);
    glDeleteBuffers(1, &ctx->modelIndexBuffer);
    glDeleteFramebuffers(1, &ctx->modelFbo);
    glDeleteRenderbuffers(1, &ctx->modelDepthBuffer);
    if (ctx->modelProgram != 0) {
        glDeleteProgram(ctx->modelProgram);
    }
    free(ctx->roomModelVerts);
    free(ctx->roomModelIndices);

    destroyArtSwapchain(ctx, &ctx->swapchain, &ctx->swapchainImages);
    destroyArtSwapchain(ctx, &ctx->overlaySwapchain, &ctx->overlayImages);
    destroyArtSwapchain(ctx, &ctx->pointerSwapchain, &ctx->pointerImages);
    destroyArtSwapchain(ctx, &ctx->barSwapchain, &ctx->barImages);
    destroyArtSwapchain(ctx, &ctx->cornerSwapchain, &ctx->cornerImages);
    destroyArtSwapchain(ctx, &ctx->pickerSwapchain, &ctx->pickerImages);
    destroyArtSwapchain(ctx, &ctx->cogPanelSwapchain, &ctx->cogPanelImages);
    destroyArtSwapchain(ctx, &ctx->cogThumbSwapchain, &ctx->cogThumbImages);
    destroyArtSwapchain(ctx, &ctx->cogReadoutSwapchain, &ctx->cogReadoutImages);
    destroyArtSwapchain(ctx, &ctx->cogMarksSwapchain, &ctx->cogMarksImages);
    destroyArtSwapchain(ctx, &ctx->cogClockSwapchain, &ctx->cogClockImages);
    destroyArtSwapchain(ctx, &ctx->kbPanelSwapchain, &ctx->kbPanelImages);
    destroyArtSwapchain(ctx, &ctx->exitPromptSwapchain, &ctx->exitPromptImages);
    destroyArtSwapchain(ctx, &ctx->reportSwapchain, &ctx->reportImages);
    destroyArtSwapchain(ctx, &ctx->buttonSwapchain, &ctx->buttonImages);
    destroyArtSwapchain(ctx, &ctx->hintSwapchain, &ctx->hintImages);
    destroyArtSwapchain(ctx, &ctx->kofiSwapchain, &ctx->kofiImages);
    destroyUiPanels(ctx);
    destroyArtSwapchain(ctx, &ctx->outlineSwapchain, &ctx->outlineImages);
    destroyArtSwapchain(ctx, &ctx->glowSwapchain, &ctx->glowImages);
    destroyArtSwapchain(ctx, &ctx->roomSwapchain, &ctx->roomImages);
    destroyArtSwapchain(ctx, &ctx->modelSwapchain, &ctx->modelImages);
    destroyArtSwapchain(ctx, &ctx->splashSwapchain, &ctx->splashImages);
    destroyArtSwapchain(ctx, &ctx->toastSwapchain, &ctx->toastImages);
    freeArtSheets(ctx);
    if (ctx->localSpace != XR_NULL_HANDLE) {
        xrDestroySpace(ctx->localSpace);
    }
    if (ctx->viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(ctx->viewSpace);
    }
    if (ctx->session != XR_NULL_HANDLE) {
        xrDestroySession(ctx->session);
    }
    if (ctx->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(ctx->instance);
    }

    if (ctx->timerSupported) {
        pfnDeleteQueries(2, ctx->timerQueries);
        pfnDeleteQueries(2, ctx->roomTimerQueries);
        if (ctx->modelPassReady) {
            pfnDeleteQueries(2, ctx->modelTimerQueries);
        }
    }

    if (ctx->eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(ctx->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        // The stage thread destroys its own context;
        // these remain only if the model never loaded.
        if (ctx->depthStagePbuffer != EGL_NO_SURFACE) {
            eglDestroySurface(ctx->eglDisplay, ctx->depthStagePbuffer);
        }
        if (ctx->depthStageContext != EGL_NO_CONTEXT) {
            eglDestroyContext(ctx->eglDisplay, ctx->depthStageContext);
        }
        if (ctx->eglPbuffer != EGL_NO_SURFACE) {
            eglDestroySurface(ctx->eglDisplay, ctx->eglPbuffer);
        }
        if (ctx->eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(ctx->eglDisplay, ctx->eglContext);
        }
        eglReleaseThread();
    }

    if (ctx->activity != NULL) {
        (*env)->DeleteGlobalRef(env, ctx->activity);
    }
    free(ctx->start);
    free(ctx);
}

JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeInit(JNIEnv* env, jobject thiz,
                                                       jobject activity, jint width, jint height,
                                                       jint fps, jint stereoMode, jint depthWidth,
                                                       jint depthHeight, jboolean depthDebug,
                                                       jint convergence, jint depthScale,
                                                       jboolean handTracking, jint sharpenMode,
                                                       jint supersampleMode,
                                                       jboolean perfOverlay, jboolean ambilight,
                                                       jint ambiLevel, jboolean roomLight,
                                                       jint envResTier) {
    XrCtx* ctx = calloc(1, sizeof(XrCtx));
    ctx->handsEnabled = handTracking;
    // 0 low, 1 standard, 2 high, 3 ultra.
    ctx->envResTier = envResTier;
    ctx->videoWidth = width;
    ctx->videoHeight = height;
    // The display rate is matched to it.
    ctx->streamFps = fps;
    ctx->perfLevel = PERF_LEVEL_SUSTAINED_HIGH;
    // Until Java sets the preference.
    ctx->depthRateSetting = DEPTH_RATE_DEFAULT;
    depthGovernorStart(&ctx->depthGov, DEPTH_RATE_DEFAULT);
    ctx->stereoMode = stereoMode;
    // Stereo sessions start with 3D on; the switch lasts the session.
    ctx->stereoLive = stereoMode != DEPTH_MODE_OFF;
    ctx->drawnEyes = ctx->stereoLive ? 2 : 1;
    // Depth map size from the model Java picked; must
    // be settled before the depth path allocates.
    if (!depthSizeOk(depthWidth, depthHeight)) {
        LOGW("depth size %dx%d out of range, using %d square", depthWidth, depthHeight,
             DEPTH_TEX_SIZE_DEFAULT);
        depthWidth = DEPTH_TEX_SIZE_DEFAULT;
        depthHeight = DEPTH_TEX_SIZE_DEFAULT;
    }
    ctx->depthTexW = depthWidth;
    ctx->depthTexH = depthHeight;
    ctx->depthDebug = depthDebug;
    ctx->sessionState = XR_SESSION_STATE_UNKNOWN;
    // Per-texel average over 30 ms and range over 150 ms of real
    // time, whatever the model rate. A scene cut resets both.
    ctx->depthTauMs = DEPTH_TAU_DEFAULT_MS;
    ctx->rangeTauMs = DEPTH_RANGE_TAU_DEFAULT_MS;
    ctx->depthCutLevel = 1;
    // 0.25 measured best: same 5 px edge as tighter values with a tenth of the speckle.
    ctx->upsampleSigmaR = 0.25f;
    ctx->upsampleEnabled = 1;
    ctx->occlusionEnabled = 1;
    // Warp edge fixes on, except the cubic depth read: 1.1-1.3 ms of
    // warp GPU time at 4K, too much for 90 Hz without a visible benefit.
    ctx->srcInsetOn = 1;
    ctx->edgeFadePx = EDGE_FADE_PX;
    ctx->depthCubic = 0;
    ctx->seamInset = 1;
    // Off until it proves itself in a blind comparison.
    ctx->depthSharp = 0.0f;
    ctx->overlayVisible = perfOverlay;
    ctx->separationOverride = -1.0f;
    ctx->distanceOverride = -1.0f;
    ctx->screenOverride = -1.0f;
    // -1: the curve and separation preferences own their values.
    ctx->panelCurve = -1.0f;
    ctx->panelSeparation = -1.0f;
    // Placeholder; the first endFrame writes the real value.
    ctx->separationCurrent = 0.005f;
    // MiDaS defaults until Java sets the running model's.
    ctx->defaultSeparation = 0.005f;
    ctx->defaultConvergence = 0.5f;
    ctx->presetUnits[COG_PRESET_COMFORT] = 2;
    ctx->presetUnits[COG_PRESET_BALANCED] = 5;
    ctx->presetUnits[COG_PRESET_STRONG] = 8;
    ctx->cogDragSlider = -1;
    ctx->cogDragHand = -1;
    ctx->cogDragFace = -1;
    ctx->cogHoverSlider = -1;
    ctx->fadeNs = FADE_NS;
    for (int i = 0; i < 3; i++) {
        ctx->splashReadyMs[i] = -1;
    }
    ctx->splashWedgesShown = -1;
    atomic_init(&ctx->depthMapsStaged, 0);
    atomic_init(&ctx->depthGaveUp, 0);
    // -1 because kind 0 is a real notice.
    noticeInit(&ctx->notices);
    ctx->toastDrawnKind = -1;
    // -1 because 0 is a real key.
    ctx->kbHoverKey = -1;
    ctx->pointerMinCutoff = POINTER_MIN_CUTOFF;
    ctx->pointerBeta = POINTER_BETA;
    ctx->aimMinCutoff = AIM_MIN_CUTOFF;
    ctx->aimBeta = AIM_BETA;
    ctx->pointerWake = POINTER_WAKE_SEC;
    ctx->pointerSleep = POINTER_SLEEP_SEC;
    atomic_store(&ctx->handAutoHide, 1);
    // 1 cm reads as a thin line at 3 m without disappearing.
    ctx->beamWidth = 0.010f;
    // Ray on until Java sets the setting.
    ctx->raySetting = 1;
    ctx->rayDrawnSaid = -1;
    // Head aim off until Java sets the setting.
    ctx->headAimSensitivity = HEAD_AIM_SENSITIVITY_DEFAULT;
    ctx->headAimDeadZone = HEAD_AIM_DEADZONE_DEFAULT;
    headAimReset(&ctx->headAim);
    pointerNudgeReset(&ctx->headAimNudge);
    ctx->headAimSaid = -1;
    // Pointer mode at start. The pad starts resting so buttons
    // already held on its first live frame are ignored.
    ctx->padDeadzone = PAD_STICK_DEADZONE_DEFAULT;
    ctx->padShortcut = PAD_SHORTCUT_MENU_GRIP;
    padToggleReset(&ctx->padToggle);
    padChordReset(&ctx->padChord, 0);
    padRest(&ctx->pad);
    ctx->padResting = 1;
    // Comfort depends on absolute disparity and perceived depth on the steps
    // between objects, so global shape is pulled toward the screen plane while
    // local detail is boosted. On captured frames this gave about 40% more depth at
    // object boundaries with slightly less clipping (best plain tone curve: 16%).
    ctx->depthGlobal = 1.0f;
    ctx->convergence = convergence / 100.0f;
    ctx->depthLocal = depthScale / 100.0f;
    ctx->sharpenMode = sharpenMode >= 0 && sharpenMode <= 2 ? sharpenMode : 0;
    ctx->supersampleMode = supersampleMode >= 0 && supersampleMode <= 2 ? supersampleMode : 0;
    ctx->ambilightOn = ambilight;
    ctx->ambiIntensity = (ambiLevel < 0 ? 0 : (ambiLevel > 100 ? 100 : ambiLevel)) / 100.0f;
    ctx->ambiOverride = -1;
    // Room light defaults until Java sets them.
    ctx->relightBrightness = RELIGHT_DEFAULT_BRIGHTNESS;
    ctx->relightAmbientTarget = 1.0f;
    ctx->relightOpacitySet = -1.0f;
    ctx->playerCrop = RELIGHT_CROP_DEFAULT;
    // Neutral picture grade until Java sets the preferences.
    for (int row = 0; row < PICTURE_VALUES; row++) {
        ctx->pictureUnits[row] = pictureDefault(row);
    }
    ctx->grade = pictureGradeFor(ctx->pictureUnits);
    ctx->gradeOn = 0;
    ctx->roomLightOn = roomLight;
    // Per-room values from the room table until Java sets the preferences.
    roomLevelsFromTable(ctx);
    // -1: no debug property overrides.
    ctx->roomOverride = -1;
    ctx->roomScaleOverride = -1.0f;
    ctx->roomDimOverride = -1.0f;
    // About ten frames across a scene cut, so the glow follows rather than flashes.
    ctx->ambiSmooth = 0.08f;
    ctx->glowNorm = 1;
    // Letterbox detection on, starting from the whole frame.
    ctx->ambiBarDetect = 1;
    ctx->ambiCrop[0] = 0.0f;
    ctx->ambiCrop[1] = 0.0f;
    ctx->ambiCrop[2] = 1.0f;
    ctx->ambiCrop[3] = 1.0f;
    // -1 because cell 0 is a real choice.
    ctx->loggedChoice = -1;
    // All cells live until the art arrives with the real count.
    ctx->pickerCells = PICKER_CELLS;
    (*env)->GetJavaVM(env, &ctx->vm);
    ctx->activity = (*env)->NewGlobalRef(env, activity);

    // Clear any report left by an earlier start.
    pthread_mutex_lock(&failedStartLock);
    failedStartSet = 0;
    pthread_mutex_unlock(&failedStartLock);
    ctx->start = calloc(1, sizeof(struct StartReport));

    int started = initXrInstance(ctx) && initEgl(ctx) && initXrSession(ctx) &&
                  initSwapchain(ctx);
    if (started && !initGl(ctx)) {
        startStopped(ctx, "gl", "initGl", XR_SUCCESS);
        startDetail(ctx, "the GL lines above say which");
        started = 0;
    }
    if (!started) {
        publishStartFailure(ctx);
        destroyCtx(env, ctx);
        return 0;
    }
    free(ctx->start);
    ctx->start = NULL;

    // Optional: without usable controller bindings playback still works, without a pointer.
    if (!initXrInput(ctx)) {
        LOGW("controller input unavailable, pointer off");
        destroyXrInput(ctx);
    }
    else if (!createPointerSwapchain(ctx)) {
        LOGW("pointer swapchain unavailable, the ray will not be drawn");
    }

    LOGEV("OpenXR init complete (cylinder=%d srgbWriteControl=%d maxLayers=%d)",
         ctx->cylinderSupported, ctx->srgbWriteControl, ctx->maxLayerCount);
    return (jlong)(intptr_t)ctx;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetCaptureDir(JNIEnv* env, jobject thiz,
                                                                jlong handle, jstring dir) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || dir == NULL) {
        return;
    }
    const char* chars = (*env)->GetStringUTFChars(env, dir, NULL);
    if (chars != NULL) {
        strncpy(ctx->captureDir, chars, sizeof(ctx->captureDir) - 1);
        (*env)->ReleaseStringUTFChars(env, dir, chars);
        LOGI("capture dir %s, setprop %s to dump a frame", ctx->captureDir, CAPTURE_PROP);
    }
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetTexId(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    return ctx != NULL ? (jint)ctx->oesTexture : 0;
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeWaitBeginFrame(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return FRAME_EXIT;
    }

    pollEvents(ctx);

    if (ctx->exitRequested) {
        return FRAME_EXIT;
    }

    if (!ctx->sessionRunning) {
        // Never gives up waiting for the runtime (a slow boundary prompt included); just logs.
        int64_t now = nowNs();
        if (ctx->waitingSinceNs == 0) {
            ctx->waitingSinceNs = now;
            ctx->waitingLoggedNs = now;
        }
        else if ((now - ctx->waitingLoggedNs) / 1000000L >= 10000L) {
            ctx->waitingLoggedNs = now;
            LOGEV("waiting for the headset: session state %d, not running for %ld s",
                  ctx->sessionState, (long)((now - ctx->waitingSinceNs) / 1000000000L));
        }
        usleep(10000);
        return FRAME_IDLE;
    }
    ctx->waitingSinceNs = 0;

    // Fail knob "exit" (debug builds): request the runtime to end a focused session.
    if (ctx->exitKnob && ctx->sessionState == XR_SESSION_STATE_FOCUSED) {
        int64_t now = nowNs();
        if (ctx->exitKnobAtNs == 0) {
            ctx->exitKnobAtNs = now + EXIT_KNOB_DELAY_NS;
        }
        else if (now >= ctx->exitKnobAtNs) {
            ctx->exitKnob = 0;
            LOGEV("exit knob: asking the runtime to end the session");
            checkXr(xrRequestExitSession(ctx->session), "xrRequestExitSession");
        }
    }

    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    if (!checkXr(xrWaitFrame(ctx->session, NULL, &frameState), "xrWaitFrame")) {
        runtimeEnded(ctx, "xrWaitFrame failed", lastFailedResult);
        return FRAME_EXIT;
    }
    if (!checkXr(xrBeginFrame(ctx->session, NULL), "xrBeginFrame")) {
        runtimeEnded(ctx, "xrBeginFrame failed", lastFailedResult);
        return FRAME_EXIT;
    }

    ctx->predictedDisplayTime = frameState.predictedDisplayTime;
    ctx->shouldRender = frameState.shouldRender;
    displayFrameBegun(ctx, &frameState);
    return FRAME_RENDER;
}

// Why the runtime ended the frame loop, or null if it did not.
JNIEXPORT jstring JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetExitReason(JNIEnv* env, jobject thiz,
                                                                jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->exitReason[0] == '\0') {
        return NULL;
    }
    return (*env)->NewStringUTF(env, ctx->exitReason);
}

// Presence for a removed headset: away once the session goes
// to stopping or idle after first focus, until focused again.
JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetPresence(JNIEnv* env, jobject thiz,
                                                              jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return PRESENCE_OTHER;
    }
    if (ctx->sessionState == XR_SESSION_STATE_FOCUSED) {
        return PRESENCE_FOCUSED;
    }
    if (ctx->everFocused && (ctx->sessionState == XR_SESSION_STATE_STOPPING
                             || ctx->sessionState == XR_SESSION_STATE_IDLE)) {
        return PRESENCE_AWAY;
    }
    return PRESENCE_OTHER;
}

// The last failed start, taken once: step, call, XrResult number
// and name, detail, runtime, extensions offered and requested,
// in the order XrStartFailure reads them. Null if none.
JNIEXPORT jobjectArray JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeTakeStartFailure(JNIEnv* env, jclass clazz) {
    struct StartReport* r = malloc(sizeof(struct StartReport));
    if (r == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&failedStartLock);
    int set = failedStartSet;
    if (set) {
        *r = failedStart;
        failedStartSet = 0;
    }
    pthread_mutex_unlock(&failedStartLock);
    if (!set) {
        free(r);
        return NULL;
    }

    char result[16];
    snprintf(result, sizeof(result), "%d", (int)r->result);
    const char* fields[] = {
        r->step, r->call, result, r->result != XR_SUCCESS ? xrResultName(r->result) : "",
        r->detail, r->runtime, r->offered, r->requested
    };
    const int count = (int)(sizeof(fields) / sizeof(fields[0]));
    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    jobjectArray out = stringClass != NULL
            ? (*env)->NewObjectArray(env, count, stringClass, NULL) : NULL;
    for (int i = 0; out != NULL && i < count; i++) {
        jstring s = (*env)->NewStringUTF(env, fields[i]);
        if (s == NULL) {
            out = NULL;
            break;
        }
        (*env)->SetObjectArrayElement(env, out, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    free(r);
    return out;
}

// Runtime name and version, or null before the instance is created.
JNIEXPORT jstring JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetRuntime(JNIEnv* env, jobject thiz,
                                                             jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->runtimeLabel[0] == '\0') {
        return NULL;
    }
    return (*env)->NewStringUTF(env, ctx->runtimeLabel);
}

// Whether curved screens are available; the panel greys out the curve row if not.
JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetCylinderSupported(JNIEnv* env, jobject thiz,
                                                                       jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    return (ctx != NULL && ctx->cylinderSupported) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeDestroy(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    destroyCtx(env, ctx);
    LOGI("OpenXR renderer destroyed");
}
