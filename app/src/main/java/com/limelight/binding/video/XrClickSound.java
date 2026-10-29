package com.limelight.binding.video;

import android.annotation.TargetApi;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.Build;

import com.limelight.LimeLog;

/**
 * The tick played for a press on the in-headset panels, synthesized at startup
 * and played on its own small sonification track so the video's audio is untouched.
 *
 * A streaming track only starts once its buffer is full, so it is primed with
 * silence (and refilled if it stalls), then the frame loop keeps a little
 * silence queued and a press writes the tick behind it. All frame loop writes
 * are non-blocking. Requires M for WRITE_NON_BLOCKING; the caller checks.
 */
@TargetApi(Build.VERSION_CODES.M)
final class XrClickSound {
    private static final int RATE = 48000;
    // Silence kept queued: a couple of frames plus a mixer cycle, which also bounds
    // the tick's latency. While the frame loop runs slow, the feed gap plus a mixer cycle.
    private static final int QUEUE_FRAMES = RATE * 30 / 1000;
    private static final int MIXER_FRAMES = RATE * 20 / 1000;
    private static final int TICK_MS = 18;
    private static final double TICK_HZ = 2200.0;
    private static final double TICK_BODY_HZ = 600.0;
    private static final double DECAY_SEC = 0.0035;
    private static final double PEAK = 0.28;
    // Feeds with an unmoving play head before the track counts as stalled and is
    // refilled: about a third of a second at 72 Hz or more
    private static final int STALL_FEEDS = 30;

    private final AudioTrack track;
    private final short[] tick;
    private final short[] silence;
    private final int bufferFrames;
    // QUEUE_FRAMES, or less where the buffer would not then fit a whole tick behind it
    private final int queueFrames;
    private long writtenFrames;
    private int clicks;
    private int lastHead;
    private int stillFeeds;
    private int restarts;
    private long lastFeedNs;

    private XrClickSound(AudioTrack track, short[] tick, int bufferFrames, int queueFrames) {
        this.track = track;
        this.tick = tick;
        this.bufferFrames = bufferFrames;
        this.queueFrames = queueFrames;
        this.silence = new short[bufferFrames];
    }

    /**
     * Builds and starts the track, or null if unavailable. Talks to the audio system, so not on the
     * frame loop.
     */
    static XrClickSound start() {
        short[] tick = tickSamples(RATE);
        int min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        int bytes = Math.max(min, 4 * (QUEUE_FRAMES + tick.length));
        AudioTrack track;
        try {
            track = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setSampleRate(RATE)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .build())
                    .setBufferSizeInBytes(bytes)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build();
        } catch (IllegalArgumentException | UnsupportedOperationException e) {
            LimeLog.warning("Click sound unavailable: " + e);
            return null;
        }
        if (track.getState() != AudioTrack.STATE_INITIALIZED) {
            track.release();
            LimeLog.warning("Click sound unavailable: track not initialised");
            return null;
        }
        // The buffer in use can start smaller than requested, which would cut a tick
        // short, so open it to full capacity
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            track.setBufferSizeInFrames(track.getBufferCapacityInFrames());
        }
        int frames = track.getBufferSizeInFrames();
        int queue = queueFor(frames, tick.length);
        XrClickSound sound = new XrClickSound(track, tick, frames, queue);
        sound.write(sound.silence, frames);
        track.play();
        LimeLog.info("Click sound ready: " + tick.length + " frame tick, " + queue
                + " frames of silence queued, buffer " + frames + " frames");
        return sound;
    }

    /** Frame loop, every frame: tops the silence back up. Never blocks. */
    void feed() {
        int head = track.getPlaybackHeadPosition();
        stillFeeds = head == lastHead ? stillFeeds + 1 : 0;
        lastHead = head;
        if (stillFeeds == STALL_FEEDS) {
            // Stalled after a long frame loop or audio system pause: fill it so it restarts
            restarts++;
            write(silence, framesToQueue(writtenFrames, head, bufferFrames));
            LimeLog.info("Click sound: track stalled, filled to start it again (" + restarts + ")");
            return;
        }
        long now = System.nanoTime();
        int gap = lastFeedNs == 0 ? 0 : (int)Math.min(bufferFrames, (now - lastFeedNs) * RATE / 1000000000L);
        lastFeedNs = now;
        int target = queueTarget(queueFrames, gap + MIXER_FRAMES, bufferFrames, tick.length);
        int want = framesToQueue(writtenFrames, head, target);
        if (want > 0) {
            write(silence, want);
        }
    }

    /** Frame loop: queues one tick behind the silence. Never blocks. */
    void click() {
        int queued = queueFrames - framesToQueue(writtenFrames, track.getPlaybackHeadPosition(),
                queueFrames);
        int wrote = write(tick, tick.length);
        clicks++;
        LimeLog.info("Click sound: press " + clicks + ", " + wrote + " of " + tick.length
                + " frames written, " + (queued * 1000 / RATE) + " ms queued ahead"
                + (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N
                        ? ", underruns " + track.getUnderrunCount() : ""));
    }

    void release() {
        try {
            track.stop();
        } catch (IllegalStateException ignored) {
        }
        track.release();
    }

    private int write(short[] samples, int frames) {
        int wrote = track.write(samples, 0, frames, AudioTrack.WRITE_NON_BLOCKING);
        if (wrote > 0) {
            writtenFrames += wrote;
        }
        return wrote;
    }

    /**
     * Silence to keep queued in a buffer this size so a tick still fits: up to QUEUE_FRAMES, at
     * least a tenth of it.
     */
    static int queueFor(int bufferFrames, int tickFrames) {
        int room = bufferFrames - tickFrames - tickFrames / 4;
        return Math.max(QUEUE_FRAMES / 10, Math.min(QUEUE_FRAMES, room));
    }

    /**
     * This frame's silence target: the usual amount, or enough to last until the next feed when the
     * loop runs slow, capped so a tick still fits.
     */
    static int queueTarget(int usual, int needed, int bufferFrames, int tickFrames) {
        int most = Math.max(usual, bufferFrames - tickFrames - tickFrames / 4);
        return Math.min(most, Math.max(usual, needed));
    }

    /**
     * Frames to write to bring the queue back up to the target. The play head is
     * an int that wraps, so the written count is compared modulo 2^32.
     */
    static int framesToQueue(long written, int playedHead, int target) {
        long played = playedHead & 0xFFFFFFFFL;
        long queued = (written & 0xFFFFFFFFL) - played;
        if (queued < 0) {
            queued += 1L << 32;
        }
        long want = target - queued;
        return want > 0 ? (int)want : 0;
    }

    /**
     * The tick: a bright partial over a lower body, decaying within a few ms, with a short attack
     * ramp. Ends on silence.
     */
    static short[] tickSamples(int rate) {
        int frames = rate * TICK_MS / 1000;
        short[] out = new short[frames];
        int attack = Math.max(1, rate / 2000);
        for (int i = 0; i < frames; i++) {
            double t = (double)i / rate;
            double envelope = Math.exp(-t / DECAY_SEC);
            if (i < attack) {
                envelope *= (double)i / attack;
            }
            // Fade to zero over the last fifth
            int tail = frames - i;
            if (tail < frames / 5) {
                envelope *= (double)tail / (frames / 5);
            }
            double value = 0.7 * Math.sin(2.0 * Math.PI * TICK_HZ * t)
                    + 0.3 * Math.sin(2.0 * Math.PI * TICK_BODY_HZ * t);
            out[i] = (short)Math.round(PEAK * envelope * value * Short.MAX_VALUE);
        }
        return out;
    }
}
