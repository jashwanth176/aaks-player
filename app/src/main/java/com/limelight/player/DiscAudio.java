package com.limelight.player;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.SystemClock;

import com.limelight.LimeLog;

import java.nio.ByteBuffer;
import java.util.List;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Plays one disc audio stream: decodes packets (MediaCodec or FFmpeg) into an AudioTrack whose
 * position is the master clock for video. Runs its own thread; public methods are thread safe.
 */
final class DiscAudio {

    interface Callback {
        /** Enough audio is buffered and the track has started. */
        void audioStarted();

        /** Audible media time in 90 kHz ticks, valid at this CLOCK_MONOTONIC time in ns. */
        void clockSync(long media90, long nanos);

        void audioFailed(String reason);
    }

    private static final class Job {
        final int generation;
        final long pts90;
        final byte[] data;

        Job(int generation, long pts90, byte[] data) {
            this.generation = generation;
            this.pts90 = pts90;
            this.data = data;
        }
    }

    private static final long SYNC_EVERY_MS = 200;

    private final Callback callback;
    private final LinkedBlockingQueue<Job> queue = new LinkedBlockingQueue<>();
    private final Thread thread;
    private volatile boolean quit;
    // Bumped on seek or track change; the thread then resets
    private volatile int generation;
    private volatile long floor90;
    private volatile boolean playing;
    private volatile int coding;

    // Audio thread only
    private MediaCodec codec;
    private int codecCoding = -1;
    // FFmpeg decoder handle and its output buffer
    private long ff;
    private int ffCoding = -1;
    private final ByteBuffer ffOut = ByteBuffer.allocateDirect(1 << 20).order(java.nio.ByteOrder.nativeOrder());
    private AudioTrack track;
    private int sampleRate = 48000;
    private int channels = 2;
    private int frameBytes = 4;
    private long basePts90 = -1;
    private long framesWritten;
    private boolean trackStarted;
    private long lastSync;

    DiscAudio(Callback callback) {
        this.callback = callback;
        thread = new Thread(this::run, "DiscAudio");
        thread.setPriority(Thread.MAX_PRIORITY);
        thread.start();
    }

    /** Whether this disc stream coding can be decoded, by MediaCodec or FFmpeg. */
    static boolean supports(int coding) {
        return coding == DiscNativeCodings.AC3 || coding == DiscNativeCodings.EAC3
                || coding == DiscNativeCodings.EAC3_SECONDARY || FfAudio.handles(coding);
    }

    /** Switches to a stream of this coding, dropping audio before newFloor90. */
    void select(int newCoding, long newFloor90) {
        coding = newCoding;
        floor90 = newFloor90;
        queue.clear();
        generation++;
    }

    /** Discards buffered audio after a seek. */
    void flush(long newFloor90) {
        floor90 = newFloor90;
        queue.clear();
        generation++;
    }

    void feed(long pts90, byte[] pes) {
        queue.offer(new Job(generation, pts90, pes));
    }

    void setPlaying(boolean on) {
        playing = on;
    }

    // Requested speed, and what the audio thread has applied to the track
    private volatile float speed = 1.0f;
    private float trackSpeed = 1.0f;

    /** Playback speed, pitch preserved. Any thread. */
    void setSpeed(float value) {
        speed = value;
    }

    private void applySpeed() {
        float want = speed;
        if (track == null || want == trackSpeed) {
            return;
        }
        trackSpeed = want;
        try {
            track.setPlaybackParams(track.getPlaybackParams().setSpeed(want).setPitch(1.0f));
            LimeLog.info("DiscAudio: speed " + want);
        }
        catch (IllegalArgumentException | IllegalStateException e) {
            LimeLog.warning("DiscAudio: speed " + want + " refused: " + e);
        }
    }

    void release() {
        quit = true;
        thread.interrupt();
        try {
            thread.join(500);
        }
        catch (InterruptedException ignored) {
        }
    }

    // ---- audio thread -------------------------------------------------------------------------

    // Generation the thread last reset for; in-flight work from an older one is abandoned
    private int activeGeneration = -1;

    private void run() {
        boolean wasPlaying = false;
        while (!quit) {
            try {
                if (activeGeneration != generation) {
                    activeGeneration = generation;
                    reset();
                }
                if (playing != wasPlaying) {
                    wasPlaying = playing;
                    applyPlaying();
                }
                applySpeed();
                Job j = queue.poll(15, TimeUnit.MILLISECONDS);
                if (j != null && j.generation == generation && (codec != null || ff != 0)) {
                    feedJob(j);
                }
                drain();
                startTrackIfReady();
                syncClock();
                logState();
            }
            catch (InterruptedException e) {
                break;
            }
            catch (RuntimeException e) {
                LimeLog.warning("DiscAudio: " + e);
                callback.audioFailed(String.valueOf(e));
                quit = true;
            }
        }
        teardown();
    }

