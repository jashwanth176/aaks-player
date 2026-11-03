package com.limelight.binding.video;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.CornerPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;

import static com.limelight.binding.video.XrShared.*;

/** The launch splash art, drawn once and handed to the renderer as a sprite sheet. */
final class XrSplash {

    private XrSplash() {
    }

    // Splash: the logo over the app name (a product name, never translated)
    private static final String SPLASH_NAME = "AaksVR";
    // Logo colours
    private static final int SPLASH_GROUND = 0xFF0B0E13;
    private static final int SPLASH_AMBER = 0xF59E0B;
    private static final int SPLASH_DISC = 0xFFE2E8F0;
    private static final int SPLASH_WHITE = 0xFFF8FAFC;
    // Row layout as fractions of the row height: disc radius, gap under it, name size
    private static final float SPLASH_DISC_R = 0.32f;
    private static final float SPLASH_GAP = 0.082f;
    private static final float SPLASH_NAME_SIZE = 0.18f;
    // Ground glow, amber opacity with distances in disc radii: a wide glow, plus a halo that
    // is flat to its inner radius and fades out by its outer one
    private static final float SPLASH_GLOW = 0.18f;
    private static final float SPLASH_GLOW_REACH = 8.7f;
    private static final float SPLASH_HALO = 0.18f;
    private static final float SPLASH_HALO_IN = 0.4f;
    private static final float SPLASH_HALO_OUT = 2.6f;

