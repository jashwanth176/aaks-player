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
// Depth from a model (ZipDepth or MiDaS) run in Java on LiteRT
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

// Stats overlay texture
#define OVERLAY_WIDTH 768
#define OVERLAY_HEIGHT 512

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

// Toast sheet and notice kinds. TOAST_RATE's arg is Hz; TOAST_TEXT's arg is a Java slot.
#define TOAST_TEX_W 1024
#define TOAST_TEX_H 160
#define TOAST_RATE 0
#define TOAST_HANDS_LOCKED 1
#define TOAST_HANDS_UNLOCKED 2
#define TOAST_3D_OFF 3
#define TOAST_3D_ON 4
#define TOAST_TEXT 5
#define TOAST_TEXT_SLOTS 4
#define TOAST_HEAD_AIM_OFF 6
#define TOAST_HEAD_AIM_ON 7
#define TOAST_GAMEPAD_MODE 8
#define TOAST_POINTER_MODE 9

// Slots in the float array returned to Java each frame
#define IN_HIT      0
#define IN_U        1
#define IN_V        2
#define IN_BUTTONS  3
#define IN_SCROLL   4
#define IN_POINTER  5
#define IN_POSE_DIRTY 6
// Setting just changed, or -1 (0 is a valid id)
#define IN_SETTING  7
// x y z, orientation quaternion, width, cylinder radius, curvature
#define IN_POSE     8
#define POSE_VALUES 10
// Environment grid cell just chosen, or -1
#define IN_PICKER_PICK 18
#define IN_SETTING_VALUE 19
// Key typed on the in-world keyboard, or -1: Unicode with shift applied, or a control code
#define IN_KEY      20
// 1 on the frame the exit prompt is confirmed
#define IN_EXIT     21
// Head yaw from the screen, radians, positive left; 0 when head-locked. Every frame.
#define IN_HEAD_YAW 22
// Picker cell of the room the setting belongs to, or -1. Every frame.
#define IN_SETTING_ROOM 23
// Readout strip: which READOUT_ tab (or -1), then one value per row. Room: percent
// (-1 for a fixed-size room); Picture: whole units; Screen: head aim values.
#define IN_READOUT  24
#define READOUT_VALUES 5
#define READOUT_ROOM 0
#define READOUT_PICTURE 1
#define READOUT_SCREEN 2
// 1 while 3D is on (Java feeds the model only then). Every frame.
#define IN_STEREO   (IN_READOUT + READOUT_VALUES)
// Toast to show this frame: a TOAST_ kind or -1, and its argument
#define IN_TOAST    (IN_STEREO + 1)
#define IN_TOAST_ARG (IN_TOAST + 1)
// 1 on the frame a press lands on a control, so Java can play the click
#define IN_CLICK    (IN_TOAST_ARG + 1)
// Display tab: selected cell per option row, -1 for a greyed row; first value -1 when the
// tab is hidden. MARK_VALUES must equal COG_OPTION_COUNT (checked natively).
#define IN_MARKS    (IN_CLICK + 1)
#define MARK_VALUES 12
// 1 while the settings panel is visible (fade included), for the clock strip
#define IN_COG_OPEN (IN_MARKS + MARK_VALUES)
// Keyboard: KB_MOD_ bits held with this frame's key, bits currently lit, and the sheet
// shown (-1 when hidden)
#define IN_KEY_MODS (IN_COG_OPEN + 1)
#define IN_KB_MODS  (IN_KEY_MODS + 1)
#define IN_KB_SHEET (IN_KB_MODS + 1)
// Report sheet: zone pressed this frame (REPORT_ zone, REPORT_OPENED or -1), then the
// zone under the ray every frame (-1 when hidden)
#define IN_REPORT   (IN_KB_SHEET + 1)
#define IN_REPORT_ZONE (IN_REPORT + 1)
// 1 while head aim is active (head-locked screen, no room). Every frame.
#define IN_HEAD_AIM (IN_REPORT_ZONE + 1)
// Relative mouse motion this frame in whole pixels, right/down positive
#define IN_MOUSE_DX (IN_HEAD_AIM + 1)
#define IN_MOUSE_DY (IN_MOUSE_DX + 1)
// Gamepad mode: 1 while active, then PAD_ bits, triggers 0..255 and sticks
// -32766..32766 (right/up positive); at rest while a panel is up or unfocused
#define IN_PAD      (IN_MOUSE_DY + 1)
#define IN_PAD_BUTTONS (IN_PAD + 1)
#define IN_PAD_LT   (IN_PAD_BUTTONS + 1)
#define IN_PAD_RT   (IN_PAD_LT + 1)
#define IN_PAD_LX   (IN_PAD_RT + 1)
#define IN_PAD_LY   (IN_PAD_LX + 1)
#define IN_PAD_RX   (IN_PAD_LY + 1)
#define IN_PAD_RY   (IN_PAD_RX + 1)
// 1 on the frame the hand-lock hint is dismissed with "Don't show this again"
#define IN_HINT     (IN_PAD_RY + 1)
// Player panels: UI_PANEL_ under the ray (or -1), hit point 0..1 from top left, trigger held
#define IN_UI_PANEL (IN_HINT + 1)
#define IN_UI_U     (IN_UI_PANEL + 1)
#define IN_UI_V     (IN_UI_U + 1)
#define IN_UI_PRESS (IN_UI_V + 1)
// Controller state regardless of the ray: face buttons (A 1, B 2, X 4, Y 8) and the
// larger thumbstick X, -1..1
#define IN_PLAYER_BUTTONS (IN_UI_PRESS + 1)
#define IN_PLAYER_STICK_X (IN_PLAYER_BUTTONS + 1)
#define IN_SLOTS    (IN_PLAYER_STICK_X + 1)

