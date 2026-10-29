// Tests for xr_layout: hover zones, the stand-in screen, the bar and its buttons, and
// the settings panel's rows, tracks and sheets.
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "xr_layout.h"
#include "xr_shared.h"

// Default screen: 3 m wide, 16:9.
static const float W = 3.0f;
static const float H = 3.0f * 9.0f / 16.0f;

static void testCornersFollowTheirArt(void) {
    float side = CORNER_FRAC * W;
    // Brackets are centred half a bracket outside each corner; so are their zones.
    float outU = side * 0.5f / W;
    float outV = side * 0.5f / H;
    int corner = -1;
    CHECK(hoverTest(-outU, -outV, W, H, side, &corner) == HOVER_CORNER && corner == 0);
    CHECK(hoverTest(1.0f + outU, -outV, W, H, side, &corner) == HOVER_CORNER && corner == 1);
    CHECK(hoverTest(-outU, 1.0f + outV, W, H, side, &corner) == HOVER_CORNER && corner == 2);
    CHECK(hoverTest(1.0f + outU, 1.0f + outV, W, H, side, &corner) == HOVER_CORNER
          && corner == 3);

    // The corner itself and just inside it are in the zone.
    CHECK(hoverTest(0.0f, 0.0f, W, H, side, &corner) == HOVER_CORNER && corner == 0);
    CHECK(hoverTest(0.01f, 0.01f, W, H, side, &corner) == HOVER_CORNER);

    // Zone half-width is CORNER_HOVER * side / 2.
    float reachU = side * CORNER_HOVER * 0.5f / W;
    CHECK(hoverTest(-outU - reachU * 0.98f, -outV, W, H, side, &corner) == HOVER_CORNER);
    CHECK(hoverTest(-outU - reachU * 1.02f, -outV, W, H, side, &corner) != HOVER_CORNER);
    CHECK(hoverTest(-outU + reachU * 1.02f, -outV, W, H, side, &corner) != HOVER_CORNER);

    CHECK(hoverTest(0.5f, 0.0f, W, H, side, &corner) == HOVER_SCREEN);
    CHECK(hoverTest(0.5f, -outV, W, H, side, &corner) == HOVER_HALO);
}

static void testNoCornersWhereThereAreNone(void) {
    // side == 0: no brackets.
    int corner = -1;
    float side = CORNER_FRAC * W;
    float outU = side * 0.5f / W;
    float outV = side * 0.5f / H;
    CHECK(hoverTest(-outU, -outV, W, H, 0.0f, &corner) == HOVER_HALO);
    CHECK(hoverTest(0.0f, 0.0f, W, H, 0.0f, &corner) == HOVER_SCREEN);
    CHECK(hoverTest(1.0f + outU, 1.0f + outV, W, H, 0.0f, &corner) == HOVER_HALO);
    CHECK(corner == -1);
}

static void testTheRestOfThePicture(void) {
    int corner = -1;
    float side = CORNER_FRAC * W;
    CHECK(hoverTest(0.5f, 0.5f, W, H, side, &corner) == HOVER_SCREEN);
    CHECK(hoverTest(0.5f, 1.05f, W, H, side, &corner) == HOVER_BAR);
    CHECK(hoverTest(0.5f, 2.0f, W, H, side, &corner) == HOVER_NONE);
    CHECK(hoverTest(-0.6f, 0.5f, W, H, side, &corner) == HOVER_NONE);
}

static void testTheStandIn(void) {
    XrPosef pose = standInPose();
    CHECK_NEAR(pose.position.x, 0.0f, 1e-6);
    CHECK_NEAR(pose.position.y, 0.0f, 1e-6);
    CHECK_NEAR(pose.position.z, -STAND_IN_DISTANCE_M, 1e-6);
    CHECK_NEAR(pose.orientation.w, 1.0f, 1e-6);
    CHECK_NEAR(STAND_IN_WIDTH_M, 3.0f, 1e-6);

    // Rays from the seat hit the centre and the bar zone where they are drawn.
    XrPosef aim;
    aim.position.x = 0.0f;
    aim.position.y = 0.0f;
    aim.position.z = 0.0f;
    aim.orientation.x = 0.0f;
    aim.orientation.y = 0.0f;
    aim.orientation.z = 0.0f;
    aim.orientation.w = 1.0f;
    float u, v;
    CHECK(screenProject(aim, pose, STAND_IN_WIDTH_M, H, 0.0f, 0, &u, &v));
    CHECK_NEAR(u, 0.5f, 1e-5);
    CHECK_NEAR(v, 0.5f, 1e-5);

    float barY = -(H * 0.5f + STAND_IN_WIDTH_M * (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 0.5f));
    float pitch = atan2f(barY, STAND_IN_DISTANCE_M);
    aim.orientation.x = sinf(pitch * 0.5f);
    aim.orientation.w = cosf(pitch * 0.5f);
    CHECK(screenProject(aim, pose, STAND_IN_WIDTH_M, H, 0.0f, 0, &u, &v));
    int corner;
    CHECK(hoverTest(u, v, STAND_IN_WIDTH_M, H, 0.0f, &corner) == HOVER_BAR);
}

static XrPosef poseAt(float x, float y, float z) {
    XrPosef pose;
    pose.orientation.x = 0.0f;
    pose.orientation.y = 0.0f;
    pose.orientation.z = 0.0f;
    pose.orientation.w = 1.0f;
    pose.position.x = x;
    pose.position.y = y;
    pose.position.z = z;
    return pose;
}

static Vec3 posOf(XrPosef pose) {
    Vec3 p = { pose.position.x, pose.position.y, pose.position.z };
    return p;
}

static float lengthOf(Vec3 v) {
    return sqrtf(vecDot(v, v));
}

// Frame-local point to world space.
static Vec3 framePoint(const BarFrame* frame, Vec3 local) {
    Vec3 r = quatRotate(frame->pose.orientation, local);
    Vec3 p = { frame->pose.position.x + r.x, frame->pose.position.y + r.y,
               frame->pose.position.z + r.z };
    return p;
}

// Centre of the move bar (and of the button row).
static Vec3 barMiddle(const BarFrame* frame) {
    Vec3 local = { 0.0f, -(frame->height * 0.5f + frame->width * BAR_DROP_FRAC), 0.0f };
    return framePoint(frame, local);
}

// Aim pose from the origin at a point.
static XrPosef aimAt(Vec3 target) {
    Vec3 dir = vecNorm(target);
    Vec3 fwd = { 0.0f, 0.0f, -1.0f };
    Vec3 axis = vecCross(fwd, dir);
    float s = lengthOf(axis);
    float angle = atan2f(s, vecDot(fwd, dir));
    XrPosef aim = poseAt(0.0f, 0.0f, 0.0f);
    if (s > 1e-6f) {
        aim.orientation = axisAngleQuat(vecNorm(axis), angle);
    }
    return aim;
}

