// Tests for xr_handmesh: joint skinning matrices.
#include <math.h>

#include "check.h"
#include "xr_handmesh.h"

static Vec3 apply(const float* m, Vec3 p) {
    Vec3 out = {
        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14],
    };
    return out;
}

static XrPosef pose(float qx, float qy, float qz, float qw, float x, float y, float z) {
    XrPosef p;
    p.orientation.x = qx;
    p.orientation.y = qy;
    p.orientation.z = qz;
    p.orientation.w = qw;
    p.position.x = x;
    p.position.y = y;
    p.position.z = z;
    return p;
}

static void testSkin(void) {
    float m[16];
    Vec3 v = { 0.01f, 0.02f, -0.03f };
    // A joint at its bind pose leaves the mesh where it is
    XrPosef bind = pose(0.1f, 0.7f, -0.1f, 0.7f, 0.05f, -0.02f, 0.1f);
    float n = sqrtf(0.01f + 0.49f + 0.01f + 0.49f);
    bind.orientation.x /= n;
    bind.orientation.y /= n;
    bind.orientation.z /= n;
    bind.orientation.w /= n;
    handSkinMatrix(m, bind, bind, 1.0f);
    Vec3 same = apply(m, v);
    CHECK_NEAR(same.x, v.x, 1e-5f);
    CHECK_NEAR(same.y, v.y, 1e-5f);
    CHECK_NEAR(same.z, v.z, 1e-5f);
    // Moved: the mesh moves with it
    XrPosef moved = bind;
    moved.position.x += 0.3f;
    moved.position.z -= 0.5f;
    handSkinMatrix(m, moved, bind, 1.0f);
    Vec3 shifted = apply(m, v);
    CHECK_NEAR(shifted.x, v.x + 0.3f, 1e-5f);
    CHECK_NEAR(shifted.z, v.z - 0.5f, 1e-5f);
    // Turned a quarter about up from a bind at the origin: x goes to -z
    float h = sqrtf(0.5f);
    handSkinMatrix(m, pose(0.0f, h, 0.0f, h, 0.0f, 0.0f, 0.0f), pose(0, 0, 0, 1, 0, 0, 0), 1.0f);
    Vec3 x = { 1.0f, 0.0f, 0.0f };
    Vec3 turned = apply(m, x);
    CHECK_NEAR(turned.x, 0.0f, 1e-5f);
    CHECK_NEAR(turned.z, -1.0f, 1e-5f);
    // A bigger hand: the mesh grows, and a point on a joint lands on where it is now
    XrPosef at = pose(0, 0, 0, 1, 0.02f, 0.05f, 0.0f);
    XrPosef now = pose(0, 0, 0, 1, 0.3f, 1.2f, -0.4f);
    handSkinMatrix(m, now, at, 1.1f);
    Vec3 onJoint = apply(m, (Vec3){ 0.02f, 0.05f, 0.0f });
    CHECK_NEAR(onJoint.x, 0.3f, 1e-5f);
    CHECK_NEAR(onJoint.y, 1.2f, 1e-5f);
    Vec3 skin = apply(m, (Vec3){ 0.03f, 0.05f, 0.0f });
    CHECK_NEAR(skin.x - onJoint.x, 0.011f, 1e-5f);
}

static void testFade(void) {
    const float dt = 1.0f / 72.0f;
    // Fades in within a few frames
    float level = 0.0f;
    for (int i = 0; i < 9; i++) {
        level = handFadeStep(level, 1, 0.0f, dt);
    }
    CHECK_NEAR(level, 1.0f, 1e-6f);
    // A short drop at the edge of view: held, no blink
    float lost = 0.0f;
    for (int i = 0; i < 14; i++) {
        lost += dt;
        level = handFadeStep(level, 0, lost, dt);
    }
    CHECK_NEAR(level, 1.0f, 1e-6f);
    level = handFadeStep(level, 1, 0.0f, dt);
    CHECK_NEAR(level, 1.0f, 1e-6f);
    // Gone for good: held a quarter second, then out within another fifth
    lost = 0.0f;
    float atHold = -1.0f;
    for (int i = 0; i < 40; i++) {
        lost += dt;
        level = handFadeStep(level, 0, lost, dt);
        if (atHold < 0.0f && lost >= HAND_HOLD_SEC) {
            atHold = level;
        }
    }
    CHECK(atHold > 0.9f);
    CHECK_NEAR(level, 0.0f, 1e-6f);
}

static void testReach(void) {
    // Two joints 4 cm apart; one vertex on each, one shared a centimetre past the second
    XrPosef bind[2] = { pose(0, 0, 0, 1, 0.0f, 0.0f, 0.0f), pose(0, 0, 0, 1, 0.04f, 0.0f, 0.0f) };
    float vertices[3 * HAND_VERTEX_FLOATS] = { 0 };
    float* v = vertices;
    v[1] = 0.01f;
    v[6] = 0.0f;
    v[10] = 1.0f;
    v += HAND_VERTEX_FLOATS;
    v[0] = 0.04f;
    v[2] = -0.02f;
    v[6] = 1.0f;
    v[10] = 1.0f;
    v += HAND_VERTEX_FLOATS;
    v[0] = 0.05f;
    v[6] = 0.0f;
    v[7] = 1.0f;
    v[10] = 0.25f;
    v[11] = 0.75f;
    float reach[2];
    handJointReach(reach, 2, vertices, 3, bind);
    // Every joint that moves a vertex reaches it; unused slots (weight 0) count for nothing
    CHECK_NEAR(reach[0], 0.05f, 1e-6f);
    CHECK_NEAR(reach[1], 0.02f, 1e-6f);
}