// Player panels, drawn in Java. Width and edge gap scale with the screen width.
#define UI_PANEL_TOP 0
#define UI_PANEL_RAIL 1
#define UI_PANEL_CONTROLS 2
#define UI_PANEL_SHEET 3
#define UI_PANEL_SUBS 4
#define UI_PANEL_COUNT 5
// Panel anchors. The gap is away from the edge, or towards the viewer for FRONT.
#define UI_ANCHOR_ABOVE 0
#define UI_ANCHOR_LEFT 1
#define UI_ANCHOR_BELOW 2
#define UI_ANCHOR_FRONT 3
// Largest panel texture side
#define UI_PANEL_TEX_MAX 2048
// Library window with no video: height/width before the page arrives, and first-run
// width as a share of the screen
#define PLAYER_BROWSE_ASPECT 0.75f
#define PLAYER_BROWSE_START_WIDTH 0.78f

// Gamepad buttons, as in ControllerPacket's flags
#define PAD_START   0x0010
#define PAD_LS_CLICK 0x0040
#define PAD_RS_CLICK 0x0080
#define PAD_LB      0x0100
#define PAD_RB      0x0200
#define PAD_A       0x1000
#define PAD_B       0x2000
#define PAD_X       0x4000
#define PAD_Y       0x8000
#define PAD_BUTTONS (PAD_START + PAD_LS_CLICK + PAD_RS_CLICK + PAD_LB + PAD_RB + PAD_A + PAD_B + PAD_X + PAD_Y)

// Controller shortcut to toggle pointer/gamepad
#define PAD_SHORTCUT_MENU_GRIP 0
#define PAD_SHORTCUT_STICKS 1
#define PAD_SHORTCUT_TRIGGERS_GRIPS 2

