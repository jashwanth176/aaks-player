// Open-palm gesture that wakes a sleeping hand pointer

#include <math.h>

#include "xr_palm.h"

#define DEG_TO_RAD 0.017453293f

static Vec3 position(XrPosef p) {
    Vec3 v = { p.position.x, p.position.y, p.position.z };
    return v;
}

static float distance(Vec3 a, Vec3 b) {
    Vec3 d = vecSub(a, b);
    return sqrtf(vecDot(d, d));
}

int palmShown(const PalmHand* hand, XrPosef head, int facing) {
    Vec3 palm = position(hand->palm);
    Vec3 eyes = position(head);
    float away = distance(palm, eyes);
    if (away < PALM_NEAR_M || away > PALM_FAR_M) {
        return 0;
    }

    // In front of the face, in the head's own frame
    Vec3 toPalm = vecNorm(vecSub(palm, eyes));
    Vec3 local = quatRotate(quatConj(head.orientation), toPalm);
    float ahead = -local.z;
    if (ahead <= 0.0f) {
        return 0;
    }
    float side = atan2f(fabsf(local.x), ahead) / DEG_TO_RAD;
    float up = atan2f(local.y, sqrtf(local.x * local.x + ahead * ahead)) / DEG_TO_RAD;
    if (side > PALM_SIDE_DEG || up > PALM_ABOVE_DEG || up < -PALM_BELOW_DEG) {
        return 0;
    }

    // Facing the eyes
    if (!facing) {
        Vec3 out = { 0.0f, -1.0f, 0.0f };
        out = quatRotate(hand->palm.orientation, out);
        Vec3 toEyes = { -toPalm.x, -toPalm.y, -toPalm.z };
        if (vecDot(out, toEyes) < cosf(PALM_FACING_DEG * DEG_TO_RAD)) {
            return 0;
        }
    }

    // Open: not a fist or a pinch
    int open = 0;
    for (int i = 0; i < 4; i++) {
        if (distance(hand->tips[i], palm) > PALM_FINGER_OPEN_M) {
            open++;
        }
    }
    return open >= 3 && distance(hand->thumbTip, hand->tips[0]) > PALM_TIPS_APART_M;
}

int palmHoldStep(PalmHold* hold, int shown, float dt) {
    if (!shown) {
        hold->lostFor += dt;
        if (hold->lostFor >= PALM_DROP_SEC) {
            hold->heldFor = 0.0f;
            hold->fired = 0;
        }
        return 0;
    }
    hold->lostFor = 0.0f;
    hold->heldFor += dt;
    if (!hold->fired && hold->heldFor >= PALM_HOLD_SEC) {
        hold->fired = 1;
        return 1;
    }
    return 0;
}
