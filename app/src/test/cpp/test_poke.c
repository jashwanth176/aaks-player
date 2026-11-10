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

static void testLeavingBackwards(void) {
    // A dialog opens in front of a finger pressing the sheet behind it. Drawing back
    // through the dialog never presses it, however the fingertip shakes on the way.
    Poke p = { POKE_IDLE, -1 };
    CHECK(pokeStep(&p, 1, 0.03f, 1) == POKE_HOVER);
    CHECK(pokeStep(&p, 1, -0.002f, 1) == POKE_PRESSED);
    CHECK(pokeStep(&p, 1, 0.02f, 1) == POKE_HOVER);
    // The dialog (2) is now the nearest surface, with the finger 7 cm behind it
    CHECK(pokeStep(&p, 2, -0.07f, 1) == POKE_IDLE);
    CHECK(pokeStep(&p, 2, -0.02f, 1) == POKE_IDLE);
    CHECK(pokeStep(&p, 2, 0.002f, 1) == POKE_IDLE);
    CHECK(pokeStep(&p, 2, -0.001f, 1) == POKE_IDLE);
    CHECK(pokeStep(&p, 2, 0.012f, 1) == POKE_IDLE);
    CHECK(pokeStep(&p, 2, -0.003f, 1) == POKE_IDLE);
    // Well clear of it, the finger may come back and press
    CHECK(pokeStep(&p, 2, 0.03f, 1) == POKE_HOVER && p.target == 2);
    CHECK(pokeStep(&p, 2, 0.005f, 1) == POKE_HOVER);
    CHECK(pokeStep(&p, 2, -0.001f, 1) == POKE_PRESSED);

    // A fast finger: in front on one frame, already through on the next
    Poke q = { POKE_IDLE, -1 };
    CHECK(pokeStep(&q, 5, 0.06f, 1) == POKE_HOVER);
    CHECK(pokeStep(&q, 5, -0.04f, 1) == POKE_PRESSED);
    // A surface that appears right at the fingertip is not armed
    Poke r = { POKE_IDLE, -1 };
    CHECK(pokeStep(&r, 5, 0.01f, 1) == POKE_IDLE);
    CHECK(pokeStep(&r, 5, -0.01f, 1) == POKE_IDLE);
}

static void testPinchBesideTouch(void) {
    // A pinch pressing with the far ray holds until let go, whatever happens meanwhile
    int hold = HOLD_NONE;
    hold = pinchHoldStep(hold, 1, 0, 0);
    CHECK(hold == HOLD_NONE);
    hold = pinchHoldStep(hold, 1, 1, 0);
    CHECK(hold == HOLD_RAY);
    hold = pinchHoldStep(hold, 1, 0, 1);
    CHECK(hold == HOLD_RAY);
    hold = pinchHoldStep(hold, 0, 1, 0);
    CHECK(hold == HOLD_NONE);
    // Begun at the bar it is the bar's
    hold = pinchHoldStep(hold, 1, 1, 1);
    CHECK(hold == HOLD_NEAR);
    hold = pinchHoldStep(hold, 1, 1, 0);
    CHECK(hold == HOLD_NEAR);

    // A pinch seen while touching does not click when the finger leaves; the next does
    int ignored = 0;
    ignored = pinchAfterTouch(ignored, 0, 1);
    CHECK(!ignored);
    ignored = pinchAfterTouch(ignored, 1, 1);
    CHECK(ignored);
    ignored = pinchAfterTouch(ignored, 1, 0);
    CHECK(ignored);
    ignored = pinchAfterTouch(ignored, 0, 0);
    CHECK(!ignored);
    ignored = pinchAfterTouch(ignored, 1, 0);
    CHECK(!ignored);
}

static void testNearBar(void) {
    // A 0.8 m wide window: the bar 11.2 cm long, the Back button 4 cm wide, centred
    // 8.6 cm to its left
    NearBarRow row = { 0.056f, 0.005f, 1, -0.0856f, 0.02f };
    float x = 0.0f;
    Vec3 onBar = { 0.02f, 0.01f, 0.02f };
    CHECK(nearBarZone(onBar, &row, &x) == NEAR_BAR);
    CHECK_NEAR(x, 0.02f, 1e-6f);
    // Past its right end the ray is still brought onto the bar
    Vec3 right = { 0.09f, -0.03f, 0.0f };
    CHECK(nearBarZone(right, &row, &x) == NEAR_BAR);
    CHECK_NEAR(x, 0.9f * 0.056f, 1e-6f);
    // Over the Back button it is the button's, not the bar's
    Vec3 onBack = { -0.085f, 0.0f, 0.03f };
    CHECK(nearBarZone(onBack, &row, &x) == NEAR_BACK);
    CHECK_NEAR(x, -0.085f, 1e-6f);
    Vec3 backEdge = { -0.12f, 0.0f, 0.0f };
    CHECK(nearBarZone(backEdge, &row, &x) == NEAR_BACK);
    CHECK(x >= -0.0856f - 0.02f && x <= -0.0856f + 0.02f);
    // The gap between them is split: nearer the bar is the bar's
    Vec3 barSide = { -0.058f, 0.0f, 0.0f };
    Vec3 backSide = { -0.064f, 0.0f, 0.0f };
    CHECK(nearBarZone(barSide, &row, &x) == NEAR_BAR);
    CHECK(nearBarZone(backSide, &row, &x) == NEAR_BACK);
    // Too far in front, above, or off either end
    Vec3 front = { 0.0f, 0.0f, 0.08f };
    Vec3 above = { 0.0f, 0.08f, 0.0f };
    Vec3 farLeft = { -0.18f, 0.0f, 0.0f };
    Vec3 farRight = { 0.13f, 0.0f, 0.0f };
    CHECK(nearBarZone(front, &row, &x) == NEAR_AWAY);
    CHECK(nearBarZone(above, &row, &x) == NEAR_AWAY);
    CHECK(nearBarZone(farLeft, &row, &x) == NEAR_AWAY);
    CHECK(nearBarZone(farRight, &row, &x) == NEAR_AWAY);
    // Without a Back button its place is the bar's
    row.hasBack = 0;
    CHECK(nearBarZone(onBack, &row, &x) == NEAR_BAR);
    CHECK_NEAR(x, -0.9f * 0.056f, 1e-6f);
}

static void testUiPress(void) {
    // A press that begins on a menu is the menu's until let go, on or off it
    int press = 0;
    press = uiPressStep(press, 1, 1, 1);
    CHECK(press);
    press = uiPressStep(press, 1, 0, 0);
    CHECK(press);
    press = uiPressStep(press, 0, 0, 1);
    CHECK(!press);
    // A press held on the picture is not adopted by a menu that appears under it
    press = uiPressStep(press, 1, 1, 0);
    CHECK(!press);
    press = uiPressStep(press, 1, 0, 1);
    CHECK(!press);
    press = uiPressStep(press, 1, 0, 1);
    CHECK(!press);
    // Let go and pressed again on the menu, it counts
    press = uiPressStep(press, 0, 0, 1);
    press = uiPressStep(press, 1, 1, 1);
    CHECK(press);
}

int main(void) {
    testPress();
    testFromBehind();
    testTargets();
    testLocal();
    testLeavingBackwards();
    testPinchBesideTouch();
    testNearBar();
    testUiPress();
    return checksDone("poke");
}
