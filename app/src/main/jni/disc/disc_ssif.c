// SSIF reading and splitting, see disc_ssif.h.
#include "disc_ssif.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PKT 192
#define CHUNK_PKTS 1024
#define MAX_OTHER 12
#define MAX_PENDING 400
// Seek reads start this far before the guess: the dependent view's interleave
// extent precedes the base view's.
#define SEEK_BACKOFF (6 << 20)
#define SEEK_ITERATIONS 10

typedef struct Node {
    struct Node *next;
    int type;
    int pid;
    int64_t pts;
    int gop;
    size_t base_size;
    size_t size;
    // File offset of the first packet
    int64_t off;
    uint8_t *data;
} Node;

typedef struct {
    int pid;
    int active;
    int64_t pts;
    int64_t start_off;
    uint8_t *buf;
    size_t len, cap;
} Pes;

typedef struct {
    Node *head, *tail;
    int count;
} Queue;

struct DiscSource {
    Disc *disc;
    const DiscPlaylist *pl;
    int clip;
    int64_t clip_start90[DISC_MAX_CLIPS + 1];
    // First/last base-view PTS per clip in stream time; 0 until measured
    int64_t first_pts[DISC_MAX_CLIPS], last_pts[DISC_MAX_CLIPS];
    UDFFILE *file;
    int64_t fsize;
    int64_t pos;
    uint8_t chunk[PKT * CHUNK_PKTS];
    size_t chunk_len, chunk_off;
    int64_t chunk_base; // file offset of the chunk's first byte
    int64_t pkt_off;    // file offset of the packet being split
    Pes base, dep, other[MAX_OTHER];
    int nother;
    Queue base_q, dep_q, out;
    Node *held; // last item returned by ds_next
    // Latest PTS per view, and units dropped without a partner
    int64_t base_latest, dep_latest;
    int pairs_lost, deps_lost;
    int eof;
    int64_t bytes;
    // PID selections from other threads, applied by the reader
    pthread_mutex_t want_lock;
    int want_pid[16], want_on[16], want_n;
};

static void apply_wants(DiscSource *s);

// ---- queues ---------------------------------------------------------------------

static void push(Queue *q, Node *n) {
    n->next = NULL;
    if (q->tail != NULL) {
        q->tail->next = n;
    }
    else {
        q->head = n;
    }
    q->tail = n;
    q->count++;
}

static void push_front(Queue *q, Node *n) {
    n->next = q->head;
    q->head = n;
    if (q->tail == NULL) {
        q->tail = n;
    }
    q->count++;
}

static Node *pop(Queue *q) {
    Node *n = q->head;
    if (n != NULL) {
        q->head = n->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
        q->count--;
    }
    return n;
}

static void free_node(Node *n) {
    if (n != NULL) {
        free(n->data);
        free(n);
    }
}

static void clear_queue(Queue *q) {
    Node *n;
    while ((n = pop(q)) != NULL) {
        free_node(n);
    }
}

static void clear_pes(Pes *p) {
    p->active = 0;
    p->len = 0;
}

// ---- clip -----------------------------------------------------------------------

static void close_clip(DiscSource *s) {
    if (s->file != NULL) {
        udfread_file_close(s->file);
        s->file = NULL;
    }
}

static int open_clip(DiscSource *s, int index) {
    close_clip(s);
    char path[64];
    snprintf(path, sizeof(path), "/BDMV/STREAM/SSIF/%s.ssif", s->pl->clips[index].name);
    s->file = udfread_file_open(disc_udf(s->disc), path);
    if (s->file == NULL) {
        return -1;
    }
    s->clip = index;
    s->fsize = udfread_file_size(s->file);
    s->pos = 0;
    s->chunk_len = s->chunk_off = 0;
    return 0;
}

static int64_t in90(const DiscSource *s, int clip) {
    return (int64_t)s->pl->clips[clip].in45 * 2;
}

