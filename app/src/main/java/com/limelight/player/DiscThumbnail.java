package com.limelight.player;

import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;

import com.limelight.LimeLog;

import java.io.File;

/**
 * One frame of a Blu-ray 3D image for the library: libdisc decodes both eyes side by side
 * into an ImageReader, paused on the first picture, and the left eye is kept. Call off the
 * main thread; it blocks for up to a few seconds.
 */
final class DiscThumbnail {

    private static final long WAIT_MS = 8000;
    private static final int DECODER_THREADS = 2;

    private DiscThumbnail() {
    }

    private static final DiscNative.Sink QUIET = new DiscNative.Sink() {
        @Override
        public void onPes(int pid, long pts90, byte[] data) {
        }

        @Override
        public void onFirstFrame(long pts90) {
        }

        @Override
        public void onEnded() {
        }
    };

    /**
     * The left eye 20% into the main title, RGBA, or null. `duration90` receives the title's
     * length in 90 kHz ticks when the disc opens.
     */
    static Bitmap grab(File iso, long[] duration90) {
        long h = DiscNative.open(iso.getAbsolutePath(), DECODER_THREADS);
        if (h == 0) {
            return null;
        }
        ImageReader reader = ImageReader.newInstance(3840, 1080, PixelFormat.RGBA_8888, 2);
        try {
            long length = durationOf(DiscNative.describe(h));
            if (duration90 != null) {
                duration90[0] = length;
            }
            if (!DiscNative.start(h, reader.getSurface(), length / 5, QUIET, false)) {
                return null;
            }
            long deadline = SystemClock.uptimeMillis() + WAIT_MS;
            Image image = null;
            while (image == null && SystemClock.uptimeMillis() < deadline) {
                image = reader.acquireLatestImage();
                if (image == null) {
                    Thread.sleep(40);
                }
            }
            if (image == null) {
                LimeLog.warning("DiscThumbnail: no picture from " + iso.getName());
                return null;
            }
            try {
                return leftEye(image);
            }
            finally {
                image.close();
            }
        }
        catch (InterruptedException e) {
            return null;
        }
        catch (RuntimeException e) {
            LimeLog.warning("DiscThumbnail: " + e);
            return null;
        }
        finally {
            DiscNative.close(h);
            reader.close();
        }
    }

    private static long durationOf(String describe) {
        if (describe == null) {
            return 0;
        }
        for (String line : describe.split("\n")) {
            String[] f = line.split("\t", -1);
            if (f.length >= 2 && f[0].equals("D")) {
                try {
                    return Long.parseLong(f[1]);
                }
                catch (NumberFormatException e) {
                    return 0;
                }
            }
        }
        return 0;
    }

    private static Bitmap leftEye(Image image) {
        Image.Plane plane = image.getPlanes()[0];
        int stride = plane.getRowStride() / 4;
        int w = image.getWidth(), h = image.getHeight();
        Bitmap whole = Bitmap.createBitmap(stride, h, Bitmap.Config.ARGB_8888);
        whole.copyPixelsFromBuffer(plane.getBuffer());
        Bitmap eye = Bitmap.createBitmap(whole, 0, 0, Math.max(1, w / 2), h);
        whole.recycle();
        return eye;
    }
}
