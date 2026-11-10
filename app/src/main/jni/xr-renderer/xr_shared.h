// Constants shared by the native renderer and Java. The build generates XrShared.java from
// this file, so only plain int/float #defines and expressions over earlier ones are allowed.

#ifndef XR_SHARED_H
#define XR_SHARED_H

// File log levels
#define FILE_LOG_OFF 0
#define FILE_LOG_BASIC 1
#define FILE_LOG_VERBOSE 2

// Return codes for waitBeginFrame
#define FRAME_EXIT   -1
#define FRAME_IDLE    0
#define FRAME_RENDER  1

// Session state used to hold the picture while the headset is off
#define PRESENCE_OTHER   0
#define PRESENCE_FOCUSED 1
#define PRESENCE_AWAY    2

// Synthetic depth patterns for the stereo test path
#define DEPTH_MODE_OFF   0
#define DEPTH_MODE_FLAT  1
#define DEPTH_MODE_RAMP  2
#define DEPTH_MODE_BLOB  3
// Tints each eye instead of warping, to check eye routing
#define DEPTH_MODE_EYETEST 4
// Draws a test bar through the warp and reads back its shift per eye
#define DEPTH_MODE_SHIFTTEST 5
// Depth from the ZipDepth model, run in Java on LiteRT
#define DEPTH_MODE_MODEL 6
// Source is already stereo (SBS or over-under); each eye samples its half, no warp
#define DEPTH_MODE_NATIVE_SBS 7
#define DEPTH_MODE_NATIVE_OU 8

// Depth map size follows the model input, chosen by Java and passed to nativeInit
// (512x288 ZipDepth on Gen 2, 256x256 otherwise). Default is the out-of-range fallback.
#define DEPTH_TEX_SIZE_DEFAULT 256
#define DEPTH_TEX_W_MAX 512
#define DEPTH_TEX_H_MAX 288

// Staging sets (readback, model input, model output), double-buffered against the model
#define DEPTH_PAIRS 2

// Depth maps per second: setting range, default, and the Gen 1 default
#define DEPTH_RATE_MIN 5
#define DEPTH_RATE_MAX 45
#define DEPTH_RATE_DEFAULT 20
#define DEPTH_RATE_GEN1 12

// Launch splash sheet: one row per number of open logo wedges (0..6), so animating is a
// sub-rect change, not an upload; below the rows a glow square for the backdrop, inset
// so filtering stays inside it.
#define SPLASH_WEDGES 6
#define SPLASH_ROWS (SPLASH_WEDGES + 1)
#define SPLASH_TEX_W 384
#define SPLASH_ROW_H 256
#define SPLASH_GROUND_PX 256
#define SPLASH_GROUND_INSET 8
#define SPLASH_TEX_H (SPLASH_ROW_H * SPLASH_ROWS + 8 + SPLASH_GROUND_PX)
// Head-locked, metres
#define SPLASH_GROUND_M 6.0f
#define SPLASH_GROUND_DISTANCE_M 1.5f
#define SPLASH_SHEET_W_M 0.45f
#define SPLASH_SHEET_DISTANCE_M 1.45f

// Slots in the float array returned to Java each frame
#define IN_HIT      0
#define IN_U        1
#define IN_V        2
#define IN_BUTTONS  3
#define IN_SCROLL   4
#define IN_POINTER  5
#define IN_POSE_DIRTY 6
// x y z, orientation quaternion, width, cylinder radius, curvature
#define IN_POSE     7
#define POSE_VALUES 10
// 1 while 3D is on (Java feeds the model only then). Every frame.
#define IN_STEREO   (IN_POSE + POSE_VALUES)
// Player panels: UI_PANEL_ under the ray (or -1), hit point 0..1 from top left, trigger held
#define IN_UI_PANEL (IN_STEREO + 1)
#define IN_UI_U     (IN_UI_PANEL + 1)
#define IN_UI_V     (IN_UI_U + 1)
#define IN_UI_PRESS (IN_UI_V + 1)
// Controller state regardless of the ray: face buttons (A 1, B 2, X 4, Y 8) and the
// larger thumbstick X, -1..1
#define IN_PLAYER_BUTTONS (IN_UI_PRESS + 1)
#define IN_PLAYER_STICK_X (IN_PLAYER_BUTTONS + 1)
// Fingertip touch of the pointing hand: a TOUCH_ value
#define IN_TOUCH    (IN_PLAYER_STICK_X + 1)
// 1 on the one frame the hands are found hard to see (once a session)
#define IN_HANDS_POOR (IN_TOUCH + 1)
#define IN_SLOTS    (IN_HANDS_POOR + 1)
#define TOUCH_NONE 0
#define TOUCH_NEAR 1
#define TOUCH_PRESSING 2

