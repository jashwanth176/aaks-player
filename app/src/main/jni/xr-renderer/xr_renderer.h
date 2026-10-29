
// OpenXR renderer for the video player. The decoder's OES texture is drawn
// into a swapchain shown on a quad or cylinder layer, so the compositor does
// the reprojection. The 3D room is drawn per eye into a projection layer.

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
#include "xr_roommesh.h"
#include "xr_layout.h"
#include "xr_rate.h"
#include "xr_gate.h"
#include "xr_pinch.h"
#include "xr_palm.h"
#include "xr_notice.h"
#include "xr_glow.h"
#include "xr_grade.h"
#include "xr_keys.h"
#include "xr_controller.h"
#include "xr_headaim.h"
#include "xr_gamepad.h"

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
#define HAND_POINTER_SLEEP_SEC 1.5f
// Ray fade-out when the pointer sleeps.
#define POINTER_FADE_NS 400000000LL
// Wake thresholds, m/s and rad/s.
#define POINTER_MOVE_SPEED 0.06f
#define POINTER_TURN_SPEED 0.35f

#define VR_BUTTON_LEFT   0x1
#define VR_BUTTON_RIGHT  0x2
#define VR_BUTTON_MIDDLE 0x4

// Resize limits, metres.
#define SCREEN_MIN_WIDTH 0.8f
#define SCREEN_MAX_WIDTH 8.0f

// Hover zones and handle sizes are in xr_layout.h.

#define GRAB_NONE   0
#define GRAB_MOVE   1
#define GRAB_RESIZE 2

// Sized for a five-column grid.
#define PICKER_WIDTH_FRAC 0.91f
#define OUTLINE_TEX 128

#define COG_WIDTH_FRAC 0.36f
#define COG_THUMB_TEX 64
// Panel face for the Room tab, which replaces the first
// tab while a room is up. Numbered past the real tabs.
#define COG_FACE_ROOM COG_TAB_COUNT

// Metres. Below the settings slider's 1 m floor so the screen can come right up to the face.
#define COG_DIST_MIN 0.2f
#define COG_DIST_MAX 8.0f
// Metres either side of eye level.
#define COG_HEIGHT_MIN -2.0f
#define COG_HEIGHT_MAX 2.0f
// Radians, 40 degrees each way
#define COG_TILT_MAX 0.6981f
// Same track fraction as the roll snap (TILT_MAX * ROLL_SNAP / ROLL_MAX).
#define COG_TILT_SNAP 0.0388f
// Radians, 90 degrees each way, for watching lying down.
#define COG_ROLL_MAX 1.5708f
// Within 5 degrees of level snaps to level.
#define COG_ROLL_SNAP 0.0873f

#define KB_WIDTH_FRAC 0.55f
#define KB_MAX_KEYS 64

#define EXIT_WIDTH_FRAC 0.30f

// Report sheet, centred over the bottom of the picture above the keyboard.
#define REPORT_WIDTH_FRAC 0.40f

// Hand-lock hint. Waits for a hand to point this long, so a hand
// that flickers in as a controller is put down does not open it.
#define HINT_WIDTH_FRAC 0.40f
#define HINT_POINTING_NS 500000000L

#define KOFI_WIDTH_FRAC 0.40f

// Fading panels, one Fade each.
#define FADE_COG 0
#define FADE_PICKER 1
#define FADE_KB 2
#define FADE_EXIT 3
#define FADE_REPORT 4
#define FADE_HINT 5
#define FADE_KOFI 6
#define FADE_PANELS 7
// Colour scale slots: the panels above, then splash and toast.
#define FADE_SLOT_SPLASH FADE_PANELS
#define FADE_SLOT_TOAST (FADE_PANELS + 1)
// One per player UI panel.
#define FADE_SLOT_UI (FADE_PANELS + 2)
#define FADE_SLOT_POINTER (FADE_SLOT_UI + UI_PANEL_COUNT)
#define FADE_SLOTS (FADE_SLOT_POINTER + 1)

// Toast placement relative to the eyes, metres.
#define TOAST_W_M 0.56f
#define TOAST_DISTANCE_M 0.80f
#define TOAST_DROP_M 0.14f

// Ray length when it hits nothing, metres.
#define FREE_BEAM_M 4.0f

