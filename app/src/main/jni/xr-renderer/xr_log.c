// Native side of the file log. Lines go to logcat, and to the file the Java
// side opened when file logging is enabled.
#include "xr_renderer.h"

// Globals rather than context fields: configured before any context exists
static char fileLogPath[512];
static int fileLogLevel = FILE_LOG_OFF;

// Fixed ring buffer drained by a writer thread, so render and depth threads
// never block on file I/O. Sized for the startup burst.
#define LOG_QUEUE_LINES 256
#define LOG_LINE_MAX 1024
static char logQueue[LOG_QUEUE_LINES][LOG_LINE_MAX];
static int logQueueLens[LOG_QUEUE_LINES];
static int logHead;
static int logCount;
static int logDropped;
// Enqueued/written counters, so a caller can wait for its own line
static unsigned long logEnqueued;
static unsigned long logWritten;
static int logWriterStarted;
static pthread_mutex_t logMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t logLinesReady = PTHREAD_COND_INITIALIZER;
static pthread_cond_t logLineWritten = PTHREAD_COND_INITIALIZER;

// Errors wait (up to this long) for the writer, in case the process dies next
#define LOG_ERROR_FLUSH_MS 250

// Writer thread only. Kept open: each open in Download goes through the media
// provider (about 0.3 ms average, 10 ms worst case).
static int logFd = -1;
static ino_t logIno;
static char logOpenPath[512];

/**
 * Appends one line in a single write so it cannot interleave with the Java
 * logger's lines. The cached fd is reopened if the path changed or the inode
 * at the path differs (the Java side deletes the file when it rotates).
 */
static void appendLine(const char* path, const char* line, int n) {
    if (logFd >= 0) {
        struct stat current;
        if (strcmp(path, logOpenPath) != 0 || stat(path, &current) != 0
                || current.st_ino != logIno) {
            close(logFd);
            logFd = -1;
        }
    }

    if (logFd < 0) {
        struct stat opened;
        logFd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (logFd < 0 || fstat(logFd, &opened) != 0) {
            if (logFd >= 0) {
                close(logFd);
                logFd = -1;
            }
            // Retried on the next line
            return;
        }
        logIno = opened.st_ino;
        strncpy(logOpenPath, path, sizeof(logOpenPath) - 1);
        logOpenPath[sizeof(logOpenPath) - 1] = '\0';
    }

    ssize_t ignored = write(logFd, line, (size_t)n);
    (void)ignored;
}

// Same timestamp format as the Java logger. Returns the length, or 0 if the
// prefix does not fit.
static int formatLine(char* line, size_t size, char level, const char* fmt, va_list args) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm parts;
    localtime_r(&ts.tv_sec, &parts);

    int n = snprintf(line, size, "%02d-%02d %02d:%02d:%02d.%03d %c xr: ",
                     parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                     parts.tm_sec, (int)(ts.tv_nsec / 1000000), level);
    if (n < 0 || (size_t)n >= size) {
        return 0;
    }

    size_t room = size - (size_t)n - 1;
    int m = vsnprintf(line + n, room, fmt, args);
    if (m < 0) {
        return 0;
    }
    if ((size_t)m >= room) {
        m = (int)room - 1;
    }
    n += m;
    line[n++] = '\n';
    return n;
}