// Settings Java applies and stores
#define SETTING_SHARPEN 0
#define SETTING_STATS   1
#define SETTING_SEPARATION 2
#define SETTING_CONVERGENCE 3
#define SETTING_RESET_3D 4
#define SETTING_AMBILIGHT 5
#define SETTING_AMBI_LEVEL 6
#define SETTING_ROOM_LIGHT 7
#define SETTING_HEAD_LOCK 8
#define SETTING_SUPERSAMPLE 9
// Per-room values, stored under the room id (IN_SETTING_ROOM)
#define SETTING_ROOM_BRIGHTNESS 10
#define SETTING_ROOM_GLOW 11
#define SETTING_ROOM_LIGHT_LEVEL 12
#define SETTING_ROOM_SCREEN 13
// Off 0, On 1
#define SETTING_POINTER_SLEEP 14
// Off 0, On 1
#define SETTING_CLICK_SOUND 15
// Picture grade values in PICTURE_ order, then reset all four
#define SETTING_PICTURE_BRIGHTNESS 16
#define SETTING_PICTURE_CONTRAST 17
#define SETTING_PICTURE_GAMMA 18
#define SETTING_PICTURE_SATURATION 19
#define SETTING_RESET_PICTURE 20
// Off 0, On 1
#define SETTING_CONTROLLER_MODEL 21
// Head aim sensitivity and dead zone, in HEAD_AIM_ units
#define SETTING_HEAD_AIM_SENSITIVITY 22
#define SETTING_HEAD_AIM_DEADZONE 23

// Room tab ranges, in stored units. Brightness: hundredths of the baked room.
// Light: hundredths of the room's gain. Screen: percent of the room's screen anchor.
#define ROOM_BRIGHTNESS_MIN 5
#define ROOM_BRIGHTNESS_MAX 165
#define ROOM_LIGHT_MIN 0
#define ROOM_LIGHT_MAX 200
#define ROOM_LIGHT_DEFAULT 150
#define ROOM_SCREEN_MIN 25
#define ROOM_SCREEN_MAX 100

// Per-room defaults; xr_room.c's room table and the Java preferences both use these.
// Glow and resizable are 0/1.
#define ROOM_THEATER_BRIGHTNESS 37
#define ROOM_THEATER_SCREEN 80
#define ROOM_THEATER_GLOW 1
#define ROOM_THEATER_RESIZABLE 1
#define ROOM_GRAND_CINEMA_BRIGHTNESS 21
#define ROOM_GRAND_CINEMA_SCREEN 100
#define ROOM_GRAND_CINEMA_GLOW 0
#define ROOM_GRAND_CINEMA_RESIZABLE 0
#define ROOM_SYNTHWAVE_BRIGHTNESS 53
#define ROOM_SYNTHWAVE_SCREEN 100
#define ROOM_SYNTHWAVE_GLOW 1
#define ROOM_SYNTHWAVE_RESIZABLE 1

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

// Head aim: pixels per degree, and dead zone in degrees per second (absorbs tracking jitter)
#define HEAD_AIM_SENSITIVITY_MIN 1
#define HEAD_AIM_SENSITIVITY_MAX 30
#define HEAD_AIM_SENSITIVITY_DEFAULT 8
#define HEAD_AIM_DEADZONE_MIN 0
#define HEAD_AIM_DEADZONE_MAX 20
#define HEAD_AIM_DEADZONE_DEFAULT 2

// Environment resolution tiers
#define ENV_RES_LOW 0
#define ENV_RES_STANDARD 1
#define ENV_RES_HIGH 2
#define ENV_RES_ULTRA 3

// Environment picker: one Java-drawn sheet (header strip plus one row of cells);
// hover/selection outlines are separate quads to avoid uploads.
#define PICKER_COLS 2
#define PICKER_ROWS 1
#define PICKER_CELLS (PICKER_COLS * PICKER_ROWS)
#define PICKER_HEADER_PX 40
#define PICKER_CELL_PX 256
// Derived from the cell size so cells stay square
#define PICKER_TEX_W (PICKER_CELL_PX * PICKER_COLS)
#define PICKER_BAND_PX (PICKER_HEADER_PX + PICKER_CELL_PX)
#define PICKER_TEX_H (PICKER_BAND_PX * PICKER_ROWS)
// Grid cells in draw order. Saved values are the stable ids the cells map to.
#define ENV_CELL_PASSTHROUGH 0
#define ENV_CELL_VOID 1
#define ENV_CELL_COUNT 2

#define BUTTON_TEX 128

