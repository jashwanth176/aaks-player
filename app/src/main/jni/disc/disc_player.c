// Native Blu-ray 3D player: reads the disc image, decodes both eyes and draws them
// side by side into the renderer's surface. Audio and subtitle PES go to Java.
// Threads:
//   reader   demuxes the SSIF, queues picture pairs, delivers audio/subtitle PES
//   decoder  decodes pairs with edge264
//   render   waits for each picture's time, draws it, swaps
// The clock is native; the audio side corrects it via syncClock.

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "disc_nav.h"
#include "disc_ssif.h"
#include "disc_video.h"

#define LOG_TAG "disc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_QUEUE 120
#define QUEUE_BYTES (56 << 20)
// Present this early (20 ms) to allow for the VR renderer's pickup latency.
#define PRESENT_LEAD_90K 1800
// Drop pictures more than 100 ms late.
#define DROP_LATE_90K 9000

typedef struct PairNode {
    struct PairNode *next;
    int64_t pts;
    size_t size;
    uint8_t *data;
} PairNode;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    PairNode *head, *tail;
    int count;
    size_t bytes;
} PairQueue;

typedef struct {
    Disc *disc;
    DiscPlaylist playlist;
    DiscSource *src;
    DiscDecoder *dd;
    int threads;

    // Java
    JavaVM *vm;
    jobject sink;
    jmethodID on_pes, on_first_frame, on_ended;

    // Output
    ANativeWindow *window;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    GLuint program, tex[6];
    int out_w, out_h;

    // Threads
    pthread_t reader, decoder, render;
    int running;
    atomic_int stop;
    PairQueue queue;

    // Clock and position
    pthread_mutex_t clock_lock;
    int64_t clock_media90;
    int64_t clock_nano;
    int playing;       // clock running
    double speed;      // media time per real time
    int want_play;     // play requested
    int64_t floor90;   // pictures before this are decoded but not shown
    int first_shown;   // a picture has been shown since the last start
    int show_one;      // show a picture even though paused (after a seek)
    int64_t last_shown90;
    int ended;
    int ended_told;
    int video_offset_ms;

    // Since the last start; for diagnosing a missing first picture
    int64_t started_ns;
    int pairs_pushed;
    int watch_logged;
    int skipped_to_floor;

    // Counters
    int shown, dropped, starved;
    int sync_count;
    int64_t sync_worst;
} DiscPlayer;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ---- clock -----------------------------------------------------------------------

// Media time (90 kHz) elapsed in ns real time at the current speed. Under clock_lock.
static int64_t film90(DiscPlayer *p, int64_t ns) {
    if (p->speed == 1.0) {
        return ns * 9 / 100000;
    }
    return (int64_t)((double)ns * 9e-5 * p->speed);
}

static int64_t clock_now90(DiscPlayer *p) {
    pthread_mutex_lock(&p->clock_lock);
    int64_t v = p->clock_media90;
    if (p->playing) {
        v += film90(p, now_ns() - p->clock_nano);
    }
    pthread_mutex_unlock(&p->clock_lock);
    return v;
}

static void clock_set(DiscPlayer *p, int64_t media90, int64_t nano, int playing) {
    pthread_mutex_lock(&p->clock_lock);
    p->clock_media90 = media90;
    p->clock_nano = nano;
    p->playing = playing;
    pthread_mutex_unlock(&p->clock_lock);
}

// ---- queue -----------------------------------------------------------------------

