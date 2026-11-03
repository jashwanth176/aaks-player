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

// Angular size of the bar from the seat.
static float barLooks(const BarFrame* frame) {
    return frame->width * BAR_WIDTH_FRAC / lengthOf(barMiddle(frame));
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
}

static void testRoomCornersLookTheSameEverywhere(void) {
    // At the stand-in distance, same as on a 3 m screen.
    CHECK_NEAR(roomCornerSide(STAND_IN_DISTANCE_M), CORNER_FRAC * STAND_IN_WIDTH_M, 1e-6);
    // Constant angular size.
    CHECK_NEAR(roomCornerSide(14.0f), CORNER_FRAC * STAND_IN_WIDTH_M * 14.0f / 3.0f, 1e-5);
    CHECK_NEAR(roomCornerSide(4.15f) / 4.15f, roomCornerSide(14.0f) / 14.0f, 1e-6);
    CHECK_NEAR(roomCornerSide(0.0f), 0.0f, 1e-6);
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

int main(void) {
    testCornersFollowTheirArt();
    testNoCornersWhereThereAreNone();
    testTheRestOfThePicture();
    testTheStandIn();
    testTheBarUnderARoomsPicture();
    testTheBarUnderATurnedPicture();
    testRoomCornersLookTheSameEverywhere();
    testTheSizeClamp();
    testTheCornerDrag();
    return checksDone("xr_layout");
}
