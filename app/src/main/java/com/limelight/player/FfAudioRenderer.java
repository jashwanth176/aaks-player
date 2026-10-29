package com.limelight.player;

import android.content.Context;
import android.os.Handler;

import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.decoder.CryptoConfig;
import androidx.media3.decoder.DecoderException;
import androidx.media3.exoplayer.DefaultRenderersFactory;
import androidx.media3.exoplayer.Renderer;
import androidx.media3.exoplayer.RendererCapabilities;
import androidx.media3.exoplayer.audio.AudioRendererEventListener;
import androidx.media3.exoplayer.audio.AudioSink;
import androidx.media3.exoplayer.audio.DecoderAudioRenderer;
import androidx.media3.exoplayer.mediacodec.MediaCodecSelector;

import java.util.ArrayList;

/**
 * FFmpeg audio renderer for formats the device cannot decode: TrueHD (Atmos as its 7.1 bed) and
 * DTS. Added after the MediaCodec renderers, so it only takes what they reject.
 */
@UnstableApi
final class FfAudioRenderer extends DecoderAudioRenderer<FfAudioDecoder> {

    /** The disc coding FfAudio uses for this MIME type, or 0. */
    static int codingFor(String mime) {
        if (mime == null) {
            return 0;
        }
        switch (mime) {
            case MimeTypes.AUDIO_TRUEHD:
                return DiscNativeCodings.TRUEHD;
            case MimeTypes.AUDIO_DTS:
                return DiscNativeCodings.DTS;
            case MimeTypes.AUDIO_DTS_HD:
                return DiscNativeCodings.DTSHD_MA;
            default:
                return 0;
        }
    }

    FfAudioRenderer(Handler handler, AudioRendererEventListener listener, AudioSink sink) {
        super(handler, listener, sink);
    }

    @Override
    public String getName() {
        return "FfAudioRenderer";
    }

    @Override
    protected int supportsFormatInternal(Format format) {
        return RendererCapabilities.create(codingFor(format.sampleMimeType) != 0
                ? C.FORMAT_HANDLED : C.FORMAT_UNSUPPORTED_SUBTYPE);
    }

    @Override
    protected FfAudioDecoder createDecoder(Format format, CryptoConfig cryptoConfig) throws DecoderException {
        return new FfAudioDecoder(codingFor(format.sampleMimeType));
    }

    @Override
    protected Format getOutputFormat(FfAudioDecoder decoder) {
        return new Format.Builder()
                .setSampleMimeType(MimeTypes.AUDIO_RAW)
                .setChannelCount(decoder.channels())
                .setSampleRate(decoder.rate())
                .setPcmEncoding(C.ENCODING_PCM_FLOAT)
                .build();
    }

    /** Default renderers plus FfAudioRenderer last, with text renderers wrapped for subtitle delay. */
    static final class Factory extends DefaultRenderersFactory {
        private final java.util.function.LongSupplier subtitleShiftUs;

        Factory(Context context, java.util.function.LongSupplier subtitleShiftUs) {
            super(context);
            this.subtitleShiftUs = subtitleShiftUs;
            setEnableAudioFloatOutput(true);
        }

        @Override
        protected void buildTextRenderers(Context context, androidx.media3.exoplayer.text.TextOutput output,
                                          android.os.Looper outputLooper, int extensionRendererMode,
                                          ArrayList<Renderer> out) {
            int before = out.size();
            super.buildTextRenderers(context, output, outputLooper, extensionRendererMode, out);
            for (int i = before; i < out.size(); i++) {
                if (out.get(i).getTrackType() == C.TRACK_TYPE_TEXT) {
                    out.set(i, new ShiftedRenderer(out.get(i), subtitleShiftUs));
                }
            }
        }

        @Override
        protected void buildAudioRenderers(Context context, int extensionRendererMode,
                                           MediaCodecSelector selector, boolean enableDecoderFallback,
                                           AudioSink sink, Handler handler,
                                           AudioRendererEventListener listener, ArrayList<Renderer> out) {
            super.buildAudioRenderers(context, extensionRendererMode, selector, enableDecoderFallback, sink,
                    handler, listener, out);
            out.add(new FfAudioRenderer(handler, listener, sink));
        }
    }
}
