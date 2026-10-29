// Stereo MVC decoder on top of edge264: access unit pairs in, picture pairs out.

#ifndef DISC_VIDEO_H
#define DISC_VIDEO_H

#include <stddef.h>
#include <stdint.h>

typedef struct DiscDecoder DiscDecoder;

typedef struct {
    // [0] base view (left), [1] dependent view (right)
    const uint8_t *y[2];
    const uint8_t *u[2];
    const uint8_t *v[2];
    int stride_y, stride_c;
    int width, height;    // luma
    int cwidth, cheight;  // chroma
    int64_t pts90;        // playlist time
    void *token;          // for dd_release
} DiscFrame;

// threads: edge264 worker threads, 0 for none. ring: decoded pictures buffered
// before dd_feed blocks.
DiscDecoder *dd_open(int threads, int ring);
void dd_close(DiscDecoder *dd);

// Feeds one Annex B pair (base then dependent). Blocks while the ring is full.
// Returns 0, or -1 once aborted.
int dd_feed(DiscDecoder *dd, const uint8_t *data, size_t size, int64_t pts90);

// Next picture pair in display order, waiting up to wait_ms. Returns 1, or 0 if none.
int dd_pop(DiscDecoder *dd, DiscFrame *frame, int wait_ms);

// Returns a popped pair once uploaded.
void dd_release(DiscDecoder *dd, DiscFrame *frame);

// Drops all state; for seeks.
void dd_flush(DiscDecoder *dd);

// Unblocks dd_feed until the next dd_flush.
void dd_abort(DiscDecoder *dd);

// Diagnostics: skipped and damaged units, one-eye pictures, ring waits, and pair
// times not yet matched to a picture.
void dd_stats(DiscDecoder *dd, int *skipped, int *damaged, int *one_eye, int *waits, int *pending);

int dd_frame_size(DiscDecoder *dd, int *width, int *height);

#endif