// Stream PTS to playlist time.
static int64_t to_playlist(const DiscSource *s, int64_t pts) {
    return pts - in90(s, s->clip) + s->clip_start90[s->clip];
}

static int read_chunk(DiscSource *s) {
    if (s->file == NULL || s->pos >= s->fsize) {
        return 0;
    }
    if (udfread_file_seek(s->file, s->pos, 0) < 0) {
        return 0;
    }
    size_t want = PKT * CHUNK_PKTS;
    if ((int64_t)want > s->fsize - s->pos) {
        want = (size_t)(s->fsize - s->pos);
    }
    ssize_t got = udfread_file_read(s->file, s->chunk, want);
    if (got <= 0) {
        return 0;
    }
    s->chunk_len = (size_t)got - (size_t)got % PKT;
    s->chunk_off = 0;
    s->chunk_base = s->pos;
    s->pos += got;
    s->bytes += got;
    return s->chunk_len > 0;
}

// ---- PES ---------------------------------------------------------------------------

static int64_t parse_pts(const uint8_t *p) {
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] >> 1) << 15)
            | ((int64_t)p[3] << 7) | (p[4] >> 1);
}

static int has_sps(const uint8_t *es, size_t len) {
    for (size_t i = 0; i + 4 < len; i++) {
        if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1 && (es[i + 3] & 0x1f) == 7) {
            return 1;
        }
    }
    return 0;
}

static void pes_add(Pes *p, const uint8_t *d, size_t n) {
    if (p->len + n > p->cap) {
        size_t cap = p->cap ? p->cap : 1 << 16;
        while (cap < p->len + n) {
            cap *= 2;
        }
        uint8_t *nb = realloc(p->buf, cap);
        if (nb == NULL) {
            return;
        }
        p->buf = nb;
        p->cap = cap;
    }
    memcpy(p->buf + p->len, d, n);
    p->len += n;
}

static Node *make_node(int type, int pid, int64_t pts, const uint8_t *d, size_t n) {
    Node *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    node->data = malloc(n ? n : 1);
    if (node->data == NULL) {
        free(node);
        return NULL;
    }
    memcpy(node->data, d, n);
    node->type = type;
    node->pid = pid;
    node->pts = pts;
    node->size = n;
    return node;
}

// Pairs base and dependent access units by PTS. Units arrive in decode order (PTS
// not monotonic) and the dependent view's come in their own extents, usually ahead,
// so each base unit searches the whole dependent queue. A unit is dropped only once
// the other view is PAIR_GIVE_UP_90K past it.
#define PAIR_GIVE_UP_90K (3 * 90000)

static void emit_pair(DiscSource *s, Node *b, Node *d) {
    Node *pair = make_node(DS_PAIR, DISC_PID_BASE, b->pts, NULL, 0);
    if (pair == NULL) {
        return;
    }
    free(pair->data);
    pair->data = malloc(b->size + d->size);
    if (pair->data == NULL) {
        free(pair);
        return;
    }
    memcpy(pair->data, b->data, b->size);
    memcpy(pair->data + b->size, d->data, d->size);
    pair->size = b->size + d->size;
    pair->base_size = b->size;
    pair->gop = b->gop;
    pair->off = b->off < d->off ? b->off : d->off;
    push(&s->out, pair);
}

