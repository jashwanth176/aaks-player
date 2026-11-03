package com.limelight.binding.video;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.SurfaceTexture;
import android.os.Build;
import android.os.Process;
import android.preference.PreferenceManager;
import android.view.Surface;

import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.binding.input.EyeTrackingPermission;
import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.preferences.XrDisplayRates;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import static com.limelight.binding.video.XrShared.*;

/**
 * Presents decoded video in an OpenXR session. The decoder renders into this class's
 * SurfaceTexture, which the frame loop thread consumes; all OpenXR work is native.
 */
public class XrRenderer implements SurfaceTexture.OnFrameAvailableListener {

    static {
        System.loadLibrary("xr-renderer");
    }

    // Maps averaged per depth stage log line
    private static final int DEPTH_STATS_INTERVAL = 30;
    // A gap between maps longer than this is a stall or the headset off, and stays out of the
    // period
    private static final long DEPTH_PERIOD_GAP_NS = 1000000000L;
    private static final int DEPTH_AGE_INTERVAL = 300;

    // Written by the frame loop, read from other threads
    private volatile long nativeCtx;
    // Held around native calls made off the frame loop, and while the frame loop frees the
    // context, so no thread reaches a half-destroyed context
    private final Object nativeLock = new Object();
    private Thread renderThread;
    private Thread depthThread;
    private Thread depthStageThread;
    private SurfaceTexture surfaceTexture;
    private Surface inputSurface;
    // The frame loop reads the SurfaceTexture every frame. A cleanup() while it runs sets
    // releaseOnExit, and the loop releases the surfaces on its way out.
    private final Object teardownLock = new Object();
    private boolean renderThreadDone;
    private boolean releaseOnExit;

    private final AtomicInteger pendingFrames = new AtomicInteger(0);
    private final float[] texMatrix = new float[16];
    private volatile boolean stopping;
    // Session went to stopping or idle after its first focus, as last reported. Frame loop only.
    private boolean sessionAway;
    // Whether the session has had focus yet, and whether it has it now, as last reported
    private boolean everFocusedSeen;
    private boolean sessionFocused;
    private long videoFrameIndex;

    // Depth pipeline over DEPTH_PAIRS pairs of native staging (see DepthPairs): the frame loop
    // queues a capture, the stage thread reads it back into model input, the depth thread runs
    // the model, and the stage thread uploads the map. The frame loop never waits; with no pair
    // free it skips the frame. All of it under depthLock.
    private final Object depthLock = new Object();
    private final DepthPairs pairs = new DepthPairs(DEPTH_PAIRS);
    // Per-pair stage costs, stage start and source frame, written by the thread doing that
    // stage before it hands the pair on under the lock
    private final long[] pairCaptureNs = new long[DEPTH_PAIRS];
    private final long[] pairFinishNs = new long[DEPTH_PAIRS];
    private final long[] pairStartNs = new long[DEPTH_PAIRS];
    private final long[] pairInferenceNs = new long[DEPTH_PAIRS];
    private final long[] pairFrameIndex = new long[DEPTH_PAIRS];
    private final long[] pairFrameNs = new long[DEPTH_PAIRS];
    private boolean depthExit;
    private int skippedFrames;
    private volatile boolean depthReady;

    // Kept so a later film can start the depth thread in the same session
    private Activity sessionActivity;
    private MidasDepthSource.Spec sessionDepthSpec;
    private MidasDepthSource.Route sessionDepthRoute;
    private int depthModeNow;
    // Width, height, fps, depth mode, screen hidden, same film, 3D on: applied between frames
    private final AtomicReference<int[]> pendingReconfig = new AtomicReference<>();
    // The frame still latched is drawn again on the next frame, for a new layout of a paused film
    private boolean redrawLatched;

    // Frame index and time of the frame the live depth map came from, to measure its lag
    private volatile long publishedFrameIndex;
    private volatile long publishedFrameNs;

    // 3D on, as of the last frame. While it is off the model is not fed.
    private volatile boolean stereoLive = true;
    // False while the eye tracking permission is refused; gaze pointing needs it
    private volatile boolean gazeAllowed = true;

    // Native input results, indexed by the IN_ constants in XrShared
    private final float[] inputState = new float[IN_SLOTS];
    private int heldButtons;
    private InputListener inputListener;
    private volatile SessionListener sessionListener;
    private Context prefsContext;
    private PreferenceConfiguration prefConfig;

    // Drawn at each hand when the setting is on, painted from its vertex colours
    private static final String CONTROLLER_MODEL = "models/controller.room";

    // The splash, drawn before the frame loop starts and uploaded by it, since it owns the
    // GL context
    private final AtomicReference<ByteBuffer> pendingSplash = new AtomicReference<>();
    // Read every session, shown or not, so the Display tab can switch it on at once
    private final AtomicReference<ByteBuffer> pendingControllerModel = new AtomicReference<>();
    private volatile boolean passthroughOn;

    /**
     * Pointer events from the VR session, on the frame loop thread. Buttons: 0 left, 1 right, 2
     * middle.
     */
    public interface InputListener {
        void onVrPointerMove(float u, float v);
        void onVrButton(int button, boolean down);
        void onVrScroll(int clicks);
        // Player controller reads every frame: face buttons as bits (A 1, B 2, X 4, Y 8) and
        // thumbstick X, -1 to 1
        default void onVrPlayerInput(int buttons, float stickX) {
        }
    }

    public void setInputListener(InputListener listener) {
        this.inputListener = listener;
    }

    // The film screen's saved placement
    private static final String PLAYER_SCREEN_POSE_PREF = "player_screen_pose";

    private volatile float playerAmbient = 1.0f;
    private volatile float playerBrightness = 0.4f;
    private volatile float playerHouse;
    private volatile float playerCrop = 0.015f;
    private volatile float playerBarTop, playerBarBottom;
    private volatile boolean playerScreenHidden;
    private volatile float playerCurve;

    /** How curved the screen is, 0 flat to 1 round the viewer. Any thread. */
    public void setPlayerCurve(float curve) {
        playerCurve = curve;
        long ctx = nativeCtx;
        if (ctx != 0) {
            nativeSetPlayerCurve(ctx, curve);
        }
    }

    private volatile int colourMode = XrShared.COLOUR_MODE_NATURAL;
    private volatile boolean handAutoHide = true;
    private volatile int videoTransfer = XrShared.VIDEO_TRANSFER_CODE_SDR;
    private volatile float videoMaxCll, videoMasteringNits;

