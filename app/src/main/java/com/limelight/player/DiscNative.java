package com.limelight.player;

import android.view.Surface;

/**
 * JNI bindings for libdisc: reads a Blu-ray 3D disc image, decodes both eyes side by side into
 * a surface and passes audio and subtitle packets back. Times are playlist time in 90 kHz ticks.
 */
final class DiscNative {

    static {
        System.loadLibrary("disc");
    }

    private DiscNative() {
    }

    /** Callbacks from libdisc. Called on native threads. */
    interface Sink {
        /** Payload of one PES packet from a pid enabled with wantPid. */
        void onPes(int pid, long pts90, byte[] data);

        /** First picture shown at this time; audio can start. */
        void onFirstFrame(long pts90);

        /** Playback reached the end. */
        void onEnded();
    }

    /** Opens a disc image; 0 if it is not a Blu-ray. */
    static native long open(String path, int decoderThreads);

    /**
     * Main playlist summary, one tab-separated line per entry: T title, D duration, M 1 for 3D,
     * C chapter times, V/A/S streams (pid, coding, channels, rate, language).
     */
    static native String describe(long handle);

    static native boolean start(long handle, Surface surface, long start90, Sink sink, boolean play);

    static native void wantPid(long handle, int pid, boolean on);

    static native void setPlaying(long handle, boolean play);

    static native boolean seek(long handle, long time90);

    /** Audio clock report: media time and the CLOCK_MONOTONIC time it was valid at. */
    static native void syncClock(long handle, long media90, long nanos);

    static native long position(long handle);

    /** Playback rate relative to real time; 1 is normal speed. */
    static native void setSpeed(long handle, float speed);

    static native void setVideoOffset(long handle, int millis);

    static native void close(long handle);
}