static void queue_init(PairQueue *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void queue_clear(PairQueue *q) {
    pthread_mutex_lock(&q->lock);
    PairNode *n = q->head;
    while (n != NULL) {
        PairNode *next = n->next;
        free(n->data);
        free(n);
        n = next;
    }
    q->head = q->tail = NULL;
    q->count = 0;
    q->bytes = 0;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

// Waits for room, then adds the pair. Returns 0 when stopped.
static int queue_push(DiscPlayer *p, PairQueue *q, const DiscItem *it) {
    pthread_mutex_lock(&q->lock);
    while (!atomic_load(&p->stop) && (q->count >= MAX_QUEUE || q->bytes > QUEUE_BYTES)) {
        pthread_cond_wait(&q->cond, &q->lock);
    }
    if (atomic_load(&p->stop)) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }
    pthread_mutex_unlock(&q->lock);
    PairNode *n = calloc(1, sizeof(*n));
    uint8_t *copy = n != NULL ? malloc(it->size) : NULL;
    if (copy == NULL) {
        free(n);
        return 1;
    }
    memcpy(copy, it->data, it->size);
    n->data = copy;
    n->size = it->size;
    n->pts = it->pts90;
    pthread_mutex_lock(&q->lock);
    if (q->tail != NULL) {
        q->tail->next = n;
    }
    else {
        q->head = n;
    }
    q->tail = n;
    q->count++;
    q->bytes += n->size;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return 1;
}

static PairNode *queue_pop(DiscPlayer *p, PairQueue *q) {
    pthread_mutex_lock(&q->lock);
    while (!atomic_load(&p->stop) && q->head == NULL) {
        pthread_cond_wait(&q->cond, &q->lock);
    }
    PairNode *n = q->head;
    if (n != NULL && !atomic_load(&p->stop)) {
        q->head = n->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
        q->count--;
        q->bytes -= n->size;
        pthread_cond_broadcast(&q->cond);
    }
    else {
        n = NULL;
    }
    pthread_mutex_unlock(&q->lock);
    return n;
}

// ---- Java callbacks ---------------------------------------------------------------

static JNIEnv *attach(DiscPlayer *p) {
    JNIEnv *env = NULL;
    if ((*p->vm)->GetEnv(p->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        (*p->vm)->AttachCurrentThread(p->vm, &env, NULL);
    }
    return env;
}

static void deliver_pes(DiscPlayer *p, JNIEnv *env, const DiscItem *it) {
    if (p->sink == NULL || env == NULL) {
        return;
    }
    jbyteArray arr = (*env)->NewByteArray(env, (jsize)it->size);
    if (arr == NULL) {
        return;
    }
    (*env)->SetByteArrayRegion(env, arr, 0, (jsize)it->size, (const jbyte *)it->data);
    (*env)->CallVoidMethod(env, p->sink, p->on_pes, (jint)it->pid, (jlong)it->pts90, arr);
    (*env)->DeleteLocalRef(env, arr);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
}

// ---- threads -----------------------------------------------------------------------

static void *reader_main(void *arg) {
    DiscPlayer *p = arg;
    JNIEnv *env = attach(p);
    DiscItem it;
    int r;
    while (!atomic_load(&p->stop) && (r = ds_next(p->src, &it)) > 0) {
        if (it.type == DS_PAIR) {
            if (!queue_push(p, &p->queue, &it)) {
                break;
            }
            if (p->pairs_pushed++ == 0) {
                LOGI("reader: first picture after the start at %.2f s, %.0f ms in", it.pts90 / 90000.0,
                     (now_ns() - p->started_ns) / 1e6);
            }
        }
        else if (it.type == DS_PES && it.pts90 >= p->floor90 - 90000) {
            deliver_pes(p, env, &it);
        }
    }
    if (!atomic_load(&p->stop)) {
        LOGI("reader: end of the playlist");
        // The render thread reports the end once the queue drains.
        pthread_mutex_lock(&p->queue.lock);
        p->ended = 1;
        pthread_cond_broadcast(&p->queue.cond);
        pthread_mutex_unlock(&p->queue.lock);
    }
    (*p->vm)->DetachCurrentThread(p->vm);
    return NULL;
}

static void *decoder_main(void *arg) {
    DiscPlayer *p = arg;
    PairNode *n;
    while ((n = queue_pop(p, &p->queue)) != NULL) {
        int r = dd_feed(p->dd, n->data, n->size, n->pts);
        free(n->data);
        free(n);
        if (r < 0) {
            break;
        }
    }
    return NULL;
}

// ---- drawing -------------------------------------------------------------------------

static const char *VERT =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    uv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// Left half is eye 0, right half eye 1. BT.709, limited range.
static const char *FRAG =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "uniform sampler2D ty0, tu0, tv0, ty1, tu1, tv1;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    bool right = uv.x >= 0.5;\n"
    "    vec2 t = vec2(right ? uv.x * 2.0 - 1.0 : uv.x * 2.0, uv.y);\n"
    "    float y = right ? texture(ty1, t).r : texture(ty0, t).r;\n"
    "    float u = (right ? texture(tu1, t).r : texture(tu0, t).r) - 0.5;\n"
    "    float v = (right ? texture(tv1, t).r : texture(tv0, t).r) - 0.5;\n"
    "    y = (y - 16.0 / 255.0) * 1.164383;\n"
    "    u *= 255.0 / 224.0;\n"
    "    v *= 255.0 / 224.0;\n"
    "    color = vec4(clamp(vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u), 0.0, 1.0), 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOGE("shader: %s", log);
    }
    return s;
}

