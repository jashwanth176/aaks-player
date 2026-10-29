// Stereo MVC decoder, see disc_video.h.
#include "disc_video.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edge264.h"

#define MAX_RING 16
#define MAX_PENDING 128

typedef struct {
    Edge264Frame frame;
    int64_t pts;
} Held;

struct DiscDecoder {
    Edge264Decoder *dec;
    int threads;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    Held ring[MAX_RING];
    int ring_size;
    int head, count; // oldest and how many
    int aborted;
    // PTS of fed pairs not yet output, sorted. Output is in display order, so the
    // oldest belongs to the next picture.
    int64_t pending[MAX_PENDING];
    int npending;
    int width, height;
    // Diagnostics
    int skipped, damaged, unsupported, one_eye, waits;
};

static void add_pending(DiscDecoder *dd, int64_t pts) {
    if (dd->npending >= MAX_PENDING) {
        memmove(dd->pending, dd->pending + 1, (MAX_PENDING - 1) * sizeof(int64_t));
        dd->npending = MAX_PENDING - 1;
    }
    int i = dd->npending++;
    while (i > 0 && dd->pending[i - 1] > pts) {
        dd->pending[i] = dd->pending[i - 1];
        i--;
    }
    dd->pending[i] = pts;
}

static int64_t take_pending(DiscDecoder *dd) {
    if (dd->npending == 0) {
        return -1;
    }
    int64_t t = dd->pending[0];
    memmove(dd->pending, dd->pending + 1, (size_t)(dd->npending - 1) * sizeof(int64_t));
    dd->npending--;
    return t;
}

DiscDecoder *dd_open(int threads, int ring) {
    DiscDecoder *dd = calloc(1, sizeof(*dd));
    if (dd == NULL) {
        return NULL;
    }
    dd->threads = threads;
    dd->ring_size = ring < 2 ? 2 : (ring > MAX_RING ? MAX_RING : ring);
    dd->dec = edge264_alloc(threads, NULL, NULL, 0, NULL, NULL, NULL);
    if (dd->dec == NULL) {
        free(dd);
        return NULL;
    }
    pthread_mutex_init(&dd->lock, NULL);
    pthread_cond_init(&dd->changed, NULL);
    return dd;
}

void dd_close(DiscDecoder *dd) {
    if (dd == NULL) {
        return;
    }
    edge264_free(&dd->dec);
    pthread_mutex_destroy(&dd->lock);
    pthread_cond_destroy(&dd->changed);
    free(dd);
}

void dd_abort(DiscDecoder *dd) {
    pthread_mutex_lock(&dd->lock);
    dd->aborted = 1;
    pthread_cond_broadcast(&dd->changed);
    pthread_mutex_unlock(&dd->lock);
}

// Moves decoded pictures into the ring, waiting for room. Returns the count, or -1
// once aborted.
static int drain(DiscDecoder *dd) {
    int moved = 0;
    for (;;) {
        pthread_mutex_lock(&dd->lock);
        while (dd->count >= dd->ring_size && !dd->aborted) {
            pthread_cond_wait(&dd->changed, &dd->lock);
        }
        if (dd->aborted) {
            pthread_mutex_unlock(&dd->lock);
            return -1;
        }
        Held *h = &dd->ring[(dd->head + dd->count) % MAX_RING];
        pthread_mutex_unlock(&dd->lock);

        // Borrowed until dd_release.
        if (edge264_get_frame(dd->dec, &h->frame, 1) != 0) {
            return moved;
        }
        moved++;
        if (h->frame.samples_mvc[0] == NULL) {
            dd->one_eye++;
        }
        pthread_mutex_lock(&dd->lock);
        h->pts = take_pending(dd);
        dd->width = h->frame.width_Y;
        dd->height = h->frame.height_Y;
        dd->count++;
        pthread_cond_broadcast(&dd->changed);
        pthread_mutex_unlock(&dd->lock);
    }
}