    /** Whether a hand's pointer hides once the hand is idle, until a palm is shown. Any thread. */
    public void setHandAutoHide(boolean on) {
        handAutoHide = on;
        long ctx = nativeCtx;
        if (ctx != 0) {
            nativeSetHandAutoHide(ctx, on);
        }
    }

    /** Natural or vivid colours, a COLOUR_MODE_ value. Any thread. */
    public void setColourMode(int mode) {
        colourMode = mode;
        long ctx = nativeCtx;
        if (ctx != 0) {
            nativeSetColourMode(ctx, mode);
        }
    }

    /**
     * What the video's light is coded as, a VIDEO_TRANSFER_CODE_ value, with the file's
     * brightest pixel (MaxCLL) and its mastering display's peak in nits, each 0 when the
     * file does not say. The renderer picks the peak to tone map from. Any thread.
     */
    public void setVideoColour(int transfer, float maxCll, float masteringNits) {
        if (transfer == videoTransfer && maxCll == videoMaxCll && masteringNits == videoMasteringNits) {
            return;
        }
        videoTransfer = transfer;
        videoMaxCll = maxCll;
        videoMasteringNits = masteringNits;
        long ctx = nativeCtx;
        if (ctx != 0) {
            nativeSetVideoColour(ctx, transfer, maxCll, masteringNits);
        }
    }

    /** Hides the screen and keeps the panels, for the library with no film. Any thread. */
    public void setPlayerScreenHidden(boolean hidden) {
        playerScreenHidden = hidden;
        long ctx = nativeCtx;
        if (ctx != 0) {
            nativeSetPlayerScreenHidden(ctx, hidden);
        }
    }

    /**
     * The share of the frame's height that is black bar, at the top and at the
     * bottom, so the window takes the picture's own shape. Any thread.
     */
    public void setPlayerContent(float top, float bottom) {
        playerBarTop = top;
        playerBarBottom = bottom;
        long ctx = nativeCtx;
        if (ctx != 0) {
            nativeSetPlayerContent(ctx, top, bottom);
        }
    }

    /**
     * How far each eye of a stereo film is shifted outward, as a share of its width: 0.015 puts
     * the scene behind the screen, 0 leaves it as mastered. Any thread.
     */
    public void setPlayerDepth(float crop) {
        playerCrop = crop;
        long ctx = nativeCtx;
        if (ctx != 0) {
            nativeSetPlayerCrop(ctx, crop);
        }
    }

    /**
     * Ambient light reach round the screen, 0..1, and brightness 0..1, which sets how far the
     * picture dims and tints passthrough. Any thread.
     */
    public void setPlayerLighting(float ambient, float brightness) {
        playerAmbient = ambient;
        playerBrightness = brightness;
        long ctx = nativeCtx;
        if (ctx != 0) {
            nativeSetPlayerLighting(ctx, ambient, brightness);
        }
    }

    /**
     * House lights, 0 (the film's lighting) to 1 (the room fully up; in the dark, a dim
     * passthrough). The renderer fades between levels unless now. Any thread.
     */
    public void setHouseLights(float level, boolean now) {
        playerHouse = level;
        long ctx = nativeCtx;
        if (ctx != 0) {
            nativeSetHouseLights(ctx, level, now);
        }
    }

    // The library without a film keeps its own placement and size, apart from the film screen's
    private static final String PLAYER_BROWSE_POSE_PREF = "player_browse_pose";

    private String screenPoseKey() {
        return playerScreenHidden ? PLAYER_BROWSE_POSE_PREF : PLAYER_SCREEN_POSE_PREF;
    }

    /**
     * The ray on the player's panels, every input frame: the panel it is on,
     * a UI_PANEL_ or -1, where on it from the top left in 0..1, and whether
     * the trigger is held. Called on the frame loop thread.
     */
    public interface UiListener {
        void onUiPointer(int panel, float u, float v, boolean pressed);
    }

    private volatile UiListener uiListener;

    public void setUiListener(UiListener listener) {
        uiListener = listener;
    }

    // A panel's latest sheet waiting for the frame loop; a newer one replaces it
    private static final class UiSheet {
        final ByteBuffer pixels;
        final int width;
        final int height;

        UiSheet(ByteBuffer pixels, int width, int height) {
            this.pixels = pixels;
            this.width = width;
            this.height = height;
        }
    }

    private final ByteBuffer[][] uiBuffers = new ByteBuffer[UI_PANEL_COUNT][2];
    private final int[] uiBufferNext = new int[UI_PANEL_COUNT];

    private final java.util.concurrent.atomic.AtomicReferenceArray<UiSheet> pendingUiSheets =
            new java.util.concurrent.atomic.AtomicReferenceArray<>(UI_PANEL_COUNT);

    /**
     * A panel's picture, premultiplied ARGB as Android draws it. Any thread;
     * it goes up on the next frame.
     */
    public void uploadUiPanel(int panel, android.graphics.Bitmap bitmap) {
        if (panel < 0 || panel >= UI_PANEL_COUNT || bitmap == null) {
            return;
        }
        // Two buffers per panel, used in turn: a dragged slider sends a picture every frame
        int need = bitmap.getByteCount();
        int slot = uiBufferNext[panel];
        uiBufferNext[panel] = 1 - slot;
        ByteBuffer pixels = uiBuffers[panel][slot];
        if (pixels == null || pixels.capacity() != need) {
            pixels = ByteBuffer.allocateDirect(need).order(java.nio.ByteOrder.nativeOrder());
            uiBuffers[panel][slot] = pixels;
        }
        pixels.clear();
        bitmap.copyPixelsToBuffer(pixels);
        pixels.rewind();
        pendingUiSheets.set(panel, new UiSheet(pixels, bitmap.getWidth(), bitmap.getHeight()));
    }

    /**
     * Where a panel hangs off the screen and whether it shows: a UI_ANCHOR_, and its gap from
     * that edge (or towards the viewer, for the front). A fixed panel keeps its size whatever the
     * screen, with the gap in its pixels; otherwise width and gap are shares of the screen's
     * width. xOff moves it sideways from the centre line, in the gap's units. Any thread.
     */
    public void setUiPanel(int panel, boolean visible, int anchor, boolean fixed, float widthFrac,
                           float gap, float xOff) {
        if (panel < 0 || panel >= UI_PANEL_COUNT) {
            return;
        }
        // Applied on the frame loop after any new picture, so a panel never shows the old picture
        // at the new size
        pendingUiPlacements.set(panel, new float[] { visible ? 1 : 0, anchor, fixed ? 1 : 0,
                widthFrac, gap, xOff });
    }