    /**
     * The launch splash: the logo over the app name, one row per number of open holes, stacked
     * so the native side advances by showing the next row. The bottom strip holds the ground
     * square behind the rows, so the two fade together with no box showing.
     */
    static ByteBuffer buildSplash() {
        Paint name = new Paint(Paint.ANTI_ALIAS_FLAG);
        name.setTypeface(Typeface.DEFAULT_BOLD);
        float size = SPLASH_NAME_SIZE * SPLASH_ROW_H;
        name.setTextSize(size);
        float nameW = name.measureText(SPLASH_NAME);
        // Shrink to fit should the font run wide
        float room = SPLASH_TEX_W - 32.0f;
        if (nameW > room) {
            float k = room / nameW;
            size *= k;
            name.setTextSize(size);
            nameW *= k;
        }
        float left = (SPLASH_TEX_W - nameW) * 0.5f;
        float cap = -inkAtSize(name, "M", size).top;
        float radius = SPLASH_DISC_R * SPLASH_ROW_H;
        float gap = SPLASH_GAP * SPLASH_ROW_H;
        float discY = (SPLASH_ROW_H - (2.0f * radius + gap + cap)) * 0.5f + radius;
        float baseline = discY + radius + gap + cap;

        // Drawn once, then copied into each row after cutting one more hole. The gap under the
        // rows stays clear.
        ByteBuffer pixels = ByteBuffer.allocateDirect(SPLASH_TEX_W * SPLASH_TEX_H * 4);
        Bitmap row = Bitmap.createBitmap(SPLASH_TEX_W, SPLASH_ROW_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(row);
        Logo.draw(canvas, SPLASH_TEX_W * 0.5f, discY, radius, 0, SPLASH_DISC);
        name.setColor(SPLASH_WHITE);
        canvas.drawText(SPLASH_NAME, left, baseline, name);
        for (int open = 0; open < SPLASH_ROWS; open++) {
            if (open > 0) {
                Logo.cutHole(canvas, SPLASH_TEX_W * 0.5f, discY, radius, open - 1);
            }
            pixels.position(open * SPLASH_ROW_H * SPLASH_TEX_W * 4);
            row.copyPixelsToBuffer(pixels);
        }
        row.recycle();

        Bitmap ground = drawSplashGround(discY - SPLASH_ROW_H * 0.5f, radius);
        pixels.position((SPLASH_TEX_H - SPLASH_GROUND_PX) * SPLASH_TEX_W * 4);
        ground.copyPixelsToBuffer(pixels);
        ground.recycle();
        pixels.rewind();
        return pixels;
    }

    // Bottom strip: the ground square at its left, navy with the glow centred under the disc.
    // The ground is further away and wider than the sheet, so the disc's offset and radius
    // are converted through metres.
    private static Bitmap drawSplashGround(float discDy, float discR) {
        Bitmap bitmap = Bitmap.createBitmap(SPLASH_TEX_W, SPLASH_GROUND_PX,
                Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.clipRect(0, 0, SPLASH_GROUND_PX, SPLASH_GROUND_PX);
        canvas.drawColor(SPLASH_GROUND);

        float groundM = SPLASH_GROUND_M / (SPLASH_GROUND_PX - 2 * SPLASH_GROUND_INSET);
        float k = SPLASH_SHEET_W_M / SPLASH_TEX_W
                * (SPLASH_GROUND_DISTANCE_M / SPLASH_SHEET_DISTANCE_M) / groundM;
        float cx = SPLASH_GROUND_PX * 0.5f;
        float cy = SPLASH_GROUND_PX * 0.5f + discDy * k;
        float r = discR * k;
        Paint glow = new Paint();
        glow.setShader(new RadialGradient(cx, cy, SPLASH_GLOW_REACH * r,
                new int[] { amber(SPLASH_GLOW), amber(SPLASH_GLOW * 0.25f), amber(0.0f) },
                new float[] { 0.0f, 0.55f, 1.0f }, Shader.TileMode.CLAMP));
        canvas.drawCircle(cx, cy, SPLASH_GLOW_REACH * r, glow);
        glow.setShader(new RadialGradient(cx, cy, SPLASH_HALO_OUT * r,
                new int[] { amber(SPLASH_HALO), amber(SPLASH_HALO), amber(0.0f) },
                new float[] { 0.0f, SPLASH_HALO_IN / SPLASH_HALO_OUT, 1.0f },
                Shader.TileMode.CLAMP));
        canvas.drawCircle(cx, cy, SPLASH_HALO_OUT * r, glow);
        return bitmap;
    }

    private static int amber(float opacity) {
        return Math.round(opacity * 255.0f) << 24 | SPLASH_AMBER;
    }

    // Ink bounds at a text size, measured at 1000 px for precision and scaled down
    private static RectF inkAtSize(Paint paint, String text, float size) {
        float was = paint.getTextSize();
        paint.setTextSize(1000.0f);
        Rect ink = new Rect();
        paint.getTextBounds(text, 0, text.length(), ink);
        paint.setTextSize(was);
        float k = size / 1000.0f;
        return new RectF(ink.left * k, ink.top * k, ink.right * k, ink.bottom * k);
    }

    /**
     * The logo, a film reel: a disc with a play triangle cut through the hub and six holes that
     * open one at a time, clockwise from twelve, as the session starts. Lengths are fractions
     * of the disc radius, y down; angles are degrees clockwise from three o'clock.
     */
    static final class Logo {
        static final int HOLES = 6;
        // Hole ring radius, hole radius, and the first hole's angle
        static final float HOLE_RING = 0.60f;
        static final float HOLE_R = 0.19f;
        static final float FIRST_DEG = -90.0f;
        static final float HOLE_STEP_DEG = 360.0f / HOLES;
        // Play triangle: corner radius, rightward shift so it looks centred, corner rounding
        static final float PLAY_R = 0.30f;
        static final float PLAY_SHIFT = 0.04f;
        static final float PLAY_ROUND = 0.035f;

        private Logo() {
        }

        // Hole k's centre
        static float[] hole(int k) {
            double a = Math.toRadians(FIRST_DEG + HOLE_STEP_DEG * k);
            return new float[] { (float) (HOLE_RING * Math.cos(a)), (float) (HOLE_RING * Math.sin(a)) };
        }

        // The play triangle's corners, pointing right: x0 y0 x1 y1 x2 y2
        static float[] play() {
            float[] p = new float[6];
            for (int i = 0; i < 3; i++) {
                double a = Math.toRadians(120.0 * i);
                p[2 * i] = (float) (PLAY_SHIFT + PLAY_R * Math.cos(a));
                p[2 * i + 1] = (float) (PLAY_R * Math.sin(a));
            }
            return p;
        }

        static boolean inHole(int k, double x, double y) {
            float[] c = hole(k);
            return Math.hypot(x - c[0], y - c[1]) <= HOLE_R;
        }

        static boolean inPlay(double x, double y) {
            float[] p = play();
            boolean sign = false;
            for (int i = 0; i < 3; i++) {
                double ax = p[2 * i], ay = p[2 * i + 1];
                double bx = p[(2 * i + 2) % 6], by = p[(2 * i + 3) % 6];
                double cross = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
                if (i == 0) {
                    sign = cross >= 0;
                }
                else if ((cross >= 0) != sign) {
                    return false;
                }
            }
            return true;
        }

        // The disc with the triangle and the first open holes cut, on its own layer so the cuts
        // take only the disc
        static void draw(Canvas canvas, float cx, float cy, float radius, int open, int colour) {
            int saved = canvas.saveLayer(new RectF(cx - radius - 2.0f, cy - radius - 2.0f,
                    cx + radius + 2.0f, cy + radius + 2.0f), null);
            Paint disc = new Paint(Paint.ANTI_ALIAS_FLAG);
            disc.setColor(colour);
            canvas.drawCircle(cx, cy, radius, disc);

            float[] p = play();
            Path tri = new Path();
            tri.moveTo(cx + p[0] * radius, cy + p[1] * radius);
            tri.lineTo(cx + p[2] * radius, cy + p[3] * radius);
            tri.lineTo(cx + p[4] * radius, cy + p[5] * radius);
            tri.close();
            Paint cut = cutPaint();
            cut.setPathEffect(new CornerPathEffect(PLAY_ROUND * radius));
            canvas.drawPath(tri, cut);

            for (int k = 0; k < Math.min(open, HOLES); k++) {
                cutHole(canvas, cx, cy, radius, k);
            }
            canvas.restoreToCount(saved);
        }

        static void cutHole(Canvas canvas, float cx, float cy, float radius, int k) {
            float[] c = hole(k);
            canvas.drawCircle(cx + c[0] * radius, cy + c[1] * radius, HOLE_R * radius, cutPaint());
        }

        private static Paint cutPaint() {
            Paint cut = new Paint(Paint.ANTI_ALIAS_FLAG);
            cut.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.DST_OUT));
            return cut;
        }
    }

    static void closeQuietly(InputStream in) {
        if (in != null) {
            try {
                in.close();
            } catch (IOException ignored) {
            }
        }
    }
}
