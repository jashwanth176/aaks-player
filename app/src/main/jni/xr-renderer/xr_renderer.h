
// OpenXR renderer for the video player. The decoder's OES texture is drawn
// into a swapchain shown on a quad or cylinder layer, so the compositor does
// the reprojection.

#ifndef XR_RENDERER_H
#define XR_RENDERER_H

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

#include <android/log.h>
#include <sys/system_properties.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// XR_META_boundary_visibility, missing from the
// pinned headers; values from the OpenXR registry.
#ifndef XR_META_boundary_visibility
#define XR_META_boundary_visibility 1
#define XR_META_BOUNDARY_VISIBILITY_EXTENSION_NAME "XR_META_boundary_visibility"
typedef enum XrBoundaryVisibilityMETA {
    XR_BOUNDARY_VISIBILITY_NOT_SUPPRESSED_META = 1,
    XR_BOUNDARY_VISIBILITY_SUPPRESSED_META = 2,
    XR_BOUNDARY_VISIBILITY_MAX_ENUM_META = 0x7FFFFFFF
} XrBoundaryVisibilityMETA;
#define XR_TYPE_SYSTEM_BOUNDARY_VISIBILITY_PROPERTIES_META ((XrStructureType)1000528000)
#define XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META ((XrStructureType)1000528001)
#define XR_BOUNDARY_VISIBILITY_SUPPRESSION_NOT_ALLOWED_META ((XrResult)1000528000)
typedef struct XrSystemBoundaryVisibilityPropertiesMETA {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    XrBool32 supportsBoundaryVisibility;
} XrSystemBoundaryVisibilityPropertiesMETA;
typedef struct XrEventDataBoundaryVisibilityChangedMETA {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    XrBoundaryVisibilityMETA boundaryVisibility;
} XrEventDataBoundaryVisibilityChangedMETA;
typedef XrResult (XRAPI_PTR *PFN_xrRequestBoundaryVisibilityMETA)(
        XrSession session, XrBoundaryVisibilityMETA boundaryVisibility);
#endif

#include "xr_math.h"
#include "xr_shared.h"
#include "xr_videocolour.h"
#include "xr_depthmap.h"
#include "xr_poke.h"
#include "xr_handmesh.h"
#include "xr_roommesh.h"
#include "xr_layout.h"
#include "xr_rate.h"
#include "xr_gate.h"
#include "xr_pinch.h"
#include "xr_palm.h"
#include "xr_notice.h"
#include "xr_glow.h"
#include "xr_grade.h"
#include "xr_controller.h"

#define TAG "aaks"

// Pseudo-priority for key events, understood only by
// xrLog: info in logcat, quiet level in the log file.
#define PRIO_EVENT 90

