// Tests for xr_layout: hover zones on the picture.
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

int main(void) {
    testCornersFollowTheirArt();
    testNoCornersWhereThereAreNone();
    testTheRestOfThePicture();
    return checksDone("xr_layout");
}