// Settings panel: Java-drawn sheet on one quad; thumbs are separate quads
#define COG_TEX_W 768
#define COG_TEX_H 640

// Layout, as fractions of the panel texture
#define COG_TRACK_L 0.42f
#define COG_TRACK_R 0.93f
// Step buttons at each end of a track; the thumb's run is inset so it clears them
#define COG_CHEVRON_W 0.036f
#define COG_CHEVRON_REACH 0.02f
#define COG_RUN_INSET 0.075f
#define COG_RUN_L (COG_TRACK_L + COG_RUN_INSET)
#define COG_RUN_R (COG_TRACK_R - COG_RUN_INSET)
// Tab bar height; tabs split it evenly
#define COG_TAB_BAR_B 0.16f
// Row layout for the 3D, Picture and Room tabs
#define COG_ROW_V0 0.25f
#define COG_ROW_STEP 0.11f
// Half height of a row's hit band; under half the pitch so bands don't overlap
#define COG_ROW_HALF 0.05f
#define COG_RESET_L 0.35f
#define COG_RESET_R 0.65f
// Below the last row (0.80 + half band)
#define COG_RESET_T 0.87f
#define COG_RESET_B 0.97f
// Matches the drawn ring
#define COG_CELL_HALF 0.045f

// One preloaded sheet per tab; switching is one upload
#define COG_TAB_SCREEN  0
#define COG_TAB_DISPLAY 1
#define COG_TAB_3D      2
#define COG_TAB_PICTURE 3
#define COG_TAB_ABOUT   4
#define COG_TAB_COUNT   5
// Room variants: the first tab becomes the Room tab, the others carry the room name
#define COG_ART_ROOM         5
#define COG_ART_ROOM_FIXED   6
#define COG_ART_ROOM_DISPLAY 7
#define COG_ART_ROOM_3D      8
#define COG_ART_ROOM_PICTURE 9
#define COG_ART_ROOM_ABOUT   10
// Screen and display tabs with head aim greyed (screen not head-locked)
#define COG_ART_SCREEN_WORLD 11
#define COG_ART_DISPLAY_WORLD 12
#define COG_ART_COUNT        13

// Screen tab rows: six placement rows (reset by the button), then head aim's two
#define COG_SLIDER_DISTANCE 0
#define COG_SLIDER_HEIGHT   1
#define COG_SLIDER_TILT     2
#define COG_SLIDER_ROTATE   3
#define COG_SLIDER_CURVE    4
#define COG_SLIDER_SIZE     5
#define COG_SLIDER_COUNT    6
#define COG_SLIDER_AIM_SENSITIVITY 6
#define COG_SLIDER_AIM_DEADZONE 7
#define COG_SCREEN_ROW_COUNT 8
// Eight rows: tighter spacing; last band ends at 0.8575, above the reset button
#define COG_SCREEN_ROW_V0 0.22f
#define COG_SCREEN_ROW_STEP 0.085f
#define COG_SCREEN_ROW_HALF 0.0425f
#define COG_SCREEN_CELL_HALF 0.036f

// Room tab rows (shown in a room instead of the screen tab)
#define COG_ROOM_ROW_BRIGHTNESS 0
#define COG_ROOM_ROW_GLOW 1
#define COG_ROOM_ROW_LIGHT 2
#define COG_ROOM_ROW_LIGHT_LEVEL 3
#define COG_ROOM_ROW_SIZE 4
#define COG_ROOM_ROW_COUNT 5
#define COG_ROOM_SWITCH_CELLS 2
// Readout strip beside the Room and Picture tracks, its own small texture so a drag
// doesn't re-upload the sheet. Position as fractions of the panel.
#define COG_READOUT_TEX_W 96
#define COG_READOUT_TEX_H 448
#define COG_READOUT_L 0.26f
#define COG_READOUT_T 0.19f

