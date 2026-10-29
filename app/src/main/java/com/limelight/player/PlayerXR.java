package com.limelight.player;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import android.view.Surface;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.preference.PreferenceManager;

import com.limelight.LimeLog;
import com.limelight.binding.video.XrRenderer;
import com.limelight.binding.video.XrShared;
import com.limelight.binding.video.XrStartFailure;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.File;
import java.util.List;
import java.util.Locale;

/**
 * VR player activity. Plays a video file (ExoPlayer) or Blu-ray 3D disc image (libdisc) into
 * the XR renderer's input surface. Stereo files are shown per eye; mono files are shown flat or
 * through the renderer's 2D-to-3D model. Resume position and letterbox bars are stored per file.
 *
 * Intent data is the file; optional extra:
 *   layout  "auto" (default, from the file name), "mono", "sbs" or "ou"
 */
public class PlayerXR extends Activity implements XrRenderer.InputListener, XrRenderer.SessionListener {

    public static final String EXTRA_LAYOUT = "layout";
    // Start position for a restart in place
    public static final String EXTRA_POSITION_MS = "positionMs";
    // Library tab, page and folder to return to
    private static final String EXTRA_LIBRARY_PLACE = "libraryPlace";
    private static final String EXTRA_SPEED = "speed";
    // Selected track indices kept across a restart; the count guards against a different list
    private static final String EXTRA_SUB_TRACK = "subTrack", EXTRA_SUB_COUNT = "subCount";
    private static final String EXTRA_AUDIO_TRACK = "audioTrack", EXTRA_AUDIO_COUNT = "audioCount";

    // Ambient light reach and room brightness, 0..1
    private static final String PREF_IMMERSION = "player_immersion";
    private static final String PREF_BRIGHTNESS = "player_brightness";
    private static final String PREF_CURVE = "player_curve";
    // A COLOUR_MODE_ value
    private static final String PREF_COLOUR = "player_colour";
    private static final String PREF_HAND_AUTO_HIDE = "player_hand_auto_hide";
    // Per-file subtitle delay in ms, keyed by suffixing the file key
    private static final String PREF_SUB_DELAY = "player_subdelay:";
    private static final float DEFAULT_BRIGHTNESS = 0.4f;
    // Stereo eye separation preset, see cropFor
    private static final String PREF_DEPTH = "player_depth";
    static final String DEPTH_POP = "pop", DEPTH_BALANCED = "balanced", DEPTH_FULL = "full";
    private static final String PREF_CONVERT = "player_convert_2d";

    private static final long SAVE_EVERY_MS = 5000;

    private final Handler main = new Handler(Looper.getMainLooper());
    private XrRenderer renderer;
    private PlaybackEngine engine;
    private PlayerUi ui;
    private volatile boolean finishing;
    private SharedPreferences saved;
    private PlaybackStore store;
    private String filmKey;
    private StereoLayout layout;
    private boolean disc;
    // Launched without a file: library only
    private boolean browse;
    private float immersion;
    private float brightness;
    private float curve;
    private int colourMode;
    private boolean handAutoHide;
    // Not persisted; a new file starts at 1x
    private float speed = 1.0f;
    private long subtitleDelayMs;
    private String depth;
    private boolean convert2d;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Uri uri = getIntent().getData();
        browse = uri == null;
        if (!browse) {
            filmKey = "file".equals(uri.getScheme()) && uri.getPath() != null ? uri.getPath() : uri.toString();
            disc = "file".equals(uri.getScheme()) && uri.getPath() != null && VideoLibrary.isDisc(new File(uri.getPath()));
        }

        // Disc: 1920x1080 per eye, decoded side by side, 23.976 Hz. Browse: placeholder 16:9.
        VideoProbe probe = browse ? new VideoProbe(1920, 1080, 24)
                : disc ? new VideoProbe(3840, 1080, 24) : VideoProbe.read(this, uri);
        layout = browse ? StereoLayout.MONO : disc ? StereoLayout.SBS
                : StereoLayout.resolve(getIntent().getStringExtra(EXTRA_LAYOUT),
                        uri.getLastPathSegment(), probe.width, probe.height);
        int eyeWidth = layout.eyeWidth(probe.width, probe.height);
        int eyeHeight = layout.eyeHeight(probe.width, probe.height);

