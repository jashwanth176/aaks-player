package com.limelight.binding.video;

import android.content.SharedPreferences;

import com.limelight.preferences.PreferenceConfiguration;

import static com.limelight.binding.video.XrShared.*;

/**
 * Maps environment picker cells to saved environment ids. Cells are grid
 * positions; ids are stable, so the preference stores ids and everything
 * else in the renderer works in cells.
 */
public final class EnvironmentIds {

    // Ids of removed environments, never reused, so a saved one means "gone"
    static final int RETIRED_MINIMAL_ROOM = 2;
    static final int RETIRED_PSX_CINEMA = 3;
    static final int RETIRED_FIRST_PHOTO = 100;

    // The original grid layout, only to read an old saved cell
    private static final int[] LEGACY_CELL_IDS = {
            PreferenceConfiguration.VR_ENV_PASSTHROUGH,
            PreferenceConfiguration.VR_ENV_VOID,
            RETIRED_FIRST_PHOTO,
            RETIRED_FIRST_PHOTO + 1,
            RETIRED_FIRST_PHOTO + 2,
            RETIRED_FIRST_PHOTO + 3,
            RETIRED_MINIMAL_ROOM,
            RETIRED_PSX_CINEMA,
    };

    private EnvironmentIds() {
    }

    // A cell that is a full 3D room. None ship, so always false.
    static boolean isRoomCell(int cell) {
        return false;
    }

    static int idForCell(int cell) {
        switch (cell) {
            case ENV_CELL_PASSTHROUGH: return PreferenceConfiguration.VR_ENV_PASSTHROUGH;
            case ENV_CELL_VOID: return PreferenceConfiguration.VR_ENV_VOID;
            default: return -1;
        }
    }

    static int cellForId(int id) {
        switch (id) {
            case PreferenceConfiguration.VR_ENV_PASSTHROUGH: return ENV_CELL_PASSTHROUGH;
            case PreferenceConfiguration.VR_ENV_VOID: return ENV_CELL_VOID;
            default: return -1;
        }
    }

    // The starting cell: with no saved id the passthrough setting decides, and
    // an id that names nothing offered starts in the void
    static int startCell(int id, boolean passthrough) {
        if (id < 0) {
            return passthrough ? ENV_CELL_PASSTHROUGH : ENV_CELL_VOID;
        }
        int cell = cellForId(id);
        return cell >= 0 ? cell : ENV_CELL_VOID;
    }

    // The id an old saved cell meant, or -1
    static int idForLegacyCell(int legacy) {
        return legacy >= 0 && legacy < LEGACY_CELL_IDS.length ? LEGACY_CELL_IDS[legacy] : -1;
    }

    /**
     * The list value for the environment the next session opens with: the saved
     * id, else an old saved cell, else the passthrough setting. Read only.
     */
    public static String listValue(SharedPreferences prefs) {
        return listValue(prefs, prefs.getBoolean(PreferenceConfiguration.VR_PASSTHROUGH_PREF_STRING,
                PreferenceConfiguration.DEFAULT_VR_PASSTHROUGH));
    }

    /** The same with the passthrough setting given explicitly. */
    public static String listValue(SharedPreferences prefs, boolean passthrough) {
        int id = prefs.getInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, -1);
        if (id < 0) {
            id = idForLegacyCell(prefs.getInt(PreferenceConfiguration.VR_ENVIRONMENT_PREF_STRING, -1));
        }
        return Integer.toString(idForCell(startCell(id, passthrough)));
    }

    /** The id a list value names, or -1 for one that names nothing offered. */
    public static int idForListValue(String value) {
        if (value == null) {
            return -1;
        }
        try {
            int id = Integer.parseInt(value.trim());
            return cellForId(id) >= 0 ? id : -1;
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    /**
     * Saves the id, with the passthrough setting following it. Returns false, and
     * writes nothing, for an id that names nothing offered.
     */
    public static boolean store(SharedPreferences prefs, int id) {
        if (cellForId(id) < 0) {
            return false;
        }
        prefs.edit()
                .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, id)
                .putBoolean(PreferenceConfiguration.VR_PASSTHROUGH_PREF_STRING,
                        id == PreferenceConfiguration.VR_ENV_PASSTHROUGH)
                .apply();
        return true;
    }
}
