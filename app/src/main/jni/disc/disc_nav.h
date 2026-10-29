// Blu-ray image navigation: UDF filesystem, playlists, main title selection.
// No menus, no BD-J; encrypted discs are refused, not decrypted.

#ifndef DISC_NAV_H
#define DISC_NAV_H

#include <stddef.h>
#include <stdint.h>

#include "udfread.h"

#define DISC_MAX_CLIPS 64
#define DISC_MAX_STREAMS 64
#define DISC_MAX_CHAPTERS 128

// Stream kinds, as grouped in the playlist
enum {
    DISC_VIDEO = 0,
    DISC_AUDIO = 1,
    DISC_SUBTITLE = 2,
    DISC_GRAPHICS = 3,
};

// Stream coding types (playlist and PMT)
#define DISC_CODING_MPEG2 0x02
#define DISC_CODING_AVC 0x1b
#define DISC_CODING_MVC 0x20
#define DISC_CODING_HEVC 0x24
#define DISC_CODING_VC1 0xea
#define DISC_CODING_LPCM 0x80
#define DISC_CODING_AC3 0x81
#define DISC_CODING_DTS 0x82
#define DISC_CODING_TRUEHD 0x83
#define DISC_CODING_EAC3 0x84
#define DISC_CODING_DTSHD 0x85
#define DISC_CODING_DTSHD_MA 0x86
#define DISC_CODING_EAC3_SECONDARY 0xa1
#define DISC_CODING_DTSHD_SECONDARY 0xa2
#define DISC_CODING_PGS 0x90
#define DISC_CODING_IGS 0x91
#define DISC_CODING_TEXT 0x92

// Fixed PIDs for Blu-ray 3D base and dependent views
#define DISC_PID_BASE 0x1011
#define DISC_PID_DEPENDENT 0x1012

typedef struct {
    int kind;
    int pid;
    int coding;
    // Audio codes. channels: 1 mono, 3 stereo, 6 multichannel, 12/14 combined.
    // rate: 1 48 kHz, 4 96 kHz, 5 192 kHz, 12/14 combined.
    int channels;
    int rate;
    char lang[4];
} DiscStream;

typedef struct {
    char name[6]; // "00001"
    uint32_t in45;  // 45 kHz ticks
    uint32_t out45;
    int ssif;       // BDMV/STREAM/SSIF/<name>.ssif exists (has a dependent view)
} DiscClip;

typedef struct {
    char name[8]; // "00800"
    int nclips;
    DiscClip clips[DISC_MAX_CLIPS];
    int64_t duration45;
    int nstreams;
    DiscStream streams[DISC_MAX_STREAMS];
    int is3d; // every clip has an SSIF
    int nchapters;
    int64_t chapter45[DISC_MAX_CHAPTERS]; // from playlist start
} DiscPlaylist;

typedef struct Disc Disc;

// Opens an ISO with plain file I/O.
Disc *disc_open(const char *iso_path);
void disc_close(Disc *disc);
const char *disc_label(Disc *disc);
udfread *disc_udf(Disc *disc);

// Main title: the longest 3D playlist, else the longest. Returns 0, or -1.
int disc_main_playlist(Disc *disc, DiscPlaylist *out);
// Playlist by name ("00800"), and all playlist names.
int disc_read_playlist(Disc *disc, const char *name, DiscPlaylist *out);
int disc_list_playlists(Disc *disc, char names[][8], int max);

// Reads a small file whole; malloc'd, NULL if missing.
uint8_t *disc_read_file(Disc *disc, const char *path, size_t *size);

int disc_parse_mpls(const uint8_t *data, size_t size, DiscPlaylist *out);

#endif
