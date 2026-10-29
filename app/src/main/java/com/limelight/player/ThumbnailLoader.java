package com.limelight.player;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Library thumbnails, generated one at a time on a background thread and cached on disk; also
 * records each file's duration. Stereo files show one eye. get() returns null and queues the
 * work if not ready; the callback fires when it is.
 */
final class ThumbnailLoader {

    static final int WIDTH = 480;
    static final int HEIGHT = 270;

    private final Context context;
    private final PlaybackStore store;
    private final Runnable changed;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "ThumbnailLoader");
        t.setPriority(Thread.MIN_PRIORITY);
        return t;
    });
    private final LruCache<String, Bitmap> memory = new LruCache<String, Bitmap>(24 * 1024 * 1024) {
        @Override
        protected int sizeOf(String key, Bitmap value) {
            return value.getByteCount();
        }
    };
    private final Set<String> asked = new HashSet<>();
    private final Set<String> failed = new HashSet<>();
    private final File dir;

    ThumbnailLoader(Context context, PlaybackStore store, Runnable changed) {
        this.context = context;
        this.store = store;
        this.changed = changed;
        dir = new File(context.getCacheDir(), "thumbs");
        dir.mkdirs();
    }

    void stop() {
        worker.shutdownNow();
    }

    /** The thumbnail, or null if not ready yet or unavailable. Main thread. */
    Bitmap get(File file) {
        String key = file.getAbsolutePath();
        Bitmap b = memory.get(key);
        if (b != null || failed.contains(key)) {
            return b;
        }
        if (asked.add(key)) {
            worker.execute(() -> load(file));
        }
        return null;
    }

    private void load(File file) {
        String key = file.getAbsolutePath();
        File cached = new File(dir, Integer.toHexString((key + file.lastModified() + file.length()).hashCode()) + ".jpg");
        Bitmap b = cached.isFile() ? BitmapFactory.decodeFile(cached.getAbsolutePath()) : null;
        if (b == null) {
            b = make(file);
            if (b != null) {
                try (FileOutputStream out = new FileOutputStream(cached)) {
                    b.compress(Bitmap.CompressFormat.JPEG, 86, out);
                }
                catch (IOException ignored) {
                }
            }
        }
        final Bitmap result = b;
        main.post(() -> {
            if (result != null) {
                memory.put(key, result);
            }
            else {
                failed.add(key);
            }
            changed.run();
        });
    }

    private Bitmap make(File file) {
        if (VideoLibrary.isDisc(file)) {
            return null;
        }
        MediaMetadataRetriever r = new MediaMetadataRetriever();
        try {
            r.setDataSource(file.getAbsolutePath());
            long durationMs = parse(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION));
            int w = (int) parse(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH));
            int h = (int) parse(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT));
            if (durationMs > 0) {
                store.saveDuration(file.getAbsolutePath(), durationMs);
            }
            if (w <= 0 || h <= 0) {
                return null;
            }
            // 20% in: past opening logos, before credits
            long us = Math.max(1, durationMs) * 1000L / 5;
            int dstW = Math.min(1280, w);
            int dstH = Math.max(8, Math.round(dstW * (float) h / (float) w));
            Bitmap frame = Build.VERSION.SDK_INT >= 27
                    ? r.getScaledFrameAtTime(us, MediaMetadataRetriever.OPTION_CLOSEST_SYNC, dstW, dstH)
                    : r.getFrameAtTime(us, MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
            if (frame == null) {
                return null;
            }
            StereoLayout layout = StereoLayout.resolve(null, file.getName(), w, h);
            Rect from = new Rect(0, 0, frame.getWidth(), frame.getHeight());
            if (layout == StereoLayout.SBS) {
                from.right = frame.getWidth() / 2;
            }
            else if (layout == StereoLayout.OU) {
                from.bottom = frame.getHeight() / 2;
            }
            Bitmap out = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
            new Canvas(out).drawBitmap(frame, from, new Rect(0, 0, WIDTH, HEIGHT), null);
            frame.recycle();
            return out;
        }
        catch (RuntimeException e) {
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

    private static long parse(String s) {
        try {
            return s == null ? 0 : Long.parseLong(s);
        }
        catch (NumberFormatException e) {
            return 0;
        }
    }
}
