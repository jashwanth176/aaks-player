// Layout geometry, see xr_layout.h
#include <string.h>

#include "xr_layout.h"
#include "xr_shared.h"

int hoverTest(float u, float v, float width, float height, float cornerSide, int* corner) {
    if (cornerSide > 0.0f) {
        // Centred on the bracket, which sits half a bracket outside the corner
        float reachM = cornerSide * CORNER_HOVER * 0.5f;
        float cu = reachM / width;
        float cv = reachM / height;
        float outU = cornerSide * 0.5f / width;
        float outV = cornerSide * 0.5f / height;

        int left = fabsf(u + outU) < cu;
        int right = fabsf(u - (1.0f + outU)) < cu;
        int top = fabsf(v + outV) < cv;
        int bottom = fabsf(v - (1.0f + outV)) < cv;
        if ((left || right) && (top || bottom)) {
            *corner = (top ? 0 : 2) + (right ? 1 : 0);
            return HOVER_CORNER;
        }
    }

    if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
        return HOVER_SCREEN;
    }

    // The move bar is below the bottom edge (v > 1)
    float barU = BAR_WIDTH_FRAC * BAR_HOVER * 0.5f;
    float reach = (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 3.0f) * width / height;
    if (v > 1.0f && v < 1.0f + reach && fabsf(u - 0.5f) < barU) {
        return HOVER_BAR;
    }

    // Keep the ray drawn in a margin around the picture so it does not
    // flicker while moving to the handles
    if (u > -HALO_FRAC && u < 1.0f + HALO_FRAC && v > -HALO_FRAC && v < 1.0f + HALO_FRAC) {
        return HOVER_HALO;
    }

    return HOVER_NONE;
}

// Default screen pose: straight ahead at eye level, facing the seat
XrPosef standInPose(void) {
    XrPosef pose;
    memset(&pose, 0, sizeof(pose));
    pose.orientation.w = 1.0f;
    pose.position.z = -STAND_IN_DISTANCE_M;
    return pose;
}

// Distance from the seat to the bar centre on the stand-in screen
static float standInBarDistance(float aspect) {
    float drop = STAND_IN_WIDTH_M * (aspect * 0.5f + BAR_DROP_FRAC);
    return sqrtf(STAND_IN_DISTANCE_M * STAND_IN_DISTANCE_M + drop * drop);
}

// The bar's drop depends on the width, so iterate; it converges by ~20x per
// round, so four rounds suffice.
float barFrameWidth(Vec3 bottomMid, Vec3 down, float aspect) {
    float perMetre = STAND_IN_WIDTH_M / standInBarDistance(aspect);
    float width = perMetre * sqrtf(vecDot(bottomMid, bottomMid));
    for (int i = 0; i < 4; i++) {
        float drop = BAR_DROP_FRAC * width;
        Vec3 bar = { bottomMid.x + down.x * drop, bottomMid.y + down.y * drop,
                     bottomMid.z + down.z * drop };
        width = perMetre * sqrtf(vecDot(bar, bar));
    }
    return width;
}

BarFrame roomBarFrame(XrPosef picture, float pictureHeight, float aspect) {
    Vec3 downLocal = { 0.0f, -1.0f, 0.0f };
    Vec3 down = quatRotate(picture.orientation, downLocal);
    float half = pictureHeight * 0.5f;
    Vec3 bottomMid = { picture.position.x + down.x * half, picture.position.y + down.y * half,
                       picture.position.z + down.z * half };

    BarFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.width = barFrameWidth(bottomMid, down, aspect);
    frame.height = frame.width * aspect;
    // Align the frame's bottom edge with the picture's
    float up = frame.height * 0.5f;
    frame.pose.orientation = picture.orientation;
    frame.pose.position.x = bottomMid.x - down.x * up;
    frame.pose.position.y = bottomMid.y - down.y * up;
    frame.pose.position.z = bottomMid.z - down.z * up;
    return frame;
}