// Head aim drops frames this long after a recentre,
// whatever the runtime says about when it lands.
#define HEAD_AIM_RECENTRE_HOLD_NS 100000000LL
// Interval of head aim's movement log line.
#define HEAD_AIM_COUNT_NS 10000000000L

// Ambilight: the frame is reduced to a tiny colour texture that fills
// a glow quad behind the screen (sizes and colour maths in xr_glow.h).

// Letterbox detection: a copy of the sample pass is read back periodically,
// black bars are counted per edge, and the glow's sample is cropped to the
// picture. Sample-pass frames between readbacks (4 KB each).
#define AMBI_BAR_PERIOD 30
// Mean max(r,g,b) below which a row or column is bar;
// covers unexpanded limited-range black (16/255).
#define AMBI_BAR_LUMA 0.09f
// Max texels (of 32) one edge may crop.
#define AMBI_BAR_MAX 10
// Ticks a larger bar must hold before the crop grows (slowly, so a dark
// scene is not taken for a bar), and a smaller one before it shrinks.
#define AMBI_BAR_APPLY 4
#define AMBI_BAR_RELEASE 2
// Edge order of the bar arrays, in the quad's uv space.
#define AMBI_EDGE_LEFT 0
#define AMBI_EDGE_RIGHT 1
#define AMBI_EDGE_BOTTOM 2
#define AMBI_EDGE_TOP 3
#define AMBI_EDGES 4

// 3D room styles: baked models from the assets, drawn per eye into
// the projection layer. 0 is none; each has a row in xr_room.c.
#define ROOM_STYLE_THEATER 1
#define ROOM_STYLE_GRAND_CINEMA 2
#define ROOM_STYLE_SYNTHWAVE 3
#define ROOM_STYLE_FIRST ROOM_STYLE_THEATER
#define ROOM_STYLE_LAST ROOM_STYLE_SYNTHWAVE
#define ROOM_EYES 2
// Per-eye room size by Environment Res tier. Low takes half the
// runtime's recommendation, standard and high cap the recommendation,
// and ultra (experimental) uses a fixed size regardless of it.
#define ROOM_MAX_EYE_FULL 2560     // high
#define ROOM_MAX_EYE_STANDARD 1760 // standard
#define ROOM_MAX_EYE 1280          // low, on half the recommendation
#define ROOM_ULTRA_EYE 2800        // ultra, taken rather than capped
// Floats per vertex: position, colour, spill weight, uv, one spare.
#define ROOM_VERTEX_FLOATS 10
// Floats per vertex in the baked model file: position, normal, uv, colour.
#define ROOM_MODEL_FLOATS ROOM_MESH_VERTEX_FLOATS
// Clip planes. Far is the room's extent times the margin, never below ROOM_FAR_MIN_M.
#define ROOM_NEAR_M 0.05f
#define ROOM_FAR_MIN_M 60.0f
#define ROOM_FAR_MARGIN 1.5f
// Range of the scale property; rooms default to their built size.
#define ROOM_SCALE_MIN 0.25f
#define ROOM_SCALE_MAX 4.0f
// Range of the brightness property; 1 is as baked.
#define ROOM_DIM_MIN 0.10f
#define ROOM_DIM_MAX 2.0f
// Anisotropic filtering on compressed atlases, where supported.
#define ROOM_ANISOTROPY_MAX 4.0f

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

