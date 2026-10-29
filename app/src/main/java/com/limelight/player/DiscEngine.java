package com.limelight.player;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;

import androidx.media3.common.text.Cue;

import com.limelight.LimeLog;

import java.io.File;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * Plays a Blu-ray 3D disc image. libdisc decodes both eyes into the surface; DiscAudio plays
 * the audio and its AudioTrack is the master clock; PGS subtitles are timed against that clock.
 */
final class DiscEngine implements PlaybackEngine, DiscNative.Sink, DiscAudio.Callback {

    private static final class Stream {
        final int pid;
        final int coding;
        final int channels;
        final String lang;

        Stream(int pid, int coding, int channels, String lang) {
            this.pid = pid;
            this.coding = coding;
            this.channels = channels;
            this.lang = lang;
        }
    }

    private static final int DECODER_THREADS = 4;
    private static final long SUBTITLE_TICK_MS = 40;

    private final Context context;
    private final File iso;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor(r -> new Thread(r, "DiscEngine"));
    private volatile long handle;
    private volatile boolean opened;
    private long duration90;
    private final List<Stream> audioStreams = new ArrayList<>();
    private final List<Stream> subtitleStreams = new ArrayList<>();
    private volatile int audioPid = -1;
    private volatile int subtitlePid = -1;
    private DiscAudio audio;
    private PgsSubtitles pgs;
    private final ArrayDeque<PgsSubtitles.Event> events = new ArrayDeque<>();
    private volatile boolean wantPlaying = true;
    private volatile boolean frameShown;
    // Audio has started since the last start or seek
    private volatile boolean audioStartedOnce;
    private boolean released;
    // Re-seek if no picture appears after a seek
    private int placeToken;
    private int placeRetries;
    private static final long PICTURE_WAIT_MS = 1500;
    private static final int PICTURE_RETRIES = 3;
    // How long video waits for audio to start before running on its own clock
    private static final long SOUND_WAIT_MS = 800;
    private int playToken;
    // Positive = subtitles later, in 90 kHz ticks
    private volatile long subtitleDelay90;
    // Pause on focus loss, resume on regain. Without focus the system fades the app's audio out for good.
    private AudioFocusRequest focusRequest;
    private boolean pausedByFocus;

    DiscEngine(Context context, File iso, Listener listener) {
        this.context = context;
        this.iso = iso;
        this.listener = listener;
    }

    @Override
    public void start(Surface surface, long startMs, boolean play) {
        wantPlaying = play;
        if (play) {
            takeFocus();
        }
        worker.execute(() -> open(surface, startMs));
    }

    private void open(Surface surface, long startMs) {
        long h = DiscNative.open(iso.getAbsolutePath(), DECODER_THREADS);
        if (h == 0) {
            listener.onError("Can't open this disc", "It isn't a Blu-ray image, or it needs a key");
            return;
        }
        parse(DiscNative.describe(h));
        long start90 = startMs * 90L;
        audio = new DiscAudio(this);
        audio.setSpeed(speed);
        DiscNative.setSpeed(h, speed);
        pgs = new PgsSubtitles(event -> {
            synchronized (events) {
                events.add(event);
            }
        });
        pickAudio(h, start90);
        handle = h;
        opened = DiscNative.start(h, surface, start90, this, false);
        if (!opened) {
            listener.onError("Can't play this disc", "The picture could not start");
            return;
        }
        main.postDelayed(subtitleTick, SUBTITLE_TICK_MS);
        watchPicture(start90);
    }

    // The decoder occasionally stalls after a seek with no picture; seeking again clears it
    private void watchPicture(final long t90) {
        final int token = ++placeToken;
        main.postDelayed(() -> {
            if (token != placeToken || frameShown || released || !opened) {
                return;
            }
            if (placeRetries >= PICTURE_RETRIES) {
                LimeLog.warning("DiscEngine: still no picture after " + placeRetries + " tries");
                return;
            }
            placeRetries++;
            LimeLog.warning("DiscEngine: no picture " + PICTURE_WAIT_MS + " ms after a seek to "
                    + t90 / 90000.0 + " s, trying again (" + placeRetries + ")");
            worker.execute(() -> seekTo(t90));
        }, PICTURE_WAIT_MS);
    }

