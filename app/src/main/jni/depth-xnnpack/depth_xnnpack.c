// LiteRT's XNNPACK delegate with a chosen thread count, for the depth model's CPU path.
// The default XNNPACK path is single-threaded and LiteRT has no Java class for the
// delegate, so its C functions are looked up in the AAR's runtime library. If any are
// missing, no delegate is created.
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>

#include <android/log.h>

#define TAG "DepthXnnpack"

// Not in the AAR's headers. Assumes numThreads comes first and the struct fits
// (64 bytes on arm64 in LiteRT 1.4.2). The count is read back after creation, so a
// changed layout yields no delegate rather than a wrong one.
typedef struct {
    int32_t numThreads;
    unsigned char rest[508];
} __attribute__((aligned(16))) XnnpackOptions;

typedef XnnpackOptions (*OptionsDefaultFn)(void);
typedef void* (*CreateFn)(const XnnpackOptions* options);
typedef void (*DeleteFn)(void* delegate);
typedef const int32_t* (*GetOptionsFn)(void* delegate);
typedef void* (*GetThreadPoolFn)(void* delegate);

static OptionsDefaultFn optionsDefault;
static CreateFn create;
static DeleteFn destroy;
static GetOptionsFn getOptions;
static GetThreadPoolFn getThreadPool;
static pthread_once_t lookUpOnce = PTHREAD_ONCE_INIT;

static void* find(void* lib, const char* name) {
    void* fn = dlsym(lib, name);
    if (fn == NULL) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "LiteRT has no %s", name);
    }
    return fn;
}

static void lookUp(void) {
    // Java has already loaded it; RTLD_NOLOAD finds that copy. The handle is never closed.
    void* lib = dlopen("libtensorflowlite_jni.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib == NULL) {
        const char* why = dlerror();
        __android_log_print(ANDROID_LOG_WARN, TAG, "LiteRT is not loaded: %s",
                            why != NULL ? why : "no reason given");
        return;
    }
    OptionsDefaultFn foundDefault =
            (OptionsDefaultFn) find(lib, "TfLiteXNNPackDelegateOptionsDefault");
    CreateFn foundCreate = (CreateFn) find(lib, "TfLiteXNNPackDelegateCreate");
    DeleteFn foundDelete = (DeleteFn) find(lib, "TfLiteXNNPackDelegateDelete");
    GetOptionsFn foundOptions = (GetOptionsFn) find(lib, "TfLiteXNNPackDelegateGetOptions");
    GetThreadPoolFn foundPool = (GetThreadPoolFn) find(lib, "TfLiteXNNPackDelegateGetThreadPool");
    if (foundDefault == NULL || foundCreate == NULL || foundDelete == NULL
            || foundOptions == NULL || foundPool == NULL) {
        return;
    }
    optionsDefault = foundDefault;
    destroy = foundDelete;
    getOptions = foundOptions;
    getThreadPool = foundPool;
    create = foundCreate;
}

JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XnnpackDelegate_nativeCreate(
        JNIEnv* env, jclass clazz, jint threads) {
    (void) env;
    (void) clazz;
    pthread_once(&lookUpOnce, lookUp);
    if (create == NULL) {
        return 0;
    }
    XnnpackOptions options = optionsDefault();
    options.numThreads = threads;
    void* delegate = create(&options);
    if (delegate == NULL) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "XNNPACK delegate on %d threads not created",
                            threads);
        return 0;
    }
    // Verify the delegate's copy of the options and its pool (one thread has no pool).
    const int32_t* held = getOptions(delegate);
    int heldThreads = held != NULL ? *held : -1;
    int pooled = getThreadPool(delegate) != NULL;
    if (heldThreads != threads || pooled != (threads > 1)) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
                            "XNNPACK delegate asked for %d threads came back with %d and %s pool",
                            threads, heldThreads, pooled ? "a" : "no");
        destroy(delegate);
        return 0;
    }
    return (jlong) (intptr_t) delegate;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XnnpackDelegate_nativeDelete(
        JNIEnv* env, jclass clazz, jlong handle) {
    (void) env;
    (void) clazz;
    // A handle only exists once the lookup has found everything
    if (handle != 0 && destroy != NULL) {
        destroy((void*) (intptr_t) handle);
    }
}
