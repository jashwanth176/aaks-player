package com.limelight.binding.video;

import com.limelight.utils.BugReport;

import static com.limelight.binding.video.XrShared.*;

/**
 * The report sheet's note and address fields, filled from the in-world
 * keyboard, and which field has focus. Frame loop only.
 */
final class XrReportForm {
    // The collector truncates the mailed copy at 2000 characters
    static final int NOTE_MAX = 2000;
    static final int ADDRESS_MAX = 254;

    private final StringBuilder note = new StringBuilder();
    private final StringBuilder address = new StringBuilder();
    private int focus = REPORT_ZONE_NOTE;

    /** Clears the note, restores the remembered address, and focuses the note. */
    void open(String rememberedAddress) {
        note.setLength(0);
        address.setLength(0);
        if (rememberedAddress != null) {
            address.append(rememberedAddress.trim());
        }
        focus = REPORT_ZONE_NOTE;
    }

    void focus(int zone) {
        if (zone == REPORT_ZONE_NOTE || zone == REPORT_ZONE_EMAIL) {
            focus = zone;
        }
    }

    /**
     * Applies one keyboard code to the focused field: backspace, tab switches
     * field, enter is a newline in the note and returns to the note from the
     * address, printable characters append. Other keys are ignored. Returns
     * whether anything changed.
     */
    boolean type(int code) {
        StringBuilder into = focus == REPORT_ZONE_NOTE ? note : address;
        int max = focus == REPORT_ZONE_NOTE ? NOTE_MAX : ADDRESS_MAX;
        if (code == 8) {
            if (into.length() == 0) {
                return false;
            }
            into.setLength(into.length() - 1);
            return true;
        }
        if (code == 9) {
            focus = focus == REPORT_ZONE_NOTE ? REPORT_ZONE_EMAIL : REPORT_ZONE_NOTE;
            return true;
        }
        if (code == 13) {
            if (focus == REPORT_ZONE_EMAIL) {
                focus = REPORT_ZONE_NOTE;
                return true;
            }
            return append(into, '\n', max);
        }
        if (code >= 32 && code < KB_CODE_VK && code != 127) {
            return append(into, (char) code, max);
        }
        return false;
    }

    private static boolean append(StringBuilder into, char c, int max) {
        if (into.length() >= max) {
            return false;
        }
        into.append(c);
        return true;
    }

    String note() {
        return note.toString();
    }

    String address() {
        return address.toString();
    }

    int focus() {
        return focus;
    }

    /** A non-empty note, and an address that is valid or empty. */
    boolean canSend() {
        return BugReport.canSend(note.toString(), address.toString());
    }

    /** The address is non-empty but not valid; the sheet flags it. */
    boolean addressWrong() {
        return !BugReport.addressOk(address.toString());
    }
}
