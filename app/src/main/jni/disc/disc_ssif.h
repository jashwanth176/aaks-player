// Demuxes a Blu-ray 3D playlist's SSIF files into base+dependent access unit pairs
// (matched by PTS) and the PES of selected other streams. Times are playlist time,
// 90 kHz, continuous across clips.

#ifndef DISC_SSIF_H
#define DISC_SSIF_H

#include <stddef.h>
#include <stdint.h>

#include "disc_nav.h"

typedef struct DiscSource DiscSource;

enum {
    DS_END = 0,
    DS_PAIR = 1,   // base then dependent access unit, Annex B
    DS_PES = 2,    // one PES payload of a selected stream
    DS_ERROR = -1,
};

typedef struct {
    int type;
    int pid;
    int64_t pts90;     // playlist time, 90 kHz; -1 if none
    const uint8_t *data;
    size_t size;
    size_t base_size;  // pairs: bytes of the base view at the start of data
    int gop_start;     // pairs: base view contains an SPS
} DiscItem;

// The playlist and the Disc must outlive the source.
DiscSource *ds_open(Disc *disc, const DiscPlaylist *playlist);
void ds_close(DiscSource *src);

// Selects PIDs delivered as DS_PES. Any thread; applied by the reader.
void ds_want_pid(DiscSource *src, int pid, int on);

// Item data is valid until the next call.
int ds_next(DiscSource *src, DiscItem *item);

// Seeks to the GOP at or just before time90. Returns 0 and the first pair's time, or -1.
int ds_seek(DiscSource *src, int64_t time90, int64_t *landed90);

// Unpaired units dropped so far: base view (pictures lost) and dependent view.
void ds_lost(DiscSource *src, int *pairs, int *deps);

// Total bytes read.
int64_t ds_bytes_read(DiscSource *src);

#endif
