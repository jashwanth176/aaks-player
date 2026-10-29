package com.limelight.player;

import java.util.Locale;

/** Blu-ray playlist stream coding types and their display names. */
final class DiscNativeCodings {

    private DiscNativeCodings() {
    }

    static final int LPCM = 0x80;
    static final int AC3 = 0x81;
    static final int DTS = 0x82;
    static final int TRUEHD = 0x83;
    static final int EAC3 = 0x84;
    static final int DTSHD = 0x85;
    static final int DTSHD_MA = 0x86;
    static final int EAC3_SECONDARY = 0xa1;
    static final int DTSHD_SECONDARY = 0xa2;
    static final int PGS = 0x90;
    static final int IGS = 0x91;
    static final int TEXT = 0x92;

    static String audioName(int coding) {
        switch (coding) {
            case LPCM: return "LPCM";
            case AC3: return "Dolby Digital";
            case DTS: return "DTS";
            case TRUEHD: return "Dolby TrueHD";
            case EAC3:
            case EAC3_SECONDARY: return "Dolby Digital Plus";
            case DTSHD: return "DTS-HD";
            case DTSHD_MA: return "DTS-HD MA";
            case DTSHD_SECONDARY: return "DTS-HD";
            default: return "Audio";
        }
    }

    /** Display name for a playlist audio channel-layout code. */
    static String channelName(int code) {
        switch (code) {
            case 1: return "Mono";
            case 3: return "Stereo";
            case 6:
            case 12: return "5.1";
            case 14: return "7.1";
            default: return "";
        }
    }

    /** Display name for a three-letter language code. */
    static String languageName(String code) {
        if (code == null || code.isEmpty() || code.equals("und")) {
            return "";
        }
        // Disc codes are ISO 639-2, which Locale also accepts
        String name = new Locale(code).getDisplayLanguage(Locale.ENGLISH);
        if (name == null || name.isEmpty() || name.equalsIgnoreCase(code)) {
            return code.toUpperCase(Locale.ROOT);
        }
        return Character.toUpperCase(name.charAt(0)) + name.substring(1);
    }
}