// Probed and logged only, not enabled.
#ifndef XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME
#define XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME "XR_META_virtual_keyboard"
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
#define PROP_OVERLAY "debug.moonlight.overlay"
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
#define PROP_AMBILIGHT "debug.moonlight.ambilight"
#define PROP_AMBI_SMOOTH "debug.moonlight.ambismooth"
#define PROP_LETTERBOX "debug.moonlight.letterbox"
// 1 normalises the glow to a steady luma with a dark edge roll-off, 0 uses the raw sample.
#define PROP_GLOW_NORM "debug.moonlight.glownorm"
#define PROP_ROOM "debug.moonlight.room"
#define PROP_ROOM_SCALE "debug.moonlight.roomscale"
#define PROP_ROOM_DIM "debug.moonlight.roomdim"
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

    // Stats overlay, a layer of its own so it is never warped or doubled.
    XrSwapchain overlaySwapchain;
    uint32_t overlayImageCount;
    XrSwapchainImageOpenGLESKHR* overlayImages;
    int overlayHasContent;
    int overlayVisible;

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

    // Letterbox detection: unsmoothed, uncropped copy of the sample, read back on the CPU.
    GLuint ambiDetectTexture;
    GLuint ambiDetectFbo;
    // Collected a frame later.
    GLuint ambiDetectPbo;
    int ambiDetectPending;
    // 1 detects and crops, 0 uses the whole frame.
    int ambiBarDetect;
    // Sample-pass frames since the last readback.
    int ambiBarCounter;
    // AMBI_EDGE_* order. Applied is the current crop; the tick
    // counters track how long a larger or smaller count has held.
    int ambiBarApplied[AMBI_EDGES];
    int ambiBarGrowTicks[AMBI_EDGES];
    int ambiBarGrowMin[AMBI_EDGES];
    int ambiBarShrinkTicks[AMBI_EDGES];
    int ambiBarShrinkMax[AMBI_EDGES];
    // x0, y0, w, h in quad uv; 0 0 1 1 is the whole frame.
    float ambiCrop[4];
    GLuint glowProgram;
    GLint glowIntensityUniform;
    GLint glowBlurUniform;
    GLint glowScaleUniform;
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
    float glowDrawnLevel;
    int ambilightOn;
    float ambiIntensity;
    // Debug property override, -1 when unset.
    int ambiOverride;

    // Picture grade in preference units (PICTURE_ order).
    // gradeOn is 0 at the defaults so the shaders skip it.
    int pictureUnits[PICTURE_VALUES];
    int gradeOn;
    PictureGrade grade;

    // 0 none, else ROOM_STYLE_*; roomOverride is the debug property.
    int roomStyle;
    int roomOverride;
    // Debug property overrides, -1 when unset.
    float roomScaleOverride;
    float roomDimOverride;
    // Picture light washed over the room, independent of the glow; global.
    int roomLightOn;
    // Per-style Room tab values in preference units: brightness and light level in
    // hundredths, glow 0/1, screen size in percent. Seeded from the room table.
    int roomBrightness[ROOM_STYLE_LAST + 1];
    int roomGlow[ROOM_STYLE_LAST + 1];
    int roomLightLevel[ROOM_STYLE_LAST + 1];
    int roomScreen[ROOM_STYLE_LAST + 1];
    // Built on first use. One side-by-side image, half per eye.
    XrSwapchain roomSwapchain;
    uint32_t roomImageCount;
    XrSwapchainImageOpenGLESKHR* roomImages;
    GLuint roomFbo;
    GLuint roomDepthBuffer;
    GLuint roomProgram;
    GLint roomViewProjUniform;
    GLint roomSpillGainUniform;
    GLint roomTexMixUniform;
    GLint roomDimUniform;
    GLuint roomVertexBuffer;
    GLuint roomIndexBuffer;
    int roomVertexCount;
    int roomIndexCount;
    // Copied at build so a model loading for another room cannot change the parts in use.
    RoomMeshPart roomParts[ROOM_MESH_PARTS_MAX];
    int roomPartCount;
    // The baked model as read; anchor and scale are applied at build,
    // so a scale change rebuilds without rereading the assets.
    float* roomModelVerts;
    uint32_t* roomModelIndices;
    int roomModelVertexCount;
    int roomModelIndexCount;
    RoomMeshPart roomModelParts[ROOM_MESH_PARTS_MAX];
    int roomModelPartCount;
    int roomAtlasCount;
    int roomModelReady;
    // Plus a 1x1 white texture so the sampler always has a complete texture bound.
    GLuint roomTextures[ROOM_MESH_ATLASES_MAX];
    int roomTextureReady[ROOM_MESH_ATLASES_MAX];
    // Owning style. Only one room is resident; a model
    // for another room drops the atlases it replaces.
    int roomModelStyle;
    int roomTextureStyle[ROOM_MESH_ATLASES_MAX];
    GLuint roomWhiteTexture;
    float roomTexMix;
    float roomDim;
    // 0 before the first build.
    int roomBuiltStyle;
    // Last request, so a pending or failed build is not retried every frame.
    int roomWantedStyle;
    int roomAssetsSeen;
    float roomWantedScale;
    int roomEyeWidth;
    int roomEyeHeight;
    int roomReady;
    // Not retried after a failure.
    int roomFailed;
    int roomRendered;
    float roomSpillGain;
    // Far plane of the built room.
    float roomFarZ;
    float roomClear[3];
    // Views the room was drawn with, submitted with the projection layer.
    XrView roomViews[ROOM_EYES];
    int roomViewsValid;
    // A room hangs the screen on its wall; the user's placement is saved and restored on exit.
    int roomHoldingScreen;
    // Last room placement, for the log.
    int roomPlacedStyle;
    float roomPlacedWidth;
    XrPosef savedScreenPose;
    float savedScreenWidth;
    float savedScreenRadius;
    // A recentre in a room changed the saved placement; persist it once restored.
    int recentredInRoom;
    // Per-eye sizes from the runtime, read at startup; used by the room.
    int recommendedEyeWidth;
    int recommendedEyeHeight;
    int maxEyeWidth;
    int maxEyeHeight;
    // Set from Java.
    int envResTier;

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
    Fade panelFades[FADE_PANELS];
    int panelFadingOut;

    // Launch splash, up from the first frame until panels, room and depth model are
    // ready. splashReadyMs: ms from the first frame each was ready, -1 if not yet.
    Splash splash;
    int splashReadyMs[3];
    XrSwapchain splashSwapchain;
    uint32_t splashImageCount;
    XrSwapchainImageOpenGLESKHR* splashImages;
    int splashArtReady;
    // -1 before the first frame.
    int splashWedgesShown;
    // Last panel art reached the frame loop, uploaded or not.
    int panelArtArrived;
    // The splash stops waiting on depth when a map is published or the model gives up.
    atomic_int depthMapsStaged;
    atomic_int depthGaveUp;

    // Toast queue, raised here or by Java, held while the splash is up. Java
    // draws the text; the sheet shows only while it matches the current notice.
    NoticeBoard notices;
    XrSwapchain toastSwapchain;
    uint32_t toastImageCount;
    XrSwapchainImageOpenGLESKHR* toastImages;
    int toastArtReady;
    int toastDrawnKind;
    int toastDrawnArg;
    Fade toastFade;

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

    // Probed and logged only.
    int virtualKeyboardSupported;
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
    int colorSpaceRec709Offered;
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
    // ASTC atlas support, and anisotropy: 1 without
    // the extension, else up to ROOM_ANISOTROPY_MAX.
    int astcSupported;
    float roomAnisotropy;
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
    // 0..1, from the player.
    float relightBrightness;
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
    // Hands locked for the session so stray pinches cannot click or drag.
    // Triple pinch toggles; controllers are unaffected; off at session start.
    int handsLocked;
    // Read even while locked, since it is the way back.
    TriplePinch triplePinch[HAND_COUNT];
    // Hand-lock hint, shown at most once a session the first time a hand points,
    // unless dismissed for good or hands are off (Java decides at start).
    int hintWanted;
    int hintShown;
    int hintOpen;
    int64_t hintPointingNs;
    XrSwapchain hintSwapchain;
    uint32_t hintImageCount;
    XrSwapchainImageOpenGLESKHR* hintImages;
    int hintReady;
    int hintHoverZone;
    XrPosef hintPose;
    float hintW, hintH;

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
    // Left menu button, gamepad mode only: Start, and with the left grip
    // the pad/pointer toggle. menuBound is 0 if no profile took it.
    XrAction menuAction;
    int menuBound;
    // Vibration for gamepad rumble; hapticBound if any profile took it.
    XrAction hapticAction;
    int hapticBound;
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
    // Head aim: start setting and flip (as for the ray), pixels per
    // degree and dead zone in deg/s (preference units), head state,
    // and the pointer nudge. Only acts with the screen head-locked.
    int headAimSetting;
    int headAimFlipped;
    int headAimSensitivity;
    int headAimDeadZone;
    HeadAim headAim;
    PointerNudge headAimNudge;
    // Active this frame; makes the pointer relative.
    int headAimActive;
    // After a recentre, frames are dropped until one displays at or after headAimRecentreAt.
    int headAimRecentring;
    XrTime headAimRecentreAt;
    // Last logged state (-1 before the first frame), and pixels sent since the last count line.
    int headAimSaid;
    long headAimSentX;
    long headAimSentY;
    XrTime headAimCountNs;
    // Counts the input passes, so the pointer can tell when it missed one
    long inputFrames;
    // Gamepad mode: the controllers act as one gamepad instead of the
    // pointer for the rest of the session. Stick dead zone, toggle shortcut,
    // grips as bumpers, last read per controller and last pad sent to Java,
    // attachment, resting (under a panel or unfocused), buttons held until
    // released, panels to close, and the last logged buttons.
    int padMode;
    float padDeadzone;
    int padShortcut;
    PadToggle padToggle;
    PadChord padChord;
    int padGrip[HAND_COUNT];
    PadHand padRead[HAND_COUNT];
    PadState pad;
    int padAttached;
    int padResting;
    int padHeld;
    int padPutAway;
    int padButtonsSaid;
    // Gamepad rumble; for the log, pulses since enabled and
    // whether each controller's first result was logged.
    PadRumble rumble;
    int rumbleOn;
    long rumblePulses;
    int rumbleResultSaid[HAND_COUNT];
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
    int modelEyeWidth;
    int modelEyeHeight;
    int modelPassReady;
    int modelPassFailed;
    XrView modelViews[ROOM_EYES];
    int modelRendered;
    // Own timer query pair, like the room.
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
    XrQuaternionf screenOrientation;
    float beamWidth;
    // Head yaw relative to the screen, for IN_HEAD_YAW.
    float audioYaw;

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
    float grabU, grabV;
    XrPosef grabAim;
    XrPosef grabScreen;
    float grabWidth;
    float grabHeight;
    float grabRadius;
    // Tilt and roll at pickup; a move keeps them and recomputes only the yaw.
    float grabPitch;
    float grabRoll;
    // Resize scales about the centre; this is the opposite
    // corner, and its signs identify the held one.
    float grabOppX, grabOppY;
    // In a room a resize changes the share of the room's screen instead of
    // the width: share and reach along the half diagonal at pickup. The
    // share is saved to the preference once a frame has a free setting slot.
    int grabRoomPercent;
    float grabRoomReach;
    // Local player mode: handles per xr_handles.h. A move bar pull
    // scales the distance by the hand's distance to the eyes relative
    // to the grab; the width follows the distance proportionally.
    int playerMode;
    float grabHandDist;
    float grabScreenDist;
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
        float widthFrac;
        float gapFrac;
        // Fraction of the distance toward the viewer the panel floats off the screen.
        float depthK;
        float level;
    } ui[UI_PANEL_COUNT];
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
    int roomScreenUnsaved;
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

    XrSwapchain pickerSwapchain;
    XrSwapchain outlineSwapchain;
    uint32_t pickerImageCount;
    uint32_t outlineImageCount;
    XrSwapchainImageOpenGLESKHR* pickerImages;
    XrSwapchainImageOpenGLESKHR* outlineImages;
    int pickerReady;
    int envButtonReady;
    int outlineReady;
    int pickerOpen;
    int pickerHover;
    int pickerChoice;
    int pickerPick;
    // Cells with content; the rest are blank and inert.
    int pickerCells;
    int envButtonHot;
    // Last choice logged, so reapplying it is not logged again.
    int loggedChoice;

    // One swapchain shows the current sheet; sheets are cached
    // here pre-flipped so a tab change is one upload. A swapchain
    // per sheet would exceed the Pico 4 Ultra's limit of 32.
    XrSwapchain cogPanelSwapchain;
    XrSwapchain cogThumbSwapchain;
    uint32_t cogPanelImageCount;
    uint32_t cogThumbImageCount;
    XrSwapchainImageOpenGLESKHR* cogPanelImages;
    XrSwapchainImageOpenGLESKHR* cogThumbImages;
    unsigned char* cogPanelPixels[COG_ART_COUNT];
    int cogPanelReady[COG_ART_COUNT];
    // -1 for none.
    int cogArtShown;
    int cogButtonReady;
    int cogThumbReady;
    int cogOpen;
    int cogTab;
    int cogButtonHot;
    // Slider being dragged and its hand, -1 for none. The drag
    // keeps its hand so the other cannot steal it; the face matters
    // because the first tab's rows differ while a room is up.
    int cogDragSlider;
    int cogDragHand;
    int cogDragFace;
    // Gaze-picked slider: the hand moves the thumb from where the press put it.
    int cogDragByGaze;
    Vec3 cogDragHandStart;
    float cogDragScale;
    float cogDragStartU;
    DragRamp cogDragRamp;
    // Percent readout beside the Room tab's tracks, and the values last drawn.
    XrSwapchain cogReadoutSwapchain;
    uint32_t cogReadoutImageCount;
    XrSwapchainImageOpenGLESKHR* cogReadoutImages;
    int cogReadoutReady;
    int cogReadoutDrawn[READOUT_VALUES];
    // Display tab marks, one strip for all rows, redrawn by Java when one changes.
    XrSwapchain cogMarksSwapchain;
    uint32_t cogMarksImageCount;
    XrSwapchainImageOpenGLESKHR* cogMarksImages;
    int cogMarksReady;
    // The clock line over the panel
    XrSwapchain cogClockSwapchain;
    uint32_t cogClockImageCount;
    XrSwapchainImageOpenGLESKHR* cogClockImages;
    int cogClockReady;
    // Click sound setting (also on the Display tab), and a click for Java to play.
    int clickSoundOn;
    int clickPending;
    // From the last frame, for the Display tab marks.
    int headLockedPref;
    // Row under the ray; on the Display tab, the cell; on a track, the step button (-1/1) or 0.
    int cogHoverSlider;
    int cogHoverCell;
    int cogHoverStep;
    // Frozen at open: the distance slider moves the
    // screen and would pull the thumb from under the ray.
    XrPosef cogPose;
    float cogW, cogH;
    // Keyboard: one swapchain, sheets cached per state like the settings
    // panel. A sheet is redrawn only when its lit modifiers change.
    XrSwapchain kbPanelSwapchain;
    uint32_t kbPanelImageCount;
    XrSwapchainImageOpenGLESKHR* kbPanelImages;
    unsigned char* kbPanelPixels[KB_STATE_COUNT];
    int kbPanelReady[KB_STATE_COUNT];
    // -1 for none.
    int kbStateShown;
    int kbButtonReady;
    int kbOpen;
    int kbButtonHot;
    int kbState;
    // Lit modifiers (KB_MOD_ bits), held down by Java while lit.
    int kbMods;
    // -1 for none.
    int kbHoverKey;
    int kbKeyDown;
    // Layout from Java: four panel fractions per key (left,
    // top, right, bottom) and a code per key per state.
    int kbKeyCount;
    float kbKeyRects[KB_MAX_KEYS * 4];
    int kbCodes[KB_STATE_COUNT][KB_MAX_KEYS];
    // Frozen at open, as the screen can move while it is up.
    XrPosef kbPose;
    float kbW, kbH;

    // Every bar button face, a cell each in one texture (BTN_CELL_ in
    // xr_layout.h), with a CPU copy so a new face uploads with the rest.
    XrSwapchain buttonSwapchain;
    uint32_t buttonImageCount;
    XrSwapchainImageOpenGLESKHR* buttonImages;
    unsigned char* buttonAtlas;

    // Ready only in a stereo session.
    int stereoButtonReady;
    int stereoButtonHot;

    int rayButtonReady;
    int rayButtonHot;

    // Shown only while head aim can act.
    int aimButtonReady;
    int aimButtonHot;

    int padButtonReady;
    int padButtonHot;

    // Exit button and prompt: a cached sheet per hovered button, shown in one swapchain.
    XrSwapchain exitPromptSwapchain;
    uint32_t exitPromptImageCount;
    XrSwapchainImageOpenGLESKHR* exitPromptImages;
    unsigned char* exitPromptPixels[EXIT_ART_COUNT];
    int exitPromptReady[EXIT_ART_COUNT];
    // -1 for none.
    int exitArtShown;
    int exitButtonReady;
    int exitConfirmOpen;
    int exitButtonHot;
    // Also the sheet shown.
    int exitHoverZone;
    // Frozen at open so the buttons do not move under the ray.
    XrPosef exitPose;
    float exitW, exitH;

    // Report sheet, redrawn by Java on change and hidden until
    // drawn for this opening. The keyboard below types into it.
    XrSwapchain reportSwapchain;
    uint32_t reportImageCount;
    XrSwapchainImageOpenGLESKHR* reportImages;
    int reportReady;
    int reportOpen;
    int reportHoverZone;
    int reportSendReady;
    int cogReportHot;
    XrPosef reportPose;
    float reportW, reportH;
    int cogKofiHot;
    int kofiOpen;
    XrSwapchain kofiSwapchain;
    uint32_t kofiImageCount;
    XrSwapchainImageOpenGLESKHR* kofiImages;
    int kofiReady;
    int kofiHoverZone;
    XrPosef kofiPose;
    float kofiW, kofiH;

    // Curvature from the panel, -1 while the preference owns it. The
    // preference is kept too, for use outside the JNI entry points.
    float panelCurve;
    float prefCurvature;
    // Separation from the panel (-1 while the preference owns it), and
    // the value in force this frame, which the 3D tab's thumb shows.
    float panelSeparation;
    float separationCurrent;
    // The model's default separation and convergence (the 3D
    // tab's reset), and the preset separations in preference
    // units and cell order. Set by Java before the first frame.
    float defaultSeparation;
    float defaultConvergence;
    int presetUnits[COG_PRESET_CELLS];

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

    // The room pass has its own query pair, run before the main
    // one; nested inside it, this driver returns wrapped values.
    GLuint roomTimerQueries[2];
    int roomTimerSlot;
    int roomTimerPending[2];
    int roomTimerPendingFrames[2];
    int64_t roomGpuTotalNs;
    long roomGpuSamples;
    long roomGpuDropped;

    // Separate accumulator so the overlay does not disturb the logcat cadence.
    int64_t overlayGpuTotalNs;
    long overlayGpuSamples;
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
void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation);

