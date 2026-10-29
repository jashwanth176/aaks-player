package com.limelight.player;

import android.content.Context;
import android.graphics.Bitmap;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Build;

import com.limelight.LimeLog;

/**
 * Detects letterbox bars encoded inside the frame (e.g. scope films in a 16:9 picture). Samples
 * frames across the file; a row is bar only if black in all of them, so dark scenes don't count.
 * Call off the main thread.
 */
final class LetterboxProbe {

    private static final int[] AT_PERCENT = { 12, 24, 36, 48, 60, 72, 84 };
    private static final int MIN_FRAMES = 3;
    private static final int TARGET_WIDTH = 960;
    // Any pixel above this is picture
    private static final int BRIGHT = 48;
    // Bar row: mean below MEAN_MAX and at most LIT_SHARE_MAX of pixels above BRIGHT
    private static final int MEAN_MAX = 14;
    private static final float LIT_SHARE_MAX = 0.002f;
    // Bars below MIN_BAR are ignored; each side is capped at MAX_BAR
    private static final float MIN_BAR = 0.02f;
    private static final float MAX_BAR = 0.35f;

    private LetterboxProbe() {
    }

    /** Top and bottom bars as fractions of picture height, or null on failure. */
    static float[] detect(Context context, Uri uri, StereoLayout layout) {
        MediaMetadataRetriever r = new MediaMetadataRetriever();
        try {
            r.setDataSource(context, uri);
            long durationMs = parse(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION));
            int w = (int) parse(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH));
            int h = (int) parse(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT));
            if (durationMs <= 0 || w <= 0 || h <= 0) {
                return null;
            }
            int dstW = Math.min(TARGET_WIDTH, w);
            int dstH = Math.max(8, Math.round(dstW * (float) h / (float) w));

            int minTop = Integer.MAX_VALUE;
            int minBottom = Integer.MAX_VALUE;
            int frames = 0;
            int regionRows = 0;
            for (int pct : AT_PERCENT) {
                Bitmap frame = frameAt(r, durationMs * 1000L * pct / 100L, dstW, dstH);
                if (frame == null) {
                    continue;
                }
                // Left or top eye only
                int x1 = layout == StereoLayout.SBS ? frame.getWidth() / 2 : frame.getWidth();
                int rows = layout == StereoLayout.OU ? frame.getHeight() / 2 : frame.getHeight();
                int[] bars = bars(frame, x1, rows);
                frame.recycle();
                minTop = Math.min(minTop, bars[0]);
                minBottom = Math.min(minBottom, bars[1]);
                regionRows = rows;
                frames++;
            }
            if (frames < MIN_FRAMES || regionRows == 0) {
                return null;
            }
            // One row of margin so picture is never cropped
            float top = share(minTop - 1, regionRows);
            float bottom = share(minBottom - 1, regionRows);
            LimeLog.info("LetterboxProbe: " + frames + " frames, bars top "
                    + String.format("%.1f", top * 100) + "% bottom " + String.format("%.1f", bottom * 100) + "%");
            return new float[] { top, bottom };
        }
        catch (RuntimeException e) {
            LimeLog.warning("LetterboxProbe: " + e);
            return null;
        }
        finally {
            try {
                r.release();
            }
            catch (Exception ignored) {
            }
        }
    }

    private static float share(int rows, int total) {
        float f = Math.max(0, rows) / (float) total;
        return f < MIN_BAR ? 0.0f : Math.min(f, MAX_BAR);
    }

    private static Bitmap frameAt(MediaMetadataRetriever r, long us, int w, int h) {
        try {
            if (Build.VERSION.SDK_INT >= 27) {
                return r.getScaledFrameAtTime(us, MediaMetadataRetriever.OPTION_CLOSEST_SYNC, w, h);
            }
            return r.getFrameAtTime(us, MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
        }
        catch (RuntimeException e) {
            return null;
        }
    }

    // Bar rows from the top and from the bottom, within the first `rows` rows and `width` columns
    private static int[] bars(Bitmap frame, int width, int rows) {
        int[] px = new int[width];
        boolean[] bar = new boolean[rows];
        int lit = 0;
        for (int y = 0; y < rows; y++) {
            frame.getPixels(px, 0, width, 0, y, width, 1);
            long sum = 0;
            int bright = 0;
            for (int x = 0; x < width; x++) {
                int p = px[x];
                int m = Math.max((p >> 16) & 0xFF, Math.max((p >> 8) & 0xFF, p & 0xFF));
                sum += m;
                if (m > BRIGHT) {
                    bright++;
                }
            }
            bar[y] = sum / (float) width < MEAN_MAX && bright / (float) width <= LIT_SHARE_MAX;
            if (!bar[y]) {
                lit++;
            }
        }
        // Fully black frame: a fade, not a letterbox
        if (lit == 0) {
            return new int[] { 0, 0 };
        }
        int top = 0;
        while (top < rows && bar[top]) {
            top++;
        }
        int bottom = 0;
        while (bottom < rows && bar[rows - 1 - bottom]) {
            bottom++;
        }
        return new int[] { top, bottom };
    }

    private static long parse(String s) {
        try {
            return s == null ? 0 : Long.parseLong(s);
        }
        catch (NumberFormatException e) {
            return 0;
        }
    }
}
