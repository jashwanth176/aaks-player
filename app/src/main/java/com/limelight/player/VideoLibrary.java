package com.limelight.player;

import android.content.Context;
import android.media.MediaScannerConnection;
import android.os.Environment;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.Writer;
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

    private static final String CACHE = "library.txt";

    /** The list from the last scan, to show at once while a new scan runs; null if none yet. */
    static List<Film> cached(Context context) {
        File cache = new File(context.getFilesDir(), CACHE);
        if (!cache.isFile()) {
            return null;
        }
        List<Film> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(cache))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (!line.isEmpty()) {
                    File f = new File(line);
                    out.add(new Film(f, isDisc(f)));
                }
            }
        }
        catch (IOException e) {
            return null;
        }
        return out;
    }

    static void saveCache(Context context, List<Film> films) {
        File cache = new File(context.getFilesDir(), CACHE);
        File tmp = new File(cache.getPath() + ".tmp");
        try (Writer w = new BufferedWriter(new FileWriter(tmp))) {
            for (Film film : films) {
                w.write(film.file.getAbsolutePath());
                w.write('\n');
            }
        }
        catch (IOException e) {
            tmp.delete();
            return;
        }
        if (!tmp.renameTo(cache)) {
            tmp.delete();
        }
    }

    // A film's own subtitle files: "Film.srt", "Film.en.srt", "Film.en.forced.srt"
    private static final String SUBTITLE_TAIL = "([._ -][a-z]{2,3}([._ -](forced|sdh|cc))?)?\\.(srt|ass|ssa|vtt)";

    /**
     * Deletes a film and its subtitle files, then tells the media index. False if the film
     * itself could not be deleted.
     */
    static boolean delete(Context context, File film) {
        if (!film.delete()) {
            return false;
        }
        List<String> gone = new ArrayList<>();
        gone.add(film.getAbsolutePath());
        String name = film.getName();
        int dot = name.lastIndexOf('.');
        String base = (dot > 0 ? name.substring(0, dot) : name).toLowerCase(Locale.ROOT);
        String pattern = java.util.regex.Pattern.quote(base) + SUBTITLE_TAIL;
        File parent = film.getParentFile();
        File[] siblings = parent == null ? null : parent.listFiles();
        if (siblings != null) {
            for (File f : siblings) {
                if (f.isFile() && f.getName().toLowerCase(Locale.ROOT).matches(pattern) && f.delete()) {
                    gone.add(f.getAbsolutePath());
                }
            }
        }
        MediaScannerConnection.scanFile(context, gone.toArray(new String[0]), null, null);
        return true;
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
