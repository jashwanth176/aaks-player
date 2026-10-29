LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := depth-xnnpack
LOCAL_SRC_FILES := depth_xnnpack.c
# Only JNI entry points are exported; LiteRT is resolved at run time from the AAR's library.
LOCAL_CFLAGS := -Wall -Werror -fvisibility=hidden
LOCAL_LDLIBS := -ldl -llog

include $(BUILD_SHARED_LIBRARY)