// 3D tab rows: presets, two sliders, on/off switch. Only values that apply live.
#define COG_ROW3D_PRESET 0
#define COG_ROW3D_SEPARATION 1
#define COG_ROW3D_CONVERGENCE 2
#define COG_ROW3D_SWITCH 3
#define COG_ROW3D_COUNT 4
#define COG_STEREO_CELLS 2
// Presets, as separations: Balanced is the model default, Comfort/Strong are
// COG_PRESET_STEPS track steps below/above it
#define COG_PRESET_COMFORT 0
#define COG_PRESET_BALANCED 1
#define COG_PRESET_STRONG 2
#define COG_PRESET_CELLS 3
#define COG_PRESET_STEPS 3
// Separation track end, fraction of frame width: 3x the useful maximum of 0.5%,
// with the far end drawn marked
#define COG_SEP_MAX 0.015f
// Snaps drags to the stored 0.1% steps
#define COG_SEP_STEPS 15

// Picture tab rows: the four PICTURE_ values, each with a tick at identity, over reset

// About tab button positions
#define COG_KOFI_L 0.25f
#define COG_KOFI_R 0.75f
#define COG_KOFI_T 0.45f
#define COG_KOFI_B 0.57f
#define COG_REPORT_L 0.25f
#define COG_REPORT_R 0.75f
#define COG_REPORT_T 0.69f
#define COG_REPORT_B 0.81f

// Display tab option rows (cells). Head aim, gamepad and ray are per session, not stored.
#define COG_OPTION_SHARPEN 0
#define COG_OPTION_SUPERSAMPLE 1
#define COG_OPTION_STATS   2
#define COG_OPTION_HEAD_LOCK 3
#define COG_OPTION_HEAD_AIM 4
#define COG_OPTION_GAMEPAD 5
#define COG_OPTION_POINTER_SLEEP 6
#define COG_OPTION_RAY 7
#define COG_OPTION_CONTROLLERS 8
#define COG_OPTION_CLICK_SOUND 9
#define COG_OPTION_AMBILIGHT 10
#define COG_OPTION_ROOM_LIGHT 11
#define COG_OPTION_COUNT   12
#define COG_SHARPEN_CELLS 3
#define COG_SUPERSAMPLE_CELLS 3
#define COG_STATS_CELLS   2
#define COG_HEAD_LOCK_CELLS 2
#define COG_HEAD_AIM_CELLS 2
#define COG_GAMEPAD_CELLS 2
#define COG_POINTER_SLEEP_CELLS 2
#define COG_RAY_CELLS 2
#define COG_CONTROLLERS_CELLS 2
#define COG_CLICK_SOUND_CELLS 2
#define COG_AMBI_CELLS    2
#define COG_ROOM_LIGHT_CELLS 2
// The display tab's one track row, below the options
#define COG_DISPLAY_SLIDER_ROW 12
// Thirteen rows: tighter spacing; hit band is half the pitch
#define COG_DISPLAY_ROW_V0 0.197f
#define COG_DISPLAY_ROW_STEP 0.06f
#define COG_DISPLAY_ROW_HALF 0.03f
#define COG_DISPLAY_CELL_HALF 0.025f
// Selection marks for the display tab, one Java-drawn strip over the cell column.
// Position as fractions of the panel.
#define COG_MARKS_TEX_W 408
#define COG_MARKS_TEX_H 460
#define COG_MARKS_L 0.41f
#define COG_MARKS_T 0.17f
// Clock and battery strip above the panel, redrawn when either changes
#define COG_CLOCK_TEX_W 384
#define COG_CLOCK_TEX_H 48

// In-world keyboard: one Java-drawn sheet per state; Java also supplies key rects and
// codes. FN holds Esc, F keys, arrows and the sticky Ctrl/Alt/Win.
#define KB_TEX_W 1120
#define KB_TEX_H 448
#define KB_STATE_LOWER   0
#define KB_STATE_UPPER   1
#define KB_STATE_SYMBOLS 2
#define KB_STATE_FN      3
#define KB_STATE_COUNT   4
// Negative codes switch the keyboard; 0 is blank. 8..KB_CODE_VK-1 are characters;
// KB_CODE_VK + n is Windows virtual key n.
#define KB_CODE_SHIFT   -2
#define KB_CODE_SYMBOLS -3
#define KB_CODE_HIDE    -4
#define KB_CODE_FN      -5
#define KB_CODE_CTRL    -6
#define KB_CODE_ALT     -7
#define KB_CODE_WIN     -8
#define KB_CODE_VK      65536
// Modifier bits
#define KB_MOD_CTRL 2
#define KB_MOD_ALT  4
#define KB_MOD_WIN  8

