package com.limelight.player;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * Per-file state keyed by path: resume position, duration, last played time and detected
 * letterbox bars. One short string per file, so the library reads them all in one pass.
 */
final class PlaybackStore {

    /** Positions below this resume from the start. */
    private static final long MIN_RESUME_MS = 15_000;
    /** Within this of the end, or past 97%, counts as watched. */
    private static final long END_MARGIN_MS = 60_000;
    private static final String PREFIX = "film:";

    static final class Entry {
        long positionMs;
        long durationMs;
        long playedAt;
        float barTop;
        float barBottom;
        boolean barsKnown;

        /** Fraction watched, 0 to 1. */
        float progress() {
            if (durationMs <= 0 || positionMs <= 0) {
                return 0.0f;
            }
            return Math.min(1.0f, (float) positionMs / (float) durationMs);
        }

        /** Resume position; 0 if barely started or already watched. */
        long resumeMs() {
            if (positionMs < MIN_RESUME_MS) {
                return 0;
            }
            if (durationMs > 0 && (positionMs > durationMs - END_MARGIN_MS || progress() > 0.97f)) {
                return 0;
            }
            return positionMs;
        }

        boolean inProgress() {
            return resumeMs() > 0;
        }
    }

    private final SharedPreferences prefs;

    PlaybackStore(Context context) {
        prefs = context.getSharedPreferences("player_films", Context.MODE_PRIVATE);
    }

    // Canonical path, so /sdcard and /storage/emulated/0 map to the same key
    static String norm(String key) {
        if (key != null && key.startsWith("/")) {
            try {
                return new java.io.File(key).getCanonicalPath();
            }
            catch (java.io.IOException ignored) {
            }
        }
        return key;
    }

    Entry get(String key) {
        key = norm(key);
        Entry e = new Entry();
        String raw = prefs.getString(PREFIX + key, null);
        if (raw != null) {
            parse(raw, e);
        }
        return e;
    }

    void saveProgress(String key, long positionMs, long durationMs) {
        Entry e = get(key);
        e.positionMs = positionMs;
        if (durationMs > 0) {
            e.durationMs = durationMs;
        }
        e.playedAt = System.currentTimeMillis();
        put(key, e);
    }

    void saveDuration(String key, long durationMs) {
        Entry e = get(key);
        if (e.durationMs != durationMs) {
            e.durationMs = durationMs;
            put(key, e);
        }
    }

    void saveBars(String key, float top, float bottom) {
        Entry e = get(key);
        e.barTop = top;
        e.barBottom = bottom;
        e.barsKnown = true;
        put(key, e);
    }

    /** Keys of files in progress, most recently played first. */
    List<String> inProgress() {
        List<String> keys = new ArrayList<>();
        final Map<String, Long> when = new java.util.HashMap<>();
        for (Map.Entry<String, ?> kv : prefs.getAll().entrySet()) {
            if (!kv.getKey().startsWith(PREFIX) || !(kv.getValue() instanceof String)) {
                continue;
            }
            Entry e = new Entry();
            parse((String) kv.getValue(), e);
            if (e.inProgress()) {
                String key = kv.getKey().substring(PREFIX.length());
                keys.add(key);
                when.put(key, e.playedAt);
            }
        }
        Collections.sort(keys, (a, b) -> Long.compare(when.get(b), when.get(a)));
        return keys;
    }

    private void put(String key, Entry e) {
        key = norm(key);
        String raw = e.positionMs + "," + e.durationMs + "," + e.playedAt + ","
                + (e.barsKnown ? e.barTop + "," + e.barBottom : "-,-");
        prefs.edit().putString(PREFIX + key, raw).apply();
    }

    private static void parse(String raw, Entry e) {
        String[] f = raw.split(",");
        try {
            e.positionMs = Long.parseLong(f[0]);
            e.durationMs = Long.parseLong(f[1]);
            e.playedAt = Long.parseLong(f[2]);
            if (f.length >= 5 && !f[3].equals("-")) {
                e.barTop = Float.parseFloat(f[3]);
                e.barBottom = Float.parseFloat(f[4]);
                e.barsKnown = true;
            }
        }
        catch (RuntimeException ignored) {
            // Corrupt entry: treat as unknown
        }
    }
}
