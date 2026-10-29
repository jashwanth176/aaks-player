package com.limelight.preferences;

import android.content.Context;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;

import java.util.TreeSet;

/**
 * The display refresh rates the OpenXR runtime offered in the last session, which can
 * include modes the Android display does not list.
 */
public final class XrDisplayRates {

    static final String PREF = "xr_display_rates";

    private XrDisplayRates() {
    }

    /** Whole rates, lowest first, each once, as stored: "72,90" or "" for none. */
    public static String format(float[] rates) {
        TreeSet<Integer> whole = new TreeSet<>();
        if (rates != null) {
            for (float hz : rates) {
                int rounded = Math.round(hz);
                if (rounded > 0) {
                    whole.add(rounded);
                }
            }
        }
        StringBuilder sb = new StringBuilder();
        for (int hz : whole) {
            if (sb.length() > 0) {
                sb.append(',');
            }
            sb.append(hz);
        }
        return sb.toString();
    }

    /** The highest rate in a stored list, 0 when it holds none. */
    public static int highest(String stored) {
        int best = 0;
        if (stored == null) {
            return best;
        }
        for (String part : stored.split(",")) {
            try {
                best = Math.max(best, Integer.parseInt(part.trim()));
            } catch (NumberFormatException ignored) {
                // Skip a bad entry rather than lose the list
            }
        }
        return best;
    }

    /** Keeps what a session's runtime offered, writing only on a change. */
    public static void remember(Context context, float[] rates) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        String value = format(rates);
        if (!value.equals(prefs.getString(PREF, null))) {
            prefs.edit().putString(PREF, value).apply();
        }
    }
}
