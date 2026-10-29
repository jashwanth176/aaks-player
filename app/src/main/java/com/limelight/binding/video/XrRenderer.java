package com.limelight.binding.video;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.SurfaceTexture;
import android.os.Build;
import android.os.Process;
import android.preference.PreferenceManager;
import android.text.TextUtils;
import android.view.Surface;

import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.binding.input.EyeTrackingPermission;
import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.preferences.XrDisplayRates;
import com.limelight.utils.BugReport;

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
    // Gamepad mode's pad as last reported: plugged in, then the IN_PAD_ values. Frame loop only.
    private boolean padPlugged;
    private final int[] padSent = new int[IN_PAD_RY - IN_PAD_BUTTONS + 1];
    private InputListener inputListener;
    private volatile SessionListener sessionListener;
    private Context prefsContext;
    private PreferenceConfiguration prefConfig;

    // Baked rooms, a mesh and its atlases each, by picker cell (roomMeshFile, roomAtlasFiles)
    private static final String ROOM_DIR = "rooms";
    // Drawn at each hand when the setting is on, painted from its vertex colours
    private static final String CONTROLLER_MODEL = "models/controller.room";

    // Panel art drawn by XrPanels on the loader thread, uploaded by the frame loop, which owns
    // the GL context. The splash is drawn before the frame loop starts.
    private final AtomicReference<ByteBuffer> pendingSplash = new AtomicReference<>();
    private final AtomicReference<ByteBuffer[]> pendingKbSheets = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbButton = new AtomicReference<>();
    // Built next to the art and read on the frame loop when it uploads
    private volatile float[] kbKeyRects;
    private volatile int[][] kbCodes;
    // Modifiers each keyboard sheet was last drawn with, and those reported held. Frame loop only.
    private final int[] kbSheetMods = new int[KB_STATE_COUNT];
    private boolean kbArtUp;
    private int heldKbMods;
    // A sheet redrawn off the frame loop, one at a time
    private final AtomicReference<KbSheet> pendingKbSheet = new AtomicReference<>();
    private boolean kbSheetDrawing;

    // Report sheet state, frame loop only. The sheet is redrawn on its own thread when its
    // contents change, and uploaded only if still for the current opening.
    private final XrReportForm reportForm = new XrReportForm();
    private volatile XrPanels.ReportSheet reportArt;
    private int reportOpening;
    private String reportDrawn;
    private boolean reportDrawing;
    private boolean reportSendHanded;
    private final AtomicReference<ReportDrawing> pendingReport = new AtomicReference<>();

    private final AtomicReference<ByteBuffer> pendingExitButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingExitPlain = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingExitHot = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCancelHot = new AtomicReference<>();

    private final AtomicReference<ByteBuffer> pendingPickerArt = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingEnvButton = new AtomicReference<>();
    // Every sheet of the settings panel, in COG_ART_ order
    private final AtomicReference<ByteBuffer[]> pendingCogSheets = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogButton = new AtomicReference<>();
    // Values beside the Room or Picture tab's tracks, and the values last drawn
    private XrPanels.Readout cogReadout;
    private final int[] readoutDrawn = new int[READOUT_VALUES];
    private final int[] readoutWanted = new int[READOUT_VALUES];
    // Toast sheet. Notices raised from Java are queued from any thread, then held in slots the
    // native side refers to.
    private XrPanels.Toast toast;
    // Marks on the display tab's cells, and the cells last drawn
    private XrPanels.Marks cogMarks;
    // Time and battery for the strip over the settings panel, and the line last drawn
    private volatile XrClock clock;
    private XrPanels.ClockStrip cogClock;
    private String cogClockDrawn;
    private final int[] marksDrawn = new int[MARK_VALUES];
    private final int[] marksWanted = new int[MARK_VALUES];
    // Click sound: built once off the frame loop, fed from it. A sound ready after the session
    // has ended is released. Handed over under clickLock.
    private final Object clickLock = new Object();
    private volatile XrClickSound clickSound;
    private boolean clickSoundStarting;
    private boolean clickSessionOver;
    private final ConcurrentLinkedQueue<NoticeWords> pendingNotices = new ConcurrentLinkedQueue<>();
    private final NoticeWords[] noticeTexts = new NoticeWords[TOAST_TEXT_SLOTS];
    private int noticeSlot;
    // The hand lock hint's sheet, only drawn in a session that may show it
    private final AtomicReference<ByteBuffer> pendingHandHint = new AtomicReference<>();
    // The 3D switch's two faces, only drawn in a session with stereo to switch
    private final AtomicReference<ByteBuffer> pendingStereoOff = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingStereoOn = new AtomicReference<>();
    // The ray toggle's two faces
    private final AtomicReference<ByteBuffer> pendingRayOff = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingRayOn = new AtomicReference<>();
    // Head aim toggle's two faces
    private final AtomicReference<ByteBuffer> pendingAimOff = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingAimOn = new AtomicReference<>();
    // Gamepad mode toggle's two faces
    private final AtomicReference<ByteBuffer> pendingPadOff = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingPadOn = new AtomicReference<>();
    // A baked room on its way to the GPU. Mesh, atlases and cell travel together, so a room picked
    // mid-read never leaves the native side with half of each. The void shows until it lands.
    private static final class RoomAssets {
        final int cell;
        final ByteBuffer mesh;
        final int meshBytes;
        // Whole .atlas files, in the slot order the mesh's parts use
        final ByteBuffer[] atlases;

        RoomAssets(int cell, ByteBuffer mesh, ByteBuffer[] atlases) {
            this.cell = cell;
            this.mesh = mesh;
            this.meshBytes = mesh.remaining();
            this.atlases = atlases;
        }
    }

    private final AtomicReference<RoomAssets> pendingRoom = new AtomicReference<>();
    // Read every session, shown or not, so the Display tab can switch it on at once
    private final AtomicReference<ByteBuffer> pendingControllerModel = new AtomicReference<>();
    // Only the room on screen is resident. Each pick takes a new ticket, so a stale read is never
    // parked over a newer pick. Guarded by roomLock.
    private final Object roomLock = new Object();
    private int roomTicket;
    private int parkedRoomCell = -1;
    private XrPanels panels;
    private volatile int environmentChoice = ENV_CELL_VOID;
    private volatile boolean passthroughOn;

    /**
     * Pointer events from the VR session, on the frame loop thread. Buttons: 0 left, 1 right, 2
     * middle.
     */
    public interface InputListener {
        void onVrPointerMove(float u, float v);
        // Relative motion, right and down positive: head aim's turn plus pointer nudges
        void onVrMouseMove(int dx, int dy);
        void onVrButton(int button, boolean down);
        void onVrScroll(int clicks);
        // Player controller reads every frame: face buttons as bits (A 1, B 2, X 4, Y 8) and
        // thumbstick X, -1 to 1
        default void onVrPlayerInput(int buttons, float stickX) {
        }
        // Gamepad mode: both controllers as one Xbox pad, plugged in before its first state and
        // released before unplugging. PAD_ button bits, triggers 0 to 255, sticks -32766 to 32766,
        // up positive.
        void onVrGamepadPlugged(boolean plugged);
        void onVrGamepadState(int buttons, int leftTrigger, int rightTrigger,
                              int leftX, int leftY, int rightX, int rightY);
        // A key from the in-world keyboard: Unicode with shift applied, backspace, tab, enter and
        // space as control codes, or a virtual key code above KB_CODE_VK. modifiers are KB_MOD_
        // bits.
        void onVrKey(int code, int modifiers);
        // Ctrl, Alt and Win as lit on the keyboard now, KB_MOD_ bits
        void onVrModifiers(int modifiers);
        // Exit prompt confirmed
        void onVrExit();
    }

    public void setInputListener(InputListener listener) {
        this.inputListener = listener;
    }

    // Player session: the player's own panels and saved screen placement. Set before start.
    private boolean playerMode;
    private static final String PLAYER_SCREEN_POSE_PREF = "player_screen_pose";

    public void setPlayerMode(boolean on) {
        playerMode = on;
    }

    private volatile float playerAmbient = 1.0f;
    private volatile float playerBrightness = 0.4f;
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

    // The library without a film keeps its own placement and size, apart from the film screen's
    private static final String PLAYER_BROWSE_POSE_PREF = "player_browse_pose";

    private String screenPoseKey() {
        if (!playerMode) {
            return PreferenceConfiguration.VR_SCREEN_POSE_PREF_STRING;
        }
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
     * Where a panel hangs off the screen and whether it shows: a UI_ANCHOR_,
     * its width as a share of the screen's, and its gap from that edge (or
     * towards the viewer, for the front) as a share of the screen's width.
     * Any thread.
     */
    public void setUiPanel(int panel, boolean visible, int anchor, float widthFrac, float gapFrac) {
        if (panel < 0 || panel >= UI_PANEL_COUNT) {
            return;
        }
        // Applied on the frame loop after any new picture, so a panel never shows the old picture
        // at the new size
        pendingUiPlacements.set(panel, new float[] { visible ? 1 : 0, anchor, widthFrac, gapFrac });
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

    // Requested environment, an ENV_CELL_ or -1, applied on the frame loop
    private final java.util.concurrent.atomic.AtomicInteger pendingEnvironmentCell =
            new java.util.concurrent.atomic.AtomicInteger(-1);

    /** Passthrough, or the dark void instead of it. Any thread. */
    public void setPassthrough(boolean on) {
        pendingEnvironmentCell.set(on ? ENV_CELL_PASSTHROUGH : ENV_CELL_VOID);
    }

    /** Whether the session is showing passthrough now. */
    public boolean isPassthroughOn() {
        return passthroughOn;
    }

    private void uploadPendingUiSheets() {
        int cell = pendingEnvironmentCell.getAndSet(-1);
        if (cell >= 0) {
            chooseEnvironment(cell);
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
                nativeSetUiPanel(nativeCtx, i, place[0] != 0, (int) place[1], place[2], place[3]);
            }
        }
    }

    // Told when the session cannot start or the runtime ends it
    public void setSessionListener(SessionListener listener) {
        this.sessionListener = listener;
    }

    /**
     * Shows a notice on the in-session toast: a line, and a quieter one under it or null. Any
     * thread; it goes up on the next frame.
     */
    public void showNotice(String text, String more) {
        showNotice(text, more, false);
    }

    // As above; a path second line is cut from the middle so both ends stay visible
    private void showNotice(String text, String more, boolean path) {
        if (text != null) {
            pendingNotices.add(new NoticeWords(text, more, path));
        }
    }

    // A notice raised from Java, waiting its turn
    private static final class NoticeWords {
        final String text;
        final String more;
        final boolean path;

        NoticeWords(String text, String more, boolean path) {
            this.text = text;
            this.more = more;
            this.path = path;
        }
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
    // envResTier: 0 low, 1 standard, 2 high, 3 ultra. fps is the video's; the display rate is
    // matched to it.
    private native long nativeInit(Activity activity, int width, int height, int fps,
                                   int stereoMode, int depthWidth, int depthHeight,
                                   boolean depthDebug, int convergence, int depthScale,
                                   boolean handTracking, int sharpenMode, int supersampleMode,
                                   boolean perfOverlay,
                                   boolean ambilight, int ambiLevel, boolean roomLight,
                                   int envResTier);
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
    // The cell the room belongs to, which the native side maps to its room style
    private native void nativeUploadRoomModel(long ctx, ByteBuffer mesh, int length, int cell);
    private native void nativeUploadRoomAtlas(long ctx, ByteBuffer atlas, int cell, int slot);
    // The controller model's .room file, whole
    private native void nativeUploadControllerModel(long ctx, ByteBuffer mesh, int length);
    // Whether the controller model is drawn; the Display tab's row changes it
    private native void nativeSetControllerModel(long ctx, boolean on);
    private native void nativeUploadPicker(long ctx, ByteBuffer grid, ByteBuffer button, int cells);
    private native void nativeUploadCog(long ctx, ByteBuffer[] sheets, ByteBuffer button);
    private native void nativeUploadCogReadout(long ctx, ByteBuffer strip, int[] values);
    // One room's own Room tab values, by the picker cell that shows it
    private native void nativeSetRoomLevels(long ctx, int cell, int brightness, boolean glow,
                                            int light, int screen);
    // The running model's default separation and convergence (preference units) for the 3D
    // tab's reset, and the preset separations in cell order
    private native void nativeSetDepthDefaults(long ctx, int separation, int convergence,
                                               int[] presets);
    // Brightness, contrast, gamma and saturation, in PICTURE_ order and units
    private native void nativeSetPicture(long ctx, int[] picture);
    // The sheets and the code tables in KB_STATE_ order
    private native void nativeUploadKeyboard(long ctx, ByteBuffer[] sheets, ByteBuffer buttonIcon,
                                             float[] keyRects, int[][] codes);
    private native void nativeUploadKeyboardSheet(long ctx, int state, ByteBuffer sheet);
    private native void nativeUploadReport(long ctx, ByteBuffer sheet);
    private native void nativeSetReportSend(long ctx, boolean ready);
    private native void nativeUploadExit(long ctx, ByteBuffer button, ByteBuffer promptPlain,
                                         ByteBuffer promptExitHot, ByteBuffer promptCancelHot);
    private native boolean nativeGetCylinderSupported(long ctx);
    // A PRESENCE_ value
    private native int nativeGetPresence(long ctx);
    // Whether the headset starts apps in passthrough
    private native boolean nativeSystemPrefersPassthrough(long ctx);
    // Why the runtime ended the frame loop, or null
    private native String nativeGetExitReason(long ctx);
    private native String nativeGetRuntime(long ctx);
    private native void nativeUploadHandHint(long ctx, ByteBuffer sheet);
    // Whether this session may show the hand lock hint, before the first frame
    private native void nativeSetHandHint(long ctx, boolean wanted);
    private native void nativeSetPlayerMode(long ctx, boolean on);
    private native void nativeSetPlayerLighting(long ctx, float ambient, float brightness);
    private native void nativeSetPlayerContent(long ctx, float top, float bottom);
    private native void nativeSetPlayerScreenHidden(long ctx, boolean hidden);
    private native void nativeSetPlayerCrop(long ctx, float crop);
    private native void nativeSetPlayerCurve(long ctx, float curve);
    private native void nativeSetColourMode(long ctx, int mode);
    private native void nativeSetHandAutoHide(long ctx, boolean on);
    private native void nativeSetVideoColour(long ctx, int transfer, float maxCll, float masteringNits);
    private native void nativeUploadUiPanel(long ctx, int panel, ByteBuffer sheet, int width, int height);
    private native void nativeSetUiPanel(long ctx, int panel, boolean visible, int anchor,
                                         float widthFrac, float gapFrac);
    private native void nativeSetUiPanelDepth(long ctx, int panel, float k);
    private native void nativeUploadStereoButton(long ctx, ByteBuffer off, ByteBuffer on);
    private native void nativeUploadRayButton(long ctx, ByteBuffer off, ByteBuffer on);
    private native void nativeUploadAimButton(long ctx, ByteBuffer off, ByteBuffer on);
    private native void nativeUploadPadButton(long ctx, ByteBuffer off, ByteBuffer on);
    private native void nativeUploadSplash(long ctx, ByteBuffer sheet);
    // The toast sheet for a notice just shown, and a Java notice to queue (TOAST_TEXT and its slot)
    private native void nativeUploadToast(long ctx, ByteBuffer sheet, int kind, int arg);
    private native void nativePushNotice(long ctx, int kind, int arg);
    private native void nativeUploadCogMarks(long ctx, ByteBuffer strip);
    private native void nativeUploadCogClock(long ctx, ByteBuffer strip);
    // Whether a press clicks
    private native void nativeSetClickSound(long ctx, boolean on);
    // Whether sessions start with the ray drawn
    private native void nativeSetShowRay(long ctx, boolean on);
    // Head aim at session start: sensitivity in pixels per degree, dead zone in degrees per second
    private native void nativeSetHeadAim(long ctx, boolean on, int sensitivity, int deadZone);
    // Gamepad mode's shortcut, and the sticks' dead zone in whole percent
    private native void nativeSetGamepad(long ctx, int shortcut, int deadzonePercent);
    // The depth model will make no maps this session, so the splash stops waiting. Any thread.
    private native void nativeDepthGaveUp(long ctx);
    private native void nativeSetEnvironment(long ctx, int choice);
    // The rate the display is on
    private native float nativeGetDisplayRate(long ctx);
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
                nativeCtx = nativeInit(activity, videoWidth, videoHeight, prefs.fps,
                        prefs.vrDepthMode, mapSize.width, mapSize.height,
                        prefs.vrDepthDebug, prefs.vrConvergence, prefs.vrDepthScale,
                        prefs.vrHandTracking, prefs.vrSharpening, prefs.vrSupersampling,
                        prefs.enablePerfOverlay,
                        prefs.vrAmbilight, prefs.vrAmbilightLevel, prefs.vrRoomLight,
                        prefs.vrEnvResTier);
                if (nativeCtx == 0) {
                    initLatch.countDown();
                    return;
                }

                prefsContext = activity.getApplicationContext();
                gazeAllowed = EyeTrackingPermission.gazeAllowed(prefsContext);
                // Kept for a report made after this session; a successful start clears an earlier
                // failure
                SharedPreferences.Editor kept =
                        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                                .remove(BugReport.START_FAILURE_PREF);
                String runtime = nativeGetRuntime(nativeCtx);
                if (runtime != null) {
                    kept.putString(BugReport.RUNTIME_PREF, runtime);
                }
                kept.apply();
                // Lets the frame rate list use the runtime's rates rather than the Android
                // display's
                XrDisplayRates.remember(prefsContext, nativeGetOfferedRates(nativeCtx));
                // Kept: the panel's settings rows write back to this same instance
                prefConfig = prefs;
                nativeSetDepthDefaults(nativeCtx, depthSpec.defaultSeparation,
                        depthSpec.defaultConvergence,
                        DepthPresets.values(depthSpec.defaultSeparation));
                nativeSetPicture(nativeCtx, prefs.vrPicture);
                nativeSetDepthRate(nativeCtx, prefs.vrDepthRate);
                nativeSetPlayerMode(nativeCtx, playerMode);
                if (playerMode) {
                    nativeSetPlayerLighting(nativeCtx, playerAmbient, playerBrightness);
                    nativeSetPlayerCrop(nativeCtx, playerCrop);
                    nativeSetPlayerContent(nativeCtx, playerBarTop, playerBarBottom);
                    nativeSetPlayerScreenHidden(nativeCtx, playerScreenHidden);
                }
                restoreScreenPose();
                if (playerMode) {
                    // After the saved placement, whose own curve the player overrides
                    nativeSetPlayerCurve(nativeCtx, playerCurve);
                }
                nativeSetColourMode(nativeCtx, colourMode);
                nativeSetHandAutoHide(nativeCtx, handAutoHide);
                if (videoTransfer != XrShared.VIDEO_TRANSFER_CODE_SDR) {
                    nativeSetVideoColour(nativeCtx, videoTransfer, videoMaxCll, videoMasteringNits);
                }
                startEnvironment(prefs);
                // A few milliseconds, so the first frame has it
                long splashStartNs = System.nanoTime();
                pendingSplash.set(panels.buildSplash());
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
                // The pad unplugs with the session
                unplugPad();

                stopDepthThread();
                XrClickSound click;
                synchronized (clickLock) {
                    clickSessionOver = true;
                    click = clickSound;
                    clickSound = null;
                }
                if (click != null) {
                    click.release();
                }

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

    // Logs a failed start as one block and keeps it for a later report
    private static void keepStartFailure(Activity activity, XrStartFailure failure) {
        LimeLog.severe(failure.block(null));
        String when = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        PreferenceManager.getDefaultSharedPreferences(activity.getApplicationContext()).edit()
                .putString(BugReport.START_FAILURE_PREF, failure.block(when)).apply();
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
            int r = nativeWaitBeginFrame(nativeCtx);
            if (r == FRAME_EXIT) {
                return !stopping;
            }
            trackPresence();
            if (r == FRAME_IDLE) {
                // The native side already slept. The click track is fed anyway so it plays on
                // resume.
                XrClickSound click = clickSound;
                if (click != null) {
                    click.feed();
                }
                continue;
            }

            // Read every frame: the panel's row writes back to this object, so a press applies on
            // the next frame with no native state to sync
            boolean headLocked = prefs.vrHeadLocked;

            // Pointer sleep, likewise read fresh each frame
            nativeUpdateInput(nativeCtx, distance, quadWidth, curvature, headLocked,
                    pointer, gaze && gazeAllowed, prefs.vrPointerSleep, inputState);
            dispatchInput();
            updateReport();
            updateCogReadout();
            updateCogMarks();
            updateCogClock();
            updateKeyboardSheet();
            updateToast();
            updateClickSound(prefs);

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
            else if (stereoBack && depthReady && videoFrameIndex > 0) {
                // Nothing new from the decoder, so capture the frame still latched
                nativeDepthDue(nativeCtx, true);
                startDepthCapture();
            }
            ByteBuffer splash = pendingSplash.getAndSet(null);
            if (splash != null) {
                nativeUploadSplash(nativeCtx, splash);
            }
            if (playerMode) {
                uploadPendingUiSheets();
            }

            ByteBuffer grid = pendingPickerArt.getAndSet(null);
            ByteBuffer button = pendingEnvButton.getAndSet(null);
            if (grid != null || button != null) {
                nativeUploadPicker(nativeCtx, grid, button, ENV_CELL_COUNT);
            }

            ByteBuffer[] cogSheets = pendingCogSheets.getAndSet(null);
            ByteBuffer cog = pendingCogButton.getAndSet(null);
            if (cogSheets != null || cog != null) {
                nativeUploadCog(nativeCtx, cogSheets, cog);
            }

            ByteBuffer[] kbSheets = pendingKbSheets.getAndSet(null);
            ByteBuffer kbButton = pendingKbButton.getAndSet(null);
            if (kbSheets != null || kbButton != null) {
                nativeUploadKeyboard(nativeCtx, kbSheets, kbButton, kbKeyRects, kbCodes);
                kbArtUp |= kbSheets != null;
            }

            ByteBuffer exitButton = pendingExitButton.getAndSet(null);
            ByteBuffer exitPlain = pendingExitPlain.getAndSet(null);
            ByteBuffer exitHot = pendingExitHot.getAndSet(null);
            ByteBuffer cancelHot = pendingCancelHot.getAndSet(null);
            if (exitButton != null || exitPlain != null || exitHot != null || cancelHot != null) {
                nativeUploadExit(nativeCtx, exitButton, exitPlain, exitHot, cancelHot);
            }

            ByteBuffer hint = pendingHandHint.getAndSet(null);
            if (hint != null) {
                nativeUploadHandHint(nativeCtx, hint);
            }

            ByteBuffer stereoOff = pendingStereoOff.getAndSet(null);
            ByteBuffer stereoOnArt = pendingStereoOn.getAndSet(null);
            if (stereoOff != null && stereoOnArt != null) {
                nativeUploadStereoButton(nativeCtx, stereoOff, stereoOnArt);
            }

            ByteBuffer rayOff = pendingRayOff.getAndSet(null);
            ByteBuffer rayOn = pendingRayOn.getAndSet(null);
            if (rayOff != null && rayOn != null) {
                nativeUploadRayButton(nativeCtx, rayOff, rayOn);
            }

            ByteBuffer aimOff = pendingAimOff.getAndSet(null);
            ByteBuffer aimOn = pendingAimOn.getAndSet(null);
            if (aimOff != null && aimOn != null) {
                nativeUploadAimButton(nativeCtx, aimOff, aimOn);
            }

            ByteBuffer padOff = pendingPadOff.getAndSet(null);
            ByteBuffer padOn = pendingPadOn.getAndSet(null);
            if (padOff != null && padOn != null) {
                nativeUploadPadButton(nativeCtx, padOff, padOn);
            }

            ByteBuffer controller = pendingControllerModel.getAndSet(null);
            if (controller != null) {
                nativeUploadControllerModel(nativeCtx, controller, controller.remaining());
            }

            RoomAssets room = pendingRoom.getAndSet(null);
            if (room != null) {
                nativeUploadRoomModel(nativeCtx, room.mesh, room.meshBytes, room.cell);
                for (int slot = 0; slot < room.atlases.length; slot++) {
                    nativeUploadRoomAtlas(nativeCtx, room.atlases[slot], room.cell, slot);
                }
            }

            nativeEndFrame(nativeCtx, newFrame, texMatrix, distance, quadWidth, curvature,
                    headLocked, separation, eyeSwap, passthroughOn);
        }
        return false;
    }

    /**
     * Picks the starting environment, then reads rooms and draws the panels on another thread,
     * which would otherwise hold up the first frame.
     */
    private void startEnvironment(PreferenceConfiguration prefs) {
        panels = new XrPanels(prefsContext, prefs.vrGamepadToggle);

        SharedPreferences saved = PreferenceManager.getDefaultSharedPreferences(prefsContext);
        int id = saved.getInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, -1);
        if (id < 0) {
            // Older installs saved a cell index instead of an id. The old key is left in place.
            int legacy = saved.getInt(PreferenceConfiguration.VR_ENVIRONMENT_PREF_STRING, -1);
            if (EnvironmentIds.idForLegacyCell(legacy) >= 0) {
                id = EnvironmentIds.idForLegacyCell(legacy);
                saved.edit()
                        .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, id)
                        .apply();
            }
        }

        int cell = EnvironmentIds.startCell(id, prefs.vrPassthrough);
        if (id >= 0 && EnvironmentIds.cellForId(id) < 0) {
            // The saved id is no longer offered: log once and save the void instead
            FileLog.event("environment id " + id + " is no longer offered, starting in the void");
            saved.edit()
                    .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING,
                            EnvironmentIds.idForCell(cell))
                    .apply();
        }
        // If the headset starts apps in passthrough, the player does too; the saved choice is kept
        if (playerMode && cell != ENV_CELL_PASSTHROUGH && nativeSystemPrefersPassthrough(nativeCtx)) {
            FileLog.event("the headset starts apps in passthrough, so the player does");
            cell = ENV_CELL_PASSTHROUGH;
        }
        environmentChoice = cell;
        passthroughOn = cell == ENV_CELL_PASSTHROUGH;
        nativeSetEnvironment(nativeCtx, cell);

        // Every room's Room tab values up front, so the picker can switch rooms without asking
        // again
        for (int roomCell = 0; roomCell < ENV_CELL_COUNT; roomCell++) {
            if (!EnvironmentIds.isRoomCell(roomCell)) {
                continue;
            }
            PreferenceConfiguration.RoomLevels levels = PreferenceConfiguration.readRoomLevels(
                    saved, EnvironmentIds.idForCell(roomCell));
            nativeSetRoomLevels(nativeCtx, roomCell, levels.brightness, levels.glow,
                    levels.light, levels.screen);
        }
        cogReadout = new XrPanels.Readout();
        // Nothing drawn yet, so the first look at either tab draws it
        Arrays.fill(readoutDrawn, -2);
        toast = new XrPanels.Toast();
        cogMarks = new XrPanels.Marks();
        cogClock = new XrPanels.ClockStrip();
        clock = new XrClock(prefsContext);
        // Nothing drawn yet, so the first look at the tab draws them
        Arrays.fill(marksDrawn, -2);
        nativeSetClickSound(nativeCtx, prefs.vrClickSound);
        nativeSetShowRay(nativeCtx, prefs.vrShowRay);
        nativeSetHeadAim(nativeCtx, prefs.vrHeadAim, prefs.vrHeadAimSensitivity,
                prefs.vrHeadAimDeadZone);
        // Sessions start in pointer mode; the chosen shortcut switches to the pad
        nativeSetGamepad(nativeCtx, prefs.vrGamepadToggle, prefs.deadzonePercentage);
        nativeSetControllerModel(nativeCtx, prefs.vrControllerModel);
        final boolean handHint = prefs.vrHandTracking && !prefs.vrHandLockHintSeen;
        nativeSetHandHint(nativeCtx, handHint);

        final int startRoom = cell;
        final int roomTicketAtStart;
        synchronized (roomLock) {
            roomTicketAtStart = ++roomTicket;
        }
        final boolean click = prefs.vrClickSound;
        if (click) {
            synchronized (clickLock) {
                clickSoundStarting = true;
            }
        }
        Thread loader = new Thread() {
            @Override
            public void run() {
                if (click) {
                    startClickSound();
                }
                buildPanelArt(handHint);
                pendingControllerModel.set(readAsset(CONTROLLER_MODEL));
                loadRoomAssets(startRoom, roomTicketAtStart);
            }
        };
        loader.setName("Video - XR Environment");
        loader.start();
    }

    // Draws every panel once and parks it for the frame loop. The hand lock hint only if it may
    // show.
    private void buildPanelArt(boolean handHint) {
        pendingPickerArt.set(panels.buildPickerGrid());
        pendingEnvButton.set(panels.buildEnvButton());
        if (handHint) {
            pendingHandHint.set(panels.buildHandHint());
        }

        // Curvature needs a layer type the runtime may not offer; the slider is greyed rather than
        // hidden
        boolean curveOk;
        synchronized (nativeLock) {
            curveOk = nativeCtx != 0 && nativeGetCylinderSupported(nativeCtx);
        }
        // Likewise the 3D rows with stereo off in settings. Their ticks mark the running model's
        // pair.
        boolean stereoOk = prefConfig != null && prefConfig.vrDepthMode != DEPTH_MODE_OFF;
        MidasDepthSource.Spec spec = MidasDepthSource.specFor(
                prefConfig != null ? prefConfig.vrDepthModel : null);
        pendingCogSheets.set(panels.buildCogTabs(curveOk, stereoOk, spec.defaultSeparation,
                spec.defaultConvergence));
        pendingCogButton.set(panels.buildCogButton());
        // No 3D toggle on the bar without stereo; the 3D tab says why
        if (stereoOk) {
            ByteBuffer[] faces = panels.buildStereoButtons();
            pendingStereoOff.set(faces[0]);
            pendingStereoOn.set(faces[1]);
        }
        ByteBuffer[] rayFaces = panels.buildRayButtons();
        pendingRayOff.set(rayFaces[0]);
        pendingRayOn.set(rayFaces[1]);
        ByteBuffer[] aimFaces = panels.buildAimButtons();
        pendingAimOff.set(aimFaces[0]);
        pendingAimOn.set(aimFaces[1]);
        ByteBuffer[] padFaces = panels.buildPadButtons();
        pendingPadOff.set(padFaces[0]);
        pendingPadOn.set(padFaces[1]);

        XrPanels.Keyboard keyboard = panels.buildKeyboard();
        kbKeyRects = keyboard.keyRects;
        kbCodes = keyboard.codes;
        pendingKbSheets.set(keyboard.sheets);
        pendingKbButton.set(keyboard.button);

        ByteBuffer[] exit = panels.buildExitArt();
        pendingExitButton.set(exit[0]);
        pendingExitPlain.set(exit[1]);
        pendingExitHot.set(exit[2]);
        pendingCancelHot.set(exit[3]);
    }

    // Cells past the real ones are blank tiles
    private static boolean cellExists(int cell) {
        return cell >= 0 && cell < ENV_CELL_COUNT;
    }

    /** A cell picked in the grid. A room shows once loaded, with the void until then. */
    private void chooseEnvironment(int cell) {
        if (!cellExists(cell)) {
            return;
        }
        environmentChoice = cell;
        passthroughOn = cell == ENV_CELL_PASSTHROUGH;

        requestRoom(cell);
        nativeSetEnvironment(nativeCtx, cell);

        // Saved, so the setting matches what is on screen
        EnvironmentIds.store(PreferenceManager.getDefaultSharedPreferences(prefsContext),
                EnvironmentIds.idForCell(cell));
    }

    // The mesh for a baked room by cell, or null for a cell with no model
    private static String roomMeshFile(int cell) {
        // No rooms ship, so no cell has one
        return null;
    }

    // The room's atlases in the slot order its parts use: ASTC with mip chains, uploaded as read
    private static String[] roomAtlasFiles(int cell) {
        return null;
    }

    /**
     * Each pick takes a ticket, so a room still being read for an earlier pick is dropped. The
     * resident room is not read again.
     */
    private void requestRoom(final int cell) {
        final int ticket;
        synchronized (roomLock) {
            ticket = ++roomTicket;
            if (roomMeshFile(cell) == null || cell == parkedRoomCell) {
                return;
            }
        }
        Thread loader = new Thread() {
            @Override
            public void run() {
                loadRoomAssets(cell, ticket);
            }
        };
        loader.setName("Video - XR Environment");
        loader.start();
    }

    /**
     * Reads a room's mesh and atlases and parks them for the frame loop, which owns the GL
     * context. Any failure parks nothing, and the cell shows the void.
     */
    private void loadRoomAssets(int cell, int ticket) {
        String meshFile = roomMeshFile(cell);
        String[] atlasFiles = roomAtlasFiles(cell);
        if (meshFile == null || atlasFiles == null) {
            return;
        }
        long started = System.nanoTime();
        ByteBuffer mesh = readAsset(ROOM_DIR + "/" + meshFile);
        if (mesh == null) {
            return;
        }

        ByteBuffer[] atlases = new ByteBuffer[atlasFiles.length];
        for (int slot = 0; slot < atlasFiles.length; slot++) {
            atlases[slot] = readAsset(ROOM_DIR + "/" + atlasFiles[slot]);
            if (atlases[slot] == null) {
                return;
            }
        }
        RoomAssets room = new RoomAssets(cell, mesh, atlases);

        synchronized (roomLock) {
            // Something else was picked while this was read
            if (ticket != roomTicket) {
                return;
            }
            pendingRoom.set(room);
            parkedRoomCell = cell;
        }
        FileLog.event("room " + meshFile + " and " + TextUtils.join(", ", atlasFiles) + " read in "
                + (System.nanoTime() - started) / 1000000 + " ms");
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
            XrPanels.closeQuietly(in);
        }
    }

    // Pointer moves go before presses, so a click lands where the pointer is now
    private void dispatchInput() {
        UiListener ui = uiListener;
        if (playerMode && ui != null) {
            ui.onUiPointer((int)inputState[IN_UI_PANEL], inputState[IN_UI_U], inputState[IN_UI_V],
                           inputState[IN_UI_PRESS] != 0.0f);
        }
        // Listener events; the screen placement and environment grid below are handled either way
        if (inputListener != null) {
            if (inputState[IN_HIT] != 0.0f) {
                inputListener.onVrPointerMove(inputState[IN_U], inputState[IN_V]);
            }
            // Head aim's turn plus pointer nudges as one relative move; no hit is set in that mode
            int dx = (int)inputState[IN_MOUSE_DX];
            int dy = (int)inputState[IN_MOUSE_DY];
            if (dx != 0 || dy != 0) {
                inputListener.onVrMouseMove(dx, dy);
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
            if (playerMode) {
                inputListener.onVrPlayerInput((int)inputState[IN_PLAYER_BUTTONS],
                        inputState[IN_PLAYER_STICK_X]);
            }

            dispatchPad();

            // A key (the sentinel is negative), sent before the modifiers are released since it was
            // typed with them held. While the report sheet is up the keys are its own, below.
            int key = (int)inputState[IN_KEY];
            if (key >= 0 && inputState[IN_REPORT_ZONE] < 0.0f) {
                inputListener.onVrKey(key, (int)inputState[IN_KEY_MODS]);
            }
            int mods = (int)inputState[IN_KB_MODS];
            if (mods != heldKbMods) {
                heldKbMods = mods;
                inputListener.onVrModifiers(mods);
            }

            // Cleared here too, so a frame landing while the activity exits cannot ask twice
            if (inputState[IN_EXIT] != 0.0f) {
                inputState[IN_EXIT] = 0.0f;
                inputListener.onVrExit();
            }
        }

        // While the report sheet is up its fields take the keys. Typed text stays out of the log.
        int reportKey = (int)inputState[IN_KEY];
        if (reportKey >= 0 && inputState[IN_REPORT_ZONE] >= 0.0f && reportForm.type(reportKey)) {
            LimeLog.info("Report sheet: " + reportForm.note().length() + " characters in the note, "
                    + reportForm.address().length() + " in the address, the keys on the "
                    + (reportForm.focus() == REPORT_ZONE_NOTE ? "note" : "address")
                    + (reportForm.canSend() ? ", ready to send" : ""));
        }

        // A 3D room puts the screen on its wall, so that placement is not saved
        if (inputState[IN_POSE_DIRTY] != 0.0f && !EnvironmentIds.isRoomCell(environmentChoice)) {
            saveScreenPose();
        }

        int pick = (int)inputState[IN_PICKER_PICK];
        if (pick >= 0) {
            chooseEnvironment(pick);
        }

        // "Don't show this again" on the hand lock hint
        if (inputState[IN_HINT] != 0.0f && prefsContext != null) {
            PreferenceConfiguration.markHandLockHintSeen(
                    PreferenceManager.getDefaultSharedPreferences(prefsContext));
            FileLog.event("hand lock hint put away for good");
        }

        int setting = (int)inputState[IN_SETTING];
        if (setting >= 0) {
            applySetting(setting, (int)inputState[IN_SETTING_VALUE],
                    (int)inputState[IN_SETTING_ROOM]);
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

    // Reports plug in, state changes and unplug, in that order and only on a change; the native
    // side fills the slots every frame
    private void dispatchPad() {
        boolean plugged = inputState[IN_PAD] != 0.0f;
        if (plugged && !padPlugged) {
            padPlugged = true;
            inputListener.onVrGamepadPlugged(true);
            // Nothing sent yet, so the first state always goes
            Arrays.fill(padSent, Integer.MIN_VALUE);
        }
        if (!plugged) {
            unplugPad();
            return;
        }
        boolean changed = false;
        for (int i = 0; i < padSent.length; i++) {
            int value = (int)inputState[IN_PAD_BUTTONS + i];
            if (value != padSent[i]) {
                padSent[i] = value;
                changed = true;
            }
        }
        if (changed) {
            inputListener.onVrGamepadState(padSent[0], padSent[1], padSent[2], padSent[3],
                    padSent[4], padSent[5], padSent[6]);
        }
    }

    // The pad out, if it was in. Frame loop only.
    private void unplugPad() {
        if (padPlugged) {
            padPlugged = false;
            if (inputListener != null) {
                inputListener.onVrGamepadPlugged(false);
            }
        }
    }

    // Report sheet events: opening, field focus and Send. Redraws when the contents change and
    // passes Send's readiness down. Cancel needs nothing: each opening starts afresh.
    private void updateReport() {
        int event = (int)inputState[IN_REPORT];
        int zone = (int)inputState[IN_REPORT_ZONE];
        if (event == REPORT_OPENED) {
            reportOpening++;
            reportForm.open(prefsContext != null ? PreferenceManager
                    .getDefaultSharedPreferences(prefsContext).getString(BugReport.EMAIL_PREF, "")
                    : "");
            reportDrawn = null;
            // Each opening starts with Send held on the native side
            reportSendHanded = false;
        }
        else if (event == REPORT_ZONE_NOTE || event == REPORT_ZONE_EMAIL) {
            reportForm.focus(event);
        }
        else if (event == REPORT_ZONE_SEND) {
            sendReport();
        }

        ReportDrawing drawn = pendingReport.getAndSet(null);
        if (drawn != null) {
            reportDrawing = false;
            if (drawn.opening == reportOpening && zone >= 0 && drawn.pixels != null) {
                nativeUploadReport(nativeCtx, drawn.pixels);
                reportDrawn = drawn.key;
                LimeLog.info("Report sheet drawn in " + drawn.ms + " ms, off the frame loop");
            }
        }
        if (zone < 0) {
            return;
        }

        boolean ready = reportForm.canSend();
        if (ready != reportSendHanded) {
            nativeSetReportSend(nativeCtx, ready);
            reportSendHanded = ready;
        }
        final int opening = reportOpening;
        final String note = reportForm.note();
        final String address = reportForm.address();
        final int focus = reportForm.focus();
        final boolean wrong = reportForm.addressWrong();
        final String key = opening + "|" + focus + "|" + zone + "|" + ready + "|" + wrong + "|"
                + note + "\u0000" + address;
        if (reportDrawing || key.equals(reportDrawn) || prefsContext == null) {
            return;
        }
        reportDrawing = true;
        final int hover = zone;
        final boolean sendable = ready;
        final Context context = prefsContext;
        Thread draw = new Thread() {
            @Override
            public void run() {
                long start = System.nanoTime();
                ByteBuffer pixels = null;
                try {
                    XrPanels.ReportSheet art = reportArt;
                    if (art == null) {
                        art = new XrPanels.ReportSheet(context);
                        reportArt = art;
                    }
                    pixels = art.draw(note, address, focus, hover, sendable, wrong);
                } catch (RuntimeException | OutOfMemoryError e) {
                    LimeLog.warning("Report sheet failed to draw: " + e);
                }
                pendingReport.set(new ReportDrawing(opening, key, pixels,
                        msPer(System.nanoTime() - start, 1)));
            }
        };
        draw.setName("Video - XR Report");
        draw.start();
    }

    // A drawing of the report sheet, waiting to go up
    private static final class ReportDrawing {
        final int opening;
        final String key;
        final ByteBuffer pixels;
        final String ms;

        ReportDrawing(int opening, String key, ByteBuffer pixels, String ms) {
            this.opening = opening;
            this.key = key;
            this.pixels = pixels;
            this.ms = ms;
        }
    }

    // Saves the address, then builds and posts the report on its own thread, saving it to a file
    // where it cannot be sent. The toast says how it went.
    private void sendReport() {
        final Context context = prefsContext;
        final String note = reportForm.note();
        final String address = reportForm.address().trim();
        if (context == null || !BugReport.canSend(note, address)) {
            return;
        }
        PreferenceManager.getDefaultSharedPreferences(context).edit()
                .putString(BugReport.EMAIL_PREF, address).apply();
        final String session = "environment " + environmentName(environmentChoice)
                + ", 3d " + (stereoLive ? "on" : "off")
                + ", display " + Math.round(nativeGetDisplayRate(nativeCtx)) + " Hz";
        FileLog.event("report from the session: " + note.length() + " characters, "
                + (address.isEmpty() ? "no address" : "an address") + ", "
                + (BugReport.collectorConfigured() ? "sending" : "no collector, saving"));
        if (BugReport.collectorConfigured()) {
            showNotice(context.getString(R.string.bug_report_sending), null);
        }
        Thread send = new Thread() {
            @Override
            public void run() {
                BugReport.Outcome outcome = BugReport.send(context, note, address, session);
                FileLog.event("report " + outcome.result.name().toLowerCase(Locale.ROOT)
                        + (outcome.path != null ? " at " + outcome.path : "")
                        + (outcome.detail != null ? ": " + outcome.detail : ""));
                // The reason on the first line, the saved path under it
                String where = BugReport.shortPath(outcome.path);
                switch (outcome.result) {
                    case SENT:
                        showNotice(context.getString(R.string.bug_report_sent), null);
                        break;
                    case NOT_SENT:
                        showNotice(context.getString(R.string.bug_report_not_sent_at), where, true);
                        break;
                    case BUSY:
                        showNotice(context.getString(R.string.bug_report_busy_at), where, true);
                        break;
                    case NO_COLLECTOR:
                        showNotice(context.getString(R.string.bug_report_no_collector_at), where,
                                true);
                        break;
                    default:
                        showNotice(context.getString(R.string.vr_report_not_written),
                                outcome.detail);
                        break;
                }
            }
        };
        send.setName("Video - XR Report send");
        send.start();
    }

    // The environment's name for the report's session line
    private static String environmentName(int cell) {
        switch (cell) {
            case ENV_CELL_PASSTHROUGH: return "passthrough";
            case ENV_CELL_VOID: return "void";
            default: return "cell " + cell;
        }
    }

    // Redraws the Room or Picture tab's values when one moves and uploads the strip. The native
    // side only shows a strip drawn from the current values.
    private void updateCogReadout() {
        if (inputState[IN_READOUT] < 0.0f || cogReadout == null) {
            return;
        }
        boolean changed = false;
        for (int i = 0; i < READOUT_VALUES; i++) {
            readoutWanted[i] = (int)inputState[IN_READOUT + i];
            changed |= readoutWanted[i] != readoutDrawn[i];
        }
        if (!changed) {
            return;
        }
        nativeUploadCogReadout(nativeCtx, cogReadout.draw(readoutWanted), readoutWanted);
        System.arraycopy(readoutWanted, 0, readoutDrawn, 0, READOUT_VALUES);
    }

    // Redraws the display tab's marks when one moves and uploads the strip
    private void updateCogMarks() {
        if (inputState[IN_MARKS] < 0.0f || cogMarks == null) {
            return;
        }
        boolean changed = false;
        for (int i = 0; i < MARK_VALUES; i++) {
            marksWanted[i] = (int)inputState[IN_MARKS + i];
            changed |= marksWanted[i] != marksDrawn[i];
        }
        if (changed) {
            nativeUploadCogMarks(nativeCtx, cogMarks.draw(marksWanted));
            System.arraycopy(marksWanted, 0, marksDrawn, 0, MARK_VALUES);
        }
    }

    // Redraws the visible keyboard sheet when the lit modifiers change. A cold draw takes up to
    // 44 ms, so it runs on its own thread and is uploaded here.
    private void updateKeyboardSheet() {
        KbSheet drawn = pendingKbSheet.getAndSet(null);
        if (drawn != null) {
            nativeUploadKeyboardSheet(nativeCtx, drawn.state, drawn.pixels);
            kbSheetMods[drawn.state] = drawn.mods;
            kbSheetDrawing = false;
            LimeLog.info("Keyboard sheet " + drawn.state + " drawn with modifiers " + drawn.mods
                    + " in " + drawn.ms + " ms, off the frame loop");
        }
        final int sheet = (int)inputState[IN_KB_SHEET];
        final XrPanels art = panels;
        if (!kbArtUp || kbSheetDrawing || art == null || sheet < 0 || sheet >= KB_STATE_COUNT) {
            return;
        }
        final int mods = (int)inputState[IN_KB_MODS];
        if (mods == kbSheetMods[sheet]) {
            return;
        }
        kbSheetDrawing = true;
        Thread draw = new Thread() {
            @Override
            public void run() {
                long start = System.nanoTime();
                ByteBuffer pixels = null;
                try {
                    pixels = art.buildKeyboardSheet(sheet, mods);
                } catch (RuntimeException | OutOfMemoryError e) {
                    // Keep the old sheet rather than wait for a drawing that never comes
                    LimeLog.warning("Keyboard sheet " + sheet + " failed to draw: " + e);
                }
                pendingKbSheet.set(new KbSheet(sheet, mods, pixels,
                        msPer(System.nanoTime() - start, 1)));
            }
        };
        draw.setName("Video - XR Keyboard");
        draw.start();
    }

    // A keyboard sheet drawn off the frame loop, waiting to go up
    private static final class KbSheet {
        final int state;
        final int mods;
        final ByteBuffer pixels;
        final String ms;

        KbSheet(int state, int mods, ByteBuffer pixels, String ms) {
            this.state = state;
            this.mods = mods;
            this.pixels = pixels;
            this.ms = ms;
        }
    }

    // Keeps the clock strip over the settings panel current while the panel is open
    private void updateCogClock() {
        XrClock now = clock;
        if (inputState[IN_COG_OPEN] == 0.0f || now == null || cogClock == null) {
            return;
        }
        String line = now.line(System.currentTimeMillis());
        if (!line.equals(cogClockDrawn)) {
            nativeUploadCogClock(nativeCtx, cogClock.draw(line));
            cogClockDrawn = line;
        }
    }

    // Feeds the click track every frame and clicks on a press while the setting is on. Turned
    // on mid-session, the track is built off the frame loop.
    private void updateClickSound(PreferenceConfiguration prefs) {
        XrClickSound click = clickSound;
        if (click == null) {
            if (!prefs.vrClickSound) {
                return;
            }
            synchronized (clickLock) {
                if (clickSoundStarting) {
                    return;
                }
                clickSoundStarting = true;
            }
            Thread starter = new Thread() {
                @Override
                public void run() {
                    startClickSound();
                }
            };
            starter.setName("Video - XR Click");
            starter.start();
            return;
        }
        click.feed();
        if (inputState[IN_CLICK] != 0.0f && prefs.vrClickSound) {
            click.click();
        }
    }

    // Off the frame loop: building the track talks to the audio system. Released if the session
    // is already over.
    private void startClickSound() {
        // AudioTrack's non-blocking write needs M
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return;
        }
        XrClickSound click = XrClickSound.start();
        if (click == null) {
            return;
        }
        synchronized (clickLock) {
            if (!clickSessionOver) {
                clickSound = click;
                return;
            }
        }
        click.release();
    }

    // Queues notices raised here, then draws and uploads whichever notice the frame says has
    // just gone up
    private void updateToast() {
        NoticeWords notice;
        while ((notice = pendingNotices.poll()) != null) {
            int slot = noticeSlot;
            noticeSlot = (noticeSlot + 1) % TOAST_TEXT_SLOTS;
            noticeTexts[slot] = notice;
            nativePushNotice(nativeCtx, TOAST_TEXT, slot);
        }
        int kind = (int)inputState[IN_TOAST];
        if (kind < 0 || toast == null || prefsContext == null) {
            return;
        }
        int arg = (int)inputState[IN_TOAST_ARG];
        String text;
        String more = null;
        boolean path = false;
        switch (kind) {
            case TOAST_RATE:
                text = prefsContext.getString(R.string.vr_toast_rate, arg);
                break;
            case TOAST_HANDS_LOCKED:
                text = prefsContext.getString(R.string.vr_toast_hands_locked);
                break;
            case TOAST_HANDS_UNLOCKED:
                text = prefsContext.getString(R.string.vr_toast_hands_unlocked);
                break;
            case TOAST_3D_OFF:
                text = prefsContext.getString(R.string.vr_toast_3d_off);
                break;
            case TOAST_3D_ON:
                text = prefsContext.getString(R.string.vr_toast_3d_on);
                break;
            case TOAST_HEAD_AIM_OFF:
                text = prefsContext.getString(R.string.vr_toast_head_aim_off);
                break;
            case TOAST_HEAD_AIM_ON:
                text = prefsContext.getString(R.string.vr_toast_head_aim_on);
                more = prefsContext.getString(R.string.vr_toast_head_aim_on_more);
                break;
            case TOAST_GAMEPAD_MODE:
                // The second line says how to switch back
                text = prefsContext.getString(R.string.vr_toast_gamepad_mode);
                more = prefsContext.getString(XrPanels.gamepadBackText(
                        prefConfig != null ? prefConfig.vrGamepadToggle : PAD_SHORTCUT_MENU_GRIP));
                break;
            case TOAST_POINTER_MODE:
                text = prefsContext.getString(R.string.vr_toast_pointer_mode);
                break;
            case TOAST_TEXT:
                NoticeWords words = arg >= 0 && arg < TOAST_TEXT_SLOTS ? noticeTexts[arg] : null;
                if (words == null) {
                    return;
                }
                text = words.text;
                more = words.more;
                path = words.path;
                break;
            default:
                return;
        }
        nativeUploadToast(nativeCtx, toast.draw(text, more, path), kind, arg);
    }

    /**
     * A settings row changed. The native side has already applied it; this persists it. A room's
     * values are stored under the id of the room in roomCell.
     */
    private void applySetting(int setting, int value, int roomCell) {
        if (prefsContext == null) {
            return;
        }

        if (setting == SETTING_ROOM_BRIGHTNESS || setting == SETTING_ROOM_GLOW
                || setting == SETTING_ROOM_LIGHT_LEVEL || setting == SETTING_ROOM_SCREEN) {
            applyRoomSetting(setting, value, EnvironmentIds.idForCell(roomCell));
        }
        else if (setting == SETTING_SHARPEN) {
            String choice = value == 2 ? "quality" : (value == 1 ? "normal" : "off");
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putString(PreferenceConfiguration.VR_SHARPENING_PREF_STRING, choice)
                    .apply();
        }
        else if (setting == SETTING_SUPERSAMPLE) {
            String choice = value == 2 ? "quality" : (value == 1 ? "normal" : "off");
            if (prefConfig != null) {
                prefConfig.vrSupersampling = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putString(PreferenceConfiguration.VR_SUPERSAMPLING_PREF_STRING, choice)
                    .apply();
        }
        else if (setting == SETTING_STATS) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.enablePerfOverlay = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.ENABLE_PERF_OVERLAY_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_AMBILIGHT) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrAmbilight = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_AMBILIGHT_PREF_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_ROOM_LIGHT) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrRoomLight = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_ROOM_LIGHT_PREF_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_HEAD_LOCK) {
            boolean on = value != 0;
            // Read every frame by the frame loop. Rooms ignore it, but the row stays live.
            if (prefConfig != null) {
                prefConfig.vrHeadLocked = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_HEAD_LOCKED_PREF_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_POINTER_SLEEP) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrPointerSleep = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_POINTER_SLEEP_PREF_STRING, on)
                    .apply();
            FileLog.event("pointer sleep " + (on ? "on" : "off") + " saved");
        }
        else if (setting == SETTING_CLICK_SOUND) {
            boolean on = value != 0;
            // Read by the frame loop from this same object
            if (prefConfig != null) {
                prefConfig.vrClickSound = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_CLICK_SOUND_PREF_STRING, on)
                    .apply();
            FileLog.event("click sound " + (on ? "on" : "off") + " saved");
        }
        else if (setting == SETTING_CONTROLLER_MODEL) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrControllerModel = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_CONTROLLER_MODEL_PREF_STRING, on)
                    .apply();
            FileLog.event("controller model " + (on ? "on" : "off") + " saved");
        }
        else if (setting == SETTING_HEAD_AIM_SENSITIVITY) {
            // Already applied natively; this is for next time
            if (prefConfig != null) {
                prefConfig.vrHeadAimSensitivity = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING, value)
                    .apply();
            FileLog.event("head aim sensitivity " + value + " saved");
        }
        else if (setting == SETTING_HEAD_AIM_DEADZONE) {
            if (prefConfig != null) {
                prefConfig.vrHeadAimDeadZone = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING, value)
                    .apply();
            FileLog.event("head aim dead zone " + value + " saved");
        }
        else if (setting == SETTING_AMBI_LEVEL) {
            if (prefConfig != null) {
                prefConfig.vrAmbilightLevel = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_AMBILIGHT_LEVEL_PREF_STRING, value)
                    .apply();
        }
        else if (setting == SETTING_SEPARATION) {
            // The native panel value overrides the frame loop's copy for this session; this is for
            // next time
            if (prefConfig != null) {
                prefConfig.vrStereoSeparation = value;
            }
            SharedPreferences saved = PreferenceManager.getDefaultSharedPreferences(prefsContext);
            saved.edit().putInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING, value).apply();
            FileLog.event("separation " + value + " saved, the preference holds "
                    + saved.getInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING, -1)
                    + ", preset " + PreferenceConfiguration.presetLabel(value,
                            prefConfig != null ? prefConfig.vrDepthModel : null));
        }
        else if (setting == SETTING_CONVERGENCE) {
            if (prefConfig != null) {
                prefConfig.vrConvergence = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_CONVERGENCE_PREF_STRING, value)
                    .apply();
        }
        else if (setting == SETTING_RESET_3D) {
            // Both removed, so the pair follows the running model's defaults, also after a model
            // change
            String model = prefConfig != null ? prefConfig.vrDepthModel : null;
            if (prefConfig != null) {
                prefConfig.vrStereoSeparation = PreferenceConfiguration.defaultSeparation(model);
                prefConfig.vrConvergence = PreferenceConfiguration.defaultConvergence(model);
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .remove(PreferenceConfiguration.VR_SEPARATION_PREF_STRING)
                    .remove(PreferenceConfiguration.VR_CONVERGENCE_PREF_STRING)
                    .apply();
            FileLog.event("3d pair back to the model's own "
                    + PreferenceConfiguration.defaultPairLabel(model));
        }
        else if (pictureRow(setting) >= 0) {
            // Already live natively; only the preference is written
            int row = pictureRow(setting);
            int units = PreferenceConfiguration.clampPicture(row, value);
            if (prefConfig != null) {
                prefConfig.vrPicture[row] = units;
            }
            String key = PreferenceConfiguration.pictureKey(row);
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(key, units)
                    .apply();
            FileLog.event("picture setting " + key + " = " + units + " saved");
        }
        else if (setting == SETTING_RESET_PICTURE) {
            // Removed, which reads as the unadjusted picture
            SharedPreferences.Editor editor =
                    PreferenceManager.getDefaultSharedPreferences(prefsContext).edit();
            for (int row = 0; row < PICTURE_VALUES; row++) {
                if (prefConfig != null) {
                    prefConfig.vrPicture[row] = PreferenceConfiguration.pictureDefault(row);
                }
                editor.remove(PreferenceConfiguration.pictureKey(row));
            }
            editor.apply();
            FileLog.event("picture back to the picture as streamed");
        }
    }

    // Which picture row a setting id writes, or -1 for any other setting
    private static int pictureRow(int setting) {
        switch (setting) {
            case SETTING_PICTURE_BRIGHTNESS: return PICTURE_BRIGHTNESS;
            case SETTING_PICTURE_CONTRAST: return PICTURE_CONTRAST;
            case SETTING_PICTURE_GAMMA: return PICTURE_GAMMA;
            case SETTING_PICTURE_SATURATION: return PICTURE_SATURATION;
            default: return -1;
        }
    }

    // One room's value under that room's key, logged so a report shows each room's levels
    private void applyRoomSetting(int setting, int value, int roomId) {
        if (!PreferenceConfiguration.isRoomEnvironment(roomId)) {
            return;
        }
        SharedPreferences.Editor editor =
                PreferenceManager.getDefaultSharedPreferences(prefsContext).edit();
        String key;
        if (setting == SETTING_ROOM_GLOW) {
            key = PreferenceConfiguration.roomGlowKey(roomId);
            editor.putBoolean(key, value != 0);
        }
        else {
            if (setting == SETTING_ROOM_BRIGHTNESS) {
                key = PreferenceConfiguration.roomBrightnessKey(roomId);
            }
            else if (setting == SETTING_ROOM_LIGHT_LEVEL) {
                key = PreferenceConfiguration.roomLightKey(roomId);
            }
            else {
                key = PreferenceConfiguration.roomScreenKey(roomId);
                value = PreferenceConfiguration.clampRoomScreen(roomId, value);
            }
            editor.putInt(key, value);
        }
        editor.apply();
        FileLog.event("room setting " + key + " = "
                + (setting == SETTING_ROOM_GLOW ? String.valueOf(value != 0) : value) + " saved");
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

    private void restoreScreenPose() {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(prefsContext);
        String saved = prefs.getString(screenPoseKey(), null);
        // First time in the library: start where the film screen is, at the library's share of its
        // width, so nothing jumps
        boolean browseFromFilm = false;
        if (saved == null && playerMode && playerScreenHidden) {
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