    private void parse(String text) {
        for (String line : text.split("\n")) {
            String[] f = line.split("\t", -1);
            if (f.length < 2) {
                continue;
            }
            switch (f[0]) {
                case "D":
                    duration90 = Long.parseLong(f[1]);
                    break;
                case "A":
                    audioStreams.add(new Stream(Integer.parseInt(f[1]), Integer.parseInt(f[2]),
                            Integer.parseInt(f[3]), f.length > 5 ? f[5] : ""));
                    break;
                case "S":
                    if (Integer.parseInt(f[2]) == DiscNativeCodings.PGS) {
                        subtitleStreams.add(new Stream(Integer.parseInt(f[1]), Integer.parseInt(f[2]), 0,
                                f.length > 5 ? f[5] : ""));
                    }
                    break;
                default:
                    break;
            }
        }
        LimeLog.info("DiscEngine: " + audioStreams.size() + " audio, " + subtitleStreams.size()
                + " subtitle streams, " + duration90 / 90000 + " s");
    }

    // Prefer playable English audio, else the first playable stream
    private void pickAudio(long h, long start90) {
        Stream pick = null;
        for (Stream s : audioStreams) {
            if (DiscAudio.supports(s.coding) && (pick == null || (s.lang.equals("eng") && !pick.lang.equals("eng")))) {
                pick = s;
            }
        }
        if (pick == null) {
            listener.onError("No playable audio", "This disc's sound needs a decoder this player does not have yet");
            return;
        }
        Stream english = null;
        for (Stream s : audioStreams) {
            if (s.lang.equals("eng")) {
                english = s;
                break;
            }
        }
        if (english != null && english != pick) {
            listener.onError("English sound isn't playable yet",
                    DiscNativeCodings.audioName(english.coding) + " is not in the device's decoders; playing "
                            + DiscNativeCodings.languageName(pick.lang));
        }
        audioPid = pick.pid;
        DiscNative.wantPid(h, pick.pid, true);
        audio.select(pick.coding, start90);
        // Non-English audio: turn on English subtitles if present
        if (!pick.lang.equals("eng")) {
            for (Stream sub : subtitleStreams) {
                if (sub.lang.equals("eng")) {
                    subtitlePid = sub.pid;
                    DiscNative.wantPid(h, sub.pid, true);
                    break;
                }
            }
        }
    }

    // ---- native callbacks --------------------------------------------------------------------

    @Override
    public void onPes(int pid, long pts90, byte[] data) {
        if (pid == audioPid) {
            DiscAudio a = audio;
            if (a != null) {
                a.feed(pts90, data);
            }
        }
        else if (pid == subtitlePid) {
            PgsSubtitles p = pgs;
            if (p != null) {
                synchronized (p) {
                    p.feed(pts90, data);
                }
            }
        }
    }

    @Override
    public void onFirstFrame(long pts90) {
        main.post(() -> {
            frameShown = true;
            placeRetries = 0;
            if (wantPlaying) {
                resumeOutput();
            }
        });
    }

    @Override
    public void onEnded() {
        main.post(() -> wantPlaying = false);
    }

    @Override
    public void audioStarted() {
        audioStartedOnce = true;
        long h = handle;
        if (h != 0 && wantPlaying) {
            DiscNative.setPlaying(h, true);
        }
    }

    @Override
    public void clockSync(long media90, long nanos) {
        long h = handle;
        if (h != 0) {
            DiscNative.syncClock(h, media90, nanos);
        }
    }

    @Override
    public void audioFailed(String reason) {
        listener.onError("Audio stopped", reason);
        audio = null;
        long h = handle;
        if (h != 0 && wantPlaying) {
            DiscNative.setPlaying(h, true);
        }
    }

    // ---- control -------------------------------------------------------------------------------