// xr_depth.c: the depth model staging and the stage thread's uploads
int initDepthModel(XrCtx* ctx);
int linkDownscaleProgram(XrCtx* ctx);
void waitForDepthSlot(XrCtx* ctx);

// xr_ambilight.c: the frame colour sample, letterbox detection and the glow
int initAmbilight(XrCtx* ctx);
int linkAmbiProgram(XrCtx* ctx);
void ambiEffective(XrCtx* ctx, int* on, float* level);
void runAmbiBarDetect(XrCtx* ctx, const float* texMatrix);
void finishAmbiBarDetect(XrCtx* ctx);
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

// xr_room.c: the 3d rooms
int roomEffective(XrCtx* ctx);
int roomResizable(int style);
void roomLevelsFromTable(XrCtx* ctx);
int roomScreenPercent(XrCtx* ctx, int style);
int roomGlowOn(XrCtx* ctx, int style);
void applyRoomPlacement(XrCtx* ctx, int style, float aspect, int reseeded);
void prepareRoom(XrCtx* ctx);
void renderRoom(XrCtx* ctx);
void worldEyeSize(XrCtx* ctx, int* outW, int* outH);

// xr_model.c: the bundled controller model
void updateControllerModels(XrCtx* ctx);
void renderControllerModels(XrCtx* ctx);