static void pair_up(DiscSource *s) {
    while (s->base_q.head != NULL) {
        Node *b = s->base_q.head;
        Node *prev = NULL, *d = s->dep_q.head;
        while (d != NULL && d->pts != b->pts) {
            prev = d;
            d = d->next;
        }
        if (d != NULL) {
            // Unlink from the dependent queue.
            if (prev != NULL) {
                prev->next = d->next;
            }
            else {
                s->dep_q.head = d->next;
            }
            if (s->dep_q.tail == d) {
                s->dep_q.tail = prev;
            }
            s->dep_q.count--;
            emit_pair(s, b, d);
            free_node(pop(&s->base_q));
            free_node(d);
            continue;
        }
        // Not there yet: wait, unless the dependent view is long past.
        if (s->dep_latest > b->pts + PAIR_GIVE_UP_90K || s->base_q.count > MAX_PENDING) {
            s->pairs_lost++;
            free_node(pop(&s->base_q));
            continue;
        }
        break;
    }
    // Drop dependent units the base view is long past.
    while (s->dep_q.head != NULL
            && (s->dep_q.head->pts < s->base_latest - PAIR_GIVE_UP_90K || s->dep_q.count > MAX_PENDING)) {
        Node *d = s->dep_q.head;
        int matched = 0;
        for (Node *b = s->base_q.head; b != NULL; b = b->next) {
            if (b->pts == d->pts) {
                matched = 1;
                break;
            }
        }
        if (matched && s->dep_q.count <= MAX_PENDING) {
            break;
        }
        s->deps_lost++;
        free_node(pop(&s->dep_q));
    }
}

static void finish_pes(DiscSource *s, Pes *p) {
    if (!p->active || p->len == 0) {
        p->active = 0;
        p->len = 0;
        return;
    }
    int64_t time = p->pts >= 0 ? to_playlist(s, p->pts) : -1;
    if (p == &s->base || p == &s->dep) {
        Node *n = make_node(p == &s->base ? DS_PAIR : DS_PAIR, p->pid, p->pts >= 0 ? time : -1,
                            p->buf, p->len);
        if (n != NULL) {
            n->off = p->start_off;
            n->gop = p == &s->base && has_sps(p->buf, p->len < 400 ? p->len : 400);
            if (p == &s->base) {
                if (n->pts > s->base_latest) s->base_latest = n->pts;
            }
            else if (n->pts > s->dep_latest) {
                s->dep_latest = n->pts;
            }
            push(p == &s->base ? &s->base_q : &s->dep_q, n);
            pair_up(s);
        }
    }
    else {
        Node *n = make_node(DS_PES, p->pid, time, p->buf, p->len);
        if (n != NULL) {
            push(&s->out, n);
        }
    }
    p->active = 0;
    p->len = 0;
}

static Pes *find_pes(DiscSource *s, int pid) {
    if (pid == DISC_PID_BASE) {
        return &s->base;
    }
    if (pid == DISC_PID_DEPENDENT) {
        return &s->dep;
    }
    for (int i = 0; i < s->nother; i++) {
        if (s->other[i].pid == pid) {
            return &s->other[i];
        }
    }
    return NULL;
}

static void feed_ts(DiscSource *s, const uint8_t *pkt) {
    int pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
    Pes *p = find_pes(s, pid);
    if (p == NULL) {
        return;
    }
    int pusi = pkt[1] & 0x40;
    int afc = (pkt[3] >> 4) & 3;
    int off = 4;
    if (afc & 2) {
        off += 1 + pkt[4];
    }
    if (!(afc & 1) || off >= 188) {
        return;
    }
    const uint8_t *pl = pkt + off;
    int len = 188 - off;
    if (pusi) {
        if (len < 9 || pl[0] != 0 || pl[1] != 0 || pl[2] != 1) {
            finish_pes(s, p);
            return;
        }
        int hdr = pl[8];
        if (9 + hdr > len) {
            finish_pes(s, p);
            return;
        }
        int64_t pts = (pl[7] & 0x80) && hdr >= 5 ? parse_pts(pl + 9) : -1;
        // A PES without PTS continues the same access unit (large pictures span several).
        if (pts < 0 && p->active && (p == &s->base || p == &s->dep)) {
            pes_add(p, pl + 9 + hdr, (size_t)(len - 9 - hdr));
            return;
        }
        finish_pes(s, p);
        p->pts = pts;
        p->active = 1;
        p->len = 0;
        p->start_off = s->pkt_off;
        pes_add(p, pl + 9 + hdr, (size_t)(len - 9 - hdr));
    }
    else if (p->active) {
        pes_add(p, pl, (size_t)len);
    }
}