        saved = PreferenceManager.getDefaultSharedPreferences(this);
        store = new PlaybackStore(this);
        immersion = saved.getFloat(PREF_IMMERSION, 1.0f);
        brightness = saved.getFloat(PREF_BRIGHTNESS, DEFAULT_BRIGHTNESS);
        curve = saved.getFloat(PREF_CURVE, 0.0f);
        colourMode = saved.getInt(PREF_COLOUR, XrShared.COLOUR_MODE_NATURAL);
        handAutoHide = saved.getBoolean(PREF_HAND_AUTO_HIDE, true);
        depth = saved.getString(PREF_DEPTH, DEPTH_FULL);
        if (depth.equals("photon")) {
            // Legacy value
            depth = DEPTH_FULL;
        }
        convert2d = saved.getBoolean(PREF_CONVERT, false);
        if (com.limelight.BuildConfig.DEBUG && getIntent().hasExtra("debugConvert")) {
            convert2d = true;
        }
        // Default the environment to passthrough
        if (!saved.contains(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING)) {
            saved.edit()
                    .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING,
                            PreferenceConfiguration.VR_ENV_PASSTHROUGH)
                    .apply();
        }

        PreferenceConfiguration prefs = PreferenceConfiguration.readPreferences(this);
        prefs.vrPassthrough = true;
        // World-locked flat screen, 2.5 m away and 2.4 m wide by default (units of 0.1 m)
        prefs.vrHeadLocked = false;
        prefs.vrDistance = 25;
        prefs.vrScreenSize = 24;
        prefs.vrCurvature = 0;
        // The renderer picks a display rate that is a multiple of this (72 Hz for 24 fps)
        prefs.fps = probe.fps;
        if (layout == StereoLayout.SBS) {
            prefs.vrDepthMode = XrShared.DEPTH_MODE_NATIVE_SBS;
        }
        else if (layout == StereoLayout.OU) {
            prefs.vrDepthMode = XrShared.DEPTH_MODE_NATIVE_OU;
        }
        else {
            prefs.vrDepthMode = convert2d && !browse ? XrShared.DEPTH_MODE_MODEL : XrShared.DEPTH_MODE_OFF;
        }
        LimeLog.info("PlayerXR: " + uri + " " + probe.width + "x" + probe.height + " @" + probe.fps
                + " layout=" + layout + " eye=" + eyeWidth + "x" + eyeHeight
                + " depthMode=" + prefs.vrDepthMode + (disc ? " disc" : ""));

        renderer = new XrRenderer();
        renderer.setPlayerMode(true);
        // Must precede start() so the library window placement is restored
        renderer.setPlayerScreenHidden(browse);
        renderer.setPlayerLighting(immersion, brightness);
        renderer.setPlayerCurve(curve);
        renderer.setColourMode(colourMode);
        renderer.setHandAutoHide(handAutoHide);
        renderer.setPlayerDepth(cropFor(depth));
        PlaybackStore.Entry known = browse ? new PlaybackStore.Entry() : store.get(filmKey);
        if (known.barsKnown) {
            renderer.setPlayerContent(known.barTop, known.barBottom);
        }
        renderer.setInputListener(this);
        renderer.setSessionListener(this);
        // start() blocks until the session is running, so call it off the main thread
        new Thread(() -> {
            boolean ok = renderer.start(PlayerXR.this, eyeWidth, eyeHeight, prefs);
            main.post(() -> {
                if (!ok || finishing) {
                    finish();
                    return;
                }
                startPlayback(uri, known);
            });
        }, "PlayerXR start").start();
    }

    private void startPlayback(Uri uri, PlaybackStore.Entry known) {
        PlaybackEngine.Listener listener = new PlaybackEngine.Listener() {
            @Override
            public void onSubtitle(CharSequence text, Bitmap image, float widthShare) {
                main.post(() -> {
                    PlayerUi current = ui;
                    if (current != null) {
                        current.setSubtitle(text, image, widthShare);
                    }
                });
            }

            @Override
            public void onError(String title, String detail) {
                XrRenderer r = renderer;
                if (r != null) {
                    r.showNotice(title, detail);
                }
            }

            @Override
            public void onVideoSize(int width, int height) {
            }

            @Override
            public void onVideoColour(int transfer, float maxCll, float masteringNits) {
                XrRenderer r = renderer;
                if (r != null) {
                    r.setVideoColour(transfer, maxCll, masteringNits);
                }
            }
        };
        engine = browse ? new NullEngine()
                : disc ? new DiscEngine(this, new File(uri.getPath()), listener)
                : new ExoEngine(this, uri, listener);
        subtitleDelayMs = filmKey != null ? saved.getLong(PREF_SUB_DELAY + filmKey, 0L) : 0L;
        engine.setSubtitleDelayMs(subtitleDelayMs);
        speed = getIntent().getFloatExtra(EXTRA_SPEED, 1.0f);
        if (speed != 1.0f) {
            engine.setSpeed(speed);
        }

        long start = getIntent().getLongExtra(EXTRA_POSITION_MS, 0);
        if (start <= 0) {
            start = known.resumeMs();
        }
        Surface target = renderer.getInputSurface();
        if (com.limelight.BuildConfig.DEBUG && getIntent().hasExtra("debugHeadless")) {
            target = headlessSurface();
        }
        engine.start(target, start, true);
        if (browse) {
            showIdleFrame(target);
        }
        else {
            restoreTracks(0);
        }

        ui = new PlayerUi(this, renderer, new PlayerUi.Actions() {
            @Override
            public void onBack() {
                if (browse) {
                    finish();
                }
                else {
                    backToLibrary();
                }
            }

            @Override
            public void onTogglePlay() {
                engine.setPlaying(!engine.isPlaying());
            }

            @Override
            public void onSeek(float fraction) {
                long duration = engine.durationMs();
                if (duration > 0) {
                    engine.seekToMs((long) (fraction * duration));
                }
            }

            @Override
            public void onTogglePassthrough() {
                renderer.setPassthrough(!renderer.isPassthroughOn());
            }

            @Override
            public void onStereoMode(String mode) {
                restartWithLayout(mode);
            }

            @Override
            public void onLighting(float ambient, float level, boolean done) {
                immersion = ambient;
                brightness = level;
                renderer.setPlayerLighting(ambient, level);
                if (done) {
                    saved.edit().putFloat(PREF_IMMERSION, ambient).putFloat(PREF_BRIGHTNESS, level).apply();
                }
            }

            @Override
            public void onSubtitleDelay(long ms) {
                applySubtitleDelay(ms);
            }

            @Override
            public long subtitleDelayMs() {
                return subtitleDelayMs;
            }

            @Override
            public void onCurve(float value, boolean done) {
                curve = value;
                renderer.setPlayerCurve(value);
                if (done) {
                    saved.edit().putFloat(PREF_CURVE, value).apply();
                }
            }

            @Override
            public float curve() {
                return curve;
            }

            @Override
            public void onSpeed(float value) {
                if (value != speed) {
                    speed = value;
                    LimeLog.info("PlayerXR: speed " + value);
                    if (engine != null) {
                        engine.setSpeed(value);
                    }
                }
            }

            @Override
            public float speed() {
                return speed;
            }

            @Override
            public void onColourMode(int mode) {
                if (mode != colourMode) {
                    colourMode = mode;
                    renderer.setColourMode(mode);
                    saved.edit().putInt(PREF_COLOUR, mode).apply();
                }
            }

            @Override
            public int colourMode() {
                return colourMode;
            }

            @Override
            public void onHandAutoHide(boolean on) {
                if (on != handAutoHide) {
                    handAutoHide = on;
                    renderer.setHandAutoHide(on);
                    saved.edit().putBoolean(PREF_HAND_AUTO_HIDE, on).apply();
                }
            }

            @Override
            public boolean handAutoHide() {
                return handAutoHide;
            }

            @Override
            public void onOpen(File file) {
                openFile(Uri.fromFile(file));
            }

            @Override
            public void onDepth(String name) {
                depth = name;
                renderer.setPlayerDepth(cropFor(name));
                saved.edit().putString(PREF_DEPTH, name).apply();
            }

            @Override
            public void onConvert2d(boolean on) {
                if (on != convert2d) {
                    convert2d = on;
                    saved.edit().putBoolean(PREF_CONVERT, on).apply();
                    // The depth model is configured at session start
                    restart();
                }
            }

            @Override
            public void onChooseAudio(PlayerUi.TrackChoice choice) {
                engine.chooseAudio(choice);
            }

            @Override
            public void onChooseSubtitle(PlayerUi.TrackChoice choice) {
                engine.chooseSubtitle(choice);
            }

            @Override
            public long positionMs() {
                return engine.positionMs();
            }

            @Override
            public long durationMs() {
                return engine.durationMs();
            }

            @Override
            public boolean isPlaying() {
                return engine.isPlaying();
            }

            @Override
            public boolean isPassthroughOn() {
                return renderer.isPassthroughOn();
            }

            @Override
            public String stereoMode() {
                String requested = getIntent().getStringExtra(EXTRA_LAYOUT);
                return requested != null ? requested : "auto";
            }

            @Override
            public String depth() {
                return depth;
            }

            @Override
            public boolean convert2d() {
                return convert2d;
            }

            @Override
            public float ambient() {
                return immersion;
            }

            @Override
            public float brightness() {
                return brightness;
            }

            @Override
            public List<PlayerUi.TrackChoice> audioTracks() {
                return engine.audioTracks();
            }

            @Override
            public List<PlayerUi.TrackChoice> subtitleTracks() {
                return engine.subtitleTracks();
            }
        });
        renderer.setUiListener(ui);
        ui.start(browse ? "Library" : titleFor(uri));
        String place = getIntent().getStringExtra(EXTRA_LIBRARY_PLACE);
        if (place != null) {
            ui.restoreLibrary(place);
        }
        if (browse) {
            renderer.setPlayerScreenHidden(true);
            ui.startBrowsing();
        }
        if (com.limelight.BuildConfig.DEBUG && getIntent().hasExtra("debugSheet")) {
            ui.debugOpen(getIntent().getIntExtra("debugSheet", 0));
        }

        main.postDelayed(saver, SAVE_EVERY_MS);
        if (com.limelight.BuildConfig.DEBUG && "stress".equals(getIntent().getStringExtra("debugScript"))) {
            debugStress();
        }
        else if (com.limelight.BuildConfig.DEBUG && "seeks".equals(getIntent().getStringExtra("debugScript"))) {
            debugSeeks();
        }

        else if (com.limelight.BuildConfig.DEBUG && getIntent().hasExtra("debugScript")) {
            // Timed seek/pause/resume smoke test
            main.postDelayed(() -> engine.seekToMs(3600000), 12000);
            main.postDelayed(() -> engine.setPlaying(false), 22000);
            main.postDelayed(() -> engine.setPlaying(true), 27000);
            main.postDelayed(() -> engine.seekToMs(9000000), 32000);
        }
        if (!known.barsKnown && !disc && !browse) {
            findBars(uri);
        }
    }

    // Debug: repeated seeks, logging whether playback moves after each one
    private void debugSeeks() {
        final PlaybackEngine e = engine;
        final long[] targets = {1200, 2400, 600, 3000, 4500, 1800, 5200, 900, 3600, 2100, 4800, 300,
                6000, 1500, 2700, 3900, 750, 5400};
        for (int i = 0; i < targets.length; i++) {
            final long t = targets[i];
            final long at = 8000 + i * 6000L;
            main.postDelayed(() -> {
                e.seekToMs(t * 1000);
                LimeLog.info("PlayerXR seeks: seek to " + t);
            }, at);
            main.postDelayed(() -> {
                long pos = e.positionMs() / 1000;
                LimeLog.info("PlayerXR seeks: wanted " + t + ", at " + pos
                        + (pos >= t && pos < t + 8 ? " MOVING" : " STUCK"));
            }, at + 5000);
        }
    }

    // Debug: scripted pause/play/seek sequence, logging position after each step
    private void debugStress() {
        final PlaybackEngine e = engine;
        final long[][] steps = {
            // {delay ms, action (0 log, 1 pause, 2 play, 3 seek), seek target s}
            {8000, 0, 0}, {8500, 1, 0}, {10500, 3, 1800}, {12500, 0, 0}, {14000, 2, 0},
            {16000, 0, 0}, {20000, 0, 0}, {21000, 3, 2400}, {24000, 0, 0}, {26000, 1, 0},
            {28000, 2, 0}, {31000, 0, 0}, {33000, 3, 3000}, {34000, 1, 0}, {35000, 3, 3300},
            {37000, 2, 0}, {41000, 0, 0},
        };
        for (long[] step : steps) {
            main.postDelayed(() -> {
                switch ((int) step[1]) {
                    case 1: e.setPlaying(false); break;
                    case 2: e.setPlaying(true); break;
                    case 3: e.seekToMs(step[2] * 1000); break;
                    default: break;
                }
                LimeLog.info("PlayerXR stress: t=" + step[0] / 1000 + " s action=" + step[1]
                        + (step[1] == 3 ? " to " + step[2] : "") + " pos=" + e.positionMs() / 1000.0
                        + " playing=" + e.isPlaying());
            }, step[0]);
        }
    }

    // Debug: decode into an ImageReader instead of the headset, logging frame rate and saving
    // one frame as PNG
    private ImageReader headless;
    private int headlessFrames;
    private long headlessSince;

    // Counts frames with patches of the green (R,B ~0, G ~76) that all-zero YUV converts to
    private int greenFrames;

    private void countGreen(Image image) {
        Image.Plane plane = image.getPlanes()[0];
        java.nio.ByteBuffer b = plane.getBuffer();
        int stride = plane.getRowStride();
        int w = image.getWidth(), h = image.getHeight();
        int hits = 0, samples = 0;
        for (int y = 4; y < h; y += 16) {
            for (int x = 4; x < w; x += 16) {
                int i = y * stride + x * 4;
                int r = b.get(i) & 0xFF, g = b.get(i + 1) & 0xFF, bl = b.get(i + 2) & 0xFF;
                samples++;
                if (r <= 3 && bl <= 3 && g >= 55 && g <= 100) {
                    hits++;
                }
            }
        }
        if (hits * 200 > samples) {
            greenFrames++;
            LimeLog.warning("PlayerXR headless: green patches in frame " + headlessFrames + ", "
                    + (hits * 100 / samples) + "% of it, film at " + engine.positionMs() / 1000.0 + " s, "
                    + greenFrames + " such frames so far");
        }
    }

    private Surface headlessSurface() {
        headless = ImageReader.newInstance(3840, 1080, PixelFormat.RGBA_8888, 3);
        headlessSince = SystemClock.elapsedRealtime();
        headless.setOnImageAvailableListener(reader -> {
            Image image = reader.acquireLatestImage();
            if (image == null) {
                return;
            }
            headlessFrames++;
            countGreen(image);
            if (headlessFrames == 150) {
                Image.Plane plane = image.getPlanes()[0];
                int w = image.getWidth(), h = image.getHeight();
                int stride = plane.getRowStride() / 4;
                Bitmap bitmap = Bitmap.createBitmap(stride, h, Bitmap.Config.ARGB_8888);
                bitmap.copyPixelsFromBuffer(plane.getBuffer());
                File dir = getExternalFilesDir("ui");
                if (dir != null) {
                    try (java.io.FileOutputStream out = new java.io.FileOutputStream(new File(dir, "disc_frame.png"))) {
                        Bitmap.createBitmap(bitmap, 0, 0, w, h).compress(Bitmap.CompressFormat.PNG, 100, out);
                    }
                    catch (java.io.IOException ignored) {
                    }
                }
            }
            if (headlessFrames % 120 == 0) {
                long ms = SystemClock.elapsedRealtime() - headlessSince;
                LimeLog.info("PlayerXR headless: " + headlessFrames + " frames in " + ms / 1000 + " s, "
                        + String.format("%.1f", headlessFrames * 1000.0 / ms) + " per second, film at "
                        + engine.positionMs() / 1000 + " s");
            }
            image.close();
        }, main);
        return headless.getSurface();
    }

    // The renderer shows neither the screen nor its panels until a frame arrives, so browse
    // mode posts a placeholder frame
    private void showIdleFrame(Surface surface) {
        try {
            android.graphics.Canvas c = surface.lockHardwareCanvas();
            int w = c.getWidth(), h = c.getHeight();
            c.drawColor(0xFF0B0B10);
            android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            p.setColor(0x99FFFFFF);
            p.setTextSize(h / 18f);
            p.setTextAlign(android.graphics.Paint.Align.CENTER);
            p.setTypeface(android.graphics.Typeface.create("sans-serif-light", android.graphics.Typeface.NORMAL));
            c.drawText("Choose a film from the library", w / 2f, h / 2f, p);
            surface.unlockCanvasAndPost(c);
        }
        catch (RuntimeException e) {
            LimeLog.warning("PlayerXR: could not draw the idle frame: " + e);
        }
    }

    // ---- resume position and letterbox --------------------------------------------------

    private final Runnable saver = new Runnable() {
        @Override
        public void run() {
            saveProgress();
            main.postDelayed(this, SAVE_EVERY_MS);
        }
    };

    private void saveProgress() {
        // Debug scripts don't overwrite the real resume point
        if (com.limelight.BuildConfig.DEBUG && (getIntent().hasExtra("debugScript") || getIntent().hasExtra("debugHeadless"))) {
            return;
        }
        PlaybackEngine e = engine;
        if (e == null || filmKey == null || !e.isReady()) {
            return;
        }
        long duration = e.durationMs();
        if (duration <= 0) {
            return;
        }
        store.saveProgress(filmKey, e.positionMs(), duration);
    }

    // Detects letterbox bars in the background, stores them and crops the screen to the picture
    private void findBars(Uri uri) {
        final StereoLayout forLayout = layout;
        new Thread(() -> {
            float[] bars = LetterboxProbe.detect(PlayerXR.this, uri, forLayout);
            if (bars == null || finishing) {
                return;
            }
            store.saveBars(filmKey, bars[0], bars[1]);
            XrRenderer r = renderer;
            if (r != null) {
                r.setPlayerContent(bars[0], bars[1]);
            }
        }, "PlayerXR bars").start();
    }

    // ---- restarts and file switching ---------------------------------------------------

    // The stereo layout is fixed at session start, so changing it restarts in place
    private void restartWithLayout(String mode) {
        if (mode.equals(getIntent().getStringExtra(EXTRA_LAYOUT))
                || (mode.equals("auto") && getIntent().getStringExtra(EXTRA_LAYOUT) == null)) {
            return;
        }
        getIntent().putExtra(EXTRA_LAYOUT, mode);
        restart();
    }

    private void restart() {
        saveProgress();
        getIntent().putExtra(EXTRA_POSITION_MS, engine != null ? engine.positionMs() : 0);
        getIntent().putExtra(EXTRA_SPEED, speed);
        keepTracks();
        recreate();
    }

    // Saves the selected tracks so the new engine after a restart can reselect them
    private void keepTracks() {
        forgetTracks();
        PlaybackEngine e = engine;
        if (e == null) {
            return;
        }
        List<PlayerUi.TrackChoice> subs = e.subtitleTracks();
        int sub = selectedIn(subs);
        if (sub >= 0) {
            getIntent().putExtra(EXTRA_SUB_TRACK, sub);
            getIntent().putExtra(EXTRA_SUB_COUNT, subs.size());
        }
        List<PlayerUi.TrackChoice> audio = e.audioTracks();
        int track = selectedIn(audio);
        if (track >= 0) {
            getIntent().putExtra(EXTRA_AUDIO_TRACK, track);
            getIntent().putExtra(EXTRA_AUDIO_COUNT, audio.size());
        }
    }

    private void forgetTracks() {
        getIntent().removeExtra(EXTRA_SUB_TRACK);
        getIntent().removeExtra(EXTRA_SUB_COUNT);
        getIntent().removeExtra(EXTRA_AUDIO_TRACK);
        getIntent().removeExtra(EXTRA_AUDIO_COUNT);
    }

    private static int selectedIn(List<PlayerUi.TrackChoice> list) {
        for (int i = 0; i < list.size(); i++) {
            if (list.get(i).selected) {
                return i;
            }
        }
        return -1;
    }

    // Reselects kept tracks once the engine has listed them; polls for up to 10 s
    private static final long TRACK_LOOK_MS = 250;
    private static final int TRACK_LOOKS = 40;

    private void restoreTracks(int look) {
        if (!getIntent().hasExtra(EXTRA_SUB_TRACK) && !getIntent().hasExtra(EXTRA_AUDIO_TRACK)) {
            return;
        }
        PlaybackEngine e = engine;
        if (e == null || finishing) {
            return;
        }
        boolean subsDone = restoreTrack(e.subtitleTracks(), EXTRA_SUB_TRACK, EXTRA_SUB_COUNT, false);
        boolean audioDone = restoreTrack(e.audioTracks(), EXTRA_AUDIO_TRACK, EXTRA_AUDIO_COUNT, true);
        if ((subsDone && audioDone) || look + 1 >= TRACK_LOOKS) {
            forgetTracks();
            return;
        }
        main.postDelayed(() -> restoreTracks(look + 1), TRACK_LOOK_MS);
    }

    // True when done: reselected, already selected, or nothing was kept
    private boolean restoreTrack(List<PlayerUi.TrackChoice> list, String indexKey, String countKey,
                                 boolean audio) {
        int index = getIntent().getIntExtra(indexKey, -1);
        if (index < 0) {
            return true;
        }
        if (list.size() != getIntent().getIntExtra(countKey, -1)) {
            // Track list not complete yet
            return false;
        }
        PlayerUi.TrackChoice choice = list.get(index);
        if (!choice.selected) {
            if (audio) {
                engine.chooseAudio(choice);
            }
            else {
                engine.chooseSubtitle(choice);
            }
        }
        return true;
    }

    private void backToLibrary() {
        saveProgress();
        forgetTracks();
        getIntent().removeExtra(EXTRA_SPEED);
        getIntent().setData(null);
        getIntent().removeExtra(EXTRA_LAYOUT);
        getIntent().removeExtra(EXTRA_POSITION_MS);
        recreate();
    }

    // Restarts on another file, resuming it and detecting its layout afresh
    private void openFile(Uri uri) {
        saveProgress();
        if (ui != null) {
            getIntent().putExtra(EXTRA_LIBRARY_PLACE, ui.libraryPlace());
        }
        forgetTracks();
        getIntent().removeExtra(EXTRA_SPEED);
        getIntent().setData(uri);
        getIntent().removeExtra(EXTRA_LAYOUT);
        getIntent().removeExtra(EXTRA_POSITION_MS);
        recreate();
    }

    // Outward eye shift as a fraction of width. 1.5% suits parallel-camera footage; converged
    // Blu-ray masters keep more pop-out with less.
    static float cropFor(String name) {
        if (DEPTH_POP.equals(name)) {
            return 0.0f;
        }
        if (DEPTH_BALANCED.equals(name)) {
            return 0.0075f;
        }
        return 0.015f;
    }

    // Display title: file name without extension or stereo tags, dots/underscores as spaces
    static String titleFor(Uri uri) {
        String name = uri.getLastPathSegment();
        if (name == null) {
            return "";
        }
        int dot = name.lastIndexOf('.');
        if (dot > 0) {
            name = name.substring(0, dot);
        }
        name = name.replaceAll("[._]+", " ")
                .replaceAll("(?i)\\b(h-?sbs|f-?sbs|sbs|h-?ou|f-?ou|ou|tab|3d|mvc)\\b", " ")
                .replaceAll("\\s+", " ").trim();
        return name;
    }

    // Resume on onStart if it was playing at onStop
    private boolean resumeOnStart;

    @Override
    protected void onStart() {
        super.onStart();
        PlaybackEngine e = engine;
        if (resumeOnStart && e != null) {
            resumeOnStart = false;
            e.setPlaying(true);
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        saveProgress();
        PlaybackEngine e = engine;
        if (e != null) {
            resumeOnStart = e.isPlaying();
            e.setPlaying(false);
        }
    }

    @Override
    protected void onDestroy() {
        finishing = true;
        main.removeCallbacks(saver);
        saveProgress();
        if (ui != null) {
            ui.stop();
            ui = null;
        }
        if (renderer != null) {
            // Order: stop the frame loop, release the engine, then the surface
            renderer.prepareForStop();
        }
        if (engine != null) {
            engine.release();
            engine = null;
        }
        if (renderer != null) {
            renderer.cleanup();
            renderer = null;
        }
        super.onDestroy();
    }

    // ---- input ------------------------------------------------------------

    // A tap on the picture toggles the controls. Double-tapping the left or right third seeks
    // -/+10 s, and each further quick tap seeks again, so side taps are delayed by
    // DOUBLE_TAP_MS before counting as a single tap; centre taps act at once.
    private static final long DOUBLE_TAP_MS = 300;
    private static final float TAP_SIDE_SHARE = 1.0f / 3.0f;
    // Last pointer position across the picture, 0 = left edge, 1 = right. Frame thread.
    private volatile float pointerU = 0.5f;
    // Last tap's side (-1, 0, 1) and time. Main thread.
    private int tapSide;
    private long tapAt;
    private final Runnable singleTap = () -> {
        PlayerUi current = ui;
        if (current != null) {
            current.onPictureClick();
        }
    };

    @Override
    public void onVrButton(int button, boolean down) {
        if (button == 0 && down) {
            float u = pointerU;
            main.post(() -> onPictureTap(u));
        }
    }

    private void onPictureTap(float u) {
        main.removeCallbacks(singleTap);
        int side = u < TAP_SIDE_SHARE ? -1 : u > 1.0f - TAP_SIDE_SHARE ? 1 : 0;
        boolean seekable = !browse && ui != null && !ui.sheetOpen();
        long now = SystemClock.uptimeMillis();
        boolean again = side != 0 && side == tapSide && now - tapAt < DOUBLE_TAP_MS;
        tapSide = side;
        tapAt = now;
        if (seekable && again) {
            seekBy(side * SEEK_STEP_MS);
        }
        else if (seekable && side != 0) {
            main.postDelayed(singleTap, DOUBLE_TAP_MS);
        }
        else {
            singleTap.run();
        }
    }

    // A toggles play. Thumbstick left/right seeks 10 s, auto-repeating while held, 30 s per
    // step after SEEK_FAST_AFTER_MS.
    private static final int PAD_A = 1;
    private static final float STICK_ON = 0.6f, STICK_OFF = 0.35f;
    private static final long SEEK_FIRST_REPEAT_MS = 500, SEEK_REPEAT_MS = 250, SEEK_FAST_AFTER_MS = 1600;
    private static final long SEEK_STEP_MS = 10000, SEEK_FAST_STEP_MS = 30000;
    private int heldPadButtons;
    private boolean stickPushed;
    private long stickSince, stickNext;
    private long seekAimMs = -1, seekAimAt;
    // Start of the current run of skips, for showing the total offset
    private long seekFromMs;

    @Override
    public void onVrPlayerInput(int buttons, float stickX) {
        int pressed = buttons & ~heldPadButtons;
        heldPadButtons = buttons;
        if ((pressed & PAD_A) != 0) {
            main.post(this::togglePlay);
        }
        long now = SystemClock.uptimeMillis();
        float side = Math.abs(stickX);
        if (side < (stickPushed ? STICK_OFF : STICK_ON)) {
            stickPushed = false;
            return;
        }
        if (!stickPushed) {
            stickPushed = true;
            stickSince = now;
            stickNext = now;
        }
        if (now >= stickNext) {
            boolean fast = now - stickSince > SEEK_FAST_AFTER_MS;
            stickNext = now + (now == stickSince ? SEEK_FIRST_REPEAT_MS : SEEK_REPEAT_MS);
            long step = (stickX > 0 ? 1 : -1) * (fast ? SEEK_FAST_STEP_MS : SEEK_STEP_MS);
            main.post(() -> seekBy(step));
        }
    }

    private void applySubtitleDelay(long ms) {
        subtitleDelayMs = ms;
        engine.setSubtitleDelayMs(ms);
        if (filmKey != null) {
            saved.edit().putLong(PREF_SUB_DELAY + filmKey, ms).apply();
        }
    }

    private void togglePlay() {
        PlaybackEngine e = engine;
        if (e == null || browse) {
            return;
        }
        e.setPlaying(!e.isPlaying());
        if (ui != null) {
            ui.nudge();
        }
    }

    // Relative seek. Within 1.5 s of the previous one, steps from its target rather than the
    // current position, which may not have caught up yet.
    private void seekBy(long deltaMs) {
        PlaybackEngine e = engine;
        if (e == null || browse || (ui != null && ui.sheetOpen())) {
            return;
        }
        long duration = e.durationMs();
        if (duration <= 0) {
            return;
        }
        long now = SystemClock.uptimeMillis();
        boolean goingOn = seekAimMs >= 0 && now - seekAimAt < 1500;
        long from = goingOn ? seekAimMs : e.positionMs();
        if (!goingOn) {
            seekFromMs = from;
        }
        long to = Math.max(0, Math.min(duration - 1000, from + deltaMs));
        seekAimMs = to;
        seekAimAt = now;
        e.seekToMs(to);
        if (ui != null) {
            ui.showSkip(to, to - seekFromMs);
        }
    }

    @Override
    public void onVrExit() {
        main.post(this::finish);
    }

    @Override
    public void onVrPointerMove(float u, float v) {
        pointerU = u;
        // Pointer movement keeps the controls visible
        PlayerUi current = ui;
        if (current != null) {
            current.onActivity();
        }
    }

    @Override
    public void onVrMouseMove(int dx, int dy) {
    }

    @Override
    public void onVrScroll(int clicks) {
        main.post(() -> {
            PlayerUi current = ui;
            if (current != null) {
                current.onScroll(clicks);
            }
        });
    }

    @Override
    public void onVrGamepadPlugged(boolean plugged) {
    }

    @Override
    public void onVrGamepadState(int buttons, int leftTrigger, int rightTrigger,
                                 int leftX, int leftY, int rightX, int rightY) {
    }

    @Override
    public void onVrKey(int code, int modifiers) {
    }

    @Override
    public void onVrModifiers(int modifiers) {
    }

    // ---- session ------------------------------------------------------------

    @Override
    public void onVrUnavailable(XrStartFailure failure) {
        LimeLog.severe("PlayerXR: VR unavailable: " + failure);
        main.post(this::finish);
    }

    @Override
    public void onVrSessionEnded(String reason) {
        main.post(this::finish);
    }

    // Paused by session focus loss (system menu, headset removed); resume only on regain
    private boolean pausedForFocus;

    @Override
    public void onVrFocusLost() {
        main.post(() -> {
            PlaybackEngine e = engine;
            if (e != null && !browse && e.isPlaying()) {
                e.setPlaying(false);
                pausedForFocus = true;
                saveProgress();
                LimeLog.info("PlayerXR: paused, the session lost its focus");
            }
        });
    }

    @Override
    public void onVrFocusGained() {
        main.post(() -> {
            PlaybackEngine e = engine;
            if (pausedForFocus && e != null) {
                pausedForFocus = false;
                e.setPlaying(true);
                LimeLog.info("PlayerXR: playing again, the session has its focus back");
            }
        });
    }

    // ---- file probe -------------------------------------------------------------

    /** Video track size and frame rate, read before the renderer starts. */
    static final class VideoProbe {
        final int width;
        final int height;
        final int fps;

        VideoProbe(int width, int height, int fps) {
            this.width = width;
            this.height = height;
            this.fps = fps;
        }

        static VideoProbe read(Activity activity, Uri uri) {
            MediaExtractor ex = new MediaExtractor();
            try {
                ex.setDataSource(activity, uri, null);
                for (int i = 0; i < ex.getTrackCount(); i++) {
                    MediaFormat f;
                    try {
                        f = ex.getTrackFormat(i);
                    }
                    catch (IllegalArgumentException unsupported) {
                        // MediaExtractor rejects some MKV audio tracks; only video is needed
                        continue;
                    }
                    String mime = f.getString(MediaFormat.KEY_MIME);
                    if (mime != null && mime.startsWith("video/")) {
                        int fps = f.containsKey(MediaFormat.KEY_FRAME_RATE)
                                ? Math.round(f.getNumber(MediaFormat.KEY_FRAME_RATE).floatValue()) : 24;
                        return new VideoProbe(f.getInteger(MediaFormat.KEY_WIDTH),
                                f.getInteger(MediaFormat.KEY_HEIGHT), fps > 0 ? fps : 24);
                    }
                }
            }
            catch (Exception e) {
                LimeLog.warning("PlayerXR: could not read the video track: " + e);
            }
            finally {
                ex.release();
            }
            return new VideoProbe(1920, 1080, 24);
        }
    }
}
