package com.limelight.binding.video;

import org.tensorflow.lite.Delegate;
import org.tensorflow.lite.TensorFlowLite;

/**
 * LiteRT's XNNPACK delegate with a chosen thread count, built in libdepth-xnnpack
 * against the runtime library the AAR ships. The default XNNPACK path runs a
 * single thread, and LiteRT has no Java class for the delegate.
 *
 * <p>Close after the interpreter it was given to, never before.
 */
final class XnnpackDelegate implements Delegate {

    private static final boolean LOADED;

    static {
        boolean loaded;
        try {
            System.loadLibrary("depth-xnnpack");
            loaded = true;
        }
        catch (UnsatisfiedLinkError e) {
            loaded = false;
        }
        LOADED = loaded;
    }

    private long handle;

    private XnnpackDelegate(long handle) {
        this.handle = handle;
    }

    /**
     * A delegate on this many threads, or null if the library or runtime functions
     * are missing or it did not get the threads asked for. Logcat has the reason.
     */
    static XnnpackDelegate create(int threads) {
        if (!LOADED) {
            return null;
        }
        try {
            // The native side only looks the runtime up; it never loads it
            TensorFlowLite.init();
        }
        catch (UnsatisfiedLinkError e) {
            return null;
        }
        long handle = nativeCreate(threads);
        return handle != 0 ? new XnnpackDelegate(handle) : null;
    }

    @Override
    public long getNativeHandle() {
        return handle;
    }

    @Override
    public void close() {
        if (handle != 0) {
            nativeDelete(handle);
            handle = 0;
        }
    }

    private static native long nativeCreate(int threads);

    private static native void nativeDelete(long handle);
}
