// Direct touch: the press state machine and surface measurement.
#include <math.h>

#include "xr_poke.h"

int pokeStep(Poke* p, int target, float depth, int over) {
    switch (p->state) {
        case POKE_PRESSED:
            if (target < 0 || target != p->target || depth < -POKE_THROUGH_M) {
                p->state = POKE_IDLE;
            }
            else if (depth > POKE_RELEASE_M) {
                p->state = over && depth < POKE_HOVER_M ? POKE_HOVER : POKE_IDLE;
            }
            break;
        case POKE_HOVER:
            if (target < 0 || !over || depth >= POKE_HOVER_M) {
                p->state = POKE_IDLE;
            }
            else if (target != p->target) {
                // Another surface is now the nearest: it is armed like the first
                p->target = target;
                if (depth <= POKE_ARM_M) {
                    p->state = POKE_IDLE;
                }
            }
            else if (depth <= POKE_PRESS_M) {
                // Coming from in front, so a fast finger already past the surface presses
                p->state = POKE_PRESSED;
            }
            break;
        default:
            if (target >= 0 && over && depth > POKE_ARM_M && depth < POKE_HOVER_M) {
                p->state = POKE_HOVER;
                p->target = target;
            }
            break;
    }
    if (p->state == POKE_IDLE) {
        p->target = -1;
    }
    return p->state;
}

void pokeLocal(Vec3 local, float width, float height, float radius, int curved,
               PokeLocal* out) {
    if (curved && radius > 1e-3f) {
        // Vertical axis at z = radius, as in screenPoint; in front is toward the axis
        float dz = radius - local.z;
        float rho = sqrtf(local.x * local.x + dz * dz);
        float angle = atan2f(local.x, dz);
        out->x = angle * radius;
        out->depth = radius - rho;
        out->yaw = angle;
        out->over = fabsf(angle) <= 0.5f * cylinderAngle(width, radius)
                && fabsf(local.y) <= 0.5f * height * cylinderFit(width, radius);
    }
    else {
        out->x = local.x;
        out->depth = local.z;
        out->yaw = 0.0f;
        out->over = fabsf(local.x) <= 0.5f * width && fabsf(local.y) <= 0.5f * height;
    }
}

int pinchHoldStep(int hold, int pinch, int pressing, int near) {
    if (!pinch) {
        return HOLD_NONE;
    }
    if (hold != HOLD_NONE || !pressing) {
        return hold;
    }
    return near ? HOLD_NEAR : HOLD_RAY;
}

int pinchAfterTouch(int ignored, int pinch, int touching) {
    if (!pinch) {
        return 0;
    }
    return ignored || touching;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

int nearBarZone(Vec3 local, const NearBarRow* row, float* aimX) {
    if (fabsf(local.y) > row->halfH + NEAR_BAR_REACH_M || fabsf(local.z) > NEAR_BAR_REACH_M) {
        return NEAR_AWAY;
    }
    if (row->hasBack) {
        // The gap between the two is shared half and half
        float side = row->backX < 0.0f ? -1.0f : 1.0f;
        float gap = fmaxf(0.0f, fabsf(row->backX) - row->backHalfW - row->halfW);
        if (local.x * side >= row->halfW + 0.5f * gap) {
            if (fabsf(local.x - row->backX) > row->backHalfW + NEAR_BAR_REACH_M) {
                return NEAR_AWAY;
            }
            *aimX = clampf(local.x, row->backX - 0.6f * row->backHalfW,
                           row->backX + 0.6f * row->backHalfW);
            return NEAR_BACK;
        }
    }
    if (fabsf(local.x) > row->halfW + NEAR_BAR_REACH_M) {
        return NEAR_AWAY;
    }
    *aimX = clampf(local.x, -0.9f * row->halfW, 0.9f * row->halfW);
    return NEAR_BAR;
}

int uiPressStep(int was, int down, int edge, int onPanel) {
    return down && (was || (edge && onPanel));
}