// Angular size of the bar from the seat.
static float barLooks(const BarFrame* frame) {
    return frame->width * BAR_WIDTH_FRAC / lengthOf(barMiddle(frame));
}

// Aiming at each button hits it and no other.
static void checkButtonsHitWhereTheyHang(const BarFrame* frame) {
    for (int slot = 0; slot < BAR_SLOTS; slot++) {
        Vec3 local;
        float side;
        barSlotPlacement(slot, frame->width, frame->height, &local, &side);
        float yaw = 0.0f;
        curveLocal(&local, frame->radius, frame->curved, &yaw);
        float u, v;
        CHECK(screenProject(aimAt(framePoint(frame, local)), frame->pose, frame->width,
                            frame->height, frame->radius, frame->curved, &u, &v));
        for (int other = 0; other < BAR_SLOTS; other++) {
            CHECK(barSlotHit(other, u, v, frame->width, frame->height) == (other == slot));
        }
    }
    float u, v;
    int corner;
    CHECK(screenProject(aimAt(barMiddle(frame)), frame->pose, frame->width, frame->height,
                        frame->radius, frame->curved, &u, &v));
    CHECK(hoverTest(u, v, frame->width, frame->height, 0.0f, &corner) == HOVER_BAR);
}

static void testTheBarUnderARoomsPicture(void) {
    const float aspect = 9.0f / 16.0f;
    // A room picture matching the stand-in gives the same frame.
    BarFrame same = roomBarFrame(standInPose(), STAND_IN_WIDTH_M * aspect, aspect);
    CHECK_NEAR(same.width, STAND_IN_WIDTH_M, 1e-4);
    CHECK_NEAR(same.height, STAND_IN_WIDTH_M * aspect, 1e-4);
    CHECK_NEAR(same.pose.position.y, 0.0f, 1e-5);
    CHECK_NEAR(same.pose.position.z, -STAND_IN_DISTANCE_M, 1e-6);
    CHECK(!same.curved);
    float standInLooks = barLooks(&same);

    // Picture placements at ~4 m and ~14 m, at several sizes.
    struct { float x, y, z, width; } rooms[] = {
        { 0.0f, 0.05f, -4.152f, 3.6f },
        { 0.0f, 0.05f, -4.152f, 1.8f },
        { 0.0f, 0.05f, -4.152f, 2.88f },
        { 0.0f, 2.95f, -13.9f, 3.5f },
        { 0.0f, 2.95f, -13.9f, 14.0f },
    };
    float lastBarY = 1e9f;
    for (int i = 0; i < (int)(sizeof(rooms) / sizeof(rooms[0])); i++) {
        float pictureH = rooms[i].width * aspect;
        XrPosef picture = poseAt(rooms[i].x, rooms[i].y, rooms[i].z);
        BarFrame frame = roomBarFrame(picture, pictureH, aspect);
        // Flat, in the picture's plane, bottom edges aligned.
        CHECK(!frame.curved);
        CHECK_NEAR(frame.pose.position.z, rooms[i].z, 1e-5);
        CHECK_NEAR(frame.pose.position.x, rooms[i].x, 1e-5);
        CHECK_NEAR(frame.pose.position.y - frame.height * 0.5f,
                   rooms[i].y - pictureH * 0.5f, 1e-4);
        CHECK_NEAR(frame.height, frame.width * aspect, 1e-5);
        // Same angular size as on the stand-in.
        CHECK_NEAR(barLooks(&frame) / standInLooks, 1.0f, 1e-3);
        // Gap below the picture scales with distance, not picture size.
        Vec3 bar = barMiddle(&frame);
        float bottom = rooms[i].y - pictureH * 0.5f;
        CHECK(bar.y < bottom);
        CHECK_NEAR(bottom - bar.y, frame.width * BAR_DROP_FRAC, 1e-4);
        checkButtonsHitWhereTheyHang(&frame);
        // Entries 0 and 1 differ only in size: the smaller one raises the bar.
        if (i == 1) {
            CHECK(bar.y > lastBarY + 0.4f);
        }
        lastBarY = bar.y;
    }

    // Scales with distance.
    BarFrame near = roomBarFrame(poseAt(0.0f, 0.0f, -4.0f), 1.0f, aspect);
    BarFrame far = roomBarFrame(poseAt(0.0f, 0.0f, -16.0f), 1.0f, aspect);
    CHECK(far.width > 3.5f * near.width && far.width < 4.5f * near.width);
    CHECK_NEAR(barLooks(&near), barLooks(&far), 1e-4);
}

// Rotated picture: frame in its plane, bottom edges aligned.
static void testTheBarUnderATurnedPicture(void) {
    const float aspect = 9.0f / 16.0f;
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    XrPosef picture = poseAt(-2.0f, 0.6f, -5.0f);
    picture.orientation = quatMul(axisAngleQuat(up, 0.4f), axisAngleQuat(right, 0.2f));
    float pictureH = 2.0f * aspect;
    BarFrame frame = roomBarFrame(picture, pictureH, aspect);

    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 normal = quatRotate(picture.orientation, back);
    Vec3 offset = vecSub(posOf(frame.pose), posOf(picture));
    CHECK_NEAR(vecDot(offset, normal), 0.0f, 1e-5);
    CHECK_NEAR(frame.pose.orientation.x, picture.orientation.x, 1e-6);
    CHECK_NEAR(frame.pose.orientation.w, picture.orientation.w, 1e-6);

    Vec3 down = { 0.0f, -1.0f, 0.0f };
    Vec3 pictureDown = quatRotate(picture.orientation, down);
    Vec3 pictureEdge = { picture.position.x + pictureDown.x * pictureH * 0.5f,
                         picture.position.y + pictureDown.y * pictureH * 0.5f,
                         picture.position.z + pictureDown.z * pictureH * 0.5f };
    Vec3 frameEdgeLocal = { 0.0f, -frame.height * 0.5f, 0.0f };
    Vec3 frameEdge = framePoint(&frame, frameEdgeLocal);
    CHECK_NEAR(lengthOf(vecSub(frameEdge, pictureEdge)), 0.0f, 1e-4);
    checkButtonsHitWhereTheyHang(&frame);
}

