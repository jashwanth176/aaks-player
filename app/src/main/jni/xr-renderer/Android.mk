LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xr-renderer
LOCAL_SRC_FILES := xr_log.c \
                   xr_math.c \
                   xr_depthmap.c \
                   xr_atlas.c \
                   xr_roommesh.c \
                   xr_layout.c \
                   xr_handles.c \
                   xr_uipanels.c \
                   xr_relight.c \
                   xr_roomlight.c \
                   xr_rate.c \
                   xr_gate.c \
                   xr_pinch.c \
                   xr_palm.c \
                   xr_notice.c \
                   xr_glow.c \
                   xr_grade.c \
                   xr_videocolour.c \
                   xr_keys.c \
                   xr_controller.c \
                   xr_headaim.c \
                   xr_gamepad.c \
                   xr_shaders.c \
                   xr_session.c \
                   xr_display.c \
                   xr_gl.c \
                   xr_depth.c \
                   xr_ambilight.c \
                   xr_room.c \
                   xr_model.c \
                   xr_input.c \
                   xr_ui.c \
                   xr_assets.c \
                   xr_debug.c \
                   xr_layers.c
# Export only JNI entry points. -O2 even in debug builds: the per-texel depth
# loops are about 3x slower at -O0.
LOCAL_CFLAGS := -Wall -Werror -fvisibility=hidden -O2
# setprop tuning properties and frame capture, debug builds only
ifeq ($(XR_DEBUG_KNOBS),1)
LOCAL_CFLAGS += -DXR_DEBUG_KNOBS=1
endif
LOCAL_LDLIBS := -lEGL -lGLESv3 -landroid -llog
LOCAL_SHARED_LIBRARIES := openxr_loader

include $(BUILD_SHARED_LIBRARY)

$(call import-module,prefab/OpenXR)
