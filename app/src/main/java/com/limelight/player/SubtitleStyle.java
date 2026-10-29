package com.limelight.player;

import android.content.Context;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;

/**
 * Subtitle appearance: size, colour, background, edge, height, depth, and whether the file's
 * own styling (italics, bold, colours) is kept. Persisted in shared preferences.
 */
final class SubtitleStyle {

    static final String[] SIZE_NAMES = { "Small", "Medium", "Large", "Huge" };
    static final int[] SIZES = { 44, 56, 72, 92 };
    static final String[] COLOUR_NAMES = { "White", "Yellow", "Cyan", "Green" };
    static final int[] COLOURS = { 0xFFFFFFFF, 0xFFFFE24A, 0xFF8FE9FF, 0xFFA8F59A };
    static final String[] BACKGROUND_NAMES = { "None", "Soft", "Solid" };
    static final int[] BACKGROUNDS = { 0x00000000, 0x8C000000, 0xEB000000 };
    static final String[] EDGE_NAMES = { "None", "Shadow", "Outline" };
    static final String[] HEIGHT_NAMES = { "Low", "Middle", "High" };
    // Lift as a fraction of screen width
    static final float[] LIFTS = { 0.0f, 0.07f, 0.15f };

    // Distance in front of the screen as a fraction of viewing distance (~15/30/50 cm at 2.5 m)
    static final String[] DEPTH_NAMES = { "Screen", "Near", "Mid", "Far" };
    static final float[] DEPTHS = { 0.0f, 0.06f, 0.12f, 0.2f };

    static final int EDGE_NONE = 0, EDGE_SHADOW = 1, EDGE_OUTLINE = 2;

    private static final String PREF = "player_sub_";

    int size = 1;
    int colour = 0;
    int background = 1;
    int edge = 1;
    int height = 0;
    int depth = 0;
    boolean fileStyling = true;

    int textPx() {
        return SIZES[size];
    }

    int colourArgb() {
        return COLOURS[colour];
    }

    int plateArgb() {
        return BACKGROUNDS[background];
    }

    float lift() {
        return LIFTS[height];
    }

    float depthK() {
        return DEPTHS[depth];
    }

    /** Scale relative to the Medium size, for bitmap subtitles. */
    float scale() {
        return SIZES[size] / (float) SIZES[1];
    }

    static SubtitleStyle load(Context context) {
        SharedPreferences p = PreferenceManager.getDefaultSharedPreferences(context);
        SubtitleStyle s = new SubtitleStyle();
        s.size = clamp(p.getInt(PREF + "size", 1), SIZES.length);
        s.colour = clamp(p.getInt(PREF + "colour", 0), COLOURS.length);
        s.background = clamp(p.getInt(PREF + "background", 1), BACKGROUNDS.length);
        s.edge = clamp(p.getInt(PREF + "edge", 1), EDGE_NAMES.length);
        s.height = clamp(p.getInt(PREF + "height", 0), LIFTS.length);
        s.depth = clamp(p.getInt(PREF + "depth", 0), DEPTHS.length);
        s.fileStyling = p.getBoolean(PREF + "file", true);
        return s;
    }

    void save(Context context) {
        PreferenceManager.getDefaultSharedPreferences(context).edit()
                .putInt(PREF + "size", size)
                .putInt(PREF + "colour", colour)
                .putInt(PREF + "background", background)
                .putInt(PREF + "edge", edge)
                .putInt(PREF + "height", height)
                .putInt(PREF + "depth", depth)
                .putBoolean(PREF + "file", fileStyling)
                .apply();
    }

    private static int clamp(int v, int count) {
        return Math.max(0, Math.min(count - 1, v));
    }
}
