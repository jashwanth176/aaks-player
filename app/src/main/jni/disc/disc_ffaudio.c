// FFmpeg decoding for codecs the platform lacks: DTS, DTS-HD (HRA and MA, lossless
// when present) and TrueHD. Output is interleaved 32-bit float for AudioTrack.

#include <android/log.h>
#include <jni.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "disc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

#define CODING_DTS 0x82
#define CODING_TRUEHD 0x83
#define CODING_DTSHD 0x85
#define CODING_DTSHD_MA 0x86
#define CODING_DTSHD_SECONDARY 0xa2

typedef struct {
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    int channels;
    int rate;
    int logged;
} FfAudio;

static float sample_at(const AVFrame *f, int ch, int i) {
    switch (f->format) {
        case AV_SAMPLE_FMT_FLTP: return ((const float *)f->extended_data[ch])[i];
        case AV_SAMPLE_FMT_FLT: return ((const float *)f->extended_data[0])[i * f->ch_layout.nb_channels + ch];
        case AV_SAMPLE_FMT_S32P: return ((const int32_t *)f->extended_data[ch])[i] * (1.0f / 2147483648.0f);
        case AV_SAMPLE_FMT_S32: return ((const int32_t *)f->extended_data[0])[i * f->ch_layout.nb_channels + ch] * (1.0f / 2147483648.0f);
        case AV_SAMPLE_FMT_S16P: return ((const int16_t *)f->extended_data[ch])[i] * (1.0f / 32768.0f);
        case AV_SAMPLE_FMT_S16: return ((const int16_t *)f->extended_data[0])[i * f->ch_layout.nb_channels + ch] * (1.0f / 32768.0f);
        default: return 0.0f;
    }
}

JNIEXPORT jlong JNICALL
Java_com_limelight_player_FfAudio_open(JNIEnv *env, jclass clazz, jint coding) {
    (void)env;
    (void)clazz;
    enum AVCodecID id;
    switch (coding) {
        case CODING_DTS:
        case CODING_DTSHD:
        case CODING_DTSHD_MA:
        case CODING_DTSHD_SECONDARY:
            id = AV_CODEC_ID_DTS;
            break;
        case CODING_TRUEHD:
            id = AV_CODEC_ID_TRUEHD;
            break;
        default:
            return 0;
    }
    const AVCodec *codec = avcodec_find_decoder(id);
    FfAudio *d = calloc(1, sizeof(*d));
    if (codec == NULL || d == NULL) {
        free(d);
        return 0;
    }
    d->ctx = avcodec_alloc_context3(codec);
    d->parser = av_parser_init(id);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (d->ctx == NULL || d->parser == NULL || d->pkt == NULL || d->frame == NULL
            || avcodec_open2(d->ctx, codec, NULL) < 0) {
        LOGW("ffaudio: could not open the %s decoder", codec->name);
        return 0;
    }
    LOGI("ffaudio: %s decoder open", codec->name);
    return (jlong)(intptr_t)d;
}

// Decodes data[0..len) into out as interleaved float. Returns bytes written, or -1.
JNIEXPORT jint JNICALL
Java_com_limelight_player_FfAudio_decode(JNIEnv *env, jclass clazz, jlong handle, jbyteArray data, jint len,
                                         jobject out) {
    (void)clazz;
    FfAudio *d = (FfAudio *)(intptr_t)handle;
    uint8_t *dst = (*env)->GetDirectBufferAddress(env, out);
    jlong cap = (*env)->GetDirectBufferCapacity(env, out);
    if (d == NULL || dst == NULL) {
        return -1;
    }
    // Padded: the parser and bit readers read past the end.
    uint8_t *buf = malloc((size_t)len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (buf == NULL) {
        return -1;
    }
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);
    memset(buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    int written = 0;
    const uint8_t *p = buf;
    int left = len;
    while (left > 0) {
        uint8_t *pdata = NULL;
        int psize = 0;
        int used = av_parser_parse2(d->parser, d->ctx, &pdata, &psize, p, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) {
            break;
        }
        p += used;
        left -= used;
        if (psize <= 0) {
            continue;
        }
        d->pkt->data = pdata;
        d->pkt->size = psize;
        if (avcodec_send_packet(d->ctx, d->pkt) < 0) {
            continue;
        }
        while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
            int ch = d->frame->ch_layout.nb_channels;
            int n = d->frame->nb_samples;
            d->channels = ch;
            d->rate = d->frame->sample_rate;
            if (!d->logged) {
                d->logged = 1;
                LOGI("ffaudio: %d channels, %d Hz, sample format %d", ch, d->rate, d->frame->format);
            }
            if ((jlong)written + (jlong)n * ch * 4 > cap) {
                break;
            }
            float *o = (float *)(dst + written);
            for (int i = 0; i < n; i++) {
                for (int c = 0; c < ch; c++) {
                    *o++ = sample_at(d->frame, c, i);
                }
            }
            written += n * ch * 4;
            // Log peak levels of the first few loud blocks.
            if (d->logged >= 1 && d->logged < 6) {
                float peak[8] = { 0 };
                const float *f = (const float *)(dst + written - n * ch * 4);
                for (int i = 0; i < n; i++) {
                    for (int c = 0; c < ch && c < 8; c++) {
                        float v = f[i * ch + c] < 0 ? -f[i * ch + c] : f[i * ch + c];
                        if (v > peak[c]) peak[c] = v;
                    }
                }
                float all = 0;
                for (int c = 0; c < ch && c < 8; c++) all += peak[c];
                if (all > 0.05f) {
                    d->logged++;
                    LOGI("ffaudio: peaks %.2f %.2f %.2f %.2f %.2f %.2f", peak[0], peak[1], peak[2], peak[3],
                         ch > 4 ? peak[4] : 0.0f, ch > 5 ? peak[5] : 0.0f);
                }
            }
        }
    }
    free(buf);
    return written;
}

JNIEXPORT jint JNICALL
Java_com_limelight_player_FfAudio_channels(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env;
    (void)clazz;
    FfAudio *d = (FfAudio *)(intptr_t)handle;
    return d != NULL ? d->channels : 0;
}

JNIEXPORT jint JNICALL
Java_com_limelight_player_FfAudio_rate(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env;
    (void)clazz;
    FfAudio *d = (FfAudio *)(intptr_t)handle;
    return d != NULL ? d->rate : 0;
}

// Drops decoder state; for seeks.
JNIEXPORT void JNICALL
Java_com_limelight_player_FfAudio_flush(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env;
    (void)clazz;
    FfAudio *d = (FfAudio *)(intptr_t)handle;
    if (d != NULL) {
        avcodec_flush_buffers(d->ctx);
        av_parser_close(d->parser);
        d->parser = av_parser_init(d->ctx->codec_id);
    }
}

JNIEXPORT void JNICALL
Java_com_limelight_player_FfAudio_close(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env;
    (void)clazz;
    FfAudio *d = (FfAudio *)(intptr_t)handle;
    if (d == NULL) {
        return;
    }
    av_parser_close(d->parser);
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    free(d);
}
