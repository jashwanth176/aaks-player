// Tests for xr_keys: keyboard sheets and latched Ctrl, Alt and Win.
#include "check.h"
#include "xr_keys.h"

#define KEY_C 'c'
#define KEY_D 'd'
#define KEY_TAB 9
#define KEY_F4 (KB_CODE_VK + 0x73)
#define KEY_ESC (KB_CODE_VK + 0x1B)

static void testSheets(void) {
    // ?123 -> symbols, Fn -> Fn sheet, Fn on Fn -> symbols, ?123/ABC -> letters.
    KbPress p = kbPress(KB_STATE_LOWER, 0, KB_CODE_SYMBOLS);
    CHECK(p.sheet == KB_STATE_SYMBOLS);
    p = kbPress(p.sheet, 0, KB_CODE_FN);
    CHECK(p.sheet == KB_STATE_FN);
    CHECK(kbPress(KB_STATE_FN, 0, KB_CODE_FN).sheet == KB_STATE_SYMBOLS);
    CHECK(kbPress(KB_STATE_FN, 0, KB_CODE_SYMBOLS).sheet == KB_STATE_LOWER);
    CHECK(kbPress(KB_STATE_SYMBOLS, 0, KB_CODE_SYMBOLS).sheet == KB_STATE_LOWER);
    CHECK(kbPress(KB_STATE_UPPER, 0, KB_CODE_SYMBOLS).sheet == KB_STATE_SYMBOLS);
    // Sheet keys type nothing and keep the modifiers.
    p = kbPress(KB_STATE_SYMBOLS, KB_MOD_CTRL, KB_CODE_FN);
    CHECK(p.code == -1);
    CHECK(p.mods == KB_MOD_CTRL);
    CHECK(!p.hide);

    // Shift is one-shot for letters, sticky for the symbol row.
    p = kbPress(KB_STATE_LOWER, 0, KB_CODE_SHIFT);
    CHECK(p.sheet == KB_STATE_UPPER);
    CHECK(kbPress(KB_STATE_UPPER, 0, KB_CODE_SHIFT).sheet == KB_STATE_LOWER);
    p = kbPress(KB_STATE_UPPER, 0, 'Q');
    CHECK(p.code == 'Q');
    CHECK(p.sheet == KB_STATE_LOWER);
    p = kbPress(KB_STATE_UPPER, 0, '!');
    CHECK(p.code == '!');
    CHECK(p.sheet == KB_STATE_UPPER);
}

static void testModifiers(void) {
    // Ctrl+C: latch on Fn, switch to letters, Ctrl goes with c then clears.
    KbPress p = kbPress(KB_STATE_FN, 0, KB_CODE_CTRL);
    CHECK(p.mods == KB_MOD_CTRL);
    CHECK(p.code == -1);
    p = kbPress(p.sheet, p.mods, KB_CODE_SYMBOLS);
    CHECK(p.sheet == KB_STATE_LOWER);
    CHECK(p.mods == KB_MOD_CTRL);
    p = kbPress(p.sheet, p.mods, KEY_C);
    CHECK(p.code == KEY_C);
    CHECK(p.codeMods == KB_MOD_CTRL);
    CHECK(p.mods == 0);

    // Alt+Tab, both on the Fn sheet
    p = kbPress(KB_STATE_FN, 0, KB_CODE_ALT);
    p = kbPress(p.sheet, p.mods, KEY_TAB);
    CHECK(p.code == KEY_TAB);
    CHECK(p.codeMods == KB_MOD_ALT);
    CHECK(p.mods == 0);
    CHECK(p.sheet == KB_STATE_FN);

    // Win+D
    p = kbPress(KB_STATE_FN, 0, KB_CODE_WIN);
    p = kbPress(p.sheet, p.mods, KB_CODE_SYMBOLS);
    p = kbPress(p.sheet, p.mods, KEY_D);
    CHECK(p.code == KEY_D);
    CHECK(p.codeMods == KB_MOD_WIN);
    CHECK(p.mods == 0);

    // Pressing a latched modifier again clears it.
    p = kbPress(KB_STATE_FN, KB_MOD_WIN, KB_CODE_WIN);
    CHECK(p.mods == 0);
    CHECK(p.code == -1);

    // Ctrl and Alt together, then F4
    p = kbPress(KB_STATE_FN, 0, KB_CODE_CTRL);
    p = kbPress(p.sheet, p.mods, KB_CODE_ALT);
    CHECK(p.mods == (KB_MOD_CTRL | KB_MOD_ALT));
    p = kbPress(p.sheet, p.mods, KEY_F4);
    CHECK(p.code == KEY_F4);
    CHECK(p.codeMods == (KB_MOD_CTRL | KB_MOD_ALT));
    CHECK(p.mods == 0);

    // Ctrl+Shift+T: shift keeps Ctrl latched.
    p = kbPress(KB_STATE_LOWER, KB_MOD_CTRL, KB_CODE_SHIFT);
    CHECK(p.mods == KB_MOD_CTRL);
    p = kbPress(p.sheet, p.mods, 'T');
    CHECK(p.code == 'T');
    CHECK(p.codeMods == KB_MOD_CTRL);
    CHECK(p.sheet == KB_STATE_LOWER);
    CHECK(p.mods == 0);

    p = kbPress(KB_STATE_FN, 0, KEY_ESC);
    CHECK(p.code == KEY_ESC);
    CHECK(p.codeMods == 0);

    // Hide clears modifiers.
    p = kbPress(KB_STATE_LOWER, KB_MOD_CTRL | KB_MOD_WIN, KB_CODE_HIDE);
    CHECK(p.hide);
    CHECK(p.mods == 0);
    CHECK(p.code == -1);

    // Code 0 is a blank key.
    p = kbPress(KB_STATE_FN, KB_MOD_ALT, 0);
    CHECK(p.code == -1);
    CHECK(p.mods == KB_MOD_ALT);
    CHECK(p.sheet == KB_STATE_FN);

    CHECK(kbModifierBit(KB_CODE_CTRL) == KB_MOD_CTRL);
    CHECK(kbModifierBit(KB_CODE_ALT) == KB_MOD_ALT);
    CHECK(kbModifierBit(KB_CODE_WIN) == KB_MOD_WIN);
    CHECK(kbModifierBit(KB_CODE_SHIFT) == 0);
    CHECK(kbModifierBit('c') == 0);
}

static void testCodes(void) {
    // Codes are stored in floats; the highest virtual key must survive exactly.
    float top = (float)(KB_CODE_VK + 0xFF);
    CHECK((int)top == KB_CODE_VK + 0xFF);
    // Fixed protocol modifier bits.
    CHECK(KB_MOD_CTRL == 0x02);
    CHECK(KB_MOD_ALT == 0x04);
    CHECK(KB_MOD_WIN == 0x08);
}

int main(void) {
    testSheets();
    testModifiers();
    testCodes();
    return checksDone("xr_keys");
}
