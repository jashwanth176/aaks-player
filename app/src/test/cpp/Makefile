# Desktop tests for the renderer modules that have no GL, OpenXR or Android dependency.
#
#     make -C app/src/test/cpp test

CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Werror -O1 -g
SRC := ../../main/jni/xr-renderer
INCLUDES := -Istubs -I$(SRC)
OUT := build

TESTS := $(OUT)/test_math $(OUT)/test_depthmap $(OUT)/test_atlas $(OUT)/test_roommesh \
         $(OUT)/test_layout $(OUT)/test_rate $(OUT)/test_gate \
         $(OUT)/test_pinch $(OUT)/test_notice $(OUT)/test_glow $(OUT)/test_grade \
         $(OUT)/test_keys $(OUT)/test_controller $(OUT)/test_headaim $(OUT)/test_gamepad \
         $(OUT)/test_videocolour $(OUT)/test_palm

.PHONY: test clean

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

$(OUT)/test_math: test_math.c check.h $(SRC)/xr_math.c $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_math.c $(SRC)/xr_math.c -lm

$(OUT)/test_depthmap: test_depthmap.c check.h $(SRC)/xr_depthmap.c $(SRC)/xr_depthmap.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_depthmap.c $(SRC)/xr_depthmap.c -lm

# Also reads the shipped atlases, relative to this directory.
$(OUT)/test_atlas: test_atlas.c check.h $(SRC)/xr_atlas.c $(SRC)/xr_atlas.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_atlas.c $(SRC)/xr_atlas.c -lm

# Also reads the shipped room meshes, relative to this directory.
$(OUT)/test_roommesh: test_roommesh.c check.h $(SRC)/xr_roommesh.c $(SRC)/xr_roommesh.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_roommesh.c $(SRC)/xr_roommesh.c -lm

# Panel layout, hover zones and hit testing.
$(OUT)/test_layout: test_layout.c check.h $(SRC)/xr_layout.c $(SRC)/xr_layout.h $(SRC)/xr_math.c \
                    $(SRC)/xr_math.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_layout.c $(SRC)/xr_layout.c $(SRC)/xr_math.c -lm

# Refresh rate choice, frame budget, depth rate governor and capture gate.
$(OUT)/test_rate: test_rate.c check.h $(SRC)/xr_rate.c $(SRC)/xr_rate.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_rate.c $(SRC)/xr_rate.c -lm

# Pointer ownership, controller clocks and the gaze bridge.
$(OUT)/test_gate: test_gate.c check.h $(SRC)/xr_gate.c $(SRC)/xr_gate.h $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_gate.c $(SRC)/xr_gate.c -lm

# Hand pinch origin and press, triple-pinch lock, and gaze drag.
$(OUT)/test_pinch: test_pinch.c check.h $(SRC)/xr_pinch.c $(SRC)/xr_pinch.h $(SRC)/xr_math.c \
                   $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_pinch.c $(SRC)/xr_pinch.c $(SRC)/xr_math.c -lm

# Panel fades and splash timing.
$(OUT)/test_notice: test_notice.c check.h $(SRC)/xr_notice.c $(SRC)/xr_notice.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_notice.c $(SRC)/xr_notice.c -lm

# Ambient glow luma, edge roll-off and the edge pass.
$(OUT)/test_glow: test_glow.c check.h $(SRC)/xr_glow.c $(SRC)/xr_glow.h $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_glow.c $(SRC)/xr_glow.c -lm

# Picture grade.
$(OUT)/test_grade: test_grade.c check.h $(SRC)/xr_grade.c $(SRC)/xr_grade.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_grade.c $(SRC)/xr_grade.c -lm

# HDR-to-SDR curves and tone map; rebuilt video shaders checked against the shipped ones.
$(OUT)/test_videocolour: test_videocolour.c check.h $(SRC)/xr_videocolour.c $(SRC)/xr_videocolour.h \
                         $(SRC)/xr_shaders.c $(SRC)/xr_shaders.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_videocolour.c $(SRC)/xr_videocolour.c $(SRC)/xr_shaders.c -lm

# Keyboard sheets and held modifiers.
$(OUT)/test_keys: test_keys.c check.h $(SRC)/xr_keys.c $(SRC)/xr_keys.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_keys.c $(SRC)/xr_keys.c -lm

# Ray and controller model visibility and placement.
$(OUT)/test_controller: test_controller.c check.h $(SRC)/xr_controller.c $(SRC)/xr_controller.h \
                        $(SRC)/xr_math.c $(SRC)/xr_math.h $(SRC)/xr_gate.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_controller.c $(SRC)/xr_controller.c $(SRC)/xr_math.c -lm

# Head-aim and controller pointer mouse movement.
$(OUT)/test_headaim: test_headaim.c check.h $(SRC)/xr_headaim.c $(SRC)/xr_headaim.h $(SRC)/xr_shared.h \
                     $(SRC)/xr_math.c $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_headaim.c $(SRC)/xr_headaim.c $(SRC)/xr_math.c -lm

# Controllers as a gamepad: mapping, dead zones, and pad/pointer switching.
$(OUT)/test_gamepad: test_gamepad.c check.h $(SRC)/xr_gamepad.c $(SRC)/xr_gamepad.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_gamepad.c $(SRC)/xr_gamepad.c -lm

clean:
	rm -rf $(OUT)

# Open palm that wakes a sleeping hand pointer.
$(OUT)/test_palm: test_palm.c check.h $(SRC)/xr_palm.c $(SRC)/xr_palm.h $(SRC)/xr_math.c \
                  $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_palm.c $(SRC)/xr_palm.c $(SRC)/xr_math.c -lm
