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
            else {
                // Coming from in front, so a fast finger already past the surface presses
                p->target = target;
                if (depth <= POKE_PRESS_M) {
                    p->state = POKE_PRESSED;
                }
            }
            break;
        default:
            if (target >= 0 && over && depth > POKE_PRESS_M && depth < POKE_HOVER_M) {
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
