// Gamepad mode: both controllers mapped to one Xbox-style pad. The right menu
// button is system-reserved, so the pad/pointer toggle is left menu + grip,
// both stick clicks, or both triggers + grips (one, by setting).

#ifndef XR_GAMEPAD_H
#define XR_GAMEPAD_H

#include <stdint.h>

#include "xr_shared.h"

// Full-scale stick and trigger values
#define PAD_STICK_FULL 32766
#define PAD_TRIGGER_FULL 255
// Trigger dead zone
#define PAD_TRIGGER_DEADZONE 0.13f
// Grip-as-bumper press/release thresholds, same as the pointer trigger
#define PAD_GRIP_ON 0.65f
#define PAD_GRIP_OFF 0.35f
// Default radial stick dead zone, fraction of full tilt
#define PAD_STICK_DEADZONE_DEFAULT 0.07f
// Hold time for the menu + grip toggle
#define PAD_TOGGLE_HOLD_NS 500000000LL

// Held-back analogue controls, above the button bits
#define PAD_HELD_LT 0x10000
#define PAD_HELD_RT 0x20000
#define PAD_HELD_LS 0x40000
#define PAD_HELD_RS 0x80000

// One controller this frame; all zero when absent
typedef struct {
    // 0 to 1
    float trigger;
    float grip;
    // -1 to 1, right and up positive
    float stickX;
    float stickY;
    int stickClick;
    // Face buttons: A/B on the right, X/Y on the left
    int lower;
    int upper;
    // Menu button (left controller only; the right is reserved)
    int menu;
} PadHand;

// Pad output state
typedef struct {
    // PAD_ bits from xr_shared.h
    int buttons;
    // 0 to 255
    int leftTrigger;
    int rightTrigger;
    // -32766 to 32766, right and up positive
    int leftX;
    int leftY;
    int rightX;
    int rightY;
} PadState;

// Grip as a bumper with hysteresis
int padGripDown(float value, int was);

// Stick with a radial dead zone (not rescaled outside it)
void padStick(float x, float y, float deadzone, int* outX, int* outY);

// Trigger to 0..255, 0 within the dead zone
int padTrigger(float value);

// Stick dead zone from the settings percentage, clamped to 1..100
float padDeadzoneFromPercent(int percent);

// Maps both controllers to the pad; menu is Start only when startOk
void padMap(const PadHand* left, const PadHand* right, const int gripDown[2], int startOk,
            float stickDeadzone, PadState* out);

void padRest(PadState* s);
int padAtRest(const PadState* s);
int padSame(const PadState* a, const PadState* b);

// Button bits plus PAD_HELD_ bits for non-resting triggers and sticks
int padHeldIn(const PadState* s);

// Zeroes controls in held until they are released
void padHoldBack(int* held, PadState* s);

// Left menu + left grip toggle state
typedef struct {
    // Both held, and since when
    int holding;
    int64_t sinceNs;
    // Fired during this hold
    int fired;
    // Menu was combined with grip during this press, so it is not Start
    int menuTaken;
} PadToggle;

void padToggleReset(PadToggle* t);

// Returns 1 once per hold; startOk is cleared while menu is combined with grip
int padToggleStep(PadToggle* t, int menu, int grip, int64_t nowNs, int* startOk);

// Chord shortcut parts
#define PAD_PART_LS 0x01
#define PAD_PART_RS 0x02
#define PAD_PART_LT 0x04
#define PAD_PART_RT 0x08
#define PAD_PART_LG 0x10
#define PAD_PART_RG 0x20
#define PAD_PART_COUNT 6
// All parts must be pressed within WINDOW of the first, then held for HOLD
#define PAD_CHORD_WINDOW_NS 150000000LL
#define PAD_CHORD_HOLD_NS 300000000LL

// padChordStep results, for logging
#define PAD_CHORD_NOTHING 0
#define PAD_CHORD_FORMING 1
#define PAD_CHORD_ABANDONED 2
#define PAD_CHORD_FIRED 3

// Chord detector. Parts are suppressed while it forms. If abandoned, held parts
// pass through and released ones replay as a one-frame tap at their peak.
typedef struct {
    // Chord parts; 0 for the menu + grip shortcut or without two controllers
    int parts;
    // Last frame: off rest, and pressed
    int stirred;
    int pressed;
    // Forming state and start, and when all parts were down
    int forming;
    int64_t startNs;
    int allDown;
    int64_t allNs;
    // Suppressed while forming, with each part's peak
    int pending;
    float peak[PAD_PART_COUNT];
    // Suppressed after firing until released; spent until all at rest
    int taken;
    int spent;
    // This frame: parts suppressed, and parts replayed as a tap
    int held;
    int replay;
} PadChord;

// PAD_PART_ bits for a shortcut, 0 for menu + grip
int padChordParts(int shortcut);

// Resets; controls already down must be released before a chord can form
void padChordReset(PadChord* c, int parts);

// Returns a PAD_CHORD_ value; FIRED is the frame to toggle
int padChordStep(PadChord* c, const PadHand* left, const PadHand* right, int64_t nowNs);

// Part value after the chord: 0 if suppressed, its peak if replayed, else raw
float padChordSeen(const PadChord* c, int part, float raw);

// Applies padChordSeen; also centres a stick while its click is suppressed
void padChordApply(const PadChord* c, const PadHand* left, const PadHand* right,
                   PadHand* outLeft, PadHand* outRight);

// Rumble (low motor left, high right) as short pulses re-armed each frame,
// since requests only arrive on change
#define PAD_RUMBLE_PULSE_NS 100000000LL
#define PAD_RUMBLE_REARM_NS 50000000LL

// Per-controller haptics action
#define PAD_RUMBLE_KEEP 0
#define PAD_RUMBLE_APPLY 1
#define PAD_RUMBLE_STOP 2

typedef struct {
    // Requested amplitude, left and right, 0..1, and new since last frame
    float want[2];
    int fresh;
    // Last applied amplitude and time, 0 once stopped
    float given[2];
    int64_t givenNs[2];
} PadRumble;

// Motor level 0..65535 (a Java short read as unsigned) to amplitude 0..1
float padRumbleAmplitude(int motor);

void padRumbleReset(PadRumble* r);

// New request; repeating the same values still re-arms immediately
void padRumbleAsk(PadRumble* r, int lowMotor, int highMotor);

// Per frame; outputs a PAD_RUMBLE_ action and amplitude per controller
void padRumbleStep(PadRumble* r, const int live[2], int64_t nowNs, int action[2], float amp[2]);

#endif