// Reads until the output queue is non-empty or the playlist ends.
static int fill(DiscSource *s) {
    while (s->out.head == NULL) {
        if (s->chunk_off >= s->chunk_len) {
            apply_wants(s);
            if (!read_chunk(s)) {
                // End of clip: flush partial PES.
                finish_pes(s, &s->base);
                finish_pes(s, &s->dep);
                for (int i = 0; i < s->nother; i++) {
                    finish_pes(s, &s->other[i]);
                }
                if (s->out.head != NULL) {
                    return 1;
                }
                if (s->clip + 1 < s->pl->nclips && open_clip(s, s->clip + 1) == 0) {
                    continue;
                }
                s->eof = 1;
                return 0;
            }
        }
        const uint8_t *pkt = s->chunk + s->chunk_off;
        s->pkt_off = s->chunk_base + (int64_t)s->chunk_off;
        s->chunk_off += PKT;
        if (pkt[4] != 0x47) {
            continue;
        }
        feed_ts(s, pkt + 4);
    }
    return 1;
}

// ---- public ------------------------------------------------------------------------

DiscSource *ds_open(Disc *disc, const DiscPlaylist *pl) {
    if (pl->nclips == 0) {
        return NULL;
    }
    DiscSource *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->disc = disc;
    s->pl = pl;
    pthread_mutex_init(&s->want_lock, NULL);
    s->base.pid = DISC_PID_BASE;
    s->dep.pid = DISC_PID_DEPENDENT;
    int64_t t = 0;
    for (int i = 0; i < pl->nclips; i++) {
        s->clip_start90[i] = t;
        t += ((int64_t)pl->clips[i].out45 - (int64_t)pl->clips[i].in45) * 2;
    }
    s->clip_start90[pl->nclips] = t;
    if (open_clip(s, 0) < 0) {
        free(s);
        return NULL;
    }
    return s;
}

void ds_close(DiscSource *s) {
    if (s == NULL) {
        return;
    }
    close_clip(s);
    clear_queue(&s->base_q);
    clear_queue(&s->dep_q);
    clear_queue(&s->out);
    free_node(s->held);
    free(s->base.buf);
    free(s->dep.buf);
    for (int i = 0; i < s->nother; i++) {
        free(s->other[i].buf);
    }
    pthread_mutex_destroy(&s->want_lock);
    free(s);
}

static void apply_want(DiscSource *s, int pid, int on) {
    if (pid == DISC_PID_BASE || pid == DISC_PID_DEPENDENT) {
        return;
    }
    for (int i = 0; i < s->nother; i++) {
        if (s->other[i].pid == pid) {
            if (!on) {
                free(s->other[i].buf);
                s->other[i] = s->other[s->nother - 1];
                s->nother--;
            }
            return;
        }
    }
    if (on && s->nother < MAX_OTHER) {
        memset(&s->other[s->nother], 0, sizeof(Pes));
        s->other[s->nother].pid = pid;
        s->nother++;
    }
}

static void apply_wants(DiscSource *s) {
    pthread_mutex_lock(&s->want_lock);
    for (int i = 0; i < s->want_n; i++) {
        apply_want(s, s->want_pid[i], s->want_on[i]);
    }
    s->want_n = 0;
    pthread_mutex_unlock(&s->want_lock);
}

void ds_want_pid(DiscSource *s, int pid, int on) {
    pthread_mutex_lock(&s->want_lock);
    if (s->want_n < 16) {
        s->want_pid[s->want_n] = pid;
        s->want_on[s->want_n] = on;
        s->want_n++;
    }
    pthread_mutex_unlock(&s->want_lock);
}

int ds_next(DiscSource *s, DiscItem *item) {
    free_node(s->held);
    s->held = NULL;
    if (!fill(s)) {
        return DS_END;
    }
    Node *n = pop(&s->out);
    s->held = n;
    item->type = n->type;
    item->pid = n->pid;
    item->pts90 = n->pts;
    item->data = n->data;
    item->size = n->size;
    item->base_size = n->base_size;
    item->gop_start = n->gop;
    return n->type;
}

