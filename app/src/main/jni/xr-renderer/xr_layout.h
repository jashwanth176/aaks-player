// Layout geometry: handle hit testing, bar placement, settings panel tracks

#ifndef XR_LAYOUT_H
#define XR_LAYOUT_H

#include "xr_math.h"
#include "xr_shared.h"

// What the ray is over. Handles are shown only while hovered.
#define HOVER_NONE   0
#define HOVER_SCREEN 1
#define HOVER_BAR    2
#define HOVER_CORNER 3
#define HOVER_ENVBUTTON 4
#define HOVER_PICKER    5
// Nothing hit, but near enough to the screen to keep drawing the ray
#define HOVER_HALO      6
#define HOVER_HINT      7
#define HOVER_COGBUTTON 8
#define HOVER_COGPANEL  9
#define HOVER_KBBUTTON  10
#define HOVER_KBPANEL   11
#define HOVER_EXITBUTTON 12
#define HOVER_EXITPROMPT 13
#define HOVER_STEREOBUTTON 14
#define HOVER_REPORT    15
#define HOVER_RAYBUTTON 16
#define HOVER_AIMBUTTON 17
#define HOVER_PADBUTTON 18
#define HOVER_KOFI      19
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

// Square bar button sizes and gap, as fractions of the bar frame width
#define ENV_BUTTON_FRAC 0.048f
#define ENV_GAP_FRAC 0.02f
#define COG_BUTTON_FRAC 0.048f
// Bar row centre below the frame's bottom edge
#define BAR_DROP_FRAC (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 0.5f)

// Bar button slots
#define BAR_SLOT_ENV    0
#define BAR_SLOT_COG    1
#define BAR_SLOT_KB     2
#define BAR_SLOT_EXIT   3
#define BAR_SLOT_PAD    4
#define BAR_SLOT_AIM    5
#define BAR_SLOT_RAY    6
#define BAR_SLOT_STEREO 7
#define BAR_SLOTS       8

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

// Button centre in frame-local coordinates, and its side length
void barSlotPlacement(int slot, float width, float height, Vec3* outLocal, float* outSide);

// Hit test for a button, uv from the frame's top left, with hover margin
int barSlotHit(int slot, float u, float v, float width, float height);

// Bar button faces share one single-row atlas (avoids image rect y-origin
// differences). Toggles use two cells: off, then on.
#define BTN_CELL_KB     0
#define BTN_CELL_COG    1
#define BTN_CELL_ENV    2
#define BTN_CELL_EXIT   3
#define BTN_CELL_STEREO 4
#define BTN_CELL_RAY    6
#define BTN_CELL_AIM    8
#define BTN_CELL_PAD    10
#define BTN_CELLS       12
#define BTN_ATLAS_COLS  12
#define BTN_ATLAS_ROWS  1
#define BTN_ATLAS_W     (BTN_ATLAS_COLS * BUTTON_TEX)
#define BTN_ATLAS_H     (BTN_ATLAS_ROWS * BUTTON_TEX)

// Texel offset of a cell (the image rect offset). Returns 0 for an invalid cell.
int buttonCellOrigin(int cell, int* outX, int* outY);

// Copies a top-down RGBA face into its cell, flipped. 0 for an invalid cell.
int buttonCellPut(unsigned char* atlas, int cell, const unsigned char* face);

// Hover target at uv. cornerSide is the bracket size in metres (0 for none);
// corners are numbered TL, TR, BL, BR.
int hoverTest(float u, float v, float width, float height, float cornerSide, int* corner);

// Stand-in screen pose, facing the seat
XrPosef standInPose(void);

// Track position (0..1) to stored integer units and back, and the percent
// shown beside the track
int laneUnits(float t, int min, int max);
float lanePlace(int units, int min, int max);
int lanePercent(int units, int min, int max);

// Settings panel row geometry, as fractions of panel height
float cogRowV(int tab, int row);
float cogRowHalf(int tab);
float cogCellHalf(int tab);

// Track thumb size, fraction of panel height, before hover growth
float cogThumbSize(int tab);

// Track row parts: step-down button, step-up button, and the run between
#define TRACK_PART_NONE 0
#define TRACK_PART_RUN 1
#define TRACK_PART_DOWN 2
#define TRACK_PART_UP 3
int cogTrackPart(float pu);

// Panel u to run position 0..1 and back
float cogRunPlace(float pu);
float cogRunU(float t);

// Next whole step in direction dir from position t, clamped to 0..steps
int cogStepIndex(float t, int steps, int dir);

// Steps for a track row, 0 if not a track
int cogTrackSteps(int tab, int row);

// About tab button hit tests
int cogReportButtonAt(float pu, float pv);
int cogKofiButtonAt(float pu, float pv);

// REPORT_ZONE_ at uv on the report sheet (from top left)
int reportZone(float u, float v);

// HINT_ZONE_ at uv on the hand lock hint (from top left)
int handHintZone(float u, float v);

int kofiSheetZone(float u, float v);

// Stereo separation (fraction of frame width) to stored units (0.1%) and back
int separationUnits(float separation);
float separationOf(int units);

// Preset cell matching a separation, or -1; Balanced wins ties
int cogPresetAt(int units, const int presets[COG_PRESET_CELLS]);

// Room picture size percent, clamped; always the maximum if not resizable
int roomScreenClamp(int percent, int resizable);

// Room picture size during a corner drag, relative to the grab so it does not jump
int roomResizePercent(int startPercent, float startReach, float reach);

// Corner bracket size at a distance, matching the stand-in's angular size
float roomCornerSide(float distance);

#endif