// Slots go outward from the bar pill: picker and cog, exit and keyboard,
// gamepad and ray, head aim and 3D
void barSlotPlacement(int slot, float width, float height, Vec3* outLocal, float* outSide) {
    static const int OUT[BAR_SLOTS] = { 0, 0, 1, 1, 2, 3, 2, 3 };
    static const int LEFT[BAR_SLOTS] = { 1, 0, 0, 1, 1, 1, 0, 0 };
    if (slot < 0 || slot >= BAR_SLOTS) {
        slot = BAR_SLOT_ENV;
    }
    float side = width * (slot == BAR_SLOT_ENV ? ENV_BUTTON_FRAC : COG_BUTTON_FRAC);
    float gap = width * ENV_GAP_FRAC;
    float x = width * BAR_WIDTH_FRAC * 0.5f + gap + side * 0.5f + OUT[slot] * (side + gap);
    outLocal->x = LEFT[slot] ? -x : x;
    outLocal->y = -(height * 0.5f + width * BAR_DROP_FRAC);
    outLocal->z = 0.005f;
    *outSide = side;
}

int barSlotHit(int slot, float u, float v, float width, float height) {
    Vec3 local;
    float side;
    barSlotPlacement(slot, width, height, &local, &side);
    float cu = 0.5f + local.x / width;
    float cv = 0.5f - local.y / height;
    float halfU = side * HOVER_MARGIN * 0.5f / width;
    float halfV = side * HOVER_MARGIN * 0.5f / height;
    return fabsf(u - cu) < halfU && fabsf(v - cv) < halfV;
}

_Static_assert(BTN_CELLS <= BTN_ATLAS_COLS * BTN_ATLAS_ROWS, "a cell for every button face");

int buttonCellOrigin(int cell, int* outX, int* outY) {
    if (cell < 0 || cell >= BTN_CELLS) {
        *outX = 0;
        *outY = 0;
        return 0;
    }
    *outX = (cell % BTN_ATLAS_COLS) * BUTTON_TEX;
    *outY = (cell / BTN_ATLAS_COLS) * BUTTON_TEX;
    return 1;
}

int buttonCellPut(unsigned char* atlas, int cell, const unsigned char* face) {
    int x, y;
    if (atlas == NULL || face == NULL || !buttonCellOrigin(cell, &x, &y)) {
        return 0;
    }
    const size_t row = (size_t)BUTTON_TEX * 4;
    for (int r = 0; r < BUTTON_TEX; r++) {
        unsigned char* dst = atlas + ((size_t)(y + BUTTON_TEX - 1 - r) * BTN_ATLAS_W + x) * 4;
        memcpy(dst, face + row * r, row);
    }
    return 1;
}

// Snapped to whole units and clamped, so the thumb shows the stored value
int laneUnits(float t, int min, int max) {
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    return min + (int)roundf(t * (float)(max - min));
}