// Outside a room the frame is the picture as drawn, including the cylinder clamp's
// smaller size.
static void testTheBarOnACurvedPicture(void) {
    const float aspect = 9.0f / 16.0f;
    // 3 m at 0.2 m radius is drawn at ~42%.
    float width = 3.0f;
    float radius = 0.2f;
    float fit = cylinderFit(width, radius);
    CHECK(fit < 0.5f && fit > 0.4f);
    BarFrame frame;
    frame.pose = poseAt(0.0f, 0.0f, -0.2f);
    frame.width = width * fit;
    frame.height = frame.width * aspect;
    frame.radius = radius;
    frame.curved = 1;
    checkButtonsHitWhereTheyHang(&frame);

    // Full curve at 2 m, no clamp: buttons follow the surface.
    frame.pose = poseAt(0.0f, 0.0f, -2.0f);
    frame.width = 3.0f;
    frame.height = frame.width * aspect;
    frame.radius = 2.0f;
    checkButtonsHitWhereTheyHang(&frame);
    // The outermost button on the surface hits its zone centre; placed flat it would not.
    Vec3 flat;
    float side;
    barSlotPlacement(BAR_SLOT_STEREO, frame.width, frame.height, &flat, &side);
    float cu = 0.5f + flat.x / frame.width;
    float cv = 0.5f - flat.y / frame.height;
    float halfU = side * HOVER_MARGIN * 0.5f / frame.width;
    float halfV = side * HOVER_MARGIN * 0.5f / frame.height;
    Vec3 curved = flat;
    curveLocal(&curved, frame.radius, 1, NULL);
    float u, v;
    CHECK(screenProject(aimAt(framePoint(&frame, curved)), frame.pose, frame.width,
                        frame.height, frame.radius, 1, &u, &v));
    // Tolerance covers the 5 mm the button stands off the surface.
    CHECK_NEAR(u, cu, 3e-3);
    CHECK_NEAR(v, cv, 3e-3);
    CHECK(screenProject(aimAt(framePoint(&frame, flat)), frame.pose, frame.width, frame.height,
                        frame.radius, 1, &u, &v));
    CHECK(fabsf(u - cu) > 0.3f * halfU && fabsf(v - cv) > 0.5f * halfV);

    frame.radius = 0.0f;
    frame.curved = 0;
    checkButtonsHitWhereTheyHang(&frame);
}

// Button row: symmetric about the pill, outward in order, one line, no overlap.
static void testTheRowsOrder(void) {
    const float w = 3.0f, h = w * 9.0f / 16.0f;
    static const int LEFT_OUT[4] = { BAR_SLOT_ENV, BAR_SLOT_EXIT, BAR_SLOT_PAD, BAR_SLOT_AIM };
    static const int RIGHT_OUT[4] = { BAR_SLOT_COG, BAR_SLOT_KB, BAR_SLOT_RAY, BAR_SLOT_STEREO };
    float lastL = -w * BAR_WIDTH_FRAC * 0.5f, lastR = w * BAR_WIDTH_FRAC * 0.5f;
    for (int i = 0; i < 4; i++) {
        Vec3 l, r;
        float sideL, sideR;
        barSlotPlacement(LEFT_OUT[i], w, h, &l, &sideL);
        barSlotPlacement(RIGHT_OUT[i], w, h, &r, &sideR);
        CHECK_NEAR(l.x, -r.x, 1e-6);
        CHECK_NEAR(l.y, -(h * 0.5f + w * BAR_DROP_FRAC), 1e-6);
        CHECK_NEAR(r.y, l.y, 1e-6);
        CHECK(l.x + sideL * 0.5f < lastL && r.x - sideR * 0.5f > lastR);
        lastL = l.x - sideL * 0.5f;
        lastR = r.x + sideR * 0.5f;
    }
    // All clear of the frame's bottom edge.
    for (int slot = 0; slot < BAR_SLOTS; slot++) {
        Vec3 p;
        float side;
        barSlotPlacement(slot, w, h, &p, &side);
        CHECK(p.y + side * HOVER_MARGIN * 0.5f < -h * 0.5f);
    }
}

static void testRoomCornersLookTheSameEverywhere(void) {
    // At the stand-in distance, same as on a 3 m screen.
    CHECK_NEAR(roomCornerSide(STAND_IN_DISTANCE_M), CORNER_FRAC * STAND_IN_WIDTH_M, 1e-6);
    // Constant angular size.
    CHECK_NEAR(roomCornerSide(14.0f), CORNER_FRAC * STAND_IN_WIDTH_M * 14.0f / 3.0f, 1e-5);
    CHECK_NEAR(roomCornerSide(4.15f) / 4.15f, roomCornerSide(14.0f) / 14.0f, 1e-6);
    CHECK_NEAR(roomCornerSide(0.0f), 0.0f, 1e-6);
}