int64_t ds_bytes_read(DiscSource *s) {
    return s->bytes;
}

void ds_lost(DiscSource *s, int *pairs, int *deps) {
    *pairs = s->pairs_lost;
    *deps = s->deps_lost;
}

// ---- seeking -------------------------------------------------------------------------

// First and last base-view PTS of a clip, read from its head and tail.
static int measure_clip(DiscSource *s, int clip) {
    if (s->first_pts[clip] != 0 && s->last_pts[clip] != 0) {
        return 0;
    }
    if (open_clip(s, clip) < 0) {
        return -1;
    }
    int64_t first = -1, last = -1;
    for (int pass = 0; pass < 2; pass++) {
        s->pos = pass == 0 ? 0 : (s->fsize > (24 << 20) ? s->fsize - (24 << 20) : 0);
        s->pos -= s->pos % PKT;
        s->chunk_len = s->chunk_off = 0;
        int chunks = 0;
        while (chunks++ < (pass == 0 ? 80 : 80) && read_chunk(s) && !(pass == 0 && first >= 0)) {
            for (size_t o = 0; o + PKT <= s->chunk_len; o += PKT) {
                const uint8_t *t = s->chunk + o + 4;
                if (t[0] != 0x47 || (((t[1] & 0x1f) << 8) | t[2]) != DISC_PID_BASE || !(t[1] & 0x40)) {
                    continue;
                }
                int afc = (t[3] >> 4) & 3;
                int off = 4 + ((afc & 2) ? 1 + t[4] : 0);
                if (!(afc & 1) || off + 14 > 188 || t[off] != 0 || t[off + 1] != 0 || t[off + 2] != 1
                        || !(t[off + 7] & 0x80)) {
                    continue;
                }
                int64_t pts = parse_pts(t + off + 9);
                if (pass == 0 && first < 0) {
                    first = pts;
                }
                if (pass == 1) {
                    last = pts;
                }
            }
        }
    }
    if (getenv("DS_DEBUG")) {
        fprintf(stderr, "measure clip %d: first %lld last %lld fsize %lld\n", clip, (long long)first,
                (long long)last, (long long)s->fsize);
    }
    if (first < 0 || last <= first) {
        return -1;
    }
    s->first_pts[clip] = first;
    s->last_pts[clip] = last;
    return 0;
}

static void reset_stream_state(DiscSource *s) {
    s->base_latest = s->dep_latest = 0;
    clear_queue(&s->base_q);
    clear_queue(&s->dep_q);
    clear_queue(&s->out);
    free_node(s->held);
    s->held = NULL;
    clear_pes(&s->base);
    clear_pes(&s->dep);
    for (int i = 0; i < s->nother; i++) {
        clear_pes(&s->other[i]);
    }
    s->chunk_len = s->chunk_off = 0;
    s->eof = 0;
}

// Reads from a file offset to the first GOP-start pair and returns it (NULL if none
// within budget). Later items stay queued.
static Node *scan_to_gop(DiscSource *s, int clip, int64_t start, int64_t budget) {
    if (start < 0) {
        start = 0;
    }
    start -= start % PKT;
    reset_stream_state(s);
    s->pos = start;
    s->clip = clip;
    Node *found = NULL;
    while (budget > 0 && s->pos < s->fsize) {
        int64_t before = s->bytes;
        if (!fill(s)) {
            break;
        }
        budget -= s->bytes - before;
        // Discard items before the first GOP.
        while (found == NULL && s->out.head != NULL) {
            Node *n = pop(&s->out);
            if (n->type == DS_PAIR && n->gop) {
                found = n;
            }
            else {
                free_node(n);
            }
        }
        if (found != NULL) {
            break;
        }
    }
    return found;
}