float lanePlace(int units, int min, int max) {
    if (max <= min) {
        return 0.0f;
    }
    float t = (float)(units - min) / (float)(max - min);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// Percent of the track, not of the value (avoids readouts over 100%)
int lanePercent(int units, int min, int max) {
    return (int)roundf(lanePlace(units, min, max) * 100.0f);
}

// Rounded so float round trips are exact. Not clamped: the debug property
// can exceed the track.
int separationUnits(float separation) {
    return (int)roundf(separation * 1000.0f);
}

float separationOf(int units) {
    return units * 0.001f;
}

int cogPresetAt(int units, const int presets[COG_PRESET_CELLS]) {
    static const int order[COG_PRESET_CELLS] = {
        COG_PRESET_BALANCED, COG_PRESET_COMFORT, COG_PRESET_STRONG
    };
    for (int i = 0; i < COG_PRESET_CELLS; i++) {
        if (presets[order[i]] == units) {
            return order[i];
        }
    }
    return -1;
}

int roomScreenClamp(int percent, int resizable) {
    if (!resizable || percent > ROOM_SCREEN_MAX) {
        return ROOM_SCREEN_MAX;
    }
    return percent < ROOM_SCREEN_MIN ? ROOM_SCREEN_MIN : percent;
}

// Whole percent, matching the size row. Reach is clamped to at least 0.05.
int roomResizePercent(int startPercent, float startReach, float reach) {
    if (startReach < 0.05f) {
        startReach = 0.05f;
    }
    if (reach < 0.05f) {
        reach = 0.05f;
    }
    float wanted = (float)startPercent * reach / startReach;
    if (wanted < (float)ROOM_SCREEN_MIN) {
        return ROOM_SCREEN_MIN;
    }
    if (wanted > (float)ROOM_SCREEN_MAX) {
        return ROOM_SCREEN_MAX;
    }
    return (int)roundf(wanted);
}

float roomCornerSide(float distance) {
    return CORNER_FRAC * STAND_IN_WIDTH_M * distance / STAND_IN_DISTANCE_M;
}

float cogRowV(int tab, int row) {
    if (tab == COG_TAB_DISPLAY) {
        return COG_DISPLAY_ROW_V0 + row * COG_DISPLAY_ROW_STEP;
    }
    if (tab == COG_TAB_SCREEN) {
        return COG_SCREEN_ROW_V0 + row * COG_SCREEN_ROW_STEP;
    }
    return COG_ROW_V0 + row * COG_ROW_STEP;
}

float cogRowHalf(int tab) {
    return tab == COG_TAB_DISPLAY ? COG_DISPLAY_ROW_HALF
            : tab == COG_TAB_SCREEN ? COG_SCREEN_ROW_HALF : COG_ROW_HALF;
}

float cogCellHalf(int tab) {
    return tab == COG_TAB_DISPLAY ? COG_DISPLAY_CELL_HALF
            : tab == COG_TAB_SCREEN ? COG_SCREEN_CELL_HALF : COG_CELL_HALF;
}

// Screen tab rows are tightly spaced, so its thumbs are smaller
float cogThumbSize(int tab) {
    return tab == COG_TAB_SCREEN ? 0.075f : 0.085f;
}

int cogTrackPart(float pu) {
    if (pu >= COG_TRACK_L - COG_CHEVRON_REACH && pu <= COG_TRACK_L + COG_CHEVRON_W) {
        return TRACK_PART_DOWN;
    }
    if (pu >= COG_TRACK_R - COG_CHEVRON_W && pu <= COG_TRACK_R + COG_CHEVRON_REACH) {
        return TRACK_PART_UP;
    }
    if (pu > COG_TRACK_L + COG_CHEVRON_W && pu < COG_TRACK_R - COG_CHEVRON_W) {
        return TRACK_PART_RUN;
    }
    return TRACK_PART_NONE;
}

float cogRunPlace(float pu) {
    float t = (pu - COG_RUN_L) / (COG_RUN_R - COG_RUN_L);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

float cogRunU(float t) {
    return COG_RUN_L + t * (COG_RUN_R - COG_RUN_L);
}

// 0.001 step tolerance for float positions that are nearly on a step
int cogStepIndex(float t, int steps, int dir) {
    if (steps <= 0) {
        return 0;
    }
    float at = t * (float)steps;
    int step = dir > 0 ? (int)floorf(at + 0.001f) + 1 : (int)ceilf(at - 0.001f) - 1;
    return step < 0 ? 0 : (step > steps ? steps : step);
}

int cogTrackSteps(int tab, int row) {
    if (tab == COG_TAB_SCREEN) {
        // Distance 0.1 m, height 5 cm, tilt 2.5 deg, roll 6 deg (both clear of
        // the level snap), curve 1/20, width 0.1 m, then head aim in its units
        static const int SCREEN_STEPS[COG_SCREEN_ROW_COUNT] = {
            78, 80, 32, 30, 20, 72,
            HEAD_AIM_SENSITIVITY_MAX - HEAD_AIM_SENSITIVITY_MIN,
            HEAD_AIM_DEADZONE_MAX - HEAD_AIM_DEADZONE_MIN
        };
        return row >= 0 && row < COG_SCREEN_ROW_COUNT ? SCREEN_STEPS[row] : 0;
    }
    if (tab == COG_TAB_DISPLAY) {
        // Glow level in 5% steps
        return row == COG_DISPLAY_SLIDER_ROW ? 20 : 0;
    }
    if (tab == COG_TAB_3D) {
        // Separation in 0.1% units, convergence in whole percent
        return row == COG_ROW3D_SEPARATION ? COG_SEP_STEPS
                : row == COG_ROW3D_CONVERGENCE ? 100 : 0;
    }
    if (tab == COG_TAB_ABOUT) {
        // Buttons only
        return 0;
    }
    if (tab == COG_TAB_PICTURE) {
        // Stored integer units
        static const int PICTURE_STEPS[PICTURE_VALUES] = {
            PICTURE_BRIGHTNESS_MAX - PICTURE_BRIGHTNESS_MIN,
            PICTURE_CONTRAST_MAX - PICTURE_CONTRAST_MIN,
            PICTURE_GAMMA_MAX - PICTURE_GAMMA_MIN,
            PICTURE_SATURATION_MAX - PICTURE_SATURATION_MIN
        };
        return row >= 0 && row < PICTURE_VALUES ? PICTURE_STEPS[row] : 0;
    }
    // Room tab: lanes in steps of 5, size in whole percent
    if (row == COG_ROOM_ROW_BRIGHTNESS) {
        return (ROOM_BRIGHTNESS_MAX - ROOM_BRIGHTNESS_MIN) / 5;
    }
    if (row == COG_ROOM_ROW_LIGHT_LEVEL) {
        return (ROOM_LIGHT_MAX - ROOM_LIGHT_MIN) / 5;
    }
    if (row == COG_ROOM_ROW_SIZE) {
        return ROOM_SCREEN_MAX - ROOM_SCREEN_MIN;
    }
    return 0;
}

int cogReportButtonAt(float pu, float pv) {
    return pu >= COG_REPORT_L && pu <= COG_REPORT_R && pv >= COG_REPORT_T && pv <= COG_REPORT_B;
}

// Stub: always 0
int cogKofiButtonAt(float pu, float pv) {
    (void)pu;
    (void)pv;
    return 0;
}

// Two stacked fields, then two buttons side by side below them
int reportZone(float u, float v) {
    if (v >= REPORT_BTN_T && v <= REPORT_BTN_B) {
        if (u >= REPORT_CANCEL_L && u <= REPORT_CANCEL_R) {
            return REPORT_ZONE_CANCEL;
        }
        if (u >= REPORT_SEND_L && u <= REPORT_SEND_R) {
            return REPORT_ZONE_SEND;
        }
        return REPORT_ZONE_NONE;
    }
    if (u < REPORT_FIELD_L || u > REPORT_FIELD_R) {
        return REPORT_ZONE_NONE;
    }
    if (v >= REPORT_NOTE_T && v <= REPORT_NOTE_B) {
        return REPORT_ZONE_NOTE;
    }
    if (v >= REPORT_EMAIL_T && v <= REPORT_EMAIL_B) {
        return REPORT_ZONE_EMAIL;
    }
    return REPORT_ZONE_NONE;
}

int kofiSheetZone(float u, float v) {
    return u >= KOFI_CLOSE_L && u <= KOFI_CLOSE_R && v >= KOFI_BTN_T && v <= KOFI_BTN_B
            ? KOFI_ZONE_CLOSE : KOFI_ZONE_NONE;
}

// Two buttons side by side along the bottom
int handHintZone(float u, float v) {
    if (v < HINT_BTN_T || v > HINT_BTN_B) {
        return HINT_ZONE_NONE;
    }
    if (u >= HINT_OK_L && u <= HINT_OK_R) {
        return HINT_ZONE_OK;
    }
    if (u >= HINT_NEVER_L && u <= HINT_NEVER_R) {
        return HINT_ZONE_NEVER;
    }
    return HINT_ZONE_NONE;
}