    private int fedFrames, droppedFrames;
    private long stateLoggedAt;

    // Periodic state log, to tell a stall from a gap in the log
    private void logState() {
        long now = SystemClock.elapsedRealtime();
        if (now - stateLoggedAt < 5000) {
            return;
        }
        stateLoggedAt = now;
        AudioTimestamp ts = new AudioTimestamp();
        boolean got = track != null && track.getTimestamp(ts);
        LimeLog.info("DiscAudio: state playing=" + playing + " started=" + trackStarted + " queue=" + queue.size()
                + " written=" + framesWritten + " fed=" + fedFrames + " base=" + basePts90 / 90000.0
                + " playState=" + (track != null ? track.getPlayState() : -1) + " timestamp=" + got
                + (got ? " pos=" + ts.framePosition : ""));
    }

    private void reset() {
        LimeLog.info("DiscAudio: reset, generation " + generation + ", floor " + floor90 / 90000.0 + " s, fed "
                + fedFrames + " dropped " + droppedFrames + " before");
        fedFrames = droppedFrames = 0;
        basePts90 = -1;
        framesWritten = 0;
        trackStarted = false;
        if (track != null) {
            try {
                track.pause();
                track.flush();
            }
            catch (IllegalStateException ignored) {
            }
        }
        int c = coding;
        if (FfAudio.handles(c)) {
            closeCodec();
            if (ff != 0 && ffCoding != c) {
                closeFf();
            }
            if (ff == 0) {
                ff = FfAudio.open(c);
                ffCoding = c;
                if (ff == 0) {
                    callback.audioFailed("No decoder for this audio");
                }
            }
            else {
                FfAudio.flush(ff);
            }
            return;
        }
        closeFf();
        if (codec != null && codecCoding != c) {
            closeCodec();
        }
        if (codec != null) {
            codec.flush();
            return;
        }
        if (!supports(c)) {
            return;
        }
        String mime = c == DiscNativeCodings.AC3 ? MediaFormat.MIMETYPE_AUDIO_AC3 : MediaFormat.MIMETYPE_AUDIO_EAC3;
        try {
            codec = MediaCodec.createDecoderByType(mime);
            codec.configure(MediaFormat.createAudioFormat(mime, 48000, 6), null, null, 0);
            codec.start();
            codecCoding = c;
            LimeLog.info("DiscAudio: " + codec.getName() + " for " + mime);
        }
        catch (Exception e) {
            LimeLog.warning("DiscAudio: no decoder for " + mime + ": " + e);
            closeCodec();
            callback.audioFailed("No decoder for this audio");
        }
    }

    private void closeCodec() {
        if (codec != null) {
            try {
                codec.stop();
            }
            catch (Exception ignored) {
            }
            codec.release();
            codec = null;
        }
        codecCoding = -1;
    }

    private void closeFf() {
        if (ff != 0) {
            FfAudio.close(ff);
            ff = 0;
        }
        ffCoding = -1;
    }

    // Decodes one packet with FFmpeg to float PCM
    private void feedFf(Job j) throws InterruptedException {
        if (j.pts90 < floor90 - 1500) {
            droppedFrames++;
            return;
        }
        ffOut.clear();
        int n = FfAudio.decode(ff, j.data, j.data.length, ffOut);
        if (n <= 0) {
            return;
        }
        int ch = FfAudio.channels(ff), rate = FfAudio.rate(ff);
        if (track == null || ch != channels || rate != sampleRate || frameBytes != ch * 4) {
            openTrack(rate, ch, AudioFormat.ENCODING_PCM_FLOAT);
        }
        if (basePts90 < 0) {
            basePts90 = j.pts90;
            framesWritten = 0;
            LimeLog.info("DiscAudio: first sound out at " + basePts90 / 90000.0 + " s (FFmpeg)");
        }
        ffOut.position(0);
        ffOut.limit(n);
        writeAll(ffOut, n);
        fedFrames++;
    }

    private void teardown() {
        closeCodec();
        closeFf();
        if (track != null) {
            try {
                track.stop();
            }
            catch (IllegalStateException ignored) {
            }
            track.release();
            track = null;
        }
    }

    private void applyPlaying() {
        if (track == null || !trackStarted) {
            return;
        }
        try {
            if (playing) {
                track.play();
            }
            else {
                track.pause();
            }
        }
        catch (IllegalStateException ignored) {
        }
    }