int ds_seek(DiscSource *s, int64_t time90, int64_t *landed90) {
    int clip = 0;
    while (clip + 1 < s->pl->nclips && time90 >= s->clip_start90[clip + 1]) {
        clip++;
    }
    if (time90 < 0) {
        time90 = 0;
    }
    if (measure_clip(s, clip) < 0) {
        return -1;
    }
    if (s->clip != clip && open_clip(s, clip) < 0) {
        return -1;
    }
    int64_t first = s->first_pts[clip], last = s->last_pts[clip];
    int64_t target = time90 - s->clip_start90[clip] + in90(s, clip);
    if (target < first) {
        target = first;
    }
    const int64_t window = 12 * 90000;
    // Bracket the target between offsets known to land before and after it;
    // bitrate varies, so interpolate within the bracket rather than the whole file.
    int64_t lo_off = 0, lo_pts = first;
    int64_t hi_off = s->fsize, hi_pts = last;
    int64_t guess = (int64_t)((double)(target - first) * ((double)s->fsize / (double)(last - first)));
    Node *found = NULL;
    for (int iter = 0; iter < SEEK_ITERATIONS * 2; iter++) {
        if (guess < lo_off) guess = lo_off;
        if (guess > hi_off) guess = hi_off;
        found = scan_to_gop(s, clip, guess - SEEK_BACKOFF, 96 << 20);
        int64_t pts = found != NULL ? found->pts - s->clip_start90[clip] + in90(s, clip) : hi_pts + 1;
        if (getenv("DS_DEBUG")) {
            fprintf(stderr, "seek iter %d guess %lld found %s pts %lld target %lld lo %lld hi %lld\n", iter,
                    (long long)guess, found ? "yes" : "no", (long long)pts, (long long)target,
                    (long long)lo_off, (long long)hi_off);
        }
        int64_t miss = target - pts; // > 0: landed before the target
        if (found != NULL && miss >= 0 && (miss < window || iter >= SEEK_ITERATIONS)) {
            break;
        }
        free_node(found);
        found = NULL;
        if (miss < 0) {
            hi_off = guess;
            hi_pts = pts;
        }
        else {
            lo_off = guess;
            lo_pts = pts;
        }
        if (hi_off - lo_off < (8 << 20)) {
            // Bracket is down to one read: take the early end.
            found = scan_to_gop(s, clip, lo_off - SEEK_BACKOFF, 96 << 20);
            break;
        }
        // Interpolate, aiming a little before the target.
        int64_t aim = target - window / 3;
        double span = (double)(hi_pts - lo_pts);
        double f = span > 0 ? (double)(aim - lo_pts) / span : 0.5;
        if (f < 0.05) f = 0.05;
        if (f > 0.95) f = 0.95;
        guess = lo_off + (int64_t)((double)(hi_off - lo_off) * f);
    }
    if (found == NULL) {
        return -1;
    }

    // Walk forward to the last GOP at or before the target, then re-read from it,
    // to minimise decoding before the target.
    int64_t best_off = found->off;
    free_node(found);
    found = NULL;
    int64_t budget = 80 << 20;
    int done = 0;
    while (!done && budget > 0) {
        int64_t before = s->bytes;
        if (!fill(s)) {
            break;
        }
        budget -= s->bytes - before;
        Node *n;
        while ((n = pop(&s->out)) != NULL) {
            if (n->type == DS_PAIR && n->gop) {
                int64_t pts = n->pts - s->clip_start90[clip] + in90(s, clip);
                if (pts > target) {
                    done = 1;
                }
                else {
                    best_off = n->off;
                }
            }
            free_node(n);
            if (done) {
                break;
            }
        }
    }
    Node *again = scan_to_gop(s, clip, best_off - PKT * 64, 96 << 20);
    if (again == NULL) {
        return -1;
    }
    push_front(&s->out, again);
    if (landed90 != NULL) {
        *landed90 = again->pts;
    }
    return 0;
}
