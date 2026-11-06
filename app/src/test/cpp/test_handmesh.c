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
    handSkinMatrix(m, bind, bind);
    Vec3 same = apply(m, v);
    CHECK_NEAR(same.x, v.x, 1e-5f);
    CHECK_NEAR(same.y, v.y, 1e-5f);
    CHECK_NEAR(same.z, v.z, 1e-5f);
    // Moved: the mesh moves with it
    XrPosef moved = bind;
    moved.position.x += 0.3f;
    moved.position.z -= 0.5f;
    handSkinMatrix(m, moved, bind);
    Vec3 shifted = apply(m, v);
    CHECK_NEAR(shifted.x, v.x + 0.3f, 1e-5f);
    CHECK_NEAR(shifted.z, v.z - 0.5f, 1e-5f);
    // Turned a quarter about up from a bind at the origin: x goes to -z
    float h = sqrtf(0.5f);
    handSkinMatrix(m, pose(0.0f, h, 0.0f, h, 0.0f, 0.0f, 0.0f), pose(0, 0, 0, 1, 0, 0, 0));
    Vec3 x = { 1.0f, 0.0f, 0.0f };
    Vec3 turned = apply(m, x);
    CHECK_NEAR(turned.x, 0.0f, 1e-5f);
    CHECK_NEAR(turned.z, -1.0f, 1e-5f);
}

int main(void) {
    testSkin();
    return checksDone("handmesh");
}
