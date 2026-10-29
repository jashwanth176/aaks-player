// Keyboard sheet and modifier logic, see xr_keys.h
#include "xr_keys.h"

int kbModifierBit(int code) {
    switch (code) {
        case KB_CODE_CTRL:
            return KB_MOD_CTRL;
        case KB_CODE_ALT:
            return KB_MOD_ALT;
        case KB_CODE_WIN:
            return KB_MOD_WIN;
        default:
            return 0;
    }
}

KbPress kbPress(int sheet, int mods, int code) {
    KbPress p;
    p.sheet = sheet;
    p.mods = mods;
    p.code = -1;
    p.codeMods = 0;
    p.hide = 0;

    int bit = kbModifierBit(code);
    if (bit != 0) {
        p.mods = mods ^ bit;
    }
    else if (code == KB_CODE_SHIFT) {
        // Shift from the symbols sheet goes to capitals
        p.sheet = sheet == KB_STATE_UPPER ? KB_STATE_LOWER : KB_STATE_UPPER;
    }
    else if (code == KB_CODE_SYMBOLS) {
        // ABC on the symbols and Fn sheets, ?123 on the letters
        p.sheet = sheet == KB_STATE_SYMBOLS || sheet == KB_STATE_FN ? KB_STATE_LOWER
                                                                    : KB_STATE_SYMBOLS;
    }
    else if (code == KB_CODE_FN) {
        // Fn on the symbols sheet, ?123 in the same slot on the Fn sheet
        p.sheet = sheet == KB_STATE_FN ? KB_STATE_SYMBOLS : KB_STATE_FN;
    }
    else if (code == KB_CODE_HIDE) {
        p.hide = 1;
        p.mods = 0;
    }
    else if (code > 0) {
        p.code = code;
        p.codeMods = mods;
        p.mods = 0;
        // Shift is one-shot for letters, sticky for the punctuation row
        if (sheet == KB_STATE_UPPER && code >= 'A' && code <= 'Z') {
            p.sheet = KB_STATE_LOWER;
        }
    }
    return p;
}
