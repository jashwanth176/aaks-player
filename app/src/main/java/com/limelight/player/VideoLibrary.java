package com.limelight.player;

import android.content.Context;
import android.os.Environment;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Finds video files and disc images in shared storage (newest first) and lists folders for the
 * file browser. Shared folders need all-files access; for a sideloaded build:
 *   adb shell appops set --uid <package> MANAGE_EXTERNAL_STORAGE allow
 * Without it only the app's own folders are listed.
 */
final class VideoLibrary {

    /** A video file or disc image. */
    static final class Film {
        final File file;
        final boolean disc;

        Film(File file, boolean disc) {
            this.file = file;
            this.disc = disc;
        }

        String key() {
            return file.getAbsolutePath();
        }
    }

    private static final Set<String> VIDEO = new HashSet<>(Arrays.asList(
            "mp4", "m4v", "mkv", "mov", "webm", "ts", "m2ts", "mts", "avi"));
    private static final Set<String> DISC = new HashSet<>(Arrays.asList("iso"));
    private static final int MAX_DEPTH = 4;
    private static final int MAX_FILES = 1500;

    private VideoLibrary() {
    }

    static boolean isFilm(File f) {
        String ext = extension(f.getName());
        return VIDEO.contains(ext) || DISC.contains(ext);
    }

    static boolean isDisc(File f) {
        return DISC.contains(extension(f.getName()));
    }

    /** Root folder of the file browser. */
    static File storageRoot() {
        return Environment.getExternalStorageDirectory();
    }

    /** Walks the storage roots. Call off the main thread. */
    static List<Film> scan(Context context) {
        List<File> roots = new ArrayList<>();
        roots.add(storageRoot());
        roots.add(new File(context.getFilesDir(), "Movies"));
        File own = context.getExternalFilesDir(Environment.DIRECTORY_MOVIES);
        if (own != null) {
            roots.add(own);
        }

        List<Film> found = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (File root : roots) {
            walk(root, 0, found, seen);
        }
        Collections.sort(found, (a, b) -> Long.compare(b.file.lastModified(), a.file.lastModified()));
        return found;
    }

    private static boolean skipFolder(File dir, int depth) {
        String name = dir.getName();
        // Skip hidden folders, top-level Android/ (app data) and LOST.DIR
        return name.startsWith(".") || (depth == 0 && name.equals("Android"))
                || name.equalsIgnoreCase("LOST.DIR");
    }

    private static void walk(File dir, int depth, List<Film> found, Set<String> seen) {
        if (dir == null || depth > MAX_DEPTH || found.size() >= MAX_FILES) {
            return;
        }
        File[] entries = dir.listFiles();
        if (entries == null) {
            return;
        }
        for (File f : entries) {
            if (f.getName().startsWith(".")) {
                continue;
            }
            if (f.isDirectory()) {
                if (!skipFolder(f, depth)) {
                    walk(f, depth + 1, found, seen);
                }
            }
            else if (isFilm(f) && seen.add(f.getAbsolutePath())) {
                found.add(new Film(f, isDisc(f)));
            }
        }
    }

    /** Folder contents for the file browser: subfolders, then videos, each sorted by name. */
    static List<File> list(File dir) {
        List<File> out = new ArrayList<>();
        File[] entries = dir.listFiles();
        if (entries == null) {
            return out;
        }
        for (File f : entries) {
            if (f.getName().startsWith(".")) {
                continue;
            }
            if (f.isDirectory()) {
                if (!(dir.equals(storageRoot()) && f.getName().equals("Android"))) {
                    out.add(f);
                }
            }
            else if (isFilm(f)) {
                out.add(f);
            }
        }
        Collections.sort(out, (a, b) -> {
            if (a.isDirectory() != b.isDirectory()) {
                return a.isDirectory() ? -1 : 1;
            }
            return a.getName().compareToIgnoreCase(b.getName());
        });
        return out;
    }

    /** Number of videos directly in a folder. */
    static int filmCount(File dir) {
        File[] entries = dir.listFiles();
        int n = 0;
        if (entries != null) {
            for (File f : entries) {
                if (f.isFile() && isFilm(f)) {
                    n++;
                }
            }
        }
        return n;
    }

    private static String extension(String name) {
        int dot = name.lastIndexOf('.');
        return dot < 0 ? "" : name.substring(dot + 1).toLowerCase(Locale.ROOT);
    }

    /** File size as "1.4 GB" or "700 MB". */
    static String sizeText(long bytes) {
        if (bytes >= 1L << 30) {
            return String.format(Locale.ROOT, "%.1f GB", bytes / (double) (1L << 30));
        }
        return String.format(Locale.ROOT, "%d MB", Math.max(1, bytes >> 20));
    }

    /** A running time as "2 h 41 min". */
    static String durationText(long ms) {
        if (ms <= 0) {
            return "";
        }
        if (ms < 60000) {
            return Math.max(1, ms / 1000) + " s";
        }
        long minutes = ms / 60000;
        return minutes >= 60 ? String.format(Locale.ROOT, "%d h %02d min", minutes / 60, minutes % 60)
                             : minutes + " min";
    }
}