    // Video starts once audio has started (see audioStarted), or immediately without audio
    private void resumeOutput() {
        long h = handle;
        if (h == 0) {
            return;
        }
        DiscAudio a = audio;
        if (a != null && !audioStartedOnce) {
            a.setPlaying(true);
            // If audio doesn't start within SOUND_WAIT_MS, run video anyway; audio joins later
            final int token = ++playToken;
            main.postDelayed(() -> {
                if (token == playToken && wantPlaying && !audioStartedOnce && handle == h && !released) {
                    LimeLog.warning("DiscEngine: the sound did not start, the picture goes without it");
                    DiscNative.setPlaying(h, true);
                }
            }, SOUND_WAIT_MS);
            return;
        }
        if (a != null) {
            a.setPlaying(true);
        }
        DiscNative.setPlaying(h, true);
    }

    @Override
    public long positionMs() {
        long h = handle;
        return h != 0 ? DiscNative.position(h) / 90 : 0;
    }

    @Override
    public long durationMs() {
        return duration90 / 90;
    }

    @Override
    public boolean isPlaying() {
        return wantPlaying;
    }

    @Override
    public void setPlaying(boolean on) {
        wantPlaying = on;
        if (on) {
            takeFocus();
        }
        else {
            pausedByFocus = false;
        }
        long h = handle;
        if (!opened || h == 0) {
            return;
        }
        if (on) {
            if (frameShown) {
                resumeOutput();
            }
        }
        else {
            playToken++;
            DiscAudio a = audio;
            if (a != null) {
                a.setPlaying(false);
            }
            DiscNative.setPlaying(h, false);
        }
    }

    // Applied in open() if set before the disc is open
    private volatile float speed = 1.0f;

    // Video clock and audio track both run at the speed, so audio sync stays in media time
    @Override
    public void setSpeed(float value) {
        speed = value;
        long h = handle;
        if (h != 0) {
            DiscNative.setSpeed(h, value);
        }
        DiscAudio a = audio;
        if (a != null) {
            a.setSpeed(value);
        }
    }

    @Override
    public void seekToMs(long ms) {
        final long t90 = Math.max(0, ms) * 90L;
        placeRetries = 0;
        worker.execute(() -> seekTo(t90));
    }

    @Override
    public void setSubtitleDelayMs(long ms) {
        subtitleDelay90 = ms * 90L;
    }

