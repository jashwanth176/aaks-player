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
    // Library tab, page and folder to return to
    private static final String EXTRA_LIBRARY_PLACE = "libraryPlace";

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

    // What the intent names: a film's frame size per eye, layout and depth mode, or the library
    private static final class Film {
        Uri uri;
        VideoProbe probe;
        int eyeWidth, eyeHeight, depthMode;
        // A 3D disc shown as 2D: one eye, flat
        boolean flat;
        PlaybackStore.Entry known;
    }

    private VideoProbe probe;

    private Film plan() {
        return plan(null);
    }

    // `playing`: the probe of the film already playing, to skip reading the file again
    private Film plan(VideoProbe playing) {
        Film f = new Film();
        Uri uri = getIntent().getData();
        f.uri = uri;
        browse = uri == null;
        filmKey = null;
        disc = false;
        if (!browse) {
            filmKey = "file".equals(uri.getScheme()) && uri.getPath() != null ? uri.getPath() : uri.toString();
            disc = "file".equals(uri.getScheme()) && uri.getPath() != null && VideoLibrary.isDisc(new File(uri.getPath()));
        }
        // Disc: 1920x1080 per eye, decoded side by side, 23.976 Hz. Browse: placeholder 16:9.
        f.probe = browse ? new VideoProbe(1920, 1080, 24)
                : disc ? new VideoProbe(3840, 1080, 24)
                : playing != null ? playing : VideoProbe.read(this, uri);
        probe = f.probe;
        String requested = getIntent().getStringExtra(EXTRA_LAYOUT);
        layout = browse ? StereoLayout.MONO : disc ? StereoLayout.SBS
                : StereoLayout.resolve(requested, uri.getLastPathSegment(), f.probe.width, f.probe.height);
        f.flat = disc && "mono".equalsIgnoreCase(requested);
        f.eyeWidth = layout.eyeWidth(f.probe.width, f.probe.height);
        f.eyeHeight = layout.eyeHeight(f.probe.width, f.probe.height);
        if (layout == StereoLayout.SBS) {
            f.depthMode = XrShared.DEPTH_MODE_NATIVE_SBS;
        }
        else if (layout == StereoLayout.OU) {
            f.depthMode = XrShared.DEPTH_MODE_NATIVE_OU;
        }
        else {
            f.depthMode = convert2d && !browse ? XrShared.DEPTH_MODE_MODEL : XrShared.DEPTH_MODE_OFF;
        }
        f.known = browse ? new PlaybackStore.Entry() : store.get(filmKey);
        LimeLog.info("PlayerXR: " + uri + " " + f.probe.width + "x" + f.probe.height + " @" + f.probe.fps
                + " layout=" + layout + " eye=" + f.eyeWidth + "x" + f.eyeHeight
                + " depthMode=" + f.depthMode + (disc ? " disc" : "") + (f.flat ? " flat" : ""));
        return f;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
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
        Film film = plan();
        // The renderer picks a display rate that is a multiple of this (72 Hz for 24 fps)
        prefs.fps = film.probe.fps;
        prefs.vrDepthMode = film.depthMode;

        renderer = new XrRenderer();
        renderer.setPlayerMode(true);
        // Must precede start() so the library window placement is restored
        renderer.setPlayerScreenHidden(browse);
        renderer.setPlayerLighting(immersion, brightness);
        renderer.setPlayerCurve(curve);
        renderer.setColourMode(colourMode);
        renderer.setHandAutoHide(handAutoHide);
        renderer.setPlayerDepth(cropFor(depth));
        if (film.known.barsKnown) {
            renderer.setPlayerContent(film.known.barTop, film.known.barBottom);
        }
        renderer.setInputListener(this);
        renderer.setSessionListener(this);
        // start() blocks until the session is running, so call it off the main thread
        new Thread(() -> {
            boolean ok = renderer.start(PlayerXR.this, film.eyeWidth, film.eyeHeight, prefs);
            main.post(() -> {
                if (!ok || finishing) {
                    finish();
                    return;
                }
                beginFilm(film, true);
            });
        }, "PlayerXR start").start();
    }

    private final PlaybackEngine.Listener engineListener = new PlaybackEngine.Listener() {
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

    // Starts the planned film (or the library) on the running session
    private void beginFilm(Film f, boolean first) {
        engine = browse ? new NullEngine()
                : disc ? new DiscEngine(this, new File(f.uri.getPath()), engineListener)
                : new ExoEngine(this, f.uri, engineListener);
        subtitleDelayMs = filmKey != null ? saved.getLong(PREF_SUB_DELAY + filmKey, 0L) : 0L;
        engine.setSubtitleDelayMs(subtitleDelayMs);
        speed = 1.0f;
        long start = f.known.resumeMs();
        Surface target = first ? renderer.getInputSurface() : renderer.newInputSurface();
        if (com.limelight.BuildConfig.DEBUG && getIntent().hasExtra("debugHeadless")) {
            target = headlessSurface();
        }
        engine.start(target, start, true);
        if (ui == null) {
            createUi();
        }
        if (browse) {
            if (first) {
                showIdleFrame(target);
            }
            ui.startBrowsing();
        }
        else {
            ui.startFilm(titleFor(f.uri));
        }
        if (!f.known.barsKnown && !disc && !browse) {
            findBars(f.uri);
        }
    }

    // Counts opens, so a probe that finishes after the next open is dropped
    private int openToken;
    // The screen's size per eye as last configured
    private int eyeW = 1920, eyeH = 1080;

    // Another film or the library, in the running session. Probing a big file takes seconds,
    // so it runs off the main thread while the film's chrome and the spinner are already up.
    private void reload() {
        PlaybackEngine old = engine;
        engine = null;
        if (old != null) {
            old.release();
        }
        final int token = ++openToken;
        final Uri uri = getIntent().getData();
        boolean probed = uri != null && !("file".equals(uri.getScheme()) && uri.getPath() != null
                && VideoLibrary.isDisc(new File(uri.getPath())));
        if (!probed) {
            show(plan());
            return;
        }
        renderer.setVideoColour(XrShared.VIDEO_TRANSFER_CODE_SDR, 0.0f, 0.0f);
        renderer.setPlayerContent(0.0f, 0.0f);
        renderer.reconfigure(eyeW, eyeH, 24, XrShared.DEPTH_MODE_OFF, false, false, true);
        ui.startFilm(titleFor(uri));
        new Thread(() -> {
            VideoProbe read = VideoProbe.read(PlayerXR.this, uri);
            main.post(() -> {
                if (token == openToken && !finishing) {
                    show(plan(read));
                }
            });
        }, "PlayerXR probe").start();
    }

    private void show(Film f) {
        renderer.setVideoColour(XrShared.VIDEO_TRANSFER_CODE_SDR, 0.0f, 0.0f);
        renderer.setPlayerContent(f.known.barsKnown ? f.known.barTop : 0.0f,
                f.known.barsKnown ? f.known.barBottom : 0.0f);
        configure(f, false);
        beginFilm(f, false);
    }

    private void configure(Film f, boolean sameFilm) {
        eyeW = f.eyeWidth;
        eyeH = f.eyeHeight;
        renderer.reconfigure(f.eyeWidth, f.eyeHeight, f.probe.fps, f.depthMode, browse, sameFilm, !f.flat);
    }

    // A new layout or 2D-to-3D setting for the film playing: the decoder runs on and only the
    // screen changes, so the picture never stops
    private void relayout() {
        if (browse || probe == null) {
            return;
        }
        configure(plan(probe), true);
    }

    private void createUi() {
        ui = new PlayerUi(this, renderer, new PlayerUi.Actions() {
            @Override
            public void onBack() {
                if (ui != null && ui.onBackPressed()) {
                    return;
                }
                if (browse) {
                    leave();
                }
                else {
                    backToLibrary();
                }
            }

            @Override
            public void onLeave() {
                leave();
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
                changeLayout(mode);
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
            public String filmPath() {
                return browse ? null : filmKey;
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
                    relayout();
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
        ui.start(browse ? "Library" : titleFor(getIntent().getData()));
        String place = getIntent().getStringExtra(EXTRA_LIBRARY_PLACE);
        if (place != null) {
            ui.restoreLibrary(place);
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
        final String key = filmKey;
        new Thread(() -> {
            float[] bars = LetterboxProbe.detect(PlayerXR.this, uri, forLayout);
            if (bars == null || finishing) {
                return;
            }
            store.saveBars(key, bars[0], bars[1]);
            main.post(() -> {
                XrRenderer r = renderer;
                if (r != null && key.equals(filmKey)) {
                    r.setPlayerContent(bars[0], bars[1]);
                }
            });
        }, "PlayerXR bars").start();
    }

    // ---- layouts and file switching ----------------------------------------------------

    private void changeLayout(String mode) {
        if (mode.equals(getIntent().getStringExtra(EXTRA_LAYOUT))
                || (mode.equals("auto") && getIntent().getStringExtra(EXTRA_LAYOUT) == null)) {
            return;
        }
        getIntent().putExtra(EXTRA_LAYOUT, mode);
        relayout();
    }

    private void backToLibrary() {
        saveProgress();
        getIntent().setData(null);
        getIntent().removeExtra(EXTRA_LAYOUT);
        reload();
    }

    // Switches to another file, resuming it and detecting its layout afresh
    private void openFile(Uri uri) {
        long now = SystemClock.uptimeMillis();
        if (now - lastOpenMs < OPEN_DEBOUNCE_MS) {
            return;
        }
        lastOpenMs = now;
        saveProgress();
        getIntent().setData(uri);
        getIntent().removeExtra(EXTRA_LAYOUT);
        reload();
    }

    private static final long OPEN_DEBOUNCE_MS = 1000;
    private long lastOpenMs;

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
        if (leaving) {
            android.os.Process.killProcess(android.os.Process.myPid());
        }
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
        main.post(this::leave);
    }

    // Out of the app for good: the task goes too, and the process once the session is down,
    // so nothing of the app lingers in the headset's shell
    private boolean leaving;

    private void leave() {
        leaving = true;
        finishAndRemoveTask();
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