// Report sheet, shown above the keyboard and redrawn in Java on every change.
// Positions as fractions of the sheet.
#define REPORT_TEX_W 1024
#define REPORT_TEX_H 640
#define REPORT_FIELD_L 0.05f
#define REPORT_FIELD_R 0.95f
#define REPORT_NOTE_T 0.175f
#define REPORT_NOTE_B 0.425f
#define REPORT_EMAIL_T 0.50f
#define REPORT_EMAIL_B 0.5875f
#define REPORT_BTN_T 0.8125f
#define REPORT_BTN_B 0.9375f
#define REPORT_CANCEL_L 0.05f
#define REPORT_CANCEL_R 0.47f
#define REPORT_SEND_L 0.53f
#define REPORT_SEND_R 0.95f
// Press zones (also the IN_REPORT values) and the open event
#define REPORT_ZONE_NONE 0
#define REPORT_ZONE_NOTE 1
#define REPORT_ZONE_EMAIL 2
#define REPORT_ZONE_CANCEL 3
#define REPORT_ZONE_SEND 4
#define REPORT_OPENED 5

// Exit prompt: one Java-drawn sheet per highlighted button
#define EXIT_TEX_W 512
#define EXIT_TEX_H 256
// Ray zone; also indexes the sheet drawn with that zone highlighted
#define EXIT_ZONE_NONE   0
#define EXIT_ZONE_EXIT   1
#define EXIT_ZONE_CANCEL 2
#define EXIT_ART_COUNT   3
// Button positions, as fractions of the sheet
#define EXIT_BTN_T 0.56f
#define EXIT_BTN_B 0.86f
#define EXIT_EXIT_L 0.08f
#define EXIT_EXIT_R 0.46f
#define EXIT_CANCEL_L 0.54f
#define EXIT_CANCEL_R 0.92f

// Hand-lock hint (shown once per session on first point): text over OK and
// "Don't show this again". Button positions as fractions of the sheet.
#define HINT_TEX_W 1024
#define HINT_TEX_H 448
#define HINT_BTN_T 0.70f
#define HINT_BTN_B 0.88f
#define HINT_OK_L 0.06f
#define HINT_OK_R 0.36f
#define HINT_NEVER_L 0.42f
#define HINT_NEVER_R 0.94f
#define HINT_ZONE_NONE  0
#define HINT_ZONE_OK    1
#define HINT_ZONE_NEVER 2

// Sheet opened from the About tab: QR code drawn 1:1 with its top left at KOFI_QR_L/T,
// one Close button. Positions as fractions of the sheet.
#define KOFI_TEX_W 1024
#define KOFI_TEX_H 512
#define KOFI_QR_PX 296
#define KOFI_QR_L 0.05f
#define KOFI_QR_T 0.22f
#define KOFI_BTN_T 0.76f
#define KOFI_BTN_B 0.92f
#define KOFI_CLOSE_L 0.64f
#define KOFI_CLOSE_R 0.95f
#define KOFI_ZONE_NONE  0
#define KOFI_ZONE_CLOSE 1

// Colour mode: Natural signals BT.709 (accurate); Vivid signals a wider gamut, so the
// display stretches colours toward its own primaries
#define COLOUR_MODE_NATURAL 0
#define COLOUR_MODE_VIVID   1

// Video transfer function from the decoder: SDR, PQ (HDR10, Dolby Vision base) or HLG
#define VIDEO_TRANSFER_CODE_SDR 0
#define VIDEO_TRANSFER_CODE_PQ  1
#define VIDEO_TRANSFER_CODE_HLG 2

#endif