int dd_feed(DiscDecoder *dd, const uint8_t *data, size_t size, int64_t pts90) {
    pthread_mutex_lock(&dd->lock);
    add_pending(dd, pts90);
    pthread_mutex_unlock(&dd->lock);

    const uint8_t *end0 = data + size;
    const uint8_t *nal = data;
    const uint8_t *first = edge264_find_start_code(nal, end0, 0);
    if (first >= end0) {
        return 0;
    }
    nal = first + 3;
    int stuck = 0;
    while (nal < end0) {
        const uint8_t *end = edge264_find_start_code(nal, end0, 0);
        int res = edge264_decode_NAL(dd->dec, nal, end, NULL, NULL);
        int moved = drain(dd);
        if (moved < 0) {
            return -1;
        }
        if (res == ENOBUFS) {
            // No free picture: the player holds them. Wait for one back rather than
            // skip the unit (which would lose a picture); skip only after seconds stuck.
            if (moved == 0) {
                pthread_mutex_lock(&dd->lock);
                if (!dd->aborted) {
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_nsec += 10000000L;
                    if (ts.tv_nsec >= 1000000000L) {
                        ts.tv_sec++;
                        ts.tv_nsec -= 1000000000L;
                    }
                    pthread_cond_timedwait(&dd->changed, &dd->lock, &ts);
                }
                int aborted = dd->aborted;
                pthread_mutex_unlock(&dd->lock);
                if (aborted) {
                    return -1;
                }
                dd->waits++;
                if (++stuck > 300) {
                    dd->skipped++;
                    nal = end + 3;
                    stuck = 0;
                }
            }
            continue;
        }
        if (res == EBADMSG) {
            dd->damaged++;
        }
        else if (res == ENOTSUP) {
            dd->unsupported++;
        }
        stuck = 0;
        // Unsupported (e.g. NAL type 24 on discs) and damaged units are skipped;
        // the decoder continues.
        nal = end + 3;
    }
    return 0;
}

int dd_pop(DiscDecoder *dd, DiscFrame *f, int wait_ms) {
    pthread_mutex_lock(&dd->lock);
    if (dd->count == 0 && wait_ms > 0 && !dd->aborted) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += wait_ms / 1000;
        ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&dd->changed, &dd->lock, &ts);
    }
    if (dd->count == 0) {
        pthread_mutex_unlock(&dd->lock);
        return 0;
    }
    Held *h = &dd->ring[dd->head];
    f->y[0] = h->frame.samples[0];
    f->u[0] = h->frame.samples[1];
    f->v[0] = h->frame.samples[2];
    f->y[1] = h->frame.samples_mvc[0] != NULL ? h->frame.samples_mvc[0] : h->frame.samples[0];
    f->u[1] = h->frame.samples_mvc[0] != NULL ? h->frame.samples_mvc[1] : h->frame.samples[1];
    f->v[1] = h->frame.samples_mvc[0] != NULL ? h->frame.samples_mvc[2] : h->frame.samples[2];
    f->stride_y = h->frame.stride_Y;
    f->stride_c = h->frame.stride_C;
    f->width = h->frame.width_Y;
    f->height = h->frame.height_Y;
    f->cwidth = h->frame.width_C;
    f->cheight = h->frame.height_C;
    f->pts90 = h->pts;
    f->token = h->frame.return_arg;
    dd->head = (dd->head + 1) % MAX_RING;
    dd->count--;
    pthread_mutex_unlock(&dd->lock);
    return 1;
}

void dd_release(DiscDecoder *dd, DiscFrame *f) {
    edge264_return_frame(dd->dec, f->token);
    pthread_mutex_lock(&dd->lock);
    pthread_cond_broadcast(&dd->changed);
    pthread_mutex_unlock(&dd->lock);
}

void dd_flush(DiscDecoder *dd) {
    pthread_mutex_lock(&dd->lock);
    while (dd->count > 0) {
        Held *h = &dd->ring[dd->head];
        edge264_return_frame(dd->dec, h->frame.return_arg);
        dd->head = (dd->head + 1) % MAX_RING;
        dd->count--;
    }
    dd->npending = 0;
    dd->aborted = 0;
    pthread_mutex_unlock(&dd->lock);
    edge264_flush(dd->dec);
    // Discard frames from the old position.
    Edge264Frame f;
    while (edge264_get_frame(dd->dec, &f, 0) == 0) {
    }
    pthread_mutex_lock(&dd->lock);
    dd->npending = 0;
    pthread_cond_broadcast(&dd->changed);
    pthread_mutex_unlock(&dd->lock);
}

void dd_stats(DiscDecoder *dd, int *skipped, int *damaged, int *one_eye, int *waits, int *pending) {
    *skipped = dd->skipped;
    *damaged = dd->damaged;
    *one_eye = dd->one_eye;
    *waits = dd->waits;
    *pending = dd->npending;
}

int dd_frame_size(DiscDecoder *dd, int *width, int *height) {
    pthread_mutex_lock(&dd->lock);
    int ok = dd->width > 0;
    if (ok) {
        *width = dd->width;
        *height = dd->height;
    }
    pthread_mutex_unlock(&dd->lock);
    return ok;
}
