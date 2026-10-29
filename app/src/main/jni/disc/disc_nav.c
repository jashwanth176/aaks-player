// Blu-ray disc image navigation, see disc_nav.h.
#include "disc_nav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Disc {
    udfread *udf;
};

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t rd16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

Disc *disc_open(const char *iso_path) {
    Disc *disc = calloc(1, sizeof(*disc));
    if (disc == NULL) {
        return NULL;
    }
    disc->udf = udfread_init();
    if (disc->udf == NULL || udfread_open(disc->udf, iso_path) < 0) {
        if (disc->udf != NULL) {
            udfread_close(disc->udf);
        }
        free(disc);
        return NULL;
    }
    // Require /BDMV.
    UDFDIR *bdmv = udfread_opendir(disc->udf, "/BDMV");
    if (bdmv == NULL) {
        disc_close(disc);
        return NULL;
    }
    udfread_closedir(bdmv);
    return disc;
}

void disc_close(Disc *disc) {
    if (disc == NULL) {
        return;
    }
    if (disc->udf != NULL) {
        udfread_close(disc->udf);
    }
    free(disc);
}

const char *disc_label(Disc *disc) {
    return udfread_get_volume_id(disc->udf);
}

udfread *disc_udf(Disc *disc) {
    return disc->udf;
}

uint8_t *disc_read_file(Disc *disc, const char *path, size_t *size) {
    UDFFILE *f = udfread_file_open(disc->udf, path);
    if (f == NULL) {
        return NULL;
    }
    int64_t len = udfread_file_size(f);
    uint8_t *buf = len > 0 && len < (64 << 20) ? malloc((size_t)len) : NULL;
    if (buf != NULL) {
        int64_t got = 0;
        while (got < len) {
            int64_t n = udfread_file_read(f, buf + got, (size_t)(len - got));
            if (n <= 0) {
                break;
            }
            got += n;
        }
        if (got != len) {
            free(buf);
            buf = NULL;
        }
        else if (size != NULL) {
            *size = (size_t)len;
        }
    }
    udfread_file_close(f);
    return buf;
}

// ---- playlists -------------------------------------------------------------------

// One STN entry: stream_entry (PID) then stream_attributes.
static int parse_stream(const uint8_t *d, size_t size, size_t *pos, int kind, DiscStream *out) {
    size_t p = *pos;
    if (p + 1 > size) {
        return -1;
    }
    size_t entry_len = d[p];
    size_t entry_end = p + 1 + entry_len;
    if (entry_end + 1 > size) {
        return -1;
    }
    int type = d[p + 1];
    int pid = 0;
    switch (type) {
        case 1: pid = (int)rd16(d + p + 2); break;
        case 2: pid = (int)rd16(d + p + 4); break;
        case 3: pid = (int)rd16(d + p + 3); break;
        case 4: pid = (int)rd16(d + p + 4); break;
        default: break;
    }
    p = entry_end;
    size_t attr_len = d[p];
    size_t attr_end = p + 1 + attr_len;
    if (attr_end > size || attr_len < 1) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->pid = pid;
    out->coding = d[p + 1];
    switch (kind) {
        case DISC_AUDIO:
            if (attr_len >= 5) {
                out->channels = d[p + 2] >> 4;
                out->rate = d[p + 2] & 0x0f;
                memcpy(out->lang, d + p + 3, 3);
            }
            break;
        case DISC_SUBTITLE:
            if (out->coding == DISC_CODING_TEXT) {
                if (attr_len >= 5) {
                    memcpy(out->lang, d + p + 3, 3);
                }
            }
            else if (attr_len >= 4) {
                memcpy(out->lang, d + p + 2, 3);
            }
            break;
        default:
            break;
    }
    out->lang[3] = 0;
    *pos = attr_end;
    return 0;
}

// Streams are taken from the first play item only; later items carry the same set.
static int parse_stn(const uint8_t *d, size_t size, size_t pos, DiscPlaylist *pl) {
    if (pos + 16 > size) {
        return -1;
    }
    size_t len = rd16(d + pos);
    size_t end = pos + 2 + len;
    if (end > size) {
        end = size;
    }
    int nvideo = d[pos + 4];
    int naudio = d[pos + 5];
    int npg = d[pos + 6];
    size_t p = pos + 16;
    int counts[3] = { nvideo, naudio, npg };
    int kinds[3] = { DISC_VIDEO, DISC_AUDIO, DISC_SUBTITLE };
    for (int g = 0; g < 3; g++) {
        for (int i = 0; i < counts[g]; i++) {
            DiscStream s;
            if (parse_stream(d, end, &p, kinds[g], &s) < 0) {
                return 0;
            }
            if (pl->nstreams < DISC_MAX_STREAMS) {
                pl->streams[pl->nstreams++] = s;
            }
        }
    }
    return 0;
}