static void testViewBox(void) {
    const XrPosef eye = pose(0, 0, 0, 1, 0.0f, 1.6f, 0.0f);
    // 1680 px over tangents -1.4..0.84, 1760 px over -1..1
    XrFovf fov = { atanf(-1.4f), atanf(0.84f), atanf(1.0f), atanf(-1.0f) };
    ViewBox box;

    // Nothing added, or only things behind the eye: nothing in view
    viewBoxClear(&box);
    CHECK(!viewBoxFit(&box, fov, 1680, 1760, 1024));
    viewBoxAdd(&box, eye, (XrVector3f){ 0.0f, 1.6f, 0.5f }, 0.05f);
    CHECK(!viewBoxFit(&box, fov, 1680, 1760, 1024));

    // A 5 cm ball half a metre ahead, a little down and right: held with room to spare
    viewBoxClear(&box);
    viewBoxAdd(&box, eye, (XrVector3f){ 0.1f, 1.5f, -0.5f }, 0.05f);
    CHECK(box.left <= (0.1f - 0.05f) / 0.5f && box.right >= (0.1f + 0.05f) / 0.5f);
    CHECK(box.down <= (-0.1f - 0.05f) / 0.5f && box.up >= (-0.1f + 0.05f) / 0.5f);
    ViewBox raw = box;
    CHECK(viewBoxFit(&box, fov, 1680, 1760, 1024));
    CHECK(box.left <= raw.left && box.right >= raw.right);
    CHECK(box.down <= raw.down && box.up >= raw.up);
    // At the eye's own density, 750 and 880 px per unit of tangent, on a 32 px grid
    CHECK(box.width % VIEW_BOX_SNAP_PX == 0 && box.height % VIEW_BOX_SNAP_PX == 0);
    CHECK_NEAR(box.width, (box.right - box.left) * 750.0f, 0.5f);
    CHECK_NEAR(box.height, (box.up - box.down) * 880.0f, 0.5f);
    CHECK(box.width < 320 && box.height < 320);
    CHECK_NEAR(box.left * 750.0f / VIEW_BOX_SNAP_PX, roundf(box.left * 750.0f / VIEW_BOX_SNAP_PX), 1e-3f);

    // A small move inside the grid step leaves the image where it was
    ViewBox moved;
    viewBoxClear(&moved);
    viewBoxAdd(&moved, eye, (XrVector3f){ 0.101f, 1.5f, -0.5f }, 0.05f);
    CHECK(viewBoxFit(&moved, fov, 1680, 1760, 1024));
    CHECK_NEAR(moved.left, box.left, 1e-6f);
    CHECK(moved.width == box.width);

    // The quad for it stands at the ball's distance and covers the same tangents
    float w, h;
    XrPosef quad = viewBoxQuad(&box, eye, &w, &h);
    CHECK_NEAR(quad.position.z, -0.5f, 1e-5f);
    CHECK_NEAR((quad.position.x - 0.5f * w) / 0.5f, box.left, 1e-5f);
    CHECK_NEAR((quad.position.x + 0.5f * w) / 0.5f, box.right, 1e-5f);
    CHECK_NEAR((quad.position.y - 1.6f + 0.5f * h) / 0.5f, box.up, 1e-5f);
    XrFovf drawn = viewBoxFov(&box);
    CHECK_NEAR(tanf(drawn.angleLeft), box.left, 1e-5f);
    CHECK_NEAR(tanf(drawn.angleUp), box.up, 1e-5f);

    // Off to the side past the view's edge: nothing to draw
    viewBoxClear(&box);
    viewBoxAdd(&box, eye, (XrVector3f){ 1.0f, 1.6f, -0.5f }, 0.05f);
    CHECK(!viewBoxFit(&box, fov, 1680, 1760, 1024));

    // Hands wide apart: the image stops at the cap, the box still holds both
    viewBoxClear(&box);
    viewBoxAdd(&box, eye, (XrVector3f){ -0.35f, 1.4f, -0.4f }, 0.1f);
    viewBoxAdd(&box, eye, (XrVector3f){ 0.3f, 1.4f, -0.4f }, 0.1f);
    CHECK(viewBoxFit(&box, fov, 1680, 1760, 1024));
    CHECK(box.width == 1024);
    CHECK(box.left <= -0.45f / 0.3f || box.left <= -1.4f);
    CHECK(box.right >= 0.84f);

    // At the face the tangents run away and the box is the whole view
    viewBoxClear(&box);
    viewBoxAdd(&box, eye, (XrVector3f){ 0.0f, 1.6f, -0.04f }, 0.08f);
    CHECK(viewBoxFit(&box, fov, 1680, 1760, 1024));
    CHECK(box.left <= -1.4f && box.right >= 0.84f && box.down <= -1.0f && box.up >= 1.0f);
    CHECK(box.width == 1024 && box.height == 1024);

    // Seen by a turned eye: a quarter turn left puts a ball on the left in front
    float s = sqrtf(0.5f);
    viewBoxClear(&box);
    viewBoxAdd(&box, pose(0, s, 0, s, 0.0f, 1.6f, 0.0f), (XrVector3f){ -0.5f, 1.6f, 0.0f }, 0.05f);
    CHECK_NEAR(0.5f * (box.left + box.right), 0.0f, 0.02f);
    CHECK_NEAR(box.depthSum, 0.5f, 1e-5f);
}

int main(void) {
    testSkin();
    testFade();
    testReach();
    testViewBox();
    return checksDone("handmesh");
}
