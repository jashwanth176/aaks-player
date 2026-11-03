// Layout geometry: handle hit testing and bar placement

#ifndef XR_LAYOUT_H
#define XR_LAYOUT_H

#include "xr_math.h"
#include "xr_shared.h"

// What the ray is over. Handles are shown only while hovered.
#define HOVER_NONE   0
#define HOVER_SCREEN 1
#define HOVER_BAR    2
#define HOVER_CORNER 3
// Nothing hit, but near enough to the screen to keep drawing the ray
#define HOVER_HALO      6
// A UI panel; which one is in ctx->uiHover
#define HOVER_UIPANEL   20
// Halo extent past each edge, fraction of the screen
#define HALO_FRAC 0.5f

// Handle art sizes; one small swapchain each
#define BAR_TEX_W 256
#define BAR_TEX_H 24
#define CORNER_TEX_W 64
#define CORNER_TEX_H 64

// Fractions of screen width, so handles scale with the screen
#define BAR_WIDTH_FRAC  0.14f
// Height derived from the art's aspect so the rounded ends are not stretched
#define BAR_HEIGHT_FRAC (BAR_WIDTH_FRAC * (float)BAR_TEX_H / (float)BAR_TEX_W)
#define BAR_GAP_FRAC    0.035f
#define CORNER_FRAC     0.075f
// Hover zones are larger than the art for easier aiming
#define HOVER_MARGIN 1.7f
#define CORNER_HOVER 1.5f
// The bar is small, so its hover zone is proportionally wider
#define BAR_HOVER 2.0f

// Stand-in screen for sizing panels in a room, so they keep a constant size
#define STAND_IN_WIDTH_M 3.0f
#define STAND_IN_DISTANCE_M 3.0f

// Bar row centre below the frame's bottom edge
#define BAR_DROP_FRAC (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 0.5f)


// Frame the bar row is laid out in, bottom-aligned with the picture. In a room
// it is flat and sized to match the stand-in.
typedef struct {
    XrPosef pose;
    float width;
    float height;
    float radius;
    int curved;
} BarFrame;

// Bar frame for a room picture; aspect is height / width
BarFrame roomBarFrame(XrPosef picture, float pictureHeight, float aspect);

// Frame width giving the bar the stand-in's angular size
float barFrameWidth(Vec3 bottomMid, Vec3 down, float aspect);


// Hover target at uv. cornerSide is the bracket size in metres (0 for none);
// corners are numbered TL, TR, BL, BR.
int hoverTest(float u, float v, float width, float height, float cornerSide, int* corner);

// Stand-in screen pose, facing the seat
XrPosef standInPose(void);



// Stereo separation from stored units (0.1% of frame width)
float separationOf(int units);

// Room picture size percent, clamped; always the maximum if not resizable
int roomScreenClamp(int percent, int resizable);

// Room picture size during a corner drag, relative to the grab so it does not jump
int roomResizePercent(int startPercent, float startReach, float reach);

// Corner bracket size at a distance, matching the stand-in's angular size
float roomCornerSide(float distance);

#endif