// xr_input.c: actions, hands, the ray and the per frame input pass
int initXrInput(XrCtx* ctx);
void destroyXrInput(XrCtx* ctx);
void refreshInputSource(XrCtx* ctx);
int updatePlacement(XrCtx* ctx, float distance, float quadWidth, float curvature);
void recentreScreen(XrCtx* ctx);

// xr_ui.c: where the furniture and the panels sit, and what the ray is over
int furnitureOnStandIn(XrCtx* ctx);
XrPosef furniturePose(XrCtx* ctx);
float furnitureWidth(XrCtx* ctx);
float furnitureHeight(XrCtx* ctx);
BarFrame barFrame(XrCtx* ctx);
int barButtonHit(XrCtx* ctx, int slot, float u, float v, const BarFrame* frame);
float cornerSide(XrCtx* ctx);
float effectiveCurvature(XrCtx* ctx);
int cogFace(XrCtx* ctx);
int cogArt(XrCtx* ctx);
float screenPitch(XrCtx* ctx);
XrQuaternionf screenOrient(float yaw, float pitch, float roll);
// The local player's panels (xr_uipanels.c)
XrPosef uiPanelPose(XrCtx* ctx, int id, float* outWidth, float* outHeight);
int uiPanelAt(XrCtx* ctx, XrPosef aim, float* outU, float* outV);
void stepUiPanels(XrCtx* ctx, int64_t nowNs);
void destroyUiPanels(XrCtx* ctx);
// The player's room light, see xr_relight.h
int relightWanted(XrCtx* ctx);
void relightSample(XrCtx* ctx);
void relightStep(XrCtx* ctx, int64_t nowNs);
int relightLayerReady(XrCtx* ctx);
void relightDestroy(XrCtx* ctx);
float screenRoll(XrCtx* ctx);
XrPosef pickerPose(XrCtx* ctx, float* outWidth, float* outHeight);
XrPosef cogPanelPose(XrCtx* ctx, float* outWidth, float* outHeight);
XrPosef kbPanelPose(XrCtx* ctx, float* outWidth, float* outHeight);
int kbKeyAt(XrCtx* ctx, float u, float v);
int headAimCanAct(XrCtx* ctx);
void setHeadAimOn(XrCtx* ctx, int on, const char* from);
void setPadMode(XrCtx* ctx, int on, const char* from);
const char* padShortcutName(int shortcut);
void setStereoLive(XrCtx* ctx, int on, const char* from);
void setRayOn(XrCtx* ctx, int on, const char* from);
int panelUp(XrCtx* ctx);
XrPosef exitPromptPose(XrCtx* ctx, float* outWidth, float* outHeight);
int exitPromptZone(float u, float v);
XrPosef reportSheetPose(XrCtx* ctx, float* outWidth, float* outHeight);
XrPosef handHintPose(XrCtx* ctx, float* outWidth, float* outHeight);
XrPosef kofiSheetPose(XrCtx* ctx, float* outWidth, float* outHeight);
int cogTabRowCount(int face);
int cogRowIsTrack(int face, int row);
int cogRowLive(XrCtx* ctx, int face, int row);
float cogSliderValue(XrCtx* ctx, int face, int slider);
void cogApplySlider(XrCtx* ctx, int face, int slider, float pu);
int cogOptionCells(int option);
int cogOptionValue(XrCtx* ctx, int option, int headLocked);
int cogApplyOption(XrCtx* ctx, int option, int cell);
int cogRowCells(int face, int row);
int cogRoomCellValue(XrCtx* ctx, int row);
void cogApplyRoomCell(XrCtx* ctx, int row, int cell, float* out);
int cogCellInForce(XrCtx* ctx, int face, int row);
void cogApplyCell(XrCtx* ctx, int face, int row, int cell, float* out);
void cogDragEnded(XrCtx* ctx, float* out);
void cogStepTrack(XrCtx* ctx, int face, int row, int dir, float* out);
int cogCellAt(float pu, int cells);
void cogReadouts(XrCtx* ctx, int* values);
void pictureSet(XrCtx* ctx, int row, int units);
void pictureReset(XrCtx* ctx);