static int gl_init(DiscPlayer *p, int w, int h) {
    p->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->dpy, NULL, NULL)) {
        return 0;
    }
    const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(p->dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        return 0;
    }
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    p->ctx = eglCreateContext(p->dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    ANativeWindow_setBuffersGeometry(p->window, w * 2, h, 0);
    p->surf = eglCreateWindowSurface(p->dpy, cfg, p->window, NULL);
    if (p->ctx == EGL_NO_CONTEXT || p->surf == EGL_NO_SURFACE
            || !eglMakeCurrent(p->dpy, p->surf, p->surf, p->ctx)) {
        LOGE("EGL setup failed: %x", eglGetError());
        return 0;
    }
    eglSwapInterval(p->dpy, 0);

    GLuint vs = compile(GL_VERTEX_SHADER, VERT), fs = compile(GL_FRAGMENT_SHADER, FRAG);
    p->program = glCreateProgram();
    glAttachShader(p->program, vs);
    glAttachShader(p->program, fs);
    glLinkProgram(p->program);
    glUseProgram(p->program);
    const char *names[6] = { "ty0", "tu0", "tv0", "ty1", "tu1", "tv1" };
    glGenTextures(6, p->tex);
    for (int i = 0; i < 6; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, p->tex[i]);
        int cw = (i % 3 == 0) ? w : w / 2, ch = (i % 3 == 0) ? h : h / 2;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, cw, ch, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUniform1i(glGetUniformLocation(p->program, names[i]), i);
    }
    p->out_w = w;
    p->out_h = h;
    LOGI("render: EGL up, %dx%d per eye into a %dx%d frame", w, h, w * 2, h);
    return 1;
}