    private void feedJob(Job j) throws InterruptedException {
        if (ff != 0) {
            feedFf(j);
            return;
        }
        List<AudioFrames.Frame> frames = codecCoding == DiscNativeCodings.AC3
                ? AudioFrames.ac3(j.data, j.pts90) : AudioFrames.eac3(j.data, j.pts90);
        for (AudioFrames.Frame f : frames) {
            // Drop frames before the seek/start point
            if (f.pts90 < floor90 - 1500) {
                droppedFrames++;
                continue;
            }
            int index = -1;
            for (int tries = 0; tries < 200 && !quit && j.generation == generation && activeGeneration == generation; tries++) {
                index = codec.dequeueInputBuffer(5000);
                if (index >= 0) {
                    break;
                }
                drain();
            }
            if (index < 0) {
                return;
            }
            ByteBuffer in = codec.getInputBuffer(index);
            in.clear();
            in.put(j.data, f.offset, f.length);
            codec.queueInputBuffer(index, 0, f.length, f.pts90 * 1000L / 90L, 0);
            if (++fedFrames == 1) {
                LimeLog.info("DiscAudio: first frame fed at " + f.pts90 / 90000.0 + " s");
            }
        }
    }

    private void drain() throws InterruptedException {
        if (codec == null || activeGeneration != generation) {
            return;
        }
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        for (;;) {
            int out = codec.dequeueOutputBuffer(info, 0);
            if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                openTrack(codec.getOutputFormat());
                continue;
            }
            if (out < 0) {
                return;
            }
            ByteBuffer buf = codec.getOutputBuffer(out);
            if (track != null && info.size > 0 && buf != null) {
                if (basePts90 < 0) {
                    basePts90 = info.presentationTimeUs * 90L / 1000L;
                    framesWritten = 0;
                    LimeLog.info("DiscAudio: first sound out at " + basePts90 / 90000.0 + " s");
                }
                buf.position(info.offset);
                buf.limit(info.offset + info.size);
                writeAll(buf, info.size);
            }
            codec.releaseOutputBuffer(out, false);
        }
    }

    private void writeAll(ByteBuffer buf, int size) throws InterruptedException {
        int left = size;
        while (left > 0 && !quit && activeGeneration == generation) {
            int n = track.write(buf, left, AudioTrack.WRITE_NON_BLOCKING);
            if (n < 0) {
                return;
            }
            left -= n;
            framesWritten += n / frameBytes;
            if (n == 0) {
                // Track buffer full
                startTrackIfReady();
                Thread.sleep(4);
            }
        }
    }

    private void openTrack(MediaFormat format) {
        int rate = format.containsKey(MediaFormat.KEY_SAMPLE_RATE) ? format.getInteger(MediaFormat.KEY_SAMPLE_RATE) : 48000;
        int ch = format.containsKey(MediaFormat.KEY_CHANNEL_COUNT) ? format.getInteger(MediaFormat.KEY_CHANNEL_COUNT) : 2;
        int encoding = format.containsKey(MediaFormat.KEY_PCM_ENCODING)
                ? format.getInteger(MediaFormat.KEY_PCM_ENCODING) : AudioFormat.ENCODING_PCM_16BIT;
        openTrack(rate, ch, encoding);
    }

    private void openTrack(int rate, int ch, int encoding) {
        sampleRate = rate;
        channels = ch;
        frameBytes = channels * (encoding == AudioFormat.ENCODING_PCM_FLOAT ? 4 : 2);
        int mask;
        switch (channels) {
            case 1: mask = AudioFormat.CHANNEL_OUT_MONO; break;
            case 2: mask = AudioFormat.CHANNEL_OUT_STEREO; break;
            case 6: mask = AudioFormat.CHANNEL_OUT_5POINT1; break;
            case 8: mask = AudioFormat.CHANNEL_OUT_7POINT1_SURROUND; break;
            default: mask = AudioFormat.CHANNEL_OUT_STEREO; break;
        }
        if (track != null) {
            track.release();
        }
        int min = AudioTrack.getMinBufferSize(sampleRate, mask, encoding);
        track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(sampleRate)
                        .setChannelMask(mask).setEncoding(encoding).build())
                .setBufferSizeInBytes(Math.max(min * 3, sampleRate * frameBytes / 2))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();
        // New track starts at 1x; reapply the requested speed
        trackSpeed = 1.0f;
        applySpeed();
        trackStarted = false;
        LimeLog.info("DiscAudio: " + channels + " channels at " + sampleRate + " Hz, encoding " + encoding);
    }

    // Start once ~1/6 s is buffered
    private void startTrackIfReady() {
        if (track == null || trackStarted || !playing || framesWritten < sampleRate / 6) {
            return;
        }
        try {
            track.play();
        }
        catch (IllegalStateException e) {
            return;
        }
        trackStarted = true;
        lastSync = 0;
        LimeLog.info("DiscAudio: track started");
        callback.audioStarted();
    }

    private void syncClock() {
        long now = SystemClock.elapsedRealtime();
        if (track == null || !trackStarted || !playing || basePts90 < 0 || now - lastSync < SYNC_EVERY_MS) {
            return;
        }
        lastSync = now;
        AudioTimestamp ts = new AudioTimestamp();
        if (track.getTimestamp(ts) && ts.framePosition > 0) {
            callback.clockSync(basePts90 + ts.framePosition * 90000L / sampleRate, ts.nanoTime);
        }
    }
}