void xrLog(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGI(...) xrLog(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) xrLog(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) xrLog(ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOGEV(...) xrLog(PRIO_EVENT, __VA_ARGS__)

// int64_t: long is 32-bit on armeabi-v7a.
static inline int64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

#define STATS_LOG_INTERVAL_FRAMES 300

// Pico controller bindings; older headers may lack the name.
#ifndef XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME
#define XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME "XR_BD_controller_interaction"
#endif

// Beam and cursor art share one swapchain: beam on top, dot below.
#define PTR_TEX_W 64
#define PTR_BEAM_H 256
#define PTR_DOT_H 64
#define PTR_TEX_H (PTR_BEAM_H + PTR_DOT_H)

#define HAND_LEFT  0
#define HAND_RIGHT 1
#define HAND_COUNT 2
// Gaze is a third ray source after the two hands. Only hands carry buttons.
#define SRC_GAZE  HAND_COUNT
#define SRC_COUNT (HAND_COUNT + 1)

// Hysteresis for the analog trigger and grip.
#define PRESS_ON   0.65f
#define PRESS_OFF  0.35f
// Thumbstick scroll deadzone, and wheel clicks per second at full deflection.
#define SCROLL_DEADZONE 0.30f
// Held bar or grip: the stick reels the screen by this factor per second at full push
#define STICK_REEL_RATE 0.9f
#define SCROLL_CLICKS_PER_SEC 7.5f

// One euro filter on the hit point: low cutoff at rest
// hides hand tremor, rising with speed to avoid lag.
#define POINTER_MIN_CUTOFF 1.2f
#define POINTER_BETA 8.0f
// A gap this long (pointer left the screen or changed hands) resets the filter.
#define POINTER_RESET_NS 250000000L

// Filter for the aim pose; position and rotation share the tunables.
#define AIM_MIN_CUTOFF 1.0f
#define AIM_BETA 20.0f

// Seconds: the pointer appears after deliberate
// movement and hides once the controller is put down.
#define POINTER_WAKE_SEC 0.5f
#define POINTER_SLEEP_SEC 5.0f
// Hand pointer hides this long after the last pinch
// or panel hover; an open palm brings it back.
#define HAND_POINTER_SLEEP_SEC 4.0f
// Ray fade-out when the pointer sleeps.
#define POINTER_FADE_NS 400000000LL
// Wake thresholds, m/s and rad/s.
#define POINTER_MOVE_SPEED 0.06f
#define POINTER_TURN_SPEED 0.35f

#define VR_BUTTON_LEFT   0x1
#define VR_BUTTON_RIGHT  0x2
#define VR_BUTTON_MIDDLE 0x4

// Hover zones and handle sizes are in xr_layout.h.

#define GRAB_NONE   0
#define GRAB_MOVE   1
#define GRAB_RESIZE 2

// Colour scale slots: the splash, one per player UI panel, and the pointer.
#define FADE_SLOT_SPLASH 0
#define FADE_SLOT_UI 1
#define FADE_SLOT_POINTER (FADE_SLOT_UI + UI_PANEL_COUNT)
// Direct touch target for the picture, after the panels
#define POKE_SCREEN UI_PANEL_COUNT
// Frosted backdrops: the menu sheet and the playback controls, which sit over the picture
#define FROST_PANELS 2
#define FADE_SLOT_FROST (FADE_SLOT_POINTER + 1)
#define FADE_SLOTS (FADE_SLOT_FROST + FROST_PANELS)
// The picture is blurred into this, and each backdrop is a quarter of its panel's size
#define FROST_TEX_W 192
#define FROST_TEX_H 108
#define FROST_MASK_DIV 4
// Panel pixels at least this opaque (0..255) are backdrop
#define FROST_MASK_ALPHA 64

// Ray length when it hits nothing, metres.
#define FREE_BEAM_M 4.0f

// Ambilight: the frame is reduced to a tiny colour texture that fills
// a glow quad behind the screen (sizes and colour maths in xr_glow.h).

// Controller model and hand outline pass: one side-by-side image, a cell per eye.
// Only the part of the view that holds them is drawn, at the eye's own pixel density
// up to this side, with a cleared margin round it for the compositor's filter; and
// these clip planes
#define MODEL_EYES 2
#define MODEL_MAX_SIDE 1024
#define MODEL_PAD 4
#define MODEL_CELL (MODEL_MAX_SIDE + 2 * MODEL_PAD)
// A controller's reach from its grip, for the part of the view it takes
#define MODEL_REACH_M 0.16f
#define MODEL_NEAR_M 0.05f
#define MODEL_FAR_M 60.0f

// Max time the warp stays flat waiting for a fresh depth map after
// 3D is switched back on (a map normally arrives within ~55 ms).
#define STEREO_WAIT_NS 400000000L

// Triple buffered, so a slot the stage thread overwrites
// was handed to the frame loop a full rotation ago.
#define DEPTH_TEX_COUNT 3

// Rate limit for scene-cut log lines; the next line reports how many were skipped.
#define DEPTH_CUT_LOG_NS 2000000000L

// Generous: the stage thread has no per-frame deadline, and a
// short timeout would only trade a hang for a torn capture.
#define CAPTURE_FENCE_TIMEOUT_NS 500000000ull

// Pinned headers may predate the extension, values from the OpenXR registry
#ifndef XR_FB_composition_layer_settings
#define XR_FB_composition_layer_settings 1
#define XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME "XR_FB_composition_layer_settings"
#define XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB ((XrStructureType)1000204000)
typedef XrFlags64 XrCompositionLayerSettingsFlagsFB;
#define XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB ((XrCompositionLayerSettingsFlagsFB)0x00000001)
#define XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB ((XrCompositionLayerSettingsFlagsFB)0x00000002)
#define XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB ((XrCompositionLayerSettingsFlagsFB)0x00000004)
#define XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB ((XrCompositionLayerSettingsFlagsFB)0x00000008)
typedef struct XrCompositionLayerSettingsFB {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    XrCompositionLayerSettingsFlagsFB layerFlags;
} XrCompositionLayerSettingsFB;
#endif


// Set this property to a new value to dump one frame
// of warp inputs and outputs for offline work.
#define CAPTURE_PROP "debug.moonlight.capture"
#define CAPTURE_POLL_FRAMES 30

// Debug tuning knobs over setprop, read by debug builds
// only. Each is an integer percent of the real value.
#define PROP_UPSAMPLE "debug.moonlight.upsample"
#define PROP_UPSAMPLE_SIGMA "debug.moonlight.upsamplesigma"
#define PROP_DEPTH_SHARP "debug.moonlight.depthsharp"
#define PROP_PASSTHROUGH "debug.moonlight.passthrough"

#define PROP_OCCLUSION "debug.moonlight.occlusion"
#define PROP_SEPARATION "debug.moonlight.separation"
#define PROP_DISTANCE "debug.moonlight.distance"
#define PROP_SCREEN "debug.moonlight.screen"
#define PROP_CONVERGENCE "debug.moonlight.convergence"
#define PROP_DEPTH_GLOBAL "debug.moonlight.depthglobal"
#define PROP_DEPTH_LOCAL "debug.moonlight.depthlocal"
#define PROP_POINTER_CUTOFF "debug.moonlight.pointercutoff"
#define PROP_POINTER_BETA "debug.moonlight.pointerbeta"
#define PROP_AIM_CUTOFF "debug.moonlight.aimcutoff"
#define PROP_AIM_BETA "debug.moonlight.aimbeta"
#define PROP_BEAM_WIDTH "debug.moonlight.beamwidth"
#define PROP_POINTER_WAKE "debug.moonlight.pointerwake"
#define PROP_POINTER_SLEEP "debug.moonlight.pointersleep"
#define PROP_SHARPEN "debug.moonlight.sharpen"
#define PROP_SUPERSAMPLE "debug.moonlight.supersample"
#define PROP_AMBI_SMOOTH "debug.moonlight.ambismooth"
// 1 normalises the glow to a steady luma with a dark edge roll-off, 0 uses the raw sample.
#define PROP_GLOW_NORM "debug.moonlight.glownorm"
// Milliseconds: time constants of the per-texel depth
// average and of the normalisation range. 0 disables.
#define PROP_DEPTH_TAU "debug.moonlight.depth_tau"
#define PROP_RANGE_TAU "debug.moonlight.range_tau"
// Scene cut detector: 0 off, 1 on (default), 2 on with a log line per capture.
#define PROP_DEPTH_CUT "debug.moonlight.depth_cut"
// Warp edge fixes: clamp the shifted sample inside the frame (0/1), side
// edge fade width (px, 0 off), cubic depth read (0/1), one-texel inset
// from the eye seam (0/1). All on by default except the cubic read.
#define PROP_SRC_INSET "debug.moonlight.srcinset"
#define PROP_EDGE_FADE "debug.moonlight.edgefade"
#define PROP_DEPTH_CUBIC "debug.moonlight.depthcubic"
#define PROP_SEAM_INSET "debug.moonlight.seaminset"
// Force the display rate in Hz, 0 for automatic. Read at start and live.
#define PROP_REFRESH "debug.moonlight.refresh"
// Depth maps per second, overriding the setting
// (resets the governor); 0 uses the setting. Live.
#define PROP_DEPTH_RATE "debug.moonlight.depthrate"
// CPU/GPU levels: 0 none, 1 sustained high (default), 2 boost. A level
// once set cannot be withdrawn, so 0 only takes effect next session.
#define PROP_PERF_LEVEL "debug.moonlight.perflevel"
// Panel fade in ms, 0 for the default 150; the splash takes twice as long.
#define PROP_FADE_MS "debug.moonlight.fadems"
#define FADE_KNOB_MAX_MS 10000
// Force a start step to fail (instance, system or session) to test the failure
// report. "exit" instead requests session exit EXIT_KNOB_DELAY_NS after first focus.
#define PROP_START_FAIL "debug.moonlight.xrfail"
#define EXIT_KNOB_DELAY_NS 5000000000LL

// Performance level requests.
#define PERF_LEVEL_NONE 0
#define PERF_LEVEL_SUSTAINED_HIGH 1
#define PERF_LEVEL_BOOST 2

// Re-ask for the rate this long after the runtime moves off it, at most RATE_REASK_MAX
// times a session; RATE_CONFIRM_MS is how long a request may take to land.
#define RATE_REASK_MS 10000
#define RATE_REASK_MAX 3
#define RATE_CONFIRM_MS 5000
// Frame budget windows between summary log lines.
#define RATE_LOG_WINDOWS 5

// Width of the side-edge disparity fade in px, and the property's maximum.
#define EDGE_FADE_PX 8
#define EDGE_FADE_MAX_PX 64
// Per-eye rect inset on the side-by-side axis, texels.
#define SEAM_INSET_TEXELS 1

// Low-pass radius splitting the depth map into global shape and
// local detail: about a tenth of a 256 map, scaled per axis.
#define DEPTH_LOWPASS_RADIUS 11

typedef struct {
    JavaVM* vm;
    jobject activity;

    EGLDisplay eglDisplay;
    EGLConfig eglConfig;
    EGLContext eglContext;
    EGLSurface eglPbuffer;

    XrInstance instance;
    // For the log; empty until the instance answers.
    char runtimeLabel[XR_MAX_RUNTIME_NAME_SIZE + 32];
    // Start progress, kept until init ends to report where it failed.
    struct StartReport* start;
    XrSystemId systemId;
    XrSession session;
    XrSpace localSpace;
    XrSpace viewSpace;

    XrSwapchain swapchain;
    uint32_t swapchainImageCount;
    XrSwapchainImageOpenGLESKHR* swapchainImages;
    int64_t swapchainFormat;
    // Some runtimes cap the swapchain count (Pico 4 Ultra at 32).
    int swapchainsAlive;

    // Compositor sharpening on the screen layers: 0 off, 1 normal, 2 quality.
    int sharpenMode;
    // Same values, for compositor supersampling.
    int supersampleMode;
    // Last logged pair + 1; 0 means not yet logged.
    int layerFlagsLogged;

    int videoWidth;
    int videoHeight;

    // When not OFF the swapchain is double width with a warped copy per eye.
    int stereoMode;
    int depthDebug;
    // Session 3D switch. Off, the frame is drawn flat into the left
    // half and shown to both eyes, and Java stops feeding the model.
    int stereoLive;
    // 3D just switched back on: stays flat until a map
    // newer than the switch lands or STEREO_WAIT_NS passes.
    int stereoWaiting;
    int stereoWaitIndex;
    int64_t stereoWaitNs;
    // Redraw without a new decoder frame, so the 3D switch shows on a paused picture.
    int warpRedraw;
    // Eyes in the last draw (both halves, or the left
    // shown to both); the video layers follow it.
    int drawnEyes;
    // Model input size. Set by nativeInit before GL init.
    int depthTexW;
    int depthTexH;
    // The frame loop reads one slot while the stage thread rotates
    // through the others. depthReadIndex is frame loop only.
    GLuint depthTextures[DEPTH_TEX_COUNT];
    int depthReadIndex;
    // Stage thread only.
    int depthWriteIndex;
    // Stage thread publishes with release after the slot's fence; the frame loop acquires.
    atomic_int depthStagedIndex;
    // Set after each upload, cleared by the first frame loop sampler.
    GLsync depthFences[DEPTH_TEX_COUNT];

    // Shared context for the depth model thread; inference outlasts a display frame.
    EGLContext depthContext;
    EGLSurface depthPbuffer;
    // Shared context for the stage thread: readback to model input, and map uploads.
    EGLContext depthStageContext;
    EGLSurface depthStagePbuffer;

    // Depth staging: the frame is downscaled on the GPU, read back, run
    // through the model in Java, and uploaded as the depth texture.
    GLuint downscaleProgram;
    GLint downscaleTexMatrixUniform;
    GLuint downscaleTexture;
    GLuint downscaleFbo;
    // Per pair: the frame loop queues a readback into the PBO
    // with a fence, and the stage thread maps it into the
    // model input. Java hands out pairs, one owner at a time.
    GLuint depthPbos[DEPTH_PAIRS];
    GLsync captureFences[DEPTH_PAIRS];
    float* modelInput[DEPTH_PAIRS];
    float* modelOutput[DEPTH_PAIRS];
    // For frame capture.
    atomic_int depthLastPair;
    unsigned char* depthUploadBuf;

    // Normalised map (0..1) and its range, smoothed over time so outlier pixels
    // do not pump the image. depthRange and rangeNs are stage thread only.
    float* depthNorm;
    float* depthLow;
    float* depthScratch;
    float* depthColSums;
    float depthGlobal;
    float depthLocal;
    DepthRange depthRange;
    int64_t rangeNs;
    // Per-texel temporal average of the model output, before normalisation. Stage thread.
    float* depthTau;
    int depthTauValid;
    int64_t depthTauNs;
    // Milliseconds, 0 for none.
    int depthTauMs;
    int rangeTauMs;
    // Scene cuts, detected on the model input: a jump resets the per-
    // texel average, a confirmed cut also the range. Stage thread,
    // except depthCutLevel (0 off, 1 on, 2 log every capture).
    int depthCutLevel;
    DepthCut depthCut;
    DepthResets depthResets;
    long depthCutChecks;
    int64_t depthCutLogNs;
    int depthCutUnlogged;

    // Edge aware upsample of the depth map, quarter of the video size
    GLuint upsampleProgram;
    GLint upsampleTexMatrixUniform;
    GLint upsampleSigmaUniform;
    GLint upsampleSharpUniform;
    float depthSharp;
    GLuint upsampleTexture;
    GLuint upsampleFbo;
    int upsampleWidth;
    int upsampleHeight;
    int upsampleEnabled;
    float upsampleSigmaR;

    // Occlusion aware offset map, both eyes packed into rg
    GLuint offsetProgram;
    GLint offsetDispUniform;
    GLint offsetConvUniform;
    GLuint offsetTexture;
    GLuint offsetFbo;
    int occlusionEnabled;
    // Edge fixes (see PROP_SRC_INSET): px for the fade, 0/1 for the rest.
    int srcInsetOn;
    int edgeFadePx;
    int depthCubic;
    int seamInset;
    float convergence;
    float separationOverride;
    float distanceOverride;
    float screenOverride;

    // Ambilight sample, independent of the depth passes.
    GLuint ambiProgram;
    GLint ambiTexMatrixUniform;
    GLint ambiCropUniform;
    GLint ambiGradeOnUniform;
    GLint ambiGradeUniform;
    GLuint ambiTexture;
    GLuint ambiFbo;
    // First frame has nothing to smooth against.
    int ambiSeeded;
    float ambiSmooth;
    // Part of the frame sampled: x0, y0, w, h in quad uv
    float ambiCrop[4];

    GLuint glowProgram;
    GLint glowIntensityUniform;
    GLint glowBlurUniform;
    GLint glowScaleUniform;
    // How much of the room the glow covers, and its light's reach, from the immersion
    GLint glowRoomUniform;
    GLint glowFrameUniform;
    GLint glowFrameLookUniform;
    GLuint glowFbo;
    // Luma-normalised copy the glow draws from; the room light reads the raw sample.
    GLuint glowEdgeProgram;
    GLuint glowEdgeTexture;
    GLuint glowEdgeFbo;
    // 1 draws from the normalised copy, 0 from the raw sample.
    int glowNorm;
    XrSwapchain glowSwapchain;
    uint32_t glowImageCount;
    XrSwapchainImageOpenGLESKHR* glowImages;
    int glowRendered;
    // Last drawn with, so changes redraw a paused picture.
    int glowDrawnOn;

    // Picture grade in preference units (PICTURE_ order).
    // gradeOn is 0 at the defaults so the shaders skip it.
    int pictureUnits[PICTURE_VALUES];
    int gradeOn;
    PictureGrade grade;

    // Per-eye sizes from the runtime, read at startup; used by the controller model.
    int recommendedEyeWidth;
    int recommendedEyeHeight;
    int maxEyeWidth;
    int maxEyeHeight;

    GLuint oesTexture;
    GLuint program;
    GLint texMatrixUniform;
    GLint disparityUniform;
    GLint tintUniform;
    GLint barTestUniform;
    GLint occlusionUniform;
    GLint eyeIndexUniform;
    GLint convergenceUniform;
    GLint dispTexelsUniform;
    GLint lowResWidthUniform;
    GLint frameWidthUniform;
    GLint srcInsetUniform;
    GLint frameUniform;
    GLint frameLookUniform;
    GLint edgeFadeUniform;
    GLint srcRectUniform;
    GLint depthCubicUniform;
    GLint gradeOnUniform;
    GLint gradeUniform;
    GLuint fbo;
    int barTestFramesLogged;

    // Frame capture for offline shader work
    char captureDir[256];
    char captureTag[PROP_VALUE_MAX];
    char lastCaptureTag[PROP_VALUE_MAX];
    int captureRequested;
    long capturePollCounter;

    XrSessionState sessionState;
    int sessionRunning;
    int exitRequested;
    // Why the runtime ended the loop, passed to the activity.
    char exitReason[96];
    // Fail knob "exit": request session end once, at exitKnobAtNs.
    int exitKnob;
    int64_t exitKnobAtNs;
    // everFocused tells Java the launch completed; the times drive the still-waiting log line.
    int everFocused;
    int64_t waitingSinceNs;
    int64_t waitingLoggedNs;
    // For the log, which should show one of each per headset removal.
    int sessionBegins;
    int sessionEnds;
    XrTime predictedDisplayTime;
    int shouldRender;
    int everRendered;
    // Depth model GL set up, so any film can switch to 2D-to-3D
    int depthGlReady;
    // The video swapchain holds a frame of the current film; when the wait for it began
    int videoReady;
    int64_t videoWaitNs;
    // The latched frame is the last film's, so it is never redrawn onto the new screen
    int videoStale;
    // Back button beside the bar: whether Java wants it, whether the ray is on it, and
    // when the bar or the button was last hovered (it stays a moment so the ray can cross)
    int backWanted;
    int backHover;
    // Frames submitted while focused; passthrough waits for the first (see nativeEndFrame).
    int focusedFrames;
    int passthroughBlendSaid;

    int cylinderSupported;
    // Last logged values, to log changes only.
    float dotSizeSaid;
    int cylinderClampSaid;
    float barSaidWidth;
    Vec3 barSaidEdge;
    int layerSettingsSupported;
    // XR_KHR_composition_layer_color_scale_bias, used to fade layers.
    // Without it panels and splash appear and disappear instantly.
    int colorScaleSupported;
    // The splash takes twice as long.
    int64_t fadeNs;
    int fadeKnobMs;
    // panelFadingOut keeps the bar hidden until a fading panel has gone.

    // Launch splash, up from the first frame until panels and depth model are
    // ready. splashReadyMs: ms from the first frame each was ready, -1 if not yet.
    Splash splash;
    int splashReadyMs[SPLASH_PARTS];
    XrSwapchain splashSwapchain;
    uint32_t splashImageCount;
    XrSwapchainImageOpenGLESKHR* splashImages;
    int splashArtReady;
    // -1 before the first frame.
    int splashWedgesShown;
    // Last panel art reached the frame loop, uploaded or not.
    // The splash stops waiting on depth when a map is published or the model gives up.
    atomic_int depthMapsStaged;
    atomic_int depthGaveUp;

    // Display refresh rate, matched to the video (runtimes keep their
    // start rate unless asked) and stepped down while the 3D warp cannot
    // keep up. Frame loop only, except the two rates the stats read.
    int refreshRateSupported;
    PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumerateDisplayRefreshRates;
    PFN_xrGetDisplayRefreshRateFB pfnGetDisplayRefreshRate;
    PFN_xrRequestDisplayRefreshRateFB pfnRequestDisplayRefreshRate;
    float displayRates[RATE_MAX];
    int displayRateCount;
    // From the preferences.
    int streamFps;
    // 0 until known.
    float rateAsked;
    float displayRate;
    int64_t rateAskedNs;
    int rateConfirmed;
    // The first rate has landed; later changes show a toast.
    int rateSettled;
    // Rate the warp stepped down to, 0 if none; kept for the session.
    float warpRateHeld;
    int rateWarpOn;
    // Floor rate last logged by the budget.
    float rateFloorSaid;
    // When the runtime moved off the requested rate (0 if not), and re-asks this session.
    int64_t rateMovedNs;
    int rateReasks;
    // debug.moonlight.refresh, 0 for automatic
    int refreshKnob;
    RateBudget rateBudget;
    int rateLogWindows;
    float rateLogFrameMs;
    float rateLogWorstMs;
    long rateLogMissed;
    // Frame loop timing for the budget. displayPeriodNs also gives
    // the rate on runtimes without the refresh rate extension.
    int64_t frameBeganNs;
    XrTime lastDisplayTime;
    int64_t displayPeriodNs;

    // Depth maps per second: the governor cuts it before the display
    // rate when the budget is missed or the runtime throttles; the gate
    // times captures. Frame loop only, except the target the stats read.
    DepthGovernor depthGov;
    DepthGate depthGate;
    // Preference, and debug.moonlight.depthrate (0 uses the preference).
    int depthRateSetting;
    int depthRateKnob;
    // Last notice level per CPU/GPU sub-domain.
    int perfNoticeLevels[2][3];

    // XR_EXT_performance_settings. A sustained level
    // keeps clocks steady for decode, warp and depth.
    int perfSettingsSupported;
    PFN_xrPerfSettingsSetPerformanceLevelEXT pfnPerfSettingsSetPerformanceLevel;
    int perfLevel;
    // Runtime's max composition layers per frame, and a warn-once flag.
    int maxLayerCount;
    int layerLimitWarned;
    // Warn once if a frame overflows the layer array.
    int layerDropWarned;

    int srgbWriteControl;
    // GL_EXT_YUV_target: raw YUV reads, needed for exact HDR colour.
    int yuvTargetSupported;
    // Colour the video shaders are built for. The playback thread writes the wanted
    // values then bumps videoColourAsked; the GL thread rebuilds when it moves.
    VideoColour videoColourBuilt;
    int videoTransferWanted;
    float videoPeakWanted;
    atomic_int videoColourAsked;
    int videoColourSeen;
    // XR_FB_color_space: tells the compositor the content's
    // primaries. colourModeSet is a COLOUR_MODE_ value, -1 until set.
    int colorSpaceSupported;
    PFN_xrEnumerateColorSpacesFB pfnEnumerateColorSpaces;
    PFN_xrSetColorSpaceFB pfnSetColorSpace;
    int colorSpaceP3Offered;
    int colorSpaceRec2020Offered;
    atomic_int colourModeWanted;
    int colourModeSet;
    // XR_META_boundary_visibility: suppressed while passthrough
    // shows the room, restored when the view is dark. Last
    // request, whether granted, when, and the last logged result.
    int boundarySupported;
    // XR_META_passthrough_preferences: whether the headset starts apps in passthrough.
    int passthroughPrefsSupported;
    PFN_xrGetPassthroughPreferencesMETA pfnGetPassthroughPreferences;
    PFN_xrRequestBoundaryVisibilityMETA pfnRequestBoundaryVisibility;
    int boundaryAsked;
    int boundaryGranted;
    int64_t boundaryAskedNs;
    XrResult boundaryLogged;
    int boundaryLoggedAsk;
    // Passthrough via the alpha-blend environment mode. Meta
    // enables the cameras only if the manifest requests it.
    int alphaBlendSupported;
    int passthrough;
    // Room light (xr_relight.h): XR_FB_passthrough as a
    // layer so its colour can be scaled to the picture.
    int fbPassthroughSupported;
    // 0 not created, 1 running, -1 refused (falls back to the blend mode).
    int fbPassthroughState;
    XrPassthroughFB fbPassthrough;
    XrPassthroughLayerFB fbPassthroughLayer;
    PFN_xrCreatePassthroughFB pfnCreatePassthrough;
    PFN_xrDestroyPassthroughFB pfnDestroyPassthrough;
    PFN_xrCreatePassthroughLayerFB pfnCreatePassthroughLayer;
    PFN_xrDestroyPassthroughLayerFB pfnDestroyPassthroughLayer;
    PFN_xrPassthroughLayerSetStyleFB pfnPassthroughLayerSetStyle;
    PFN_xrPassthroughStartFB pfnPassthroughStart;
    PFN_xrPassthroughPauseFB pfnPassthroughPause;
    // The cameras are paused once the room has been dark a while. Live: the layer
    // was wanted last frame; idle since; and held dark until the cameras are back.
    int fbPassthroughPaused;
    int relightLive;
    int64_t relightIdleNs;
    int64_t relightWarmNs;
    // 0..1, from the player.
    float relightBrightness;
    // House lights: 0 film lighting, 1 lights up. Target from the player, eased.
    float houseTarget;
    float houseLevel;
    // 0..1.
    float relightAmbientTarget;
    // Eased; the glow follows it.
    float relightAmbient;
    // Picture's average colour, read back a frame late through two PBOs so nothing stalls.
    GLuint relightFbo;
    GLuint relightPbo[2];
    int relightPboFull[2];
    int relightPboSlot;
    float relightAvg[3];
    int relightAvgValid;
    // Room colour multiplier, eased per frame, and
    // the opacity last set on the passthrough style.
    float relightScale[3];
    float relightOpacitySet;
    int64_t relightStepNs;
    // Colour samples still to take once passthrough is back, the picture paused or not
    int relightPrime;
    // Fraction of its width each eye of a stereo film shifts outward.
    float playerCrop;
    // No film: screen hidden, panels still drawn.
    int playerScreenHidden;
    // Black bar share of the height at top and bottom: target and eased current.
    float contentTarget[2];
    float contentCur[2];
    // Last drawn with, so changes redraw a paused picture.
    float glowDrawnAmbient;
    float glowDrawnBrightness;
    // Hand tracking is another interaction profile; the pointer treats it like a controller.
    int handsEnabled;
    int handInteraction;
    int msftHandInteraction;
    XrPath handProfile;
    XrPath msftHandProfile;
    int handTracking;
    int handClickOk;
    // Which hand profiles bound a pinch value, and the hands they
    // are active on. Where bound, that value is the hand's pinch.
    int extHandClick;
    int onExtHands[HAND_COUNT];
    int msftHandClick;
    int onMsftHands[HAND_COUNT];
    // Eye gaze aiming. While gazing, tracked hands only
    // pinch; a controller in use takes over pointing.
    int eyeGaze;
    int gazeEnabled;
    XrAction gazeAction;
    int lastSnapshot;
    // XR_EXT_hand_tracking joints, for runtimes that do not expose pinch as an input.
    int jointTracking;
    XrHandTrackerEXT handTrackers[HAND_COUNT];
    // XR_FB_hand_tracking_aim: offered, in use, and the per-hand pinch flag this frame.
    int handAimOffered;
    int handAim;
    int aimPinch[HAND_COUNT];
    // Thumb-index tip gap, and whether the tips were located.
    float tipGap[HAND_COUNT];
    int tipsValid[HAND_COUNT];
    // Raw pinch per hand, so release is judged against the source's
    // own off threshold. pinchSrcSaid: sources already logged.
    int pinchDown[HAND_COUNT];
    int pinchSrc[HAND_COUNT];
    unsigned pinchSrcSaid;
    // Pinch position, which a gaze-started drag follows.
    Vec3 pinchPoint[HAND_COUNT];
    // Index fingertip and its radius, for direct touch; touch state per hand, whose
    // targets are the panels and POKE_SCREEN.
    XrVector3f indexTip[HAND_COUNT];
    float indexTipRadius[HAND_COUNT];
    int indexTipValid[HAND_COUNT];
    Poke poke[HAND_COUNT];
    int pokeSaid[HAND_COUNT];
    // Fingers at the white bar: the ray aimed into it from just in front, fixed to the
    // pinch point while held so the window moves with the hand; and whether the move
    // grab running started that way
    int nearBar[HAND_COUNT];
    XrQuaternionf nearRayTurn[HAND_COUNT];
    Vec3 nearOffset[HAND_COUNT];
    int grabNear;
    // The pinch point last seen while holding the bar, kept through a short loss
    Vec3 nearLast[HAND_COUNT];
    int64_t nearLastNs[HAND_COUNT];
    // What each hand's pinch holds with (HOLD_), and a pinch made while touching, which
    // is ignored until let go
    int pinchHold[HAND_COUNT];
    int pinchIgnored[HAND_COUNT];
    // The source that pointed last frame, and whether its press began on a menu
    int pointHand;
    int uiPress[SRC_COUNT];
    // Whether each source's press went down with its ray on something; a hand resting
    // curled, aimed at nothing, then holds no press that could keep the pointer
    int pressOwned[SRC_COUNT];
    int pinchPointValid[HAND_COUNT];
    // Ray built from the joints, for runtimes without a pointer pose.
    XrPosef handRay[HAND_COUNT];
    int handRayValid[HAND_COUNT];
    PFN_xrCreateHandTrackerEXT pfnCreateHandTracker;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTracker;
    PFN_xrLocateHandJointsEXT pfnLocateHandJoints;
    int usingHands[SRC_COUNT];
    // PROFILE_* on each hand path. Only a controller takes pointing
    // from the eyes; a hand that lost tracking reports no profile.
    int profileKind[HAND_COUNT];
    // Controller aim fully tracked and active, read once a frame.
    int aimTracked[HAND_COUNT];
    // Per-controller rest clock; the shared clock below covers the other hand and gaze.
    ControllerClock aimClock[HAND_COUNT];
    // Settled at frame start so every reader agrees who points.
    int controllerAwake;
    // Hands pointing while gaze is lost.
    GazeBridge gazeBridge;
    // A pinch that woke the pointer is not a click; swallowed until the hand opens.
    int pinchSwallowed[SRC_COUNT];

    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGlesReqs;

    // Controller input.
    XrActionSet actionSet;
    XrAction aimAction;
    XrAction triggerAction;
    XrAction rightClickAction;
    XrAction middleClickAction;
    XrAction scrollAction;
    XrAction grabAction;
    XrAction toggleAction;
    // Used in place of aim, trigger and grab while a hand is on a hand profile.
    XrAction handAimAction;
    XrAction handPinchAction;
    XrAction handGraspAction;
    XrSpace aimSpaces[SRC_COUNT];
    // Null where unavailable; those hands use the joint-built ray.
    XrSpace handAimSpaces[HAND_COUNT];
    XrPath handPaths[HAND_COUNT];
    int inputReady;
    int picoInteraction;
    // Per-session toggle on top of the preference.
    int pointerOn;
    int togglePrev;
    int triggerDown[SRC_COUNT];
    // Rising edges, so a button already held when the ray reaches a handle does not grab it.
    int triggerEdge[SRC_COUNT];
    // A press consumed by a panel or the keyboard, held until release
    // so it does not click what was underneath once the panel closes.
    int triggerSwallowed[SRC_COUNT];
    // Click-path diagnostics only: analog value, and per-press minimum and
    // dip count, to tell whether fast double taps merged at the hysteresis.
    float triggerValue[HAND_COUNT];
    float triggerHoldMin[HAND_COUNT];
    int triggerDipFrames[HAND_COUNT];
    // Dips back over the press threshold without reaching release: lost taps.
    int triggerRetaps[HAND_COUNT];
    int triggerDipping[HAND_COUNT];
    int trigLogDown[HAND_COUNT];
    int trigLogRaw;
    int trigLogLeft;
    // Indexed by source; the gaze slot is never written.
    int gripEdge[SRC_COUNT];
    int grabByTrigger;
    int buttonsDown;
    float scrollCarry;
    int64_t lastInputNs;

    // One euro filter state for the hit point, per axis
    EuroState filterU;
    EuroState filterV;
    float pointerMinCutoff;
    float pointerBeta;
    int64_t lastHitNs;
    int lastHand;

    // One euro filter state for the aim pose, per hand
    EuroState aimFilterPos[HAND_COUNT][3];
    EuroQuatState aimFilterRot[HAND_COUNT];
    float aimMinCutoff;
    float aimBeta;

    // Share of joints tracked per hand. A runtime that
    // never reports "tracked" is judged by "valid" instead.
    float handTrust[HAND_COUNT];
    int jointsReportTracked;
    int aimReportsTracked[HAND_COUNT];
    // Last well-tracked aim, held briefly across tracking gaps.
    XrPosef lastGoodAim[HAND_COUNT];
    int lastGoodValid[HAND_COUNT];
    int64_t lastGoodNs[HAND_COUNT];
    // The last time a pinch was held with the hand well tracked
    int64_t pinchGoodNs[HAND_COUNT];
    int trustSaid[HAND_COUNT];

    // Movement gate, see POINTER_WAKE_SEC.
    int poseSeen[SRC_COUNT];
    XrPosef lastAim[SRC_COUNT];
    float movingFor;
    float stillFor;
    int pointerAwake;
    float pointerWake;
    float pointerSleep;
    // Whether a still controller's pointer sleeps; also set from the Display tab.
    int pointerSleepOn;
    // Time since a hand last used the pointer, and per-hand open-palm detection.
    float handIdleFor;
    int palmShown[HAND_COUNT];
    PalmHold palmHold[HAND_COUNT];
    // Hand pointer auto-hide setting. Set from Java on any thread.
    atomic_int handAutoHide;
    // Last drawn ray, faded out in place when the pointer sleeps.
    int rayWasDrawn;
    int rayFading;
    XrTime rayFadeFrom;
    Vec3 rayFadeStart;
    Vec3 rayFadeEnd;
    int rayFadeFree;

    // Laser: two small quad layers placed by the compositor; nothing drawn per frame.
    XrSwapchain pointerSwapchain;
    uint32_t pointerImageCount;
    XrSwapchainImageOpenGLESKHR* pointerImages;
    int pointerArtReady;
    int beamVisible;
    // Ray with no hit, so no cursor.
    int beamFree;
    // Gaze aim: cursor but no ray.
    int beamGaze;
    // Ray switch: session start setting, and whether the bar button
    // or Display tab flipped it. Off hides the beam unless a panel
    // is up; dot and hit test remain. Applies to hand rays too.
    int raySetting;
    int rayFlipped;
    // Beam drawn last frame, -1 before the first; for the log.
    int rayDrawnSaid;
    // The head-locked preference, from the last input pass
    int headLockedPref;
    // A restored placement waits for the head to be tracked, then turns to face it
    int placeAtHead;
    // Bundled controller model at each grip, in its own projection layer.
    // modelOn is also set from the Display tab; buffers arrive from Java.
    int modelOn;
    int modelReady;
    GLuint modelVertexBuffer;
    GLuint modelIndexBuffer;
    int modelIndexCount;
    // Model pass: side-by-side swapchain cleared around the
    // models, depth buffer so the hands occlude each other, and
    // the views last drawn from. Not retried after a failure.
    GLuint modelProgram;
    GLint modelViewProjUniform;
    GLint modelMatrixUniform;
    XrSwapchain modelSwapchain;
    uint32_t modelImageCount;
    XrSwapchainImageOpenGLESKHR* modelImages;
    GLuint modelFbo;
    GLuint modelDepthBuffer;
    int modelPassReady;
    int modelPassFailed;
    XrView modelViews[MODEL_EYES];
    // The part of each eye's view last drawn, and whether that eye has any of it
    ViewBox modelBox[MODEL_EYES];
    int modelBoxOn[MODEL_EYES];
    int modelRendered;
    // Own timer query pair
    GLuint modelTimerQueries[2];
    int modelTimerSlot;
    int modelTimerPending[2];
    int modelTimerPendingFrames[2];
    int64_t modelGpuTotalNs;
    long modelGpuSamples;
    long modelGpuDropped;
    // Per-hand grip pose where the model shows, any shown, and the last logged state.
    XrAction gripAction;
    XrSpace gripSpaces[HAND_COUNT];
    int modelShown[HAND_COUNT];
    XrPosef modelGrip[HAND_COUNT];
    int modelsShowing;
    // Hand outlines (xr_hands.c): each hand's mesh from XR_FB_hand_tracking_mesh, its
    // joints this frame in local space, and the pass drawn with the controller models.
    int handMeshOffered;
    PFN_xrGetHandMeshFB pfnGetHandMesh;
    struct {
        float* vertices;
        uint16_t* indices;
        int vertexCount;
        int indexCount;
        XrPosef bind[XR_HAND_JOINT_COUNT_EXT];
        // How far the mesh reaches from each joint
        float reach[XR_HAND_JOINT_COUNT_EXT];
        GLuint vbo;
        GLuint ibo;
    } handMesh[HAND_COUNT];
    XrPosef handJoints[HAND_COUNT][XR_HAND_JOINT_COUNT_EXT];
    int handJointsOk[HAND_COUNT];
    // The last good joints, drawn through short tracking drops, and the fade
    XrPosef handJointsLast[HAND_COUNT][XR_HAND_JOINT_COUNT_EXT];
    int handLastValid[HAND_COUNT];
    // The hand's size against the mesh's, from the runtime, and how far a pressing
    // fingertip has gone past its surface, to draw the hand stopped on it
    float handScale[HAND_COUNT];
    // Why a hand last stopped counting for pointing: 1 not active, 2 low confidence,
    // 3 not hand-sized
    int handLostWhy[HAND_COUNT];
    float handScaleSaid[HAND_COUNT];
    Vec3 pokePush[HAND_COUNT];
    float handLevel[HAND_COUNT];
    float handLostFor[HAND_COUNT];
    int64_t handStepNs;
    int handShown[HAND_COUNT];
    int handsShowing;
    int handsSaid;
    GLuint handProgram;
    GLint handViewProjUniform;
    GLint handJointsUniform;
    GLint handEyeUniform;
    GLint handLookUniform;
    int handPassFailed;
    int modelsSaid;
    // Actions already synced this frame by the input pass.
    int actionsSynced;
    XrVector3f beamStart;
    XrVector3f beamEnd;
    XrVector3f headPos;
    // Head turn rate, deg/s, and last orientation. A gaze-started drag holds while it is high.
    float headTurnRate;
    XrQuaternionf headTurnLast;
    int headTurnLastValid;
    // A gaze drag is held for a head turn; logged once.
    int dragHeldByHead;
    float beamWidth;

    // Seeded from the distance and width preferences, then owned by
    // the grab; moving a slider hands it back to the preferences.
    XrPosef screenPose;
    float screenWidth;
    float screenRadius;
    int placementValid;
    int sliderSeen;
    float lastDistance;
    float lastQuadWidth;

    int grabDown[SRC_COUNT];
    int grabMode;
    int grabHand;
    XrPosef grabAim;
    XrPosef grabScreen;
    float grabWidth;
    float grabHeight;
    float grabRadius;
    // Resize scales about the centre; this is the opposite
    // corner, and its signs identify the held one.
    float grabOppX, grabOppY;
    // Handles per xr_handles.h. A move bar pull scales the distance by the hand's
    // distance to the eyes relative to the grab; the width follows the distance.
    float grabHandDist;
    float grabScreenDist;
    // Distance factor from the stick while a move grab is held
    float grabReel;
    float grabSizeRatio;
    // Eased facing target for a carried screen, and its last step time.
    float playerYaw, playerPitch;
    int64_t playerTurnNs;
    // Player UI panels (xr_uipanels.c): swapchain sized by the first sheet
    // and recreated if that changes, anchor, wanted, and fade level.
    struct {
        XrSwapchain chain;
        XrSwapchainImageOpenGLESKHR* images;
        uint32_t imageCount;
        int texW, texH;
        int ready;
        int visible;
        int anchor;
        // Fixed: width from the texture at uiMetresPerPx, gap in pixels.
        // Otherwise both are shares of the screen width.
        int fixed;
        float widthFrac;
        float gapFrac;
        // Sideways from the screen's centre line, in the gap's units
        float xOff;
        // Fraction of the distance toward the viewer the panel floats off the screen,
        // or (auto) in front of the picture's nearest point.
        float depthK;
        int depthAuto;
        float level;
    } ui[UI_PANEL_COUNT];
    // Auto subtitle depth: the strip's pop-out from the latest map (stage thread), the
    // eye shift and stereo source of the latest draw, and the eased depth
    float subsPop;
    float subsSeparation;
    int subsNativeStereo;
    float subsDepthK;
    // The strip matcher for 3D files (xr_subsdepth.c): its disparity a frame late
    GLuint stripProgram;
    GLint stripTexMatrixUniform;
    GLint stripLeftUniform;
    GLint stripRightUniform;
    GLint stripStripUniform;
    GLint stripReachUniform;
    GLuint stripTexture;
    GLuint stripFbo;
    GLuint stripPbo[2];
    int stripPboFull[2];
    int stripPboSlot;
    int stripReady;
    float subsDisparity;
    // Frosted backdrops (xr_frost.c): the picture blurred, drawn behind the panels
    // that sit over it and masked to each panel's shape.
    GLuint frostProgram;
    GLint frostRectUniform;
    GLint frostTexelUniform;
    GLint frostFilmOnUniform;
    GLint frostLookUniform;
    GLuint frostTexture;
    GLuint frostFbo;
    GLuint frostDrawFbo;
    int frostReady;
    int frostFilmValid;
    // The blur is taken only while a backdrop shows: a film frame went by without
    // one, and how many have been taken
    int frostSkipped;
    unsigned frostSamples;
    struct {
        XrSwapchain chain;
        XrSwapchainImageOpenGLESKHR* images;
        uint32_t imageCount;
        GLuint mask;
        int w, h;
        int ready;
        // What the backdrop was last drawn from; redrawn only when that changes
        int drawn;
        float drawnRect[4];
        int drawnFilmOn;
        unsigned drawnSample;
    } frost[FROST_PANELS];
    // -1 for none.
    int uiHover[SRC_COUNT];
    int64_t uiStepNs;
    // Released past a limit, the screen springs back along the eye line.
    int springActive;
    int64_t springStartNs;
    float springFromDist, springToDist;
    float springFromWidth, springToWidth;
    // Gaze-started grab carried by a hand: hand start, travel gain, and ramp.
    int grabByGaze;
    Vec3 grabHandStart;
    float grabScale;
    DragRamp grabRamp;
    int poseDirty;

    // Hover state; picks which handle to draw.
    int hoverKind;
    int hoverCorner;
    XrSwapchain barSwapchain;
    XrSwapchain cornerSwapchain;
    uint32_t barImageCount;
    uint32_t cornerImageCount;
    XrSwapchainImageOpenGLESKHR* barImages;
    XrSwapchainImageOpenGLESKHR* cornerImages;
    int handleArtReady;

    // Curvature from the panel, -1 while the preference owns it. The
    // preference is kept too, for use outside the JNI entry points.
    float panelCurve;
    float prefCurvature;

    long statFrames;
    int64_t statTotalNs;
    int64_t statMaxNs;

    // GPU time of the warp passes from timer queries; CPU timing only measures submission.
    int timerSupported;
    GLuint timerQueries[2];
    int timerSlot;
    int timerPending[2];
    // Frames a query has been pending; a result that
    // never lands would otherwise wedge the pair.
    int timerPendingFrames[2];
    int64_t gpuTotalNs;
    int64_t gpuMaxNs;
    long gpuSamples;
    // Samples rejected as implausible, and the last rejected raw value, for the log.
    long gpuDropped;
    GLuint64 gpuLastDroppedNs;
} XrCtx;

typedef void (*PFNGENQUERIESEXT)(GLsizei, GLuint*);
typedef void (*PFNBEGINQUERYEXT)(GLenum, GLuint);
typedef void (*PFNENDQUERYEXT)(GLenum);
typedef void (*PFNGETQUERYOBJECTUIVEXT)(GLuint, GLenum, GLuint*);
typedef void (*PFNGETQUERYOBJECTUI64VEXT)(GLuint, GLenum, GLuint64*);
typedef void (*PFNDELETEQUERIESEXT)(GLsizei, const GLuint*);

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_QUERY_RESULT_EXT
#define GL_QUERY_RESULT_EXT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_EXT
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif

// xr_session.c: instance, session, swapchain and lifecycle
int checkXr(XrResult res, const char* what);
const char* xrResultName(XrResult res);

// xr_gl.c: GL setup, the warp passes and the GPU timer
extern const float VERTEX_DATA[16];
extern PFNGENQUERIESEXT pfnGenQueries;
extern PFNBEGINQUERYEXT pfnBeginQuery;
extern PFNENDQUERYEXT pfnEndQuery;
extern PFNGETQUERYOBJECTUIVEXT pfnGetQueryObjectuiv;
extern PFNGETQUERYOBJECTUI64VEXT pfnGetQueryObjectui64v;
extern PFNDELETEQUERIESEXT pfnDeleteQueries;
GLuint compileShader(GLenum type, const char* src);
void setGradeUniforms(XrCtx* ctx, GLint onUniform, GLint gradeUniform, int on);
int linkProgram(GLuint* out, const char* fragmentSrc, const char* what);
// linkProgram for a shader that reads the video, built for the video's colour
int linkVideoProgram(XrCtx* ctx, GLuint* out, const char* fragmentSrc, const char* what);
// Rebuilds the video shaders when the player has said the video's colour
// changed. GL thread, before the frame draws.
void applyVideoColour(XrCtx* ctx);
int initGl(XrCtx* ctx);
// After a new video size: the quarter-size depth targets follow it
void resizeUpsample(XrCtx* ctx);
void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation);

// xr_depth.c: the depth model staging and the stage thread's uploads
int initDepthModel(XrCtx* ctx);
int linkDownscaleProgram(XrCtx* ctx);
void waitForDepthSlot(XrCtx* ctx);

// xr_ambilight.c: the frame colour sample, letterbox detection and the glow
int initAmbilight(XrCtx* ctx);
int linkAmbiProgram(XrCtx* ctx);
void runFrameColorSample(XrCtx* ctx, const float* texMatrix);
void runGlowRender(XrCtx* ctx);
// The glow's inputs changed since it was last drawn.
int glowStale(XrCtx* ctx);
float glowScaleFor(XrCtx* ctx);
// The picture's height over its width with the bars cropped out, and the
// part of the frame an eye reads (offset and size in frame uv, bottom up)
float screenAspect(const XrCtx* ctx);
void contentRect(const XrCtx* ctx, int eye, float out[4]);
// The player's frame uniforms for one draw: crop and shift are 0 for the glow
void setFrameUniforms(XrCtx* ctx, GLint frame, GLint look, float crop, float shift);
void redrawGlow(XrCtx* ctx, const float* texMatrix);

// xr_model.c: the bundled controller model
void updateControllerModels(XrCtx* ctx);
void renderControllerModels(XrCtx* ctx);
// Sets an eye's viewport and view-projection for the pass; 0 when the eye has none
int modelEyeBegin(XrCtx* ctx, int eye, float* viewProj);
// Hand outlines (xr_hands.c)
void loadHandMeshes(XrCtx* ctx);
void updateHands(XrCtx* ctx);
void handsViewBox(XrCtx* ctx, XrPosef eye, ViewBox* box);
void drawHandOutlines(XrCtx* ctx);
void destroyHands(XrCtx* ctx);

// xr_input.c: actions, hands, the ray and the per frame input pass
int initXrInput(XrCtx* ctx);
void destroyXrInput(XrCtx* ctx);
void refreshInputSource(XrCtx* ctx);
int updatePlacement(XrCtx* ctx, float distance, float quadWidth, float curvature);
void recentreScreen(XrCtx* ctx);

// xr_ui.c: where the furniture and the panels sit, and what the ray is over
BarFrame barFrame(XrCtx* ctx);
float cornerSide(XrCtx* ctx);
float effectiveCurvature(XrCtx* ctx);
float screenPitch(XrCtx* ctx);
XrQuaternionf screenOrient(float yaw, float pitch, float roll);
// The local player's panels (xr_uipanels.c)
XrPosef uiPanelPose(XrCtx* ctx, int id, float* outWidth, float* outHeight);
// Frosted backdrops (xr_frost.c)
int initFrost(XrCtx* ctx);
int frostSlot(int id);
void runFrostSample(XrCtx* ctx, const float* texMatrix);
int frostWanted(const XrCtx* ctx);
void frostMaskFromPanel(XrCtx* ctx, int id, const unsigned char* px, int width, int height);
int renderFrostBackdrop(XrCtx* ctx, int slot, XrPosef pose, float width, float height);
void destroyFrost(XrCtx* ctx);
// Subtitle depth on 3D files (xr_subsdepth.c)
int initStripMatch(XrCtx* ctx);
void runStripMatch(XrCtx* ctx, const float* texMatrix);
void destroyStripMatch(XrCtx* ctx);
// Metres per pixel of a fixed-size panel
float uiMetresPerPx(const XrCtx* ctx);
int uiPanelAt(XrCtx* ctx, XrPosef aim, float* outU, float* outV);
void stepUiPanels(XrCtx* ctx, int64_t nowNs);
void destroyUiPanels(XrCtx* ctx);
// The player's room light, see xr_relight.h
int relightWanted(XrCtx* ctx);
void relightSample(XrCtx* ctx);
void relightStep(XrCtx* ctx, int64_t nowNs);
int relightLayerReady(XrCtx* ctx);
void relightDestroy(XrCtx* ctx);
void pictureSet(XrCtx* ctx, int row, int units);

// xr_assets.c: the swapchains the art goes into and the uploads that fill them
int createArtSwapchain(XrCtx* ctx, int width, int height, const char* what,
                       XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images,
                       uint32_t* count);
void destroyArtSwapchain(XrCtx* ctx, XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images);
// A sheet of RGBA rows running top down, flipped into a chain's images
int uploadFlipped(XrCtx* ctx, XrSwapchain chain, XrSwapchainImageOpenGLESKHR* images,
                  const unsigned char* px, int width, int height);
int createPointerSwapchain(XrCtx* ctx);
int uploadPointerArt(XrCtx* ctx);

// xr_display.c: the display refresh rate and the performance levels
void probeDisplayExtensions(XrCtx* ctx);
void startDisplay(XrCtx* ctx);
void startPerfLevels(XrCtx* ctx);
void setPerfLevel(XrCtx* ctx, int level);
void perfNotice(XrCtx* ctx, const XrEventDataPerfSettingsEXT* notice);
void displaySessionBegun(XrCtx* ctx);
// A new film's frame rate: picks the display rate again
void displayContentChanged(XrCtx* ctx);
// Compositor colour space: logged at start, set when the colour mode changes.
void startColorSpace(XrCtx* ctx);
void applyColourMode(XrCtx* ctx);
// Boundary: support checked at start, request updated every frame.
void startBoundary(XrCtx* ctx);
void applyBoundary(XrCtx* ctx);
void boundaryChanged(XrCtx* ctx, XrBoundaryVisibilityMETA visibility);
void displayFocused(XrCtx* ctx);
void displayRateChanged(XrCtx* ctx, float from, float to);
void displayFrameBegun(XrCtx* ctx, const XrFrameState* state);
void displayFrameEnded(XrCtx* ctx);
void setRefreshKnob(XrCtx* ctx, int hz);
void setDepthRate(XrCtx* ctx, int perSecond, const char* why);

// xr_debug.c: setprop knobs and frame capture
void readStartKnobs(XrCtx* ctx);
int startFailKnob(const char* step);
void propFlag(const char* name, int* target);
void pollCaptureRequest(XrCtx* ctx);
void writeCapture(XrCtx* ctx, const char* what, const void* data, size_t bytes);
void writeCaptureDepthTexture(XrCtx* ctx);

#endif