static void gl_done(DiscPlayer *p) {
    if (p->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(p->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (p->surf != EGL_NO_SURFACE) eglDestroySurface(p->dpy, p->surf);
        if (p->ctx != EGL_NO_CONTEXT) eglDestroyContext(p->dpy, p->ctx);
        eglTerminate(p->dpy);
    }
    p->dpy = EGL_NO_DISPLAY;
    p->surf = EGL_NO_SURFACE;
    p->ctx = EGL_NO_CONTEXT;
}

static void upload(DiscPlayer *p, const DiscFrame *f) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int eye = 0; eye < 2; eye++) {
        const uint8_t *planes[3] = { f->y[eye], f->u[eye], f->v[eye] };
        for (int k = 0; k < 3; k++) {
            int i = eye * 3 + k;
            int w = k == 0 ? f->width : f->cwidth, h = k == 0 ? f->height : f->cheight;
            int stride = k == 0 ? f->stride_y : f->stride_c;
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, p->tex[i]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, planes[k]);
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static void draw(DiscPlayer *p, const DiscFrame *f) {
    upload(p, f);
    glViewport(0, 0, p->out_w * 2, p->out_h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    eglSwapBuffers(p->dpy, p->surf);
}

static void *render_main(void *arg) {
    DiscPlayer *p = arg;
    JNIEnv *env = attach(p);
    DiscFrame f;
    int gl_ready = 0;
    int64_t log_at = now_ns();
    int log_shown = 0;
    while (!atomic_load(&p->stop)) {
        if (!dd_pop(p->dd, &f, 20)) {
            // Playing with nothing to show: decoder is behind.
            pthread_mutex_lock(&p->clock_lock);
            int playing = p->playing && p->first_shown;
            pthread_mutex_unlock(&p->clock_lock);
            if (playing && !p->ended) {
                p->starved++;
            }
            // No picture 2 s after start: log pipeline state.
            if (!p->first_shown && !p->watch_logged && now_ns() - p->started_ns > 2000000000LL) {
                p->watch_logged = 1;
                int skipped, damaged, one_eye, waits, pending;
                dd_stats(p->dd, &skipped, &damaged, &one_eye, &waits, &pending);
                int lost_pairs, lost_deps;
                ds_lost(p->src, &lost_pairs, &lost_deps);
                LOGW("render: no picture %.1f s after the start: %d pictures read, %d queued, decoder skipped %d, "
                     "damaged %d, one eye %d, waits %d, pending %d; lost %d pictures, %d dependent; floor %.2f s, "
                     "%d decoded before it",
                     (now_ns() - p->started_ns) / 1e9, p->pairs_pushed, p->queue.count, skipped, damaged, one_eye,
                     waits, pending, lost_pairs, lost_deps, p->floor90 / 90000.0, p->skipped_to_floor);
            }
            if (p->ended && !p->ended_told && p->queue.count == 0 && p->sink != NULL && env != NULL) {
                p->ended_told = 1;
                (*env)->CallVoidMethod(env, p->sink, p->on_ended);
                if ((*env)->ExceptionCheck(env)) {
                    (*env)->ExceptionClear(env);
                }
            }
            continue;
        }
        if (f.pts90 < p->floor90) {
            // Decoded only to reach the seek target.
            p->skipped_to_floor++;
            dd_release(p->dd, &f);
            continue;
        }
        if (!gl_ready) {
            if (!gl_init(p, f.width, f.height)) {
                dd_release(p->dd, &f);
                break;
            }
            gl_ready = 1;
        }

        // Wait for the picture's time; the first picture after a start or seek shows at once.
        for (;;) {
            if (atomic_load(&p->stop)) {
                break;
            }
            pthread_mutex_lock(&p->clock_lock);
            int hold = !p->first_shown || p->show_one;
            pthread_mutex_unlock(&p->clock_lock);
            if (hold) {
                break;
            }
            int64_t ahead = f.pts90 + (int64_t)p->video_offset_ms * 90 - PRESENT_LEAD_90K - clock_now90(p);
            if (ahead <= 0) {
                break;
            }
            // Sleep at most 4 ms so pause and seek are noticed.
            int64_t us = ahead * 1000 / 90;
            if (us > 4000) {
                us = 4000;
            }
            usleep((useconds_t)us);
        }
        if (atomic_load(&p->stop)) {
            dd_release(p->dd, &f);
            break;
        }
        for (;;) {
            pthread_mutex_lock(&p->clock_lock);
            int wait = !p->playing && p->first_shown && !p->show_one && p->want_play == 0;
            pthread_mutex_unlock(&p->clock_lock);
            if (!wait || atomic_load(&p->stop)) {
                break;
            }
            usleep(8000);
        }

        int64_t late = clock_now90(p) - (f.pts90 + (int64_t)p->video_offset_ms * 90);
        if (p->first_shown && late > DROP_LATE_90K) {
            p->dropped++;
            dd_release(p->dd, &f);
            continue;
        }
        draw(p, &f);
        p->last_shown90 = f.pts90;
        p->shown++;
        log_shown++;
        int first = 0;
        pthread_mutex_lock(&p->clock_lock);
        if (!p->first_shown) {
            p->first_shown = 1;
            first = 1;
            // Start the clock at the first picture so decode time is not skipped over.
            p->clock_media90 = f.pts90;
            p->clock_nano = now_ns();
            p->playing = p->want_play;
        }
        p->show_one = 0;
        pthread_mutex_unlock(&p->clock_lock);
        if (first && p->sink != NULL && env != NULL) {
            (*env)->CallVoidMethod(env, p->sink, p->on_first_frame, (jlong)f.pts90);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
            }
        }
        dd_release(p->dd, &f);
        if (now_ns() - log_at > 5000000000LL) {
            int skipped, damaged, one_eye, waits, pending;
            dd_stats(p->dd, &skipped, &damaged, &one_eye, &waits, &pending);
            pthread_mutex_lock(&p->clock_lock);
            int syncs = p->sync_count;
            double worst = p->sync_worst / 90.0;
            p->sync_count = 0;
            p->sync_worst = 0;
            pthread_mutex_unlock(&p->clock_lock);
            LOGI("render: %d shown in 5 s, %d dropped, %d starved in all, queue %d, at %.1f s; decoder skipped %d, "
                 "damaged %d, one eye %d, waits %d, pending %d; %d clock syncs, worst %.0f ms",
                 log_shown, p->dropped, p->starved, p->queue.count, p->last_shown90 / 90000.0, skipped, damaged,
                 one_eye, waits, pending, syncs, worst);
            log_at = now_ns();
            log_shown = 0;
        }
    }
    if (gl_ready) {
        gl_done(p);
    }
    (*p->vm)->DetachCurrentThread(p->vm);
    return NULL;
}

// ---- control -----------------------------------------------------------------------------

static void threads_start(DiscPlayer *p) {
    atomic_store(&p->stop, 0);
    p->ended = 0;
    p->ended_told = 0;
    p->first_shown = 0;
    p->dropped = 0;
    p->started_ns = now_ns();
    p->pairs_pushed = 0;
    p->watch_logged = 0;
    p->skipped_to_floor = 0;
    p->running = 1;
    pthread_create(&p->reader, NULL, reader_main, p);
    pthread_create(&p->decoder, NULL, decoder_main, p);
    pthread_create(&p->render, NULL, render_main, p);
}

static void threads_stop(DiscPlayer *p) {
    if (!p->running) {
        return;
    }
    atomic_store(&p->stop, 1);
    dd_abort(p->dd);
    pthread_mutex_lock(&p->queue.lock);
    pthread_cond_broadcast(&p->queue.cond);
    pthread_mutex_unlock(&p->queue.lock);
    pthread_join(p->reader, NULL);
    pthread_join(p->decoder, NULL);
    pthread_join(p->render, NULL);
    p->running = 0;
    queue_clear(&p->queue);
}

// ---- JNI -------------------------------------------------------------------------------------

static JavaVM *g_vm;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT jlong JNICALL
Java_com_limelight_player_DiscNative_open(JNIEnv *env, jclass clazz, jstring path, jint threads) {
    (void)clazz;
    const char *iso = (*env)->GetStringUTFChars(env, path, NULL);
    DiscPlayer *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        (*env)->ReleaseStringUTFChars(env, path, iso);
        return 0;
    }
    p->disc = disc_open(iso);
    (*env)->ReleaseStringUTFChars(env, path, iso);
    if (p->disc == NULL || disc_main_playlist(p->disc, &p->playlist) < 0) {
        LOGE("open: not a Blu-ray disc image, or no playlist");
        disc_close(p->disc);
        free(p);
        return 0;
    }
    // Open the source now so streams can be selected before start.
    p->src = ds_open(p->disc, &p->playlist);
    if (p->src == NULL) {
        LOGE("open: the first clip has no SSIF");
        disc_close(p->disc);
        free(p);
        return 0;
    }
    p->threads = threads;
    p->vm = g_vm;
    pthread_mutex_init(&p->clock_lock, NULL);
    p->speed = 1.0;
    queue_init(&p->queue);
    p->dpy = EGL_NO_DISPLAY;
    p->surf = EGL_NO_SURFACE;
    p->ctx = EGL_NO_CONTEXT;
    LOGI("open: %s, playlist %s, %.0f s, %s", disc_label(p->disc), p->playlist.name,
         p->playlist.duration45 / 45000.0, p->playlist.is3d ? "3D" : "2D");
    return (jlong)(intptr_t)p;
}