    private void takeFocus() {
        AudioManager am = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) {
            return;
        }
        if (focusRequest == null) {
            focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
                    .setOnAudioFocusChangeListener(this::onFocusChange, main)
                    .build();
        }
        int result = am.requestAudioFocus(focusRequest);
        if (result != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            LimeLog.warning("DiscEngine: audio focus not granted (" + result + ")");
        }
    }

    private void onFocusChange(int change) {
        if (change == AudioManager.AUDIOFOCUS_LOSS || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) {
            if (wantPlaying) {
                LimeLog.info("DiscEngine: audio focus lost (" + change + "), paused");
                pausedByFocus = true;
                setPlaying(false);
                // Set again: setPlaying(false) clears it
                pausedByFocus = true;
            }
        }
        else if (change == AudioManager.AUDIOFOCUS_GAIN && pausedByFocus) {
            LimeLog.info("DiscEngine: audio focus back, playing");
            pausedByFocus = false;
            setPlaying(true);
        }
    }

    private void seekTo(long t90) {
        long h = handle;
        if (!opened || h == 0) {
            return;
        }
        frameShown = false;
        audioStartedOnce = false;
        playToken++;
        DiscAudio a = audio;
        if (a != null) {
            a.setPlaying(false);
            a.flush(t90);
        }
        clearSubtitles();
        DiscNative.seek(h, t90);
        main.post(() -> watchPicture(t90));
    }

    private void clearSubtitles() {
        PgsSubtitles p = pgs;
        if (p != null) {
            synchronized (p) {
                p.reset();
            }
        }
        synchronized (events) {
            events.clear();
        }
        listener.onSubtitle(null, null, 0.0f);
    }

    @Override
    public boolean isReady() {
        return opened && frameShown;
    }

    // ---- subtitles ---------------------------------------------------------------------------------

    // Main thread poll: shows the latest display set whose time has passed
    private final Runnable subtitleTick = new Runnable() {
        @Override
        public void run() {
            if (released) {
                return;
            }
            long now90 = DiscNative.position(handle) - subtitleDelay90;
            PgsSubtitles.Event show = null;
            synchronized (events) {
                while (!events.isEmpty() && events.peekFirst().pts90 <= now90) {
                    show = events.pollFirst();
                }
            }
            if (show != null) {
                Cue picture = null;
                for (Cue c : show.cues) {
                    if (c.bitmap != null) {
                        picture = c;
                        break;
                    }
                }
                if (picture != null) {
                    listener.onSubtitle(null, picture.bitmap, picture.size != Cue.DIMEN_UNSET ? picture.size : 0.0f);
                }
                else {
                    listener.onSubtitle(null, null, 0.0f);
                }
            }
            main.postDelayed(this, SUBTITLE_TICK_MS);
        }
    };

    // ---- tracks ----------------------------------------------------------------------------------------

    @Override
    public List<PlayerUi.TrackChoice> audioTracks() {
        List<PlayerUi.TrackChoice> out = new ArrayList<>();
        int n = 0;
        for (Stream s : audioStreams) {
            n++;
            String name = DiscNativeCodings.languageName(s.lang);
            if (name.isEmpty()) {
                name = "Audio " + n;
            }
            boolean ok = DiscAudio.supports(s.coding);
            String detail = DiscNativeCodings.audioName(s.coding);
            String ch = DiscNativeCodings.channelName(s.channels);
            if (!ch.isEmpty()) {
                detail += " · " + ch;
            }
            if (!ok) {
                detail += " · not playable yet";
            }
            out.add(new PlayerUi.TrackChoice(name, detail, s.pid == audioPid, ok, s));
        }
        return out;
    }

    @Override
    public List<PlayerUi.TrackChoice> subtitleTracks() {
        List<PlayerUi.TrackChoice> out = new ArrayList<>();
        out.add(new PlayerUi.TrackChoice("Off", "", subtitlePid < 0, true, null));
        int n = 0;
        for (Stream s : subtitleStreams) {
            n++;
            String name = DiscNativeCodings.languageName(s.lang);
            out.add(new PlayerUi.TrackChoice(name.isEmpty() ? "Subtitles " + n : name, "PGS",
                    s.pid == subtitlePid, true, s));
        }
        return out;
    }

    @Override
    public void chooseAudio(PlayerUi.TrackChoice choice) {
        final Stream s = (Stream) choice.token;
        worker.execute(() -> {
            long h = handle;
            if (!opened || h == 0 || s.pid == audioPid) {
                return;
            }
            int old = audioPid;
            if (old >= 0) {
                DiscNative.wantPid(h, old, false);
            }
            audioPid = s.pid;
            DiscNative.wantPid(h, s.pid, true);
            long here = DiscNative.position(h);
            DiscAudio a = audio;
            if (a != null) {
                a.select(s.coding, here);
            }
            // Re-seek so the new stream's packets are read from the current position
            seekTo(here);
        });
    }

    @Override
    public void chooseSubtitle(PlayerUi.TrackChoice choice) {
        final Stream s = (Stream) choice.token;
        worker.execute(() -> {
            long h = handle;
            if (!opened || h == 0) {
                return;
            }
            int old = subtitlePid;
            if (old >= 0) {
                DiscNative.wantPid(h, old, false);
            }
            subtitlePid = s != null ? s.pid : -1;
            if (s != null) {
                DiscNative.wantPid(h, s.pid, true);
            }
            clearSubtitles();
        });
    }

    @Override
    public void release() {
        released = true;
        main.removeCallbacks(subtitleTick);
        if (focusRequest != null) {
            AudioManager am = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
            if (am != null) {
                am.abandonAudioFocusRequest(focusRequest);
            }
            focusRequest = null;
        }
        worker.shutdownNow();
        try {
            worker.awaitTermination(2, TimeUnit.SECONDS);
        }
        catch (InterruptedException ignored) {
        }
        DiscAudio a = audio;
        audio = null;
        if (a != null) {
            a.release();
        }
        long h = handle;
        handle = 0;
        opened = false;
        if (h != 0) {
            DiscNative.close(h);
        }
    }
}
