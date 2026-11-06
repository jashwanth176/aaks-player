// Tests for xr_poke: when a fingertip hovers, presses and lets go, and where it is.
#include "check.h"
#include "xr_poke.h"

static void testPress(void) {
    Poke p = { POKE_IDLE, -1 };
    // Far away, or off the surface: nothing
    CHECK(pokeStep(&p, 3, 0.2f, 1) == POKE_IDLE);
    CHECK(pokeStep(&p, 3, 0.04f, 0) == POKE_IDLE);
    // Close in front hovers, touching presses, holding through the surface stays pressed
    CHECK(pokeStep(&p, 3, 0.04f, 1) == POKE_HOVER && p.target == 3);
    CHECK(pokeStep(&p, 3, 0.01f, 1) == POKE_HOVER);
    CHECK(pokeStep(&p, 3, -0.002f, 1) == POKE_PRESSED);
    CHECK(pokeStep(&p, 3, -0.08f, 0) == POKE_PRESSED);
    // A shaky finger near the surface does not let go; pulling back does
    CHECK(pokeStep(&p, 3, 0.008f, 1) == POKE_PRESSED);
    CHECK(pokeStep(&p, 3, 0.02f, 1) == POKE_HOVER);
    // A fast finger already past the surface still presses
    CHECK(pokeStep(&p, 3, -0.03f, 1) == POKE_PRESSED);
    // Pushed far through, or the surface gone, ends it
    CHECK(pokeStep(&p, 3, -0.2f, 1) == POKE_IDLE && p.target == -1);
    CHECK(pokeStep(&p, 3, 0.03f, 1) == POKE_HOVER);
    CHECK(pokeStep(&p, 3, 0.0f, 1) == POKE_PRESSED);
    CHECK(pokeStep(&p, -1, 0.0f, 0) == POKE_IDLE);
}

static void testFromBehind(void) {
    // A hand reaching past a surface from behind never starts a press on it
    Poke p = { POKE_IDLE, -1 };
    CHECK(pokeStep(&p, 2, -0.01f, 1) == POKE_IDLE);
    CHECK(pokeStep(&p, 2, -0.05f, 1) == POKE_IDLE);
    CHECK(pokeStep(&p, 2, 0.0f, 1) == POKE_IDLE);
    // Once in front it works
    CHECK(pokeStep(&p, 2, 0.02f, 1) == POKE_HOVER);
    CHECK(pokeStep(&p, 2, 0.0f, 1) == POKE_PRESSED);
}

static void testTargets(void) {
    // Hovering follows the nearest surface; a press stays on its own
    Poke p = { POKE_IDLE, -1 };
    CHECK(pokeStep(&p, 1, 0.05f, 1) == POKE_HOVER && p.target == 1);
    CHECK(pokeStep(&p, 4, 0.05f, 1) == POKE_HOVER && p.target == 4);
    CHECK(pokeStep(&p, 4, -0.001f, 1) == POKE_PRESSED);
    CHECK(pokeStep(&p, 1, -0.001f, 1) == POKE_IDLE);
    // Leaving the edge while hovering stops it; while pressed (a drag) it does not
    CHECK(pokeStep(&p, 4, 0.03f, 1) == POKE_HOVER);
    CHECK(pokeStep(&p, 4, 0.0f, 1) == POKE_PRESSED);
    CHECK(pokeStep(&p, 4, -0.01f, 0) == POKE_PRESSED);
    CHECK(pokeStep(&p, 4, 0.03f, 0) == POKE_IDLE);
}

static void testLocal(void) {
    PokeLocal l;
    // Flat: depth is z, the edges are the size
    Vec3 a = { 0.5f, -0.2f, 0.03f };
    pokeLocal(a, 2.0f, 1.0f, 0.0f, 0, &l);
    CHECK_NEAR(l.depth, 0.03f, 1e-6f);
    CHECK_NEAR(l.x, 0.5f, 1e-6f);
    CHECK_NEAR(l.yaw, 0.0f, 1e-6f);
    CHECK(l.over);
    Vec3 b = { 1.1f, 0.0f, 0.03f };
    pokeLocal(b, 2.0f, 1.0f, 0.0f, 0, &l);
    CHECK(!l.over);
    // Curved: a point on the surface, then 2 cm toward the axis
    float radius = 3.0f, angle = 0.2f;
    Vec3 on = { radius * sinf(angle), 0.1f, radius - radius * cosf(angle) };
    pokeLocal(on, 2.0f, 1.0f, radius, 1, &l);
    CHECK_NEAR(l.depth, 0.0f, 1e-5f);
    CHECK_NEAR(l.x, angle * radius, 1e-5f);
    CHECK_NEAR(l.yaw, angle, 1e-5f);
    CHECK(l.over);
    Vec3 near = { (radius - 0.02f) * sinf(angle), 0.1f, radius - (radius - 0.02f) * cosf(angle) };
    pokeLocal(near, 2.0f, 1.0f, radius, 1, &l);
    CHECK_NEAR(l.depth, 0.02f, 1e-5f);
    CHECK_NEAR(l.yaw, angle, 1e-5f);
    // Past the arc's end
    Vec3 past = { radius * sinf(0.5f), 0.0f, radius - radius * cosf(0.5f) };
    pokeLocal(past, 2.0f, 1.0f, radius, 1, &l);
    CHECK(!l.over);
}

int main(void) {
    testPress();
    testFromBehind();
    testTargets();
    testLocal();
    return checksDone("poke");
}