// Drains the queue, holding the lock only while touching it
static void* logWriter(void* arg) {
    (void)arg;
    pthread_mutex_lock(&logMutex);
    for (;;) {
        while (logCount == 0) {
            pthread_cond_wait(&logLinesReady, &logMutex);
        }
        char line[LOG_LINE_MAX];
        int n = logQueueLens[logHead];
        memcpy(line, logQueue[logHead], (size_t)n);
        logHead = (logHead + 1) % LOG_QUEUE_LINES;
        logCount--;
        int dropped = logDropped;
        logDropped = 0;
        char path[sizeof(fileLogPath)];
        memcpy(path, fileLogPath, sizeof(path));
        pthread_mutex_unlock(&logMutex);

        if (path[0] != '\0') {
            if (dropped > 0) {
                // Report lines dropped while the queue was full
                char note[128];
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                struct tm parts;
                localtime_r(&ts.tv_sec, &parts);
                int len = snprintf(note, sizeof(note),
                                   "%02d-%02d %02d:%02d:%02d.%03d W xr: %d log lines dropped\n",
                                   parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                                   parts.tm_sec, (int)(ts.tv_nsec / 1000000), dropped);
                if (len > 0 && (size_t)len < sizeof(note)) {
                    appendLine(path, note, len);
                }
            }
            appendLine(path, line, n);
        }

        pthread_mutex_lock(&logMutex);
        logWritten++;
        pthread_cond_broadcast(&logLineWritten);
    }
    return NULL;
}

// Caller holds logMutex
static void startWriterLocked(void) {
    if (logWriterStarted) {
        return;
    }
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, logWriter, NULL) == 0) {
        pthread_setname_np(thread, "XR file log");
        logWriterStarted = 1;
    }
    pthread_attr_destroy(&attr);
}

// Queues a line. Errors, warnings and events always; info only at verbose.
// Errors wait for the writer so they reach disk.
static void writeFileLog(int prio, const char* fmt, va_list args) {
    char level;
    if (prio == ANDROID_LOG_ERROR) {
        level = 'E';
    }
    else if (prio == ANDROID_LOG_WARN) {
        level = 'W';
    }
    else if (prio == PRIO_EVENT) {
        level = 'I';
    }
    else if (fileLogLevel >= FILE_LOG_VERBOSE) {
        level = 'V';
    }
    else {
        return;
    }

    char line[LOG_LINE_MAX];
    int n = formatLine(line, sizeof(line), level, fmt, args);
    if (n == 0) {
        return;
    }

    pthread_mutex_lock(&logMutex);
    if (logCount == LOG_QUEUE_LINES) {
        logDropped++;
        pthread_mutex_unlock(&logMutex);
        return;
    }
    int tail = (logHead + logCount) % LOG_QUEUE_LINES;
    memcpy(logQueue[tail], line, (size_t)n);
    logQueueLens[tail] = n;
    logCount++;
    unsigned long mine = ++logEnqueued;
    startWriterLocked();
    pthread_cond_signal(&logLinesReady);

    if (prio == ANDROID_LOG_ERROR && logWriterStarted) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += LOG_ERROR_FLUSH_MS * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        while (logWritten < mine) {
            if (pthread_cond_timedwait(&logLineWritten, &logMutex, &deadline) != 0) {
                break;
            }
        }
    }
    pthread_mutex_unlock(&logMutex);
}

void xrLog(int prio, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(prio == PRIO_EVENT ? ANDROID_LOG_INFO : prio, TAG, fmt, args);
    va_end(args);

    if (fileLogLevel == FILE_LOG_OFF || fileLogPath[0] == '\0') {
        return;
    }
    va_start(args, fmt);
    writeFileLog(prio, fmt, args);
    va_end(args);
}

// Sets the log file path and level. Called before init. The writer switches
// to a new path on its next line.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetFileLog(JNIEnv* env, jclass clazz,
                                                             jstring path, jint level) {
    const char* chars = NULL;
    if (path != NULL && level > FILE_LOG_OFF) {
        chars = (*env)->GetStringUTFChars(env, path, NULL);
    }

    pthread_mutex_lock(&logMutex);
    if (chars == NULL) {
        fileLogLevel = FILE_LOG_OFF;
        fileLogPath[0] = '\0';
    }
    else {
        strncpy(fileLogPath, chars, sizeof(fileLogPath) - 1);
        fileLogPath[sizeof(fileLogPath) - 1] = '\0';
        fileLogLevel = level;
    }
    pthread_mutex_unlock(&logMutex);

    if (chars != NULL) {
        (*env)->ReleaseStringUTFChars(env, path, chars);
    }
}