// xr_assets.c: the swapchains the art goes into and the uploads that fill them
int createArtSwapchain(XrCtx* ctx, int width, int height, const char* what,
                       XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images,
                       uint32_t* count);
void destroyArtSwapchain(XrCtx* ctx, XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images);
// A sheet of RGBA rows running top down, flipped into a chain's images
int uploadFlipped(XrCtx* ctx, XrSwapchain chain, XrSwapchainImageOpenGLESKHR* images,
                  const unsigned char* px, int width, int height);
int createPointerSwapchain(XrCtx* ctx);
void freeArtSheets(XrCtx* ctx);
int showCogArt(XrCtx* ctx, int art);
int showKbSheet(XrCtx* ctx, int state);
int showExitSheet(XrCtx* ctx, int zone);
int uploadPointerArt(XrCtx* ctx);
int roomStyleForCell(int cell);
int roomCellForStyle(int style);

// xr_display.c: the display refresh rate and the performance levels
void probeDisplayExtensions(XrCtx* ctx);
void startDisplay(XrCtx* ctx);
void startPerfLevels(XrCtx* ctx);
void setPerfLevel(XrCtx* ctx, int level);
void perfNotice(XrCtx* ctx, const XrEventDataPerfSettingsEXT* notice);
void displaySessionBegun(XrCtx* ctx);
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