    /**
     * How far in front of the screen a panel floats, as a share of the viewer's distance to it
     * (0 on the screen), which sets its depth in 3D. Any thread.
     */
    public void setUiPanelDepth(int panel, float k) {
        long ctx = nativeCtx;
        if (ctx != 0 && panel >= 0 && panel < UI_PANEL_COUNT) {
            nativeSetUiPanelDepth(ctx, panel, k);
        }
    }

    private final java.util.concurrent.atomic.AtomicReferenceArray<float[]> pendingUiPlacements =
            new java.util.concurrent.atomic.AtomicReferenceArray<>(UI_PANEL_COUNT);

    // Pictures and placements made between these two go up together
    private final java.util.concurrent.atomic.AtomicInteger uiBatch = new java.util.concurrent.atomic.AtomicInteger();

    public void beginUiChange() {
        uiBatch.incrementAndGet();
    }

    public void endUiChange() {
        uiBatch.decrementAndGet();
    }

    // Requested passthrough: 1 on, 0 off, -1 nothing asked; applied on the frame loop
    private final java.util.concurrent.atomic.AtomicInteger pendingPassthrough =
            new java.util.concurrent.atomic.AtomicInteger(-1);

    /** Passthrough, or the dark void instead of it. Any thread. */
    public void setPassthrough(boolean on) {
        pendingPassthrough.set(on ? 1 : 0);
    }

    /** Whether the session is showing passthrough now. */
    public boolean isPassthroughOn() {
        return passthroughOn;
    }