// Player panels, drawn in Java. Subtitles and the browse library scale with the
// screen; the rest keep one pixel size whatever the screen's size.
#define UI_PANEL_TOP 0
#define UI_PANEL_RAIL 1
#define UI_PANEL_CONTROLS 2
#define UI_PANEL_SHEET 3
#define UI_PANEL_SUBS 4
// Spinner over the screen while a film's first frame is late; shown by the renderer
#define UI_PANEL_LOADING 5
// A dialog over the sheet (delete confirmation), floated in front of it
#define UI_PANEL_DIALOG 6
// Back button beside the move bar
#define UI_PANEL_BACK 7
#define UI_PANEL_COUNT 8
// Fixed-size panels: radians per pixel, with a floor as a share of the screen's width so
// a screen brought close and large does not dwarf its menus
#define UI_PX_ANGLE 0.0004f
#define UI_PX_SHARE (1.0f / 2800.0f)
// Panel anchors. The gap is away from the edge, or towards the viewer for FRONT.
#define UI_ANCHOR_ABOVE 0
#define UI_ANCHOR_LEFT 1
#define UI_ANCHOR_BELOW 2
#define UI_ANCHOR_FRONT 3
// Inside the picture, its bottom edge lifted from the picture's by gap (a share of the
// screen width); sized by angle like a fixed panel but never wider than the picture
#define UI_ANCHOR_BOTTOM 4
// Largest panel texture side
#define UI_PANEL_TEX_MAX 2048
// Library window with no video: height/width before the page arrives, and first-run
// width as a share of the screen
#define PLAYER_BROWSE_ASPECT 0.75f
#define PLAYER_BROWSE_START_WIDTH 0.78f

// Picture grade, stored units, global. Brightness: hundredths of full scale, added after
// contrast. Contrast: percent gain about mid grey. Gamma: hundredths. Saturation: percent.
// Defaults are identity.
#define PICTURE_BRIGHTNESS_MIN -50
#define PICTURE_BRIGHTNESS_MAX 50
#define PICTURE_BRIGHTNESS_DEFAULT 0
#define PICTURE_CONTRAST_MIN 50
#define PICTURE_CONTRAST_MAX 150
#define PICTURE_CONTRAST_DEFAULT 100
#define PICTURE_GAMMA_MIN 50
#define PICTURE_GAMMA_MAX 200
#define PICTURE_GAMMA_DEFAULT 100
#define PICTURE_SATURATION_MIN 0
#define PICTURE_SATURATION_MAX 200
#define PICTURE_SATURATION_DEFAULT 100
// Order of the four values everywhere they travel together
#define PICTURE_BRIGHTNESS 0
#define PICTURE_CONTRAST 1
#define PICTURE_GAMMA 2
#define PICTURE_SATURATION 3
#define PICTURE_VALUES 4

// Colour mode: Natural signals BT.709 (accurate); Vivid signals a wider gamut, so the
// display stretches colours toward its own primaries
#define COLOUR_MODE_NATURAL 0
#define COLOUR_MODE_VIVID   1

// Video transfer function from the decoder: SDR, PQ (HDR10, Dolby Vision base) or HLG
#define VIDEO_TRANSFER_CODE_SDR 0
#define VIDEO_TRANSFER_CODE_PQ  1
#define VIDEO_TRANSFER_CODE_HLG 2

#endif
