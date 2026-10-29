package com.limelight.player;

import androidx.media3.common.util.UnstableApi;
import androidx.media3.decoder.DecoderException;
import androidx.media3.decoder.DecoderInputBuffer;
import androidx.media3.decoder.SimpleDecoder;
import androidx.media3.decoder.SimpleDecoderOutputBuffer;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Media3 decoder for DTS and TrueHD via FFmpeg in libdisc. Sample in, interleaved float out. */
@UnstableApi
final class FfAudioDecoder
        extends SimpleDecoder<DecoderInputBuffer, SimpleDecoderOutputBuffer, DecoderException> {

    private static final int BUFFERS = 4;
    // Fits one TrueHD sample of several access units at 8 channels
    private static final int OUT_BYTES = 1 << 20;

    private final long handle;
    private final ByteBuffer scratch = ByteBuffer.allocateDirect(OUT_BYTES).order(ByteOrder.nativeOrder());

    FfAudioDecoder(int coding) throws DecoderException {
        super(new DecoderInputBuffer[BUFFERS], new SimpleDecoderOutputBuffer[BUFFERS]);
        handle = FfAudio.open(coding);
        if (handle == 0) {
            throw new DecoderException("FFmpeg has no decoder for this audio");
        }
        setInitialInputBufferSize(64 * 1024);
    }

    @Override
    public String getName() {
        return "FfAudioDecoder";
    }

    int channels() {
        return FfAudio.channels(handle);
    }

    int rate() {
        return FfAudio.rate(handle);
    }

    @Override
    protected DecoderInputBuffer createInputBuffer() {
        return new DecoderInputBuffer(DecoderInputBuffer.BUFFER_REPLACEMENT_MODE_DIRECT);
    }

    @Override
    protected SimpleDecoderOutputBuffer createOutputBuffer() {
        return new SimpleDecoderOutputBuffer(this::releaseOutputBuffer);
    }

    @Override
    protected DecoderException createUnexpectedDecodeException(Throwable error) {
        return new DecoderException("Unexpected decode error", error);
    }

    @Override
    protected DecoderException decode(DecoderInputBuffer in, SimpleDecoderOutputBuffer out, boolean reset) {
        if (reset) {
            FfAudio.flush(handle);
        }
        ByteBuffer data = in.data;
        if (data == null) {
            return null;
        }
        byte[] bytes = new byte[data.remaining()];
        data.get(bytes);
        scratch.clear();
        int n = FfAudio.decode(handle, bytes, bytes.length, scratch);
        if (n < 0) {
            return new DecoderException("FFmpeg could not decode a sample");
        }
        ByteBuffer o = out.init(in.timeUs, n);
        scratch.position(0);
        scratch.limit(n);
        o.put(scratch);
        o.flip();
        return null;
    }

    @Override
    public void release() {
        super.release();
        FfAudio.close(handle);
    }
}
