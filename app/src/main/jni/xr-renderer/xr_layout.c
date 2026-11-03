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

float separationOf(int units) {
    return units * 0.001f;
}
