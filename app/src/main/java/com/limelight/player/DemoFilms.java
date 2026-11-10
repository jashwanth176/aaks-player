package com.limelight.player;

import android.content.Context;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;

import com.limelight.LimeLog;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The short films that ship inside the app (assets/films), copied once to the app's own Movies
 * folder, where the library looks. They are there before any file access is given, so a new
 * install has something to play. One deleted in the library stays deleted.
 */
final class DemoFilms {

    private static final String ASSET_DIR = "films";
    // The set last copied; raise SET when the assets change
    private static final String PREF_SET = "demo_films_set";
    private static final int SET = 1;

    private DemoFilms() {
    }

    /** Copies the set if this install has not had it yet. Call off the main thread. */
    static synchronized void install(Context context) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        if (prefs.getInt(PREF_SET, 0) >= SET) {
            return;
        }
        File dir = new File(context.getFilesDir(), "Movies");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            LimeLog.warning("DemoFilms: cannot make " + dir);
            return;
        }
        try {
            String[] names = context.getAssets().list(ASSET_DIR);
            if (names == null) {
                return;
            }
            long started = System.nanoTime();
            long bytes = 0;
            for (String name : names) {
                File target = new File(dir, name);
                if (!VideoLibrary.isFilm(target)) {
                    continue;
                }
                // Under another name until whole, so a copy cut short is never listed
                File part = new File(dir, "." + name + ".part");
                try (InputStream in = context.getAssets().open(ASSET_DIR + "/" + name);
                     OutputStream out = new FileOutputStream(part)) {
                    byte[] buf = new byte[1 << 20];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        bytes += n;
                    }
                }
                if (!part.renameTo(target)) {
                    part.delete();
                    throw new IOException("could not rename " + part);
                }
            }
            prefs.edit().putInt(PREF_SET, SET).apply();
            LimeLog.info("DemoFilms: copied " + (bytes >> 20) + " MB in "
                    + (System.nanoTime() - started) / 1000000 + " ms");
        }
        catch (IOException e) {
            // Tried again at the next start
            LimeLog.warning("DemoFilms: copy failed: " + e);
        }
    }
}
