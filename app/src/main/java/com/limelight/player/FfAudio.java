package com.limelight.player;

import java.nio.ByteBuffer;

/**
 * FFmpeg decoders in libdisc for audio the device cannot decode (DTS family, Dolby TrueHD).
 * Packet in, interleaved 32-bit float out.
 */
final class FfAudio {

    static {
        System.loadLibrary("disc");
    }

    private FfAudio() {
    }

    /** Opens a decoder for a disc stream coding; 0 if unsupported. */
    static native long open(int coding);

    /** Decodes data[0..len) into out as float samples; returns bytes written, negative on error. */
    static native int decode(long handle, byte[] data, int len, ByteBuffer out);

    static native int channels(long handle);

    static native int rate(long handle);

    static native void flush(long handle);

    static native void close(long handle);

    /** Whether FFmpeg handles this disc stream coding. */
    static boolean handles(int coding) {
        return coding == DiscNativeCodings.DTS || coding == DiscNativeCodings.DTSHD
                || coding == DiscNativeCodings.DTSHD_MA || coding == DiscNativeCodings.DTSHD_SECONDARY
                || coding == DiscNativeCodings.TRUEHD;
    }
}
