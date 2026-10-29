package com.limelight.binding.video;

import static com.limelight.binding.video.XrShared.*;

/**
 * The 3D tab's presets as separations, in tenths of a percent: Balanced is the
 * model's default, Comfort and Strong COG_PRESET_STEPS below and above it,
 * clamped to the track.
 */
public final class DepthPresets {

    public static final int NONE = -1;
    public static final int COMFORT = COG_PRESET_COMFORT;
    public static final int BALANCED = COG_PRESET_BALANCED;
    public static final int STRONG = COG_PRESET_STRONG;

    // In cell order, for logs and reports; the panel labels are string resources
    private static final String[] NAMES = { "Comfort", "Balanced", "Strong" };

    private DepthPresets() {
    }

    /** The separation a preset writes, for a model that starts on defaultSeparation. */
    public static int value(int preset, int defaultSeparation) {
        int offset = preset == COMFORT ? -COG_PRESET_STEPS
                : preset == STRONG ? COG_PRESET_STEPS : 0;
        return Math.max(0, Math.min(COG_SEP_STEPS, defaultSeparation + offset));
    }

    /** All three, in cell order. */
    public static int[] values(int defaultSeparation) {
        int[] values = new int[COG_PRESET_CELLS];
        for (int preset = 0; preset < COG_PRESET_CELLS; preset++) {
            values[preset] = value(preset, defaultSeparation);
        }
        return values;
    }

    /**
     * Which preset a separation is, or NONE. Balanced is checked first so it wins
     * when clamping puts another preset on the same value.
     */
    public static int presetFor(int separation, int defaultSeparation) {
        for (int preset : new int[] { BALANCED, COMFORT, STRONG }) {
            if (value(preset, defaultSeparation) == separation) {
                return preset;
            }
        }
        return NONE;
    }

    /** A preset's name for the logs, or "none". */
    public static String name(int preset) {
        return preset >= 0 && preset < NAMES.length ? NAMES[preset] : "none";
    }
}
