# Native Blu-ray 3D player: disc image reading, MVC decoding, and audio codecs the
# platform lacks.
#   libudfread (LGPL-2.1)  reads the disc image's UDF filesystem
#   edge264 (BSD-3)        decodes the H.264 MVC video, both views
#   FFmpeg libavcodec (LGPL-2.1, no GPL parts) decodes DTS, DTS-HD and TrueHD;
#                          a minimal build kept as static libraries in ffmpeg/
#
# Prebuilt libraries are arm64 only, so other ABIs skip this module.

LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_ARCH_ABI),arm64-v8a)

include $(CLEAR_VARS)
LOCAL_MODULE := avcodec
LOCAL_SRC_FILES := ffmpeg/lib/$(TARGET_ARCH_ABI)/libavcodec.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avutil
LOCAL_SRC_FILES := ffmpeg/lib/$(TARGET_ARCH_ABI)/libavutil.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := disc
LOCAL_SRC_FILES := \
    disc_nav.c \
    disc_ssif.c \
    disc_video.c \
    disc_player.c \
    disc_ffaudio.c \
    edge264/src/edge264.c \
    libudfread/udfread.c \
    libudfread/default_blockinput.c \
    libudfread/ecma167.c \
    libudfread/udf_fs.c \
    libudfread/udf_volume.c \
    libudfread/udfread-version.c
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/libudfread $(LOCAL_PATH)/edge264 $(LOCAL_PATH)/edge264/src \
    $(LOCAL_PATH)/ffmpeg/include
LOCAL_CFLAGS := -O3 -std=gnu11 -flax-vector-conversions -Wno-override-init -Wno-unused-parameter \
    -DHAVE_UNISTD_H=1 -DHAVE_FCNTL_H=1 -pthread
LOCAL_STATIC_LIBRARIES := avcodec avutil
LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -lm
# Keep FFmpeg symbols private to this library.
LOCAL_LDFLAGS := -Wl,--exclude-libs,ALL
include $(BUILD_SHARED_LIBRARY)

endif