    private void uploadPendingUiSheets() {
        int pass = pendingPassthrough.getAndSet(-1);
        if (pass >= 0) {
            passthroughOn = pass == 1;
            // Saved, so the next session opens the same way
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING,
                            passthroughOn ? PreferenceConfiguration.VR_ENV_PASSTHROUGH
                                          : PreferenceConfiguration.VR_ENV_VOID)
                    .apply();
        }
        if (uiBatch.get() > 0) {
            return;
        }
        for (int i = 0; i < UI_PANEL_COUNT; i++) {
            UiSheet sheet = pendingUiSheets.getAndSet(i, null);
            if (sheet != null) {
                nativeUploadUiPanel(nativeCtx, i, sheet.pixels, sheet.width, sheet.height);
            }
            float[] place = pendingUiPlacements.getAndSet(i, null);
            if (place != null) {
                nativeSetUiPanel(nativeCtx, i, place[0] != 0, (int) place[1], place[2] != 0,
                        place[3], place[4], place[5]);
            }
        }
    }

    // Told when the session cannot start or the runtime ends it
    public void setSessionListener(SessionListener listener) {
        this.sessionListener = listener;
    }

    /**
     * Session events for the activity, so a failed start can be shown rather than leaving a
     * window the headset never displays. Called off the main thread.
     */
    public interface SessionListener {
        // failure says where the start stopped, never null
        void onVrUnavailable(XrStartFailure failure);

        // The runtime ended a running session (system menu quit, session lost, a failed frame
        // call). At most once, from the render thread, never for a stop the activity asked for.
        // reason is for the log.
        void onVrSessionEnded(String reason);

        // After the first focus the session went to stopping or idle (headset removed), and later
        // came back. Once each way, in turn, from the render thread.
        default void onVrSessionAway() {
        }

        default void onVrSessionBack() {
        }

        // After the first focus, focus was lost (system menu, a notice, headset off) or regained.
        // Once each way, in turn, from the render thread.
        default void onVrFocusLost() {
        }

        default void onVrFocusGained() {
        }
    }

    // How long a start is waited for before it counts as failed
    private static final int INIT_WAIT_SECONDS = 5;

    private static native void nativeSetFileLog(String path, int level);
    // Where the last failed start stopped, for XrStartFailure, or null. Taken once.
    private static native String[] nativeTakeStartFailure();
    // fps is the video's; the display rate is matched to it.
    private native boolean nativeReconfigure(long ctx, int width, int height, int fps, int stereoMode,
                                             boolean sameFilm, boolean stereoOn);
    private native long nativeInit(Activity activity, int width, int height, int fps,
                                   int stereoMode, int depthWidth, int depthHeight,
                                   boolean depthDebug, int convergence, int depthScale,
                                   boolean handTracking, int sharpenMode, int supersampleMode);
    private native void nativeSetCaptureDir(long ctx, String dir);
    private native int nativeGetTexId(long ctx);
    private native ByteBuffer nativeGetModelInput(long ctx, int pair);
    private native ByteBuffer nativeGetModelOutput(long ctx, int pair);
    private native long nativeCaptureDepthInput(long ctx, float[] texMatrix, int pair);
    private native long nativeFinishDepthCapture(long ctx, int pair);
    private native long nativeUploadDepth(long ctx, int pair);
    private native void nativeDropDepth(long ctx, int pair);
    private native boolean nativeBindDepthContext(long ctx);
    private native void nativeUnbindDepthContext(long ctx);
    private native boolean nativeBindDepthStageContext(long ctx);
    private native void nativeUnbindDepthStageContext(long ctx);
    private native int nativeWaitBeginFrame(long ctx);
    private native void nativeEndFrame(long ctx, boolean newFrame, float[] texMatrix,
                                       float distance, float quadWidth, float curvature,
                                       boolean headLocked, float separation, boolean eyeSwap,
                                       boolean passthrough);
    private native void nativeUpdateInput(long ctx, float distance, float quadWidth,
                                          float curvature, boolean headLocked,
                                          boolean pointerEnabled, boolean gazeEnabled,
                                          boolean pointerSleep, float[] out);
    private native void nativeSetScreenPose(long ctx, float[] pose);
    private native void nativeRecentreScreen(long ctx);
    // The controller model's .room file, whole
    private native void nativeUploadControllerModel(long ctx, ByteBuffer mesh, int length);
    // Whether the controller model is drawn; the Display tab's row changes it
    private native void nativeSetControllerModel(long ctx, boolean on);
    // Brightness, contrast, gamma and saturation, in PICTURE_ order and units
    private native void nativeSetPicture(long ctx, int[] picture);
    // A PRESENCE_ value
    private native int nativeGetPresence(long ctx);
    // Whether the headset starts apps in passthrough
    private native boolean nativeSystemPrefersPassthrough(long ctx);
    // Why the runtime ended the frame loop, or null
    private native String nativeGetExitReason(long ctx);
    private native void nativeSetPlayerLighting(long ctx, float ambient, float brightness);
    private native void nativeSetHouseLights(long ctx, float level, boolean now);
    private native void nativeSetPlayerContent(long ctx, float top, float bottom);
    private native void nativeSetPlayerScreenHidden(long ctx, boolean hidden);
    private native void nativeSetPlayerCrop(long ctx, float crop);
    private native void nativeSetPlayerCurve(long ctx, float curve);
    private native void nativeSetColourMode(long ctx, int mode);
    private native void nativeSetHandAutoHide(long ctx, boolean on);
    private native void nativeSetVideoColour(long ctx, int transfer, float maxCll, float masteringNits);
    private native void nativeUploadUiPanel(long ctx, int panel, ByteBuffer sheet, int width, int height);
    private native void nativeSetUiPanel(long ctx, int panel, boolean visible, int anchor,
                                         boolean fixed, float widthFrac, float gapFrac, float xOff);
    private native void nativeSetUiPanelDepth(long ctx, int panel, float k);
    private native void nativeUploadSplash(long ctx, ByteBuffer sheet);
    // Whether sessions start with the ray drawn
    private native void nativeSetShowRay(long ctx, boolean on);
    // The depth model will make no maps this session, so the splash stops waiting. Any thread.
    private native void nativeDepthGaveUp(long ctx);
    // Maximum depth maps per second; the governor starts here and cuts when frames are missed
    private native void nativeSetDepthRate(long ctx, int perSecond);
    // Frame loop, with a new frame: whether to capture it for the depth model. force ignores
    // the timing.
    private native boolean nativeDepthDue(long ctx, boolean force);
    // The governor's current depth rate, maps per second
    private native int nativeGetDepthTarget(long ctx);
    // Every rate the display offers, empty where the runtime does not say
    private native float[] nativeGetOfferedRates(long ctx);
    private native void nativeDestroy(long ctx);

    public boolean start(final Activity activity, final int videoWidth, final int videoHeight,
                         final PreferenceConfiguration prefs) {
        final CountDownLatch initLatch = new CountDownLatch(1);
        final boolean[] initOk = new boolean[1];

        renderThread = new Thread() {
            @Override
            public void run() {
                try {
                    runSession();
                } finally {
                    finishRenderThread();
                }
            }

            private void runSession() {
                // Submission must land inside the compositor's frame window. Thread.setPriority
                // does not reach the Linux scheduler, so this goes through Process.
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY);

                // Before init, so session setup is logged too
                nativeSetFileLog(FileLog.getLogPath(), FileLog.getLevel());

                // The depth staging is sized for the model export this headset loads, so the route
                // is settled before init. Test patterns use the default model's size.
                MidasDepthSource.Spec depthSpec = MidasDepthSource.specFor(prefs.vrDepthModel);
                MidasDepthSource.Route depthRoute =
                        depthSpec.route(PreferenceConfiguration.isXr2Gen1Headset());
                DepthSize mapSize = depthRoute.size;
                sessionActivity = activity;
                sessionDepthSpec = depthSpec;
                sessionDepthRoute = depthRoute;
                depthModeNow = prefs.vrDepthMode;
                nativeCtx = nativeInit(activity, videoWidth, videoHeight, prefs.fps,
                        prefs.vrDepthMode, mapSize.width, mapSize.height,
                        prefs.vrDepthDebug, prefs.vrConvergence, prefs.vrDepthScale,
                        prefs.vrHandTracking, prefs.vrSharpening, prefs.vrSupersampling);
                if (nativeCtx == 0) {
                    initLatch.countDown();
                    return;
                }

                prefsContext = activity.getApplicationContext();
                gazeAllowed = EyeTrackingPermission.gazeAllowed(prefsContext);
                // Lets the frame rate list use the runtime's rates rather than the Android
                // display's
                XrDisplayRates.remember(prefsContext, nativeGetOfferedRates(nativeCtx));
                // Kept: the panel's settings rows write back to this same instance
                prefConfig = prefs;
                nativeSetPicture(nativeCtx, prefs.vrPicture);
                nativeSetDepthRate(nativeCtx, prefs.vrDepthRate);
                nativeSetPlayerLighting(nativeCtx, playerAmbient, playerBrightness);
                nativeSetHouseLights(nativeCtx, playerHouse, true);
                nativeSetPlayerCrop(nativeCtx, playerCrop);
                nativeSetPlayerContent(nativeCtx, playerBarTop, playerBarBottom);
                nativeSetPlayerScreenHidden(nativeCtx, playerScreenHidden);
                restoreScreenPoseFresh();
                // After the saved placement, whose own curve the player overrides
                nativeSetPlayerCurve(nativeCtx, playerCurve);
                nativeSetColourMode(nativeCtx, colourMode);
                nativeSetHandAutoHide(nativeCtx, handAutoHide);
                if (videoTransfer != XrShared.VIDEO_TRANSFER_CODE_SDR) {
                    nativeSetVideoColour(nativeCtx, videoTransfer, videoMaxCll, videoMasteringNits);
                }
                startEnvironment(prefs);
                // A few milliseconds, so the first frame has it
                long splashStartNs = System.nanoTime();
                pendingSplash.set(XrSplash.buildSplash());
                LimeLog.info("Splash sheet drawn in "
                        + msPer(System.nanoTime() - splashStartNs, 1) + " ms, "
                        + SPLASH_ROWS + " rows");

                File captureDir = activity.getExternalFilesDir(null);
                if (captureDir != null) {
                    nativeSetCaptureDir(nativeCtx, captureDir.getAbsolutePath());
                }

                // The EGL context is current on this thread now
                surfaceTexture = new SurfaceTexture(nativeGetTexId(nativeCtx));
                surfaceTexture.setDefaultBufferSize(videoWidth, videoHeight);
                surfaceTexture.setOnFrameAvailableListener(XrRenderer.this);
                inputSurface = new Surface(surfaceTexture);

                if (prefs.vrDepthMode == DEPTH_MODE_MODEL) {
                    FileLog.event("depth model "+depthSpec.name+", "+depthRoute.label()
                            +", map "+mapSize);
                    startDepthThread(activity, depthSpec, depthRoute);
                }

                initOk[0] = true;
                initLatch.countDown();

                // Only when the runtime ended the loop; read before the context goes
                String endedFor = null;
                if (runFrameLoop(prefs)) {
                    String reason = nativeGetExitReason(nativeCtx);
                    endedFor = reason != null ? reason : "unknown";
                }
                stopDepthThread();

                // On the GL thread, under the lock so no other thread reaches a half-freed context.
                // The SurfaceTexture and Surface stay alive for the codec until cleanup().
                synchronized (nativeLock) {
                    long ctx = nativeCtx;
                    nativeCtx = 0;
                    nativeDestroy(ctx);
                }

                // After teardown, so the activity's stop finds this thread done
                if (endedFor != null) {
                    LimeLog.warning("VR session ended by the runtime: " + endedFor);
                    SessionListener listener = sessionListener;
                    if (listener != null) {
                        listener.onVrSessionEnded(endedFor);
                    }
                }
            }
        };
        renderThread.setName("Video - XR Renderer");
        renderThread.start();

        boolean initFinished;
        try {
            // Session setup can take a moment on a cold runtime
            initFinished = initLatch.await(INIT_WAIT_SECONDS, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            initFinished = false;
        }

        if (!initFinished || !initOk[0]) {
            LimeLog.severe("XR renderer init failed");
            XrStartFailure failure = initFinished
                    ? XrStartFailure.fromNative(nativeTakeStartFailure())
                    : XrStartFailure.timedOut(INIT_WAIT_SECONDS);
            keepStartFailure(activity, failure);
            prepareForStop();
            cleanup();
            return false;
        }

        LimeLog.info("XR renderer initialized at "+videoWidth+"x"+videoHeight);
        return true;
    }

    // Logs a failed start as one block
    private static void keepStartFailure(Activity activity, XrStartFailure failure) {
        LimeLog.severe(failure.block(null));
    }

    /**
     * Starts the depth thread. Inference outlasts a display frame, so it has its own context in
     * the render context's share group. It starts the stage thread once the model has loaded.
     */
    private void startDepthThread(final Activity activity, final MidasDepthSource.Spec spec,
                                  final MidasDepthSource.Route route) {
        depthThread = new Thread() {
            @Override
            public void run() {
                // Slightly favoured, but not BACKGROUND: that cpuset is little cores only on this
                // SoC
                Process.setThreadPriority(Process.THREAD_PRIORITY_MORE_FAVORABLE);

                if (!nativeBindDepthContext(nativeCtx)) {
                    nativeDepthGaveUp(nativeCtx);
                    return;
                }

                DepthSource source = null;
                try {
                    ByteBuffer[] inputs = new ByteBuffer[DEPTH_PAIRS];
                    ByteBuffer[] outputs = new ByteBuffer[DEPTH_PAIRS];
                    for (int i = 0; i < DEPTH_PAIRS; i++) {
                        inputs[i] = nativeGetModelInput(nativeCtx, i);
                        outputs[i] = nativeGetModelOutput(nativeCtx, i);
                        if (inputs[i] == null || outputs[i] == null) {
                            LimeLog.severe("Depth staging buffers missing");
                            nativeDepthGaveUp(nativeCtx);
                            return;
                        }
                    }

                    MidasDepthSource model = new MidasDepthSource(route);
                    source = model;
                    if (!source.initialize(activity, inputs, outputs)) {
                        // The depth texture keeps its initial flat map: zero disparity
                        LimeLog.severe("Depth source init failed, stereo will be flat");
                        nativeDepthGaveUp(nativeCtx);
                        return;
                    }

                    startDepthStage(source);
                    depthReady = true;
                    runDepthModel(source);
                } finally {
                    depthReady = false;
                    // The stage thread uses the native context too, so it stops first
                    stopDepthStage();
                    if (source != null) {
                        source.release();
                    }
                    nativeUnbindDepthContext(nativeCtx);
                }
            }
        };
        depthThread.setName("Video - XR Depth");
        depthThread.start();
    }

    /**
     * The stage thread, on a third shared context: reads captures back into model input and
     * uploads maps while the model runs on the other pair.
     */
    private void startDepthStage(final DepthSource source) {
        depthStageThread = new Thread() {
            @Override
            public void run() {
                // As the depth thread: a map waiting on this stage waits a whole model run
                Process.setThreadPriority(Process.THREAD_PRIORITY_MORE_FAVORABLE);
                if (!nativeBindDepthStageContext(nativeCtx)) {
                    LimeLog.severe("Depth stage context would not bind, stereo will stay as it is");
                    nativeDepthGaveUp(nativeCtx);
                    return;
                }
                try {
                    runDepthStage(source);
                } finally {
                    nativeUnbindDepthStageContext(nativeCtx);
                }
            }
        };
        depthStageThread.setName("Video - XR Depth stage");
        depthStageThread.start();
    }

    /** Ends the stage thread and waits for it, whatever it is part way through. */
    private void stopDepthStage() {
        Thread stage = depthStageThread;
        if (stage == null) {
            return;
        }
        synchronized (depthLock) {
            depthExit = true;
            depthLock.notifyAll();
        }
        boolean interrupted = false;
        while (stage.isAlive()) {
            try {
                stage.join();
            } catch (InterruptedException e) {
                interrupted = true;
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
        depthStageThread = null;
    }

    /** The depth thread's loop: the model, on whichever pair is staged. */
    private void runDepthModel(DepthSource source) {
        while (true) {
            int pair;
            synchronized (depthLock) {
                while (true) {
                    if (depthExit) {
                        return;
                    }
                    pair = pairs.takeToRun();
                    if (pair >= 0) {
                        break;
                    }
                    try {
                        depthLock.wait();
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
            }

            boolean ok = source.estimate(pair);
            long inferenceNs = (long)(source.getLastInferenceMs() * 1000000.0f);

            synchronized (depthLock) {
                pairInferenceNs[pair] = inferenceNs;
                pairs.ran(pair, ok);
                depthLock.notifyAll();
            }
        }
    }

    /**
     * Stage thread loop. A capture awaiting readback goes first, since the model may be waiting
     * on it; otherwise the oldest pair the model has finished is uploaded or dropped, so maps go
     * up in frame order.
     */
    private void runDepthStage(DepthSource source) {
        long runs = 0, skipped = 0;
        long inferenceNs = 0, uploadNs = 0, captureNs = 0, worstNs = 0;
        long periodNs = 0, periods = 0, lastMapNs = 0;

        while (true) {
            int pair;
            boolean finish;
            boolean ok = false;
            synchronized (depthLock) {
                while (true) {
                    if (depthExit) {
                        return;
                    }
                    pair = pairs.takeToFinish();
                    if (pair >= 0) {
                        finish = true;
                        break;
                    }
                    pair = pairs.takeToUpload();
                    if (pair >= 0) {
                        ok = pairs.made(pair);
                        finish = false;
                        break;
                    }
                    try {
                        depthLock.wait();
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
            }

            if (finish) {
                // The frame loop only queued the readback; wait for it here, off the frame loop
                long start = System.nanoTime();
                long finishNs = nativeFinishDepthCapture(nativeCtx, pair);
                synchronized (depthLock) {
                    pairStartNs[pair] = start;
                    pairFinishNs[pair] = finishNs;
                    pairInferenceNs[pair] = 0;
                    // A failed readback waits its turn and is dropped
                    pairs.finished(pair, finishNs >= 0);
                    depthLock.notifyAll();
                }
                continue;
            }

            long upload = 0;
            if (ok) {
                upload = nativeUploadDepth(nativeCtx, pair);
                publishedFrameIndex = pairFrameIndex[pair];
                publishedFrameNs = pairFrameNs[pair];
            }
            else {
                nativeDropDepth(nativeCtx, pair);
            }
            long end = System.nanoTime();
            long capture = pairCaptureNs[pair] + Math.max(0, pairFinishNs[pair]);
            long inference = pairInferenceNs[pair];
            // Readback finished to map uploaded, waits included
            long total = end - pairStartNs[pair];

            synchronized (depthLock) {
                pairs.freed(pair);
                skipped += skippedFrames;
                skippedFrames = 0;
                depthLock.notifyAll();
            }

            if (!ok) {
                continue;
            }

            long gap = lastMapNs == 0 ? 0 : end - lastMapNs;
            lastMapNs = end;
            captureNs += capture;
            inferenceNs += inference;
            uploadNs += upload;
            if (total > worstNs) {
                worstNs = total;
            }
            if (gap > 0 && gap < DEPTH_PERIOD_GAP_NS) {
                periodNs += gap;
                periods++;
            }
            if (++runs == DEPTH_STATS_INTERVAL) {
                LimeLog.info("Depth stage ("+(source.isGpuAccelerated() ? "GPU" : "CPU")
                        +"): capture "+msPer(captureNs, runs)
                        +" ms, inference "+msPer(inferenceNs, runs)
                        +" ms, upload "+msPer(uploadNs, runs)
                        +" ms, worst "+msPer(worstNs, 1)
                        +" ms, period "+(periods == 0 ? "0" : msPer(periodNs, periods))
                        +" ms, "+mapsPerSecond(periodNs, periods)
                        +" maps/s (target "+nativeGetDepthTarget(nativeCtx)
                        +"), frames skipped while busy "+skipped);
                runs = 0;
                skipped = 0;
                periods = 0;
                captureNs = inferenceNs = uploadNs = worstNs = periodNs = 0;
            }
        }
    }

    /** Maps a second over that many gaps between maps, one decimal, 0 for none. */
    private static String mapsPerSecond(long periodNs, long periods) {
        return periodNs <= 0 ? "0" : String.format("%.1f", periods * 1e9 / periodNs);
    }

    private void stopDepthThread() {
        if (depthThread == null) {
            return;
        }
        synchronized (depthLock) {
            depthExit = true;
            depthLock.notifyAll();
        }
        // The context is freed once this returns and both depth threads use it, so a slow inference
        // is waited out. The depth thread waits for the stage thread before it ends.
        boolean interrupted = false;
        try {
            depthThread.join(2000);
        } catch (InterruptedException e) {
            interrupted = true;
        }
        if (depthThread.isAlive()) {
            LimeLog.warning("XR depth thread did not stop in time, waiting for it");
            while (depthThread.isAlive()) {
                try {
                    depthThread.join();
                } catch (InterruptedException e) {
                    interrupted = true;
                }
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
        depthThread = null;
    }

    // True when the runtime ended the loop, false for a requested stop
    private boolean runFrameLoop(PreferenceConfiguration prefs) {
        float distance = prefs.vrDistance / 10.0f;
        float quadWidth = prefs.vrScreenSize / 10.0f;
        float curvature = prefs.vrCurvature / 100.0f;
        // Stored as tenths of a percent of frame width
        float separation = prefs.vrStereoSeparation / 1000.0f;
        boolean eyeSwap = prefs.vrEyeSwap;
        boolean pointer = prefs.vrPointer;
        boolean gaze = prefs.vrGaze;

        long ageFrames = 0, ageNs = 0, ageSamples = 0, worstAgeNs = 0;
        // When the 3D last came back on. A live map captured before then would measure how long 3D
        // was off, not the depth lag.
        long stereoBackNs = 0;

        while (!stopping) {
            int[] next = pendingReconfig.getAndSet(null);
            if (next != null) {
                applyReconfigure(next);
            }
            int r = nativeWaitBeginFrame(nativeCtx);
            if (r == FRAME_EXIT) {
                return !stopping;
            }
            trackPresence();
            if (r == FRAME_IDLE) {
                // The native side already slept
                continue;
            }

            // Read every frame: the panel's row writes back to this object, so a press applies on
            // the next frame with no native state to sync
            boolean headLocked = prefs.vrHeadLocked;

            // Pointer sleep, likewise read fresh each frame
            nativeUpdateInput(nativeCtx, distance, quadWidth, curvature, headLocked,
                    pointer, gaze && gazeAllowed, prefs.vrPointerSleep, inputState);
            dispatchInput();

            // Off: the warp draws flat and the model idles. Back on: capture the frame in hand at
            // once, whatever the depth rate says.
            boolean stereoOn = inputState[IN_STEREO] != 0.0f;
            boolean stereoBack = stereoOn && !stereoLive;
            stereoLive = stereoOn;
            if (stereoBack) {
                stereoBackNs = System.nanoTime();
            }

            boolean newFrame = pendingFrames.getAndSet(0) > 0;
            if (newFrame) {
                surfaceTexture.updateTexImage();
                surfaceTexture.getTransformMatrix(texMatrix);

                if (depthReady && stereoOn) {
                    // By time since the last capture, so the model's rate does not follow the
                    // video's
                    if (nativeDepthDue(nativeCtx, stereoBack)) {
                        startDepthCapture();
                    }
                    if (publishedFrameNs != 0 && publishedFrameNs >= stereoBackNs) {
                        long age = System.nanoTime() - publishedFrameNs;
                        ageFrames += videoFrameIndex - publishedFrameIndex;
                        ageNs += age;
                        ageSamples++;
                        if (age > worstAgeNs) {
                            worstAgeNs = age;
                        }
                        if (ageSamples == DEPTH_AGE_INTERVAL) {
                            LimeLog.info("Depth age: "+String.format("%.1f", ageFrames
                                    / (double)ageSamples)+" video frames, "
                                    +msPer(ageNs, ageSamples)+" ms avg, "
                                    +msPer(worstAgeNs, 1)+" ms worst");
                            ageFrames = ageNs = ageSamples = worstAgeNs = 0;
                        }
                    }
                }
                videoFrameIndex++;
            }
            else if ((stereoBack || redrawLatched) && depthReady && stereoOn && videoFrameIndex > 0) {
                // Nothing new from the decoder, so capture the frame still latched
                nativeDepthDue(nativeCtx, true);
                startDepthCapture();
            }
            boolean drawFrame = newFrame || redrawLatched;
            redrawLatched = false;
            ByteBuffer splash = pendingSplash.getAndSet(null);
            if (splash != null) {
                nativeUploadSplash(nativeCtx, splash);
            }
            uploadPendingUiSheets();

            ByteBuffer controller = pendingControllerModel.getAndSet(null);
            if (controller != null) {
                nativeUploadControllerModel(nativeCtx, controller, controller.remaining());
            }

            nativeEndFrame(nativeCtx, drawFrame, texMatrix, distance, quadWidth, curvature,
                    headLocked, separation, eyeSwap, passthroughOn);
        }
        return false;
    }

    // Passthrough or the dark void, as saved. The controller model is read on another thread,
    // which would otherwise hold up the first frame.
    private void startEnvironment(PreferenceConfiguration prefs) {
        SharedPreferences saved = PreferenceManager.getDefaultSharedPreferences(prefsContext);
        int id = saved.getInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING,
                PreferenceConfiguration.VR_ENV_PASSTHROUGH);
        boolean passthrough = id == PreferenceConfiguration.VR_ENV_PASSTHROUGH;
        // If the headset starts apps in passthrough, the player does too; the saved choice is kept
        if (!passthrough && nativeSystemPrefersPassthrough(nativeCtx)) {
            FileLog.event("the headset starts apps in passthrough, so the player does");
            passthrough = true;
        }
        passthroughOn = passthrough;
        nativeSetShowRay(nativeCtx, prefs.vrShowRay);
        nativeSetControllerModel(nativeCtx, prefs.vrControllerModel);

        Thread loader = new Thread() {
            @Override
            public void run() {
                pendingControllerModel.set(readAsset(CONTROLLER_MODEL));
            }
        };
        loader.setName("Video - XR Environment");
        loader.start();
    }

    // Direct buffer, so the native side can read it without a copy
    private ByteBuffer readAsset(String path) {
        InputStream in = null;
        try {
            in = prefsContext.getAssets().open(path);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] chunk = new byte[16384];
            int read;
            while ((read = in.read(chunk)) > 0) {
                out.write(chunk, 0, read);
            }
            byte[] all = out.toByteArray();
            ByteBuffer buffer = ByteBuffer.allocateDirect(all.length);
            buffer.put(all);
            buffer.rewind();
            return buffer;
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Asset " + path + " failed: " + e);
            return null;
        } finally {
            XrSplash.closeQuietly(in);
        }
    }

    // Pointer moves go before presses, so a click lands where the pointer is now
    private void dispatchInput() {
        UiListener ui = uiListener;
        if (ui != null) {
            ui.onUiPointer((int)inputState[IN_UI_PANEL], inputState[IN_UI_U], inputState[IN_UI_V],
                           inputState[IN_UI_PRESS] != 0.0f);
        }
        // Listener events; the screen placement and environment grid below are handled either way
        if (inputListener != null) {
            if (inputState[IN_HIT] != 0.0f) {
                inputListener.onVrPointerMove(inputState[IN_U], inputState[IN_V]);
            }
            int buttons = (int)inputState[IN_BUTTONS];
            int changed = buttons ^ heldButtons;
            if (changed != 0) {
                for (int i = 0; i < 3; i++) {
                    int mask = 1 << i;
                    if ((changed & mask) != 0) {
                        inputListener.onVrButton(i, (buttons & mask) != 0);
                    }
                }
                heldButtons = buttons;
            }

            int clicks = (int)inputState[IN_SCROLL];
            if (clicks != 0) {
                inputListener.onVrScroll(clicks);
            }
            inputListener.onVrPlayerInput((int)inputState[IN_PLAYER_BUTTONS],
                    inputState[IN_PLAYER_STICK_X]);
        }

        if (inputState[IN_POSE_DIRTY] != 0.0f) {
            saveScreenPose();
        }
    }

    // Reports focus changes, and the session leaving and returning after its first focus.
    // Frame loop only.
    private void trackPresence() {
        int presence = nativeGetPresence(nativeCtx);
        SessionListener listener = sessionListener;
        // Focus is lost to the system menu or a notice as well as to the headset coming off
        boolean focused = presence == PRESENCE_FOCUSED;
        if (focused) {
            everFocusedSeen = true;
        }
        if (everFocusedSeen && focused != sessionFocused) {
            sessionFocused = focused;
            LimeLog.info("VR session focus " + (focused ? "gained" : "lost"));
            if (listener != null) {
                if (focused) {
                    listener.onVrFocusGained();
                }
                else {
                    listener.onVrFocusLost();
                }
            }
        }
        if (presence == PRESENCE_AWAY && !sessionAway) {
            sessionAway = true;
            LimeLog.info("VR session stopped after its first focus");
            if (listener != null) {
                listener.onVrSessionAway();
            }
        }
        else if (presence == PRESENCE_FOCUSED && sessionAway) {
            sessionAway = false;
            LimeLog.info("VR session focused again");
            if (listener != null) {
                listener.onVrSessionBack();
            }
        }
    }

    // Written when a grab ends. Cleared by the reset in settings.
    private void saveScreenPose() {
        if (prefsContext == null) {
            return;
        }

        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < POSE_VALUES; i++) {
            if (i > 0) {
                sb.append(',');
            }
            sb.append(inputState[IN_POSE + i]);
        }

        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                .putString(screenPoseKey(), sb.toString())
                .apply();
    }

    // Placements restored this session; the first restore of each is aimed straight ahead,
    // since the save was made under an earlier session's origin
    private final java.util.Set<String> placedKeys = new java.util.HashSet<>();

    private void restoreScreenPoseFresh() {
        restoreScreenPose();
        if (placedKeys.add(screenPoseKey())) {
            nativeRecentreScreen(nativeCtx);
        }
    }

    private void restoreScreenPose() {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(prefsContext);
        String saved = prefs.getString(screenPoseKey(), null);
        // First time in the library: start where the film screen is, at the library's share of its
        // width, so nothing jumps
        boolean browseFromFilm = false;
        if (saved == null && playerScreenHidden) {
            saved = prefs.getString(PLAYER_SCREEN_POSE_PREF, null);
            browseFromFilm = saved != null;
        }
        if (saved == null) {
            return;
        }

        String[] parts = saved.split(",");
        // Older saves lack the curvature; -1 means none chosen
        if (parts.length < POSE_VALUES - 1) {
            return;
        }

        float[] pose = new float[POSE_VALUES];
        try {
            for (int i = 0; i < POSE_VALUES; i++) {
                pose[i] = i < parts.length ? Float.parseFloat(parts[i]) : -1.0f;
            }
        } catch (NumberFormatException e) {
            return;
        }
        if (browseFromFilm) {
            // Position, orientation, then the width
            pose[7] *= XrShared.PLAYER_BROWSE_START_WIDTH;
        }

        nativeSetScreenPose(nativeCtx, pose);
    }

    /**
     * Queues a downscaled GPU copy of the latched frame into a free pair and wakes the stage
     * thread, which waits for the pixels in nativeFinishDepthCapture. Runs on the frame loop
     * because it samples this context's video texture.
     */
    private void startDepthCapture() {
        int pair;
        synchronized (depthLock) {
            pair = pairs.forCapture();
            if (pair < 0) {
                skippedFrames++;
                return;
            }
        }

        // Only this thread hands out free pairs, so the pair is this capture's until marked below
        pairCaptureNs[pair] = nativeCaptureDepthInput(nativeCtx, texMatrix, pair);
        pairFrameIndex[pair] = videoFrameIndex;
        pairFrameNs[pair] = System.nanoTime();

        synchronized (depthLock) {
            pairs.captured(pair);
            depthLock.notifyAll();
        }
    }

    private static String msPer(long totalNs, long count) {
        return String.format("%.2f", totalNs / (double)count / 1000000.0);
    }

    /**
     * A new film in the running session, or a new layout for the same one, whose decoder keeps
     * running: frame size per eye, frame rate, depth mode, whether the screen is hidden
     * (library) and whether 3D is on (a stereo source can play flat). Applied between
     * frames. Any thread.
     */
    public void reconfigure(int width, int height, int fps, int depthMode, boolean screenHidden,
                            boolean sameFilm, boolean stereoOn) {
        SurfaceTexture texture = surfaceTexture;
        if (texture != null && !sameFilm) {
            texture.setDefaultBufferSize(width, height);
        }
        pendingReconfig.set(new int[] { width, height, fps, depthMode, screenHidden ? 1 : 0,
                sameFilm ? 1 : 0, stereoOn ? 1 : 0 });
    }

    private void applyReconfigure(int[] next) {
        int mode = next[3];
        boolean hidden = next[4] != 0;
        if (depthModeNow == DEPTH_MODE_MODEL && mode != DEPTH_MODE_MODEL) {
            stopDepthThread();
        }
        if (hidden != playerScreenHidden) {
            saveScreenPose();
            playerScreenHidden = hidden;
            nativeSetPlayerScreenHidden(nativeCtx, hidden);
            restoreScreenPoseFresh();
        }
        boolean sameFilm = next[5] != 0;
        // Drop a frame of the last film still queued, so it never shows on the new screen
        if (!sameFilm && pendingFrames.getAndSet(0) > 0) {
            surfaceTexture.updateTexImage();
        }
        if (!nativeReconfigure(nativeCtx, next[0], next[1], next[2], mode, sameFilm, next[6] != 0)) {
            LimeLog.severe("XR renderer: could not set up for the new film");
        }
        redrawLatched = sameFilm && videoFrameIndex > 0;
        if (mode == DEPTH_MODE_MODEL && depthThread == null) {
            synchronized (depthLock) {
                depthExit = false;
                pairs.reset();
            }
            startDepthThread(sessionActivity, sessionDepthSpec, sessionDepthRoute);
        }
        depthModeNow = mode;
        if (prefConfig != null) {
            prefConfig.vrDepthMode = mode;
        }
    }

    /** A fresh Surface for the next decoder, once the last one has let go of the old. Main thread. */
    public Surface newInputSurface() {
        if (surfaceTexture == null) {
            return inputSurface;
        }
        if (inputSurface != null) {
            inputSurface.release();
        }
        inputSurface = new Surface(surfaceTexture);
        return inputSurface;
    }

    public Surface getInputSurface() {
        return inputSurface;
    }

    // Any thread
    @Override
    public void onFrameAvailable(SurfaceTexture st) {
        pendingFrames.incrementAndGet();
    }

    /**
     * Stops the frame loop and destroys the OpenXR session. The codec-facing
     * surface stays valid until cleanup(). The join is bounded by one
     * xrWaitFrame period plus teardown.
     */
    public void prepareForStop() {
        stopping = true;

        if (renderThread != null) {
            try {
                renderThread.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (renderThread.isAlive()) {
                LimeLog.warning("XR render thread did not stop in time");
            }
        }
    }

    /**
     * Releases the surface handed to MediaCodec; call after the codec is released. If the frame
     * loop is still running, it releases the surface on its way out.
     */
    public void cleanup() {
        boolean releaseNow;
        synchronized (teardownLock) {
            releaseNow = renderThread == null || renderThreadDone;
            if (!releaseNow) {
                releaseOnExit = true;
            }
        }
        if (releaseNow) {
            releaseSurfaces();
        }
    }

    // The last thing the frame loop thread does, whichever way it ended
    private void finishRenderThread() {
        boolean release;
        synchronized (teardownLock) {
            renderThreadDone = true;
            release = releaseOnExit;
        }
        if (release) {
            releaseSurfaces();
        }
    }

    private void releaseSurfaces() {
        if (inputSurface != null) {
            inputSurface.release();
            inputSurface = null;
        }
        if (surfaceTexture != null) {
            surfaceTexture.release();
            surfaceTexture = null;
        }
    }
}