int disc_parse_mpls(const uint8_t *d, size_t size, DiscPlaylist *pl) {
    memset(pl, 0, sizeof(*pl));
    if (size < 40 || memcmp(d, "MPLS", 4) != 0) {
        return -1;
    }
    uint32_t list_start = rd32(d + 8);
    uint32_t marks_start = rd32(d + 12);
    if (list_start + 10 > size) {
        return -1;
    }
    const uint8_t *l = d + list_start;
    int nitems = (int)rd16(l + 6);
    size_t p = list_start + 10;
    int64_t item_start45[DISC_MAX_CLIPS];
    for (int i = 0; i < nitems && pl->nclips < DISC_MAX_CLIPS; i++) {
        if (p + 2 > size) {
            return -1;
        }
        size_t len = rd16(d + p);
        size_t item_end = p + 2 + len;
        if (item_end > size || len < 34) {
            return -1;
        }
        DiscClip *c = &pl->clips[pl->nclips];
        memset(c, 0, sizeof(*c));
        memcpy(c->name, d + p + 2, 5);
        c->name[5] = 0;
        int multi_angle = (d[p + 12] >> 4) & 1;
        c->in45 = rd32(d + p + 14);
        c->out45 = rd32(d + p + 18);
        // len(2) name(5) codec(4) flags(2) stc(1) in(4) out(4) uo(8) ra(1) still(1) stilltime(2)
        size_t q = p + 2 + 5 + 4 + 2 + 1 + 4 + 4 + 8 + 1 + 1 + 2;
        if (multi_angle && q + 2 <= item_end) {
            int angles = d[q];
            q += 2 + (angles > 1 ? (angles - 1) * 10 : 0);
        }
        item_start45[pl->nclips] = pl->duration45;
        pl->duration45 += (int64_t)c->out45 - (int64_t)c->in45;
        if (pl->nclips == 0 && q + 16 <= item_end) {
            parse_stn(d, item_end, q, pl);
        }
        pl->nclips++;
        p = item_end;
    }

    // Chapters: entry marks (type 1), in playlist time.
    if (marks_start != 0 && marks_start + 6 <= size) {
        int nmarks = (int)rd16(d + marks_start + 4);
        size_t m = marks_start + 6;
        for (int i = 0; i < nmarks && m + 14 <= size && pl->nchapters < DISC_MAX_CHAPTERS; i++, m += 14) {
            int type = d[m + 1];
            int item = (int)rd16(d + m + 2);
            uint32_t t = rd32(d + m + 4);
            if (type != 1 || item >= pl->nclips || t < pl->clips[item].in45) {
                continue;
            }
            pl->chapter45[pl->nchapters++] = item_start45[item] + ((int64_t)t - pl->clips[item].in45);
        }
    }
    return 0;
}

int disc_read_playlist(Disc *disc, const char *name, DiscPlaylist *out) {
    char path[64];
    snprintf(path, sizeof(path), "/BDMV/PLAYLIST/%s.mpls", name);
    size_t size = 0;
    uint8_t *data = disc_read_file(disc, path, &size);
    if (data == NULL) {
        return -1;
    }
    int ok = disc_parse_mpls(data, size, out);
    free(data);
    if (ok < 0) {
        return -1;
    }
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->is3d = out->nclips > 0;
    for (int i = 0; i < out->nclips; i++) {
        char ssif[64];
        snprintf(ssif, sizeof(ssif), "/BDMV/STREAM/SSIF/%s.ssif", out->clips[i].name);
        UDFFILE *f = udfread_file_open(disc->udf, ssif);
        out->clips[i].ssif = f != NULL;
        if (f != NULL) {
            udfread_file_close(f);
        }
        else {
            out->is3d = 0;
        }
    }
    return 0;
}

int disc_list_playlists(Disc *disc, char names[][8], int max) {
    UDFDIR *dir = udfread_opendir(disc->udf, "/BDMV/PLAYLIST");
    if (dir == NULL) {
        return 0;
    }
    int n = 0;
    struct udfread_dirent e;
    while (n < max && udfread_readdir(dir, &e) != NULL) {
        size_t len = strlen(e.d_name);
        if (e.d_type == UDF_DT_REG && len == 10 && strcmp(e.d_name + 5, ".mpls") == 0) {
            memcpy(names[n], e.d_name, 5);
            names[n][5] = 0;
            n++;
        }
    }
    udfread_closedir(dir);
    return n;
}

int disc_main_playlist(Disc *disc, DiscPlaylist *out) {
    char names[256][8];
    int n = disc_list_playlists(disc, names, 256);
    int have = 0;
    DiscPlaylist best, cur;
    for (int i = 0; i < n; i++) {
        if (disc_read_playlist(disc, names[i], &cur) < 0 || cur.nclips == 0) {
            continue;
        }
        // Prefer 3D, then longer.
        int better = !have || (cur.is3d && !best.is3d)
                || (cur.is3d == best.is3d && cur.duration45 > best.duration45);
        if (better) {
            best = cur;
            have = 1;
        }
    }
    if (!have) {
        return -1;
    }
    *out = best;
    return 0;
}
