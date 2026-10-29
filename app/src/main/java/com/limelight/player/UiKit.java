package com.limelight.player;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;

/**
 * Shared look for the player panels: smoky translucent glass with a top sheen, white text and
 * icons, buttons and sliders.
 */
final class UiKit {

    private UiKit() {
    }

    // Glass: smoky base, top-weighted sheen, thin light rim
    static final int GLASS_BASE = 0x4D2A2A30;
    static final int GLASS_SHEEN_TOP = 0x47FFFFFF;
    static final int GLASS_SHEEN_BOTTOM = 0x24FFFFFF;
    static final int GLASS_EDGE = 0x59FFFFFF;
    static final int TEXT = 0xFFFFFFFF;
    static final int TEXT_SOFT = 0xB3FFFFFF;
    static final int TEXT_ON_LIGHT = 0xFF1C1C1E;
    // Pending seek target time
    static final int AIM = 0xFF7DD3FC;
    static final int BUTTON = 0x1AFFFFFF;
    static final int BUTTON_HOVER = 0x40FFFFFF;
    static final int BUTTON_ON = 0x2EFFFFFF;
    static final int CHOSEN = 0xEBFFFFFF;
    static final int TRACK = 0x40FFFFFF;
    static final int SHADOW = 0x59000000;

    static Drawable glass(float radius) {
        GradientDrawable base = new GradientDrawable();
        base.setColor(GLASS_BASE);
        base.setCornerRadius(radius);
        GradientDrawable sheen = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[] { GLASS_SHEEN_TOP, GLASS_SHEEN_BOTTOM });
        sheen.setCornerRadius(radius);
        sheen.setStroke(2, GLASS_EDGE);
        return new LayerDrawable(new Drawable[] { base, sheen });
    }

    // Menu sheet glass: darker base for legibility, top sheen, corner highlight, gradient rim
    static final int SHEET_BASE = 0x8F1C1D23;
    static final int SHEET_SHEEN_TOP = 0x2EFFFFFF;
    static final int SHEET_SHEEN_BOTTOM = 0x08FFFFFF;

    static Drawable frost(float radius) {
        return new SheetGlass(radius);
    }

    private static final class SheetGlass extends Drawable {
        private final float radius;
        private final Paint base = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint sheen = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint shine = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint rim = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();

        SheetGlass(float radius) {
            this.radius = radius;
            base.setColor(SHEET_BASE);
            rim.setStyle(Paint.Style.STROKE);
            rim.setStrokeWidth(2.5f);
        }

        @Override
        protected void onBoundsChange(android.graphics.Rect b) {
            sheen.setShader(new LinearGradient(0, b.top, 0, b.bottom, SHEET_SHEEN_TOP, SHEET_SHEEN_BOTTOM,
                    Shader.TileMode.CLAMP));
            shine.setShader(new RadialGradient(b.left + b.width() * 0.15f, b.top, b.width() * 0.75f,
                    0x33FFFFFF, 0x00FFFFFF, Shader.TileMode.CLAMP));
            rim.setShader(new LinearGradient(0, b.top, 0, b.bottom,
                    new int[] { 0x99FFFFFF, 0x33FFFFFF, 0x55FFFFFF }, new float[] { 0f, 0.5f, 1f },
                    Shader.TileMode.CLAMP));
        }

        @Override
        public void draw(Canvas canvas) {
            rect.set(getBounds());
            canvas.drawRoundRect(rect, radius, radius, base);
            canvas.drawRoundRect(rect, radius, radius, sheen);
            canvas.drawRoundRect(rect, radius, radius, shine);
            rect.inset(1.5f, 1.5f);
            canvas.drawRoundRect(rect, radius, radius, rim);
        }

        @Override
        public void setAlpha(int alpha) {
        }

        @Override
        public void setColorFilter(ColorFilter colorFilter) {
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

    /** State list of normal, hovered and selected fills on one shape. */
    static Drawable states(GradientDrawable base, float radius, int normal, int hover, int chosen) {
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[] { android.R.attr.state_hovered }, fill(base, radius, hover));
        bg.addState(new int[] { android.R.attr.state_selected }, fill(base, radius, chosen));
        bg.addState(new int[0], fill(base, radius, normal));
        return bg;
    }

    private static GradientDrawable fill(GradientDrawable base, float radius, int color) {
        GradientDrawable d = (GradientDrawable) base.getConstantState().newDrawable().mutate();
        d.setColor(color);
        if (radius > 0) {
            d.setCornerRadius(radius);
        }
        return d;
    }

    static Drawable circle(int color) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.OVAL);
        d.setColor(color);
        return d;
    }

    static TextView text(Context context, String s, int px, boolean bold, int color) {
        TextView view = new TextView(context);
        view.setText(s);
        view.setTextSize(TypedValue.COMPLEX_UNIT_PX, px);
        view.setTextColor(color);
        view.setTypeface(Typeface.create(bold ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        view.setIncludeFontPadding(false);
        view.setShadowLayer(4, 0, 1, SHADOW);
        return view;
    }

    /** Segmented-control option: filled white when selected, highlighted on hover. */
    static TextView segment(Context context, String name, View.OnClickListener click) {
        TextView option = new TextView(context) {
            @Override
            protected void drawableStateChanged() {
                super.drawableStateChanged();
                setTextColor(isSelected() ? TEXT_ON_LIGHT : TEXT);
                setShadowLayer(isSelected() ? 0 : 4, 0, 1, SHADOW);
            }
        };
        option.setText(name);
        option.setTextSize(TypedValue.COMPLEX_UNIT_PX, 32);
        option.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        option.setGravity(Gravity.CENTER);
        option.setTextColor(TEXT);
        option.setMaxLines(1);
        option.setBackground(states(new GradientDrawable(), 50, BUTTON, BUTTON_HOVER, CHOSEN));
        option.setClickable(true);
        option.setOnClickListener(click);
        return option;
    }

    static ImageView iconButton(Context context, int res, int size, View.OnClickListener click) {
        ImageView button = new ImageView(context);
        button.setImageResource(res);
        button.setTag(res);
        button.setScaleType(ImageView.ScaleType.FIT_CENTER);
        int pad = size * 27 / 100;
        button.setPadding(pad, pad, pad, pad);
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.OVAL);
        button.setBackground(states(shape, 0, BUTTON, BUTTON_HOVER, BUTTON_ON));
        button.setClickable(true);
        button.setOnClickListener(click);
        button.setLayoutParams(new LinearLayout.LayoutParams(size, size));
        return button;
    }

    /** Transparent root frame for a panel. */
    static ViewGroup wrap(Context context, View content) {
        FrameLayout root = new FrameLayout(context);
        root.setBackground(new ColorDrawable(Color.TRANSPARENT));
        root.addView(content, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        return root;
    }

    static String formatTime(long ms) {
        long s = Math.max(0, ms) / 1000;
        long h = s / 3600;
        long m = (s / 60) % 60;
        long sec = s % 60;
        return h > 0 ? String.format(Locale.ROOT, "%d:%02d:%02d", h, m, sec)
                     : String.format(Locale.ROOT, "%d:%02d", m, sec);
    }

    /** Signed skip: +10s, -45s, +1:30, -1:02:00. */
    static String formatJump(long ms) {
        String sign = ms < 0 ? "-" : "+";
        long a = Math.abs(ms);
        return sign + (a < 60000 ? Math.round(a / 1000.0) + "s" : formatTime(a));
    }

    /** Playback speed label: 1×, 1.25×, 0.5×. */
    static String formatSpeed(float speed) {
        String s = String.format(Locale.ROOT, "%.2f", speed);
        while (s.endsWith("0")) {
            s = s.substring(0, s.length() - 1);
        }
        if (s.endsWith(".")) {
            s = s.substring(0, s.length() - 1);
        }
        return s + "×";
    }

    static String percent(float f) {
        return Math.round(f * 100) + "%";
    }

    /** Custom-drawn thin track with a round thumb; used for the seek bar and sliders. */
    static final class SeekLine extends View {
        /** Called while dragging, and once more with done=true on release. */
        interface Listener {
            void onMoved(float fraction, boolean done);
        }

        float fraction;
        Listener listener;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        SeekLine(Context context) {
            super(context);
            setClickable(true);
        }

        void setFraction(float f) {
            fraction = Math.max(0.0f, Math.min(1.0f, f));
        }

        float fractionAt(float x) {
            float inset = inset();
            float span = getWidth() - 2 * inset;
            return span > 0 ? (x - inset) / span : 0.0f;
        }

        private float inset() {
            return Math.min(getHeight() * 0.3f, 28f);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float inset = inset();
            float cy = getHeight() / 2f;
            float span = getWidth() - 2 * inset;
            float thick = isHovered() ? 12f : 8f;
            paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setStrokeWidth(thick);
            paint.setColor(TRACK);
            canvas.drawLine(inset, cy, inset + span, cy, paint);
            paint.setColor(TEXT);
            float x = inset + span * fraction;
            canvas.drawLine(inset, cy, x, cy, paint);
            paint.setShadowLayer(6, 0, 2, SHADOW);
            canvas.drawCircle(x, cy, isHovered() ? 20f : 15f, paint);
            paint.clearShadowLayer();
        }
    }
}
