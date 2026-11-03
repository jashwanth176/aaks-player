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

float separationOf(int units) {
    return units * 0.001f;
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