static void testLanes(void) {
    CHECK(lanePercent(ROOM_THEATER_BRIGHTNESS, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == 20);
    CHECK(lanePercent(ROOM_GRAND_CINEMA_BRIGHTNESS, ROOM_BRIGHTNESS_MIN,
                      ROOM_BRIGHTNESS_MAX) == 10);
    CHECK(lanePercent(ROOM_SYNTHWAVE_BRIGHTNESS, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == 30);
    CHECK(lanePercent(ROOM_LIGHT_DEFAULT, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX) == 75);

    // Ends, and clamping past them.
    CHECK(laneUnits(0.0f, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == ROOM_BRIGHTNESS_MIN);
    CHECK(laneUnits(1.0f, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == ROOM_BRIGHTNESS_MAX);
    CHECK(laneUnits(-0.3f, ROOM_SCREEN_MIN, ROOM_SCREEN_MAX) == ROOM_SCREEN_MIN);
    CHECK(laneUnits(1.4f, ROOM_SCREEN_MIN, ROOM_SCREEN_MAX) == ROOM_SCREEN_MAX);
    CHECK_NEAR(lanePlace(ROOM_BRIGHTNESS_MIN - 3, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX),
               0.0f, 1e-6);
    CHECK_NEAR(lanePlace(ROOM_LIGHT_MAX + 30, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX), 1.0f, 1e-6);

    // units -> place -> units round-trips for every value.
    int lanes[3][2] = { { ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX },
                        { ROOM_LIGHT_MIN, ROOM_LIGHT_MAX },
                        { ROOM_SCREEN_MIN, ROOM_SCREEN_MAX } };
    int trips = 0;
    for (int lane = 0; lane < 3; lane++) {
        int min = lanes[lane][0];
        int max = lanes[lane][1];
        for (int units = min; units <= max; units++) {
            trips += laneUnits(lanePlace(units, min, max), min, max) == units;
        }
    }
    CHECK(trips == (ROOM_BRIGHTNESS_MAX - ROOM_BRIGHTNESS_MIN + 1)
                   + (ROOM_LIGHT_MAX - ROOM_LIGHT_MIN + 1)
                   + (ROOM_SCREEN_MAX - ROOM_SCREEN_MIN + 1));
}

static void testTheSizeClamp(void) {
    CHECK(roomScreenClamp(80, 1) == 80);
    CHECK(roomScreenClamp(10, 1) == ROOM_SCREEN_MIN);
    CHECK(roomScreenClamp(-40, 1) == ROOM_SCREEN_MIN);
    CHECK(roomScreenClamp(130, 1) == ROOM_SCREEN_MAX);
    // Not resizable: always full size.
    CHECK(roomScreenClamp(40, 0) == ROOM_SCREEN_MAX);
    CHECK(roomScreenClamp(ROOM_SCREEN_MIN, 0) == ROOM_SCREEN_MAX);
}

static void testTheCornerDrag(void) {
    // Scaled relative to the grab point, whole percent.
    CHECK(roomResizePercent(80, 1.0f, 1.0f) == 80);
    CHECK(roomResizePercent(80, 1.0f, 0.5f) == 40);
    CHECK(roomResizePercent(80, 1.0f, 1.1f) == 88);
    CHECK(roomResizePercent(60, 1.0f, 1.333f) == 80);
    // No jump on grab, however far out the bracket was grabbed.
    CHECK(roomResizePercent(25, 1.41f, 1.41f) == 25);
    CHECK(roomResizePercent(25, 2.03f, 2.03f) == 25);
    CHECK(roomResizePercent(40, 2.0f, 1.0f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(40, 2.0f, 3.0f) == 60);
    // Clamped to [ROOM_SCREEN_MIN, ROOM_SCREEN_MAX].
    CHECK(roomResizePercent(80, 1.0f, 0.1f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(100, 1.0f, 0.25f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(100, 1.0f, 0.2f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(80, 1.0f, 2.0f) == ROOM_SCREEN_MAX);
    CHECK(roomResizePercent(25, 1.0f, 4.0f) == ROOM_SCREEN_MAX);
    // Ray behind the centre, or a grab at the centre: no flip, no divide by zero.
    CHECK(roomResizePercent(80, 1.0f, -0.5f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(80, 0.0f, 0.05f) == 80);
    int inside = 1;
    for (int i = 1; i <= 400; i++) {
        int p = roomResizePercent(60, 1.2f, i * 0.01f);
        inside &= p >= ROOM_SCREEN_MIN && p <= ROOM_SCREEN_MAX;
    }
    CHECK(inside);
}

static void testTheDepthTrack(void) {
    // Units round-trip through the warp fraction and through the thumb place.
    int trips = 0;
    for (int units = 0; units <= COG_SEP_STEPS; units++) {
        trips += separationUnits(separationOf(units)) == units;
        trips += laneUnits(lanePlace(units, 0, COG_SEP_STEPS), 0, COG_SEP_STEPS) == units;
    }
    CHECK(trips == 2 * (COG_SEP_STEPS + 1));
    CHECK(separationUnits(COG_SEP_MAX) == COG_SEP_STEPS);
    CHECK_NEAR(separationOf(5), 0.005f, 1e-7);

    // Rounds to the nearest step, clamped.
    CHECK(laneUnits(0.39f, 0, COG_SEP_STEPS) == 6);
    CHECK(laneUnits(0.36f, 0, COG_SEP_STEPS) == 5);
    CHECK(laneUnits(-0.2f, 0, COG_SEP_STEPS) == 0);
    CHECK(laneUnits(1.3f, 0, COG_SEP_STEPS) == COG_SEP_STEPS);

    // Values past the track (from the debug property) are not clamped.
    CHECK(separationUnits(0.04f) == 40);
}

static void testThePresetAccent(void) {
    // ZipDepth and MiDaS preset steps.
    int zip[COG_PRESET_CELLS] = { 3, 6, 9 };
    int midas[COG_PRESET_CELLS] = { 2, 5, 8 };
    CHECK(cogPresetAt(3, zip) == COG_PRESET_COMFORT);
    CHECK(cogPresetAt(6, zip) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(9, zip) == COG_PRESET_STRONG);
    CHECK(cogPresetAt(2, midas) == COG_PRESET_COMFORT);
    CHECK(cogPresetAt(5, midas) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(8, midas) == COG_PRESET_STRONG);

    int elsewhere = 0;
    for (int units = 0; units <= COG_SEP_STEPS; units++) {
        if (units != 3 && units != 6 && units != 9) {
            elsewhere += cogPresetAt(units, zip) == -1;
        }
    }
    CHECK(elsewhere == COG_SEP_STEPS + 1 - 3);
    CHECK(cogPresetAt(40, zip) == -1);

    // Coinciding presets resolve to Balanced.
    int low[COG_PRESET_CELLS] = { 0, 0, 3 };
    int high[COG_PRESET_CELLS] = { 12, 15, 15 };
    CHECK(cogPresetAt(0, low) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(15, high) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(3, low) == COG_PRESET_STRONG);

    // Via the warp fraction, as the panel reads it.
    CHECK(cogPresetAt(separationUnits(separationOf(9)), zip) == COG_PRESET_STRONG);
    CHECK(cogPresetAt(separationUnits(0.0061f), zip) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(separationUnits(0.0068f), zip) == -1);
}

// Rows fit below the tab bar, hit bands do not overlap, cells stay in their band.
static void testTheRowsFit(void) {
    int counts[COG_TAB_COUNT] = {
        COG_SCREEN_ROW_COUNT, COG_DISPLAY_SLIDER_ROW + 1, COG_ROW3D_COUNT, PICTURE_VALUES, 0
    };
    for (int tab = 0; tab < COG_TAB_COUNT; tab++) {
        // About tab has no rows.
        if (counts[tab] == 0) {
            continue;
        }
        float half = cogRowHalf(tab);
        CHECK(cogCellHalf(tab) < half);
        CHECK(cogRowV(tab, 0) - half >= COG_TAB_BAR_B);
        for (int row = 1; row < counts[tab]; row++) {
            CHECK(cogRowV(tab, row) - half >= cogRowV(tab, row - 1) + half - 1e-6f);
        }
        float last = cogRowV(tab, counts[tab] - 1);
        // Thumb is 0.085 of the panel height.
        CHECK(last + 0.0425f < 1.0f - 0.03f);
        CHECK(last + half <= 1.0f);
    }
    // Display tab: 13 rows, slider last.
    CHECK(COG_DISPLAY_SLIDER_ROW == 12);
    CHECK(COG_OPTION_HEAD_AIM == COG_OPTION_HEAD_LOCK + 1);
    CHECK(COG_OPTION_GAMEPAD == COG_OPTION_HEAD_AIM + 1);
    CHECK(COG_OPTION_POINTER_SLEEP == COG_OPTION_GAMEPAD + 1);
    CHECK_NEAR(cogRowV(COG_TAB_DISPLAY, 0), 0.197, 1e-6);
    CHECK_NEAR(cogRowV(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW), 0.917, 1e-5);
    CHECK(cogRowV(COG_TAB_DISPLAY, 0) - cogRowHalf(COG_TAB_DISPLAY) >= COG_TAB_BAR_B);
    CHECK(2.0f * cogCellHalf(COG_TAB_DISPLAY) < COG_DISPLAY_ROW_STEP);
    // Screen tab: 8 rows, head aim's two last, clear of the reset button.
    CHECK(COG_SCREEN_ROW_COUNT == 8);
    CHECK(COG_SLIDER_AIM_SENSITIVITY == COG_SLIDER_COUNT);
    CHECK(COG_SLIDER_AIM_DEADZONE == COG_SLIDER_COUNT + 1);
    CHECK_NEAR(cogRowV(COG_TAB_SCREEN, 0), 0.22, 1e-6);
    CHECK_NEAR(cogRowV(COG_TAB_SCREEN, COG_SLIDER_AIM_DEADZONE), 0.815, 1e-5);
    CHECK(cogRowV(COG_TAB_SCREEN, COG_SCREEN_ROW_COUNT - 1) + cogRowHalf(COG_TAB_SCREEN)
          < COG_RESET_T);
    CHECK(2.0f * cogCellHalf(COG_TAB_SCREEN) < COG_SCREEN_ROW_STEP);
    // Screen tab thumbs are smaller than the row step; others are 0.085.
    CHECK(cogThumbSize(COG_TAB_SCREEN) < COG_SCREEN_ROW_STEP);
    CHECK_NEAR(cogThumbSize(COG_TAB_DISPLAY), 0.085, 1e-6);
    CHECK_NEAR(cogThumbSize(COG_TAB_3D), 0.085, 1e-6);
    CHECK_NEAR(cogThumbSize(COG_TAB_COUNT), 0.085, 1e-6);
    // Other tabs use the standard row spacing.
    CHECK_NEAR(cogRowV(COG_TAB_3D, 3), COG_ROW_V0 + 3 * COG_ROW_STEP, 1e-6);
    CHECK_NEAR(cogRowV(COG_TAB_COUNT, 2), COG_ROW_V0 + 2 * COG_ROW_STEP, 1e-6);
    CHECK_NEAR(cogCellHalf(COG_TAB_3D), COG_CELL_HALF, 1e-6);
    // Picture tab: fourth, clear of the reset button.
    CHECK(COG_TAB_PICTURE == 3 && COG_TAB_ABOUT == 4 && COG_TAB_COUNT == 5);
    CHECK_NEAR(cogRowV(COG_TAB_PICTURE, PICTURE_SATURATION),
               COG_ROW_V0 + PICTURE_SATURATION * COG_ROW_STEP, 1e-6);
    CHECK(cogRowV(COG_TAB_PICTURE, PICTURE_VALUES - 1) + cogRowHalf(COG_TAB_PICTURE)
          < COG_RESET_T);
    // Art indices: tabs, then room sheets (one per tab plus Room), then world-locked variants.
    CHECK(COG_ART_ROOM == COG_TAB_COUNT);
    CHECK(COG_ART_SCREEN_WORLD == COG_ART_ROOM + COG_TAB_COUNT + 1);
    CHECK(COG_ART_DISPLAY_WORLD == COG_ART_SCREEN_WORLD + 1);
    CHECK(COG_ART_COUNT == COG_ART_DISPLAY_WORLD + 1);
    // The readout strip covers every Picture and Room row and head aim's two rows.
    float stripB = COG_READOUT_T + (float)COG_READOUT_TEX_H / (float)COG_TEX_H;
    for (int row = 0; row < PICTURE_VALUES; row++) {
        CHECK(cogRowV(COG_TAB_PICTURE, row) - COG_CELL_HALF > COG_READOUT_T);
        CHECK(cogRowV(COG_TAB_PICTURE, row) + COG_CELL_HALF < stripB);
    }
    for (int row = 0; row < COG_ROOM_ROW_COUNT; row++) {
        CHECK(cogRowV(COG_TAB_COUNT, row) + COG_CELL_HALF < stripB);
    }
    for (int row = COG_SLIDER_AIM_SENSITIVITY; row <= COG_SLIDER_AIM_DEADZONE; row++) {
        CHECK(cogRowV(COG_TAB_SCREEN, row) - COG_SCREEN_CELL_HALF > COG_READOUT_T);
        CHECK(cogRowV(COG_TAB_SCREEN, row) + COG_SCREEN_CELL_HALF < stripB);
    }
    CHECK(stripB <= 1.0f);
}

// Track parts: step buttons at each end and the run between.
static void testTheTrackParts(void) {
    float mid = (COG_TRACK_L + COG_TRACK_R) * 0.5f;
    CHECK(cogTrackPart(mid) == TRACK_PART_RUN);
    CHECK(cogTrackPart(COG_TRACK_L + COG_CHEVRON_W * 0.5f) == TRACK_PART_DOWN);
    CHECK(cogTrackPart(COG_TRACK_R - COG_CHEVRON_W * 0.5f) == TRACK_PART_UP);
    // Buttons reach COG_CHEVRON_REACH past the ends.
    CHECK(cogTrackPart(COG_TRACK_L - COG_CHEVRON_REACH * 0.9f) == TRACK_PART_DOWN);
    CHECK(cogTrackPart(COG_TRACK_R + COG_CHEVRON_REACH * 0.9f) == TRACK_PART_UP);
    CHECK(cogTrackPart(COG_TRACK_L - COG_CHEVRON_REACH * 1.1f) == TRACK_PART_NONE);
    CHECK(cogTrackPart(COG_TRACK_R + COG_CHEVRON_REACH * 1.1f) == TRACK_PART_NONE);
    // The run starts where the buttons end.
    CHECK(cogTrackPart(COG_TRACK_L + COG_CHEVRON_W * 1.01f) == TRACK_PART_RUN);
    CHECK(cogTrackPart(COG_TRACK_R - COG_CHEVRON_W * 1.01f) == TRACK_PART_RUN);
    CHECK(cogTrackPart(0.1f) == TRACK_PART_NONE);
    CHECK(cogTrackPart(0.99f) == TRACK_PART_NONE);

    // Room for a thumb at either end of the run.
    float thumbHalfU = 0.085f * 0.5f * (float)COG_TEX_H / (float)COG_TEX_W;
    CHECK(COG_RUN_L - thumbHalfU > COG_TRACK_L + COG_CHEVRON_W);
    CHECK(COG_RUN_R + thumbHalfU < COG_TRACK_R - COG_CHEVRON_W);
    CHECK_NEAR(cogRunPlace(COG_RUN_L), 0.0, 1e-6);
    CHECK_NEAR(cogRunPlace(COG_RUN_R), 1.0, 1e-6);
    CHECK_NEAR(cogRunPlace(COG_TRACK_L), 0.0, 1e-6);
    CHECK_NEAR(cogRunPlace(COG_TRACK_R), 1.0, 1e-6);
    CHECK_NEAR(cogRunPlace(cogRunU(0.37f)), 0.37, 1e-6);
}

static void testTheStepsLandOnTheGrid(void) {
    CHECK(cogStepIndex(5.0f / 20.0f, 20, 1) == 6);
    CHECK(cogStepIndex(5.0f / 20.0f, 20, -1) == 4);
    // Float error near a step still counts as on it.
    CHECK(cogStepIndex(5.0f / 20.0f - 1e-6f, 20, 1) == 6);
    CHECK(cogStepIndex(5.0f / 20.0f + 1e-6f, 20, -1) == 4);
    // Between steps: the next one in that direction.
    CHECK(cogStepIndex(5.4f / 20.0f, 20, 1) == 6);
    CHECK(cogStepIndex(5.4f / 20.0f, 20, -1) == 5);
    CHECK(cogStepIndex(5.6f / 20.0f, 20, 1) == 6);
    CHECK(cogStepIndex(5.6f / 20.0f, 20, -1) == 5);
    // Clamped.
    CHECK(cogStepIndex(0.0f, 20, -1) == 0);
    CHECK(cogStepIndex(1.0f, 20, 1) == 20);
    CHECK(cogStepIndex(0.5f, 0, 1) == 0);
}

static void testEveryTrackHasItsSteps(void) {
    for (int row = 0; row < COG_SCREEN_ROW_COUNT; row++) {
        CHECK(cogTrackSteps(COG_TAB_SCREEN, row) > 0);
    }
    CHECK(cogTrackSteps(COG_TAB_SCREEN, COG_SCREEN_ROW_COUNT) == 0);

    // Head aim tracks: one unit per press; defaults are on a step.
    int aimLanes[2][4] = {
        { COG_SLIDER_AIM_SENSITIVITY, HEAD_AIM_SENSITIVITY_MIN, HEAD_AIM_SENSITIVITY_MAX,
          HEAD_AIM_SENSITIVITY_DEFAULT },
        { COG_SLIDER_AIM_DEADZONE, HEAD_AIM_DEADZONE_MIN, HEAD_AIM_DEADZONE_MAX,
          HEAD_AIM_DEADZONE_DEFAULT }
    };
    for (int l = 0; l < 2; l++) {
        int min = aimLanes[l][1];
        int max = aimLanes[l][2];
        int def = aimLanes[l][3];
        int steps = cogTrackSteps(COG_TAB_SCREEN, aimLanes[l][0]);
        CHECK(steps == max - min);
        int up = cogStepIndex(lanePlace(def, min, max), steps, 1);
        CHECK(laneUnits((float)up / (float)steps, min, max) == def + 1);
        int down = cogStepIndex(lanePlace(def, min, max), steps, -1);
        CHECK(laneUnits((float)down / (float)steps, min, max) == def - 1);
        for (int units = min; units <= max; units++) {
            CHECK(laneUnits(lanePlace(units, min, max), min, max) == units);
        }
    }
    CHECK(cogTrackSteps(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW) == 20);
    CHECK(cogTrackSteps(COG_TAB_DISPLAY, COG_OPTION_STATS) == 0);
    CHECK(cogTrackSteps(COG_TAB_3D, COG_ROW3D_SEPARATION) == COG_SEP_STEPS);
    CHECK(cogTrackSteps(COG_TAB_3D, COG_ROW3D_CONVERGENCE) == 100);
    CHECK(cogTrackSteps(COG_TAB_3D, COG_ROW3D_PRESET) == 0);
    CHECK(cogTrackSteps(COG_TAB_COUNT, COG_ROOM_ROW_GLOW) == 0);
    CHECK(cogTrackSteps(COG_TAB_COUNT, COG_ROOM_ROW_SIZE) == 75);
    // About (index just below the Room tab) has no tracks.
    for (int row = 0; row < COG_ROOM_ROW_COUNT; row++) {
        CHECK(cogTrackSteps(COG_TAB_ABOUT, row) == 0);
    }

    // Room lanes: each press moves by the same whole number of units.
    int lanes[2][3] = {
        { COG_ROOM_ROW_BRIGHTNESS, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX },
        { COG_ROOM_ROW_LIGHT_LEVEL, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX }
    };
    for (int l = 0; l < 2; l++) {
        int steps = cogTrackSteps(COG_TAB_COUNT, lanes[l][0]);
        int min = lanes[l][1];
        int max = lanes[l][2];
        CHECK((max - min) % steps == 0);
        int stride = (max - min) / steps;
        int units = min;
        int presses = 0;
        while (units < max && presses < 1000) {
            int step = cogStepIndex(lanePlace(units, min, max), steps, 1);
            int next = laneUnits((float)step / (float)steps, min, max);
            CHECK(next - units == stride);
            units = next;
            presses++;
        }
        CHECK(presses == steps);
        // Off-grid value steps down to the grid.
        int off = min + stride * 3 + 1;
        int step = cogStepIndex(lanePlace(off, min, max), steps, -1);
        CHECK(laneUnits((float)step / (float)steps, min, max) == min + stride * 3);
    }

    // Picture tab: one unit per press; defaults are on a step.
    int pictureLanes[PICTURE_VALUES][3] = {
        { PICTURE_BRIGHTNESS_MIN, PICTURE_BRIGHTNESS_MAX, PICTURE_BRIGHTNESS_DEFAULT },
        { PICTURE_CONTRAST_MIN, PICTURE_CONTRAST_MAX, PICTURE_CONTRAST_DEFAULT },
        { PICTURE_GAMMA_MIN, PICTURE_GAMMA_MAX, PICTURE_GAMMA_DEFAULT },
        { PICTURE_SATURATION_MIN, PICTURE_SATURATION_MAX, PICTURE_SATURATION_DEFAULT }
    };
    CHECK(cogTrackSteps(COG_TAB_PICTURE, PICTURE_VALUES) == 0);
    CHECK(cogTrackSteps(COG_TAB_PICTURE, -1) == 0);
    for (int row = 0; row < PICTURE_VALUES; row++) {
        int min = pictureLanes[row][0];
        int max = pictureLanes[row][1];
        int def = pictureLanes[row][2];
        int steps = cogTrackSteps(COG_TAB_PICTURE, row);
        CHECK(steps == max - min);
        int up = cogStepIndex(lanePlace(def, min, max), steps, 1);
        CHECK(laneUnits((float)up / (float)steps, min, max) == def + 1);
        int down = cogStepIndex(lanePlace(def, min, max), steps, -1);
        CHECK(laneUnits((float)down / (float)steps, min, max) == def - 1);
        CHECK(laneUnits(lanePlace(def, min, max), min, max) == def);
        CHECK(laneUnits(0.0f, min, max) == min && laneUnits(1.0f, min, max) == max);
    }

    // Tilt and roll steps exceed the snap-to-level window (rad).
    float tiltStep = 2.0f * 40.0f / (float)cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_TILT);
    float rollStep = 2.0f * 90.0f / (float)cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_ROTATE);
    CHECK(tiltStep * 3.14159265f / 180.0f > 0.0388f);
    CHECK(rollStep * 3.14159265f / 180.0f > 0.0873f);
    // Even step count, so level is a step.
    CHECK(cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_TILT) % 2 == 0);
    CHECK(cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_ROTATE) % 2 == 0);
}

// About tab button and the report sheet's hit zones.
static void testTheReportParts(void) {
    CHECK(COG_REPORT_T > COG_TAB_BAR_B);
    CHECK(COG_REPORT_B < 1.0f && COG_REPORT_L > 0.0f && COG_REPORT_R < 1.0f);
    CHECK(cogReportButtonAt((COG_REPORT_L + COG_REPORT_R) * 0.5f,
                            (COG_REPORT_T + COG_REPORT_B) * 0.5f));
    CHECK(!cogReportButtonAt(COG_REPORT_L - 0.01f, COG_REPORT_T + 0.01f));
    CHECK(!cogReportButtonAt(0.5f, COG_REPORT_B + 0.01f));
    CHECK(!cogReportButtonAt(0.5f, 0.08f));

    // Nothing answers above the report button.
    CHECK(COG_KOFI_T > COG_TAB_BAR_B && COG_KOFI_T < COG_KOFI_B);
    CHECK(COG_KOFI_B + 0.08f < COG_REPORT_T);
    float kofiV = (COG_KOFI_T + COG_KOFI_B) * 0.5f;
    float reportV = (COG_REPORT_T + COG_REPORT_B) * 0.5f;
    CHECK(!cogKofiButtonAt(0.5f, kofiV));
    CHECK(!cogReportButtonAt(0.5f, kofiV));
    CHECK(!cogKofiButtonAt(0.5f, reportV));
    CHECK(!cogKofiButtonAt(0.5f, (COG_KOFI_B + COG_REPORT_T) * 0.5f));
    CHECK(!cogReportButtonAt(0.5f, (COG_KOFI_B + COG_REPORT_T) * 0.5f));
    CHECK(!cogKofiButtonAt(COG_KOFI_R + 0.01f, kofiV));

    float mid = (REPORT_FIELD_L + REPORT_FIELD_R) * 0.5f;
    CHECK(reportZone(mid, (REPORT_NOTE_T + REPORT_NOTE_B) * 0.5f) == REPORT_ZONE_NOTE);
    CHECK(reportZone(mid, (REPORT_EMAIL_T + REPORT_EMAIL_B) * 0.5f) == REPORT_ZONE_EMAIL);
    float btn = (REPORT_BTN_T + REPORT_BTN_B) * 0.5f;
    CHECK(reportZone((REPORT_CANCEL_L + REPORT_CANCEL_R) * 0.5f, btn) == REPORT_ZONE_CANCEL);
    CHECK(reportZone((REPORT_SEND_L + REPORT_SEND_R) * 0.5f, btn) == REPORT_ZONE_SEND);
    // Gaps, labels, title and margins are not zones.
    CHECK(reportZone(0.5f, btn) == REPORT_ZONE_NONE);
    CHECK(reportZone(mid, (REPORT_NOTE_B + REPORT_EMAIL_T) * 0.5f) == REPORT_ZONE_NONE);
    CHECK(reportZone(mid, (REPORT_EMAIL_B + REPORT_BTN_T) * 0.5f) == REPORT_ZONE_NONE);
    CHECK(reportZone(mid, 0.05f) == REPORT_ZONE_NONE);
    CHECK(reportZone(0.02f, (REPORT_NOTE_T + REPORT_NOTE_B) * 0.5f) == REPORT_ZONE_NONE);
    CHECK(REPORT_NOTE_T < REPORT_NOTE_B && REPORT_NOTE_B < REPORT_EMAIL_T);
    CHECK(REPORT_EMAIL_T < REPORT_EMAIL_B && REPORT_EMAIL_B < REPORT_BTN_T);
    CHECK(REPORT_BTN_T < REPORT_BTN_B && REPORT_BTN_B < 1.0f);
    CHECK(REPORT_CANCEL_R < REPORT_SEND_L);
    // REPORT_OPENED is an event id, distinct from the zones.
    CHECK(REPORT_OPENED > REPORT_ZONE_SEND);
}

// Hand-lock hint sheet buttons.
static void testTheHintButtons(void) {
    float btn = (HINT_BTN_T + HINT_BTN_B) * 0.5f;
    CHECK(handHintZone((HINT_OK_L + HINT_OK_R) * 0.5f, btn) == HINT_ZONE_OK);
    CHECK(handHintZone((HINT_NEVER_L + HINT_NEVER_R) * 0.5f, btn) == HINT_ZONE_NEVER);
    CHECK(handHintZone((HINT_OK_R + HINT_NEVER_L) * 0.5f, btn) == HINT_ZONE_NONE);
    CHECK(handHintZone(HINT_OK_L - 0.01f, btn) == HINT_ZONE_NONE);
    CHECK(handHintZone(HINT_NEVER_R + 0.01f, btn) == HINT_ZONE_NONE);
    CHECK(handHintZone(0.5f, 0.3f) == HINT_ZONE_NONE);
    CHECK(handHintZone((HINT_OK_L + HINT_OK_R) * 0.5f, HINT_BTN_T - 0.01f) == HINT_ZONE_NONE);
    CHECK(handHintZone((HINT_OK_L + HINT_OK_R) * 0.5f, HINT_BTN_B + 0.01f) == HINT_ZONE_NONE);
    CHECK(HINT_OK_L > 0.0f && HINT_OK_R < HINT_NEVER_L && HINT_NEVER_R < 1.0f);
    CHECK(HINT_BTN_T > 0.5f && HINT_BTN_T < HINT_BTN_B && HINT_BTN_B < 1.0f);
    CHECK(HINT_NEVER_R - HINT_NEVER_L > HINT_OK_R - HINT_OK_L);
}

static void testTheKofiButton(void) {
    float btn = (KOFI_BTN_T + KOFI_BTN_B) * 0.5f;
    CHECK(kofiSheetZone((KOFI_CLOSE_L + KOFI_CLOSE_R) * 0.5f, btn) == KOFI_ZONE_CLOSE);
    CHECK(kofiSheetZone(KOFI_CLOSE_L - 0.01f, btn) == KOFI_ZONE_NONE);
    CHECK(kofiSheetZone(KOFI_CLOSE_R + 0.01f, btn) == KOFI_ZONE_NONE);
    CHECK(kofiSheetZone((KOFI_CLOSE_L + KOFI_CLOSE_R) * 0.5f, KOFI_BTN_T - 0.01f)
          == KOFI_ZONE_NONE);
    CHECK(kofiSheetZone((KOFI_CLOSE_L + KOFI_CLOSE_R) * 0.5f, KOFI_BTN_B + 0.01f)
          == KOFI_ZONE_NONE);
    CHECK(kofiSheetZone(KOFI_QR_L + 0.05f, KOFI_QR_T + 0.1f) == KOFI_ZONE_NONE);
    CHECK(kofiSheetZone(0.5f, 0.1f) == KOFI_ZONE_NONE);
    CHECK(KOFI_QR_L + (float)KOFI_QR_PX / KOFI_TEX_W < KOFI_CLOSE_L);
    CHECK(KOFI_QR_T + (float)KOFI_QR_PX / KOFI_TEX_H < 1.0f);
}

// Button atlas: one row of cells (so the image rect never offsets in y); faces are
// written flipped for a bottom-row-first upload.
static void testTheButtonCells(void) {
    CHECK(BTN_ATLAS_W == 1536 && BTN_ATLAS_H == BUTTON_TEX);
    // Each toggle's on face follows its off face; the last is the final cell.
    CHECK(BTN_CELL_STEREO + 1 < BTN_CELL_RAY && BTN_CELL_RAY + 1 < BTN_CELL_AIM);
    CHECK(BTN_CELL_AIM + 1 < BTN_CELL_PAD && BTN_CELL_PAD + 1 == BTN_CELLS - 1);

    int seen[BTN_ATLAS_COLS * BTN_ATLAS_ROWS] = { 0 };
    for (int cell = 0; cell < BTN_CELLS; cell++) {
        int x = -1, y = -1;
        CHECK(buttonCellOrigin(cell, &x, &y));
        CHECK(x >= 0 && x + BUTTON_TEX <= BTN_ATLAS_W);
        CHECK(y >= 0 && y + BUTTON_TEX <= BTN_ATLAS_H);
        CHECK(x % BUTTON_TEX == 0 && y == 0);
        seen[(y / BUTTON_TEX) * BTN_ATLAS_COLS + x / BUTTON_TEX]++;
    }
    for (int i = 0; i < BTN_CELLS; i++) {
        CHECK(seen[i] == 1);
    }
    int x = -1, y = -1;
    CHECK(buttonCellOrigin(0, &x, &y) && x == 0 && y == 0);
    CHECK(buttonCellOrigin(BTN_CELL_STEREO + 1, &x, &y) && x == 640 && y == 0);
    CHECK(buttonCellOrigin(BTN_CELL_PAD + 1, &x, &y) && x == 1408 && y == 0);
    CHECK(!buttonCellOrigin(-1, &x, &y) && x == 0 && y == 0);
    CHECK(!buttonCellOrigin(BTN_CELLS, &x, &y));

    // Each pixel encodes its own coordinates.
    const size_t faceBytes = (size_t)BUTTON_TEX * BUTTON_TEX * 4;
    const size_t atlasBytes = (size_t)BTN_ATLAS_W * BTN_ATLAS_H * 4;
    unsigned char* face = malloc(faceBytes);
    unsigned char* atlas = calloc(atlasBytes, 1);
    for (int fy = 0; fy < BUTTON_TEX; fy++) {
        for (int fx = 0; fx < BUTTON_TEX; fx++) {
            unsigned char* p = face + ((size_t)fy * BUTTON_TEX + fx) * 4;
            p[0] = (unsigned char)fx;
            p[1] = (unsigned char)fy;
            p[2] = 200;
            p[3] = 255;
        }
    }
    int cell = BTN_CELL_RAY + 1;
    CHECK(buttonCellPut(atlas, cell, face));
    buttonCellOrigin(cell, &x, &y);
    int right = 1;
    int written = 0;
    for (int ty = 0; ty < BTN_ATLAS_H; ty++) {
        for (int tx = 0; tx < BTN_ATLAS_W; tx++) {
            const unsigned char* p = atlas + ((size_t)ty * BTN_ATLAS_W + tx) * 4;
            int inside = tx >= x && tx < x + BUTTON_TEX && ty >= y && ty < y + BUTTON_TEX;
            if (!inside) {
                right &= p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0;
                continue;
            }
            // Flipped: uploaded bottom row first.
            int fx = tx - x;
            int fy = BUTTON_TEX - 1 - (ty - y);
            right &= p[0] == fx && p[1] == fy && p[2] == 200 && p[3] == 255;
            written++;
        }
    }
    CHECK(right);
    CHECK(written == BUTTON_TEX * BUTTON_TEX);

    // Invalid arguments write nothing.
    unsigned char* before = malloc(atlasBytes);
    memcpy(before, atlas, atlasBytes);
    CHECK(!buttonCellPut(atlas, BTN_CELLS, face));
    CHECK(!buttonCellPut(atlas, -1, face));
    CHECK(!buttonCellPut(atlas, 0, NULL));
    CHECK(!buttonCellPut(NULL, 0, face));
    CHECK(memcmp(before, atlas, atlasBytes) == 0);

    free(before);
    free(face);
    free(atlas);
}

int main(void) {
    testTheButtonCells();
    testTheRowsFit();
    testTheReportParts();
    testTheHintButtons();
    testTheKofiButton();
    testCornersFollowTheirArt();
    testNoCornersWhereThereAreNone();
    testTheRestOfThePicture();
    testTheStandIn();
    testTheBarUnderARoomsPicture();
    testTheBarUnderATurnedPicture();
    testTheBarOnACurvedPicture();
    testTheRowsOrder();
    testRoomCornersLookTheSameEverywhere();
    testLanes();
    testTheSizeClamp();
    testTheCornerDrag();
    testTheDepthTrack();
    testThePresetAccent();
    testTheTrackParts();
    testTheStepsLandOnTheGrid();
    testEveryTrackHasItsSteps();
    return checksDone("xr_layout");
}
