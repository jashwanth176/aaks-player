// In-world keyboard rules: the sheet after a press, the latched Ctrl/Alt/Win
// modifiers, and what is typed. Key codes come from the Java layout.

#ifndef XR_KEYS_H
#define XR_KEYS_H

#include "xr_shared.h"

// Result of one key press
typedef struct {
    // Sheet shown afterwards, a KB_STATE_
    int sheet;
    // Latched modifiers afterwards, KB_MOD_ bits
    int mods;
    // Code to type (-1 for none) and the modifiers sent with it
    int code;
    int codeMods;
    // 1 if the keyboard was hidden
    int hide;
} KbPress;

// Modifier keys toggle their bit. A typing key is sent with the latched
// modifiers and clears them. Sheet keys leave modifiers latched; hiding the
// keyboard clears them.
KbPress kbPress(int sheet, int mods, int code);

// KB_MOD_ bit for a modifier key code, or 0
int kbModifierBit(int code);

#endif