// Disc description, one tab-separated line each:
//   T title / D duration in 90 kHz ticks / C chapter times, comma separated /
//   V pid coding / A pid coding channels rate lang / S pid coding lang
JNIEXPORT jstring JNICALL
Java_com_limelight_player_DiscNative_describe(JNIEnv *env, jclass clazz, jlong handle) {
    (void)clazz;
    DiscPlayer *p = (DiscPlayer *)(intptr_t)handle;
    char buf[8192];
    int n = snprintf(buf, sizeof(buf), "T\t%s\nD\t%lld\nM\t%d\n", disc_label(p->disc),
                     (long long)p->playlist.duration45 * 2, p->playlist.is3d);
    n += snprintf(buf + n, sizeof(buf) - n, "C\t");
    for (int i = 0; i < p->playlist.nchapters && n < (int)sizeof(buf) - 64; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s%lld", i ? "," : "", (long long)p->playlist.chapter45[i] * 2);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "\n");
    for (int i = 0; i < p->playlist.nstreams && n < (int)sizeof(buf) - 96; i++) {
        const DiscStream *s = &p->playlist.streams[i];
        const char *kind = s->kind == DISC_VIDEO ? "V" : s->kind == DISC_AUDIO ? "A" : "S";
        n += snprintf(buf + n, sizeof(buf) - n, "%s\t%d\t%d\t%d\t%d\t%s\n", kind, s->pid, s->coding,
                      s->channels, s->rate, s->lang);
    }
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jboolean JNICALL
Java_com_limelight_player_DiscNative_start(JNIEnv *env, jclass clazz, jlong handle, jobject surface,
                                           jlong start90, jobject sink, jboolean play) {
    (void)clazz;
    DiscPlayer *p = (DiscPlayer *)(intptr_t)handle;
    p->window = ANativeWindow_fromSurface(env, surface);
    if (p->window == NULL) {
        return JNI_FALSE;
    }
    p->dd = dd_open(p->threads, 6);
    if (p->src == NULL || p->dd == NULL) {
        return JNI_FALSE;
    }
    p->sink = (*env)->NewGlobalRef(env, sink);
    jclass cls = (*env)->GetObjectClass(env, sink);
    p->on_pes = (*env)->GetMethodID(env, cls, "onPes", "(IJ[B)V");
    p->on_first_frame = (*env)->GetMethodID(env, cls, "onFirstFrame", "(J)V");
    p->on_ended = (*env)->GetMethodID(env, cls, "onEnded", "()V");
    int64_t landed = 0;
    if (start90 > 0 && ds_seek(p->src, start90, &landed) < 0) {
        LOGW("start: seek to %.1f s failed, from the start", start90 / 90000.0);
        start90 = 0;
    }
    p->floor90 = start90;
    p->want_play = play ? 1 : 0;
    p->show_one = 1;
    clock_set(p, start90, now_ns(), 0);
    threads_start(p);
    LOGI("start: from %.2f s (landed %.2f s)", start90 / 90000.0, landed / 90000.0);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_limelight_player_DiscNative_wantPid(JNIEnv *env, jclass clazz, jlong handle, jint pid, jboolean on) {
    (void)env;
    (void)clazz;
    DiscPlayer *p = (DiscPlayer *)(intptr_t)handle;
    if (p->src != NULL) {
        ds_want_pid(p->src, pid, on);
    }
}

JNIEXPORT void JNICALL
Java_com_limelight_player_DiscNative_setPlaying(JNIEnv *env, jclass clazz, jlong handle, jboolean play) {
    (void)env;
    (void)clazz;
    DiscPlayer *p = (DiscPlayer *)(intptr_t)handle;
    int64_t at = clock_now90(p);
    pthread_mutex_lock(&p->clock_lock);
    p->want_play = play ? 1 : 0;
    // The clock holds where it is, or runs on from there
    p->clock_media90 = at;
    p->clock_nano = now_ns();
    p->playing = play && p->first_shown;
    pthread_mutex_unlock(&p->clock_lock);
}

JNIEXPORT jboolean JNICALL
Java_com_limelight_player_DiscNative_seek(JNIEnv *env, jclass clazz, jlong handle, jlong t90) {
    (void)env;
    (void)clazz;
    DiscPlayer *p = (DiscPlayer *)(intptr_t)handle;
    threads_stop(p);
    dd_flush(p->dd);
    int64_t landed = 0;
    if (ds_seek(p->src, t90, &landed) < 0) {
        LOGW("seek to %.1f s failed", t90 / 90000.0);
        threads_start(p);
        return JNI_FALSE;
    }
    p->floor90 = t90;
    p->show_one = 1;
    clock_set(p, t90, now_ns(), 0);
    threads_start(p);
    LOGI("seek: to %.2f s (landed %.2f s)", t90 / 90000.0, landed / 90000.0);
    return JNI_TRUE;
}

// Audio position: media time and the CLOCK_MONOTONIC time it was sampled at.
JNIEXPORT void JNICALL
Java_com_limelight_player_DiscNative_syncClock(JNIEnv *env, jclass clazz, jlong handle, jlong media90,
                                               jlong nano) {
    (void)env;
    (void)clazz;
    DiscPlayer *p = (DiscPlayer *)(intptr_t)handle;
    int64_t now = now_ns();
    pthread_mutex_lock(&p->clock_lock);
    if (p->playing) {
        // Both extrapolated at the current speed (audio is time-stretched to it).
        int64_t heard = media90 + film90(p, now - nano);
        int64_t clock = p->clock_media90 + film90(p, now - p->clock_nano);
        int64_t error = heard - clock;
        // Over 250 ms: snap. Otherwise correct a tenth per sync so the picture never jumps.
        int64_t step = (error > 22500 || error < -22500) ? error : error / 10;
        p->clock_media90 = clock + step;
        p->clock_nano = now;
        p->sync_count++;
        int64_t abs_err = error < 0 ? -error : error;
        if (abs_err > p->sync_worst) {
            p->sync_worst = abs_err;
        }
    }
    pthread_mutex_unlock(&p->clock_lock);
}

JNIEXPORT jlong JNICALL
Java_com_limelight_player_DiscNative_position(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env;
    (void)clazz;
    return clock_now90((DiscPlayer *)(intptr_t)handle);
}

// Clamped to 0.25-4. The clock is rebased at the current position first.
JNIEXPORT void JNICALL
Java_com_limelight_player_DiscNative_setSpeed(JNIEnv *env, jclass clazz, jlong handle, jfloat speed) {
    (void)env;
    (void)clazz;
    DiscPlayer *p = (DiscPlayer *)(intptr_t)handle;
    double s = speed < 0.25f ? 0.25 : (speed > 4.0f ? 4.0 : (double)speed);
    int64_t now = now_ns();
    pthread_mutex_lock(&p->clock_lock);
    if (p->playing) {
        p->clock_media90 += film90(p, now - p->clock_nano);
    }
    p->clock_nano = now;
    p->speed = s;
    pthread_mutex_unlock(&p->clock_lock);
    LOGI("speed %.2f", s);
}

JNIEXPORT void JNICALL
Java_com_limelight_player_DiscNative_setVideoOffset(JNIEnv *env, jclass clazz, jlong handle, jint ms) {
    (void)env;
    (void)clazz;
    ((DiscPlayer *)(intptr_t)handle)->video_offset_ms = ms;
}

JNIEXPORT void JNICALL
Java_com_limelight_player_DiscNative_close(JNIEnv *env, jclass clazz, jlong handle) {
    (void)clazz;
    DiscPlayer *p = (DiscPlayer *)(intptr_t)handle;
    threads_stop(p);
    dd_close(p->dd);
    ds_close(p->src);
    disc_close(p->disc);
    if (p->window != NULL) {
        ANativeWindow_release(p->window);
    }
    if (p->sink != NULL) {
        (*env)->DeleteGlobalRef(env, p->sink);
    }
    free(p);
}
