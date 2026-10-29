package com.limelight.player;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.text.Layout;
import android.text.SpannableStringBuilder;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.style.BackgroundColorSpan;
import android.text.style.ForegroundColorSpan;
import android.view.View;

/**
 * Draws one text subtitle in a SubtitleStyle: centred lines, optional plate, shadow or outline.
 * The outline is the same text stroked thick in black underneath, so it follows any span styling.
 */
final class SubtitleView extends View {

    private static final int PAD_X = 36, PAD_TOP = 14, PAD_BOTTOM = 18;

    private CharSequence text = "";
    private SubtitleStyle style = new SubtitleStyle();
    private StaticLayout fill, outline;
    private final TextPaint fillPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final TextPaint outlinePaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final Paint platePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF plate = new RectF();

    SubtitleView(Context context) {
        super(context);
    }

    void set(CharSequence text, SubtitleStyle style) {
        this.text = text == null ? "" : text;
        this.style = style;
        requestLayout();
        invalidate();
    }

    boolean hasText() {
        return text.length() > 0;
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        int available = MeasureSpec.getSize(widthSpec) - 2 * PAD_X;
        if (text.length() == 0 || available <= 0) {
            fill = null;
            outline = null;
            setMeasuredDimension(0, 0);
            return;
        }
        CharSequence shown = style.fileStyling ? text : text.toString();
        fillPaint.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        fillPaint.setTextSize(style.textPx());
        fillPaint.setColor(style.colourArgb());
        fillPaint.setStyle(Paint.Style.FILL);
        if (style.edge == SubtitleStyle.EDGE_SHADOW) {
            fillPaint.setShadowLayer(style.textPx() * 0.11f, 0, style.textPx() * 0.035f, 0xFF000000);
        }
        else {
            fillPaint.clearShadowLayer();
        }
        int wanted = (int) Math.ceil(Layout.getDesiredWidth(shown, 0, shown.length(), fillPaint));
        int width = Math.max(1, Math.min(available, wanted));
        fill = layout(shown, fillPaint, width);

        outline = null;
        if (style.edge == SubtitleStyle.EDGE_OUTLINE) {
            // Colour spans stripped so the stroke is uniformly black
            SpannableStringBuilder plain = new SpannableStringBuilder(shown);
            for (ForegroundColorSpan span : plain.getSpans(0, plain.length(), ForegroundColorSpan.class)) {
                plain.removeSpan(span);
            }
            for (BackgroundColorSpan span : plain.getSpans(0, plain.length(), BackgroundColorSpan.class)) {
                plain.removeSpan(span);
            }
            outlinePaint.setTypeface(fillPaint.getTypeface());
            outlinePaint.setTextSize(style.textPx());
            outlinePaint.setColor(0xFF000000);
            outlinePaint.setStyle(Paint.Style.STROKE);
            outlinePaint.setStrokeJoin(Paint.Join.ROUND);
            outlinePaint.setStrokeWidth(style.textPx() * 0.16f);
            outline = layout(plain, outlinePaint, width);
        }
        int pad = style.background == 0 ? 8 : PAD_X;
        setMeasuredDimension(fill.getWidth() + 2 * pad, fill.getHeight() + PAD_TOP + PAD_BOTTOM);
    }

    private static StaticLayout layout(CharSequence s, TextPaint paint, int width) {
        return StaticLayout.Builder.obtain(s, 0, s.length(), paint, width)
                .setAlignment(Layout.Alignment.ALIGN_CENTER)
                .setLineSpacing(0, 1.05f)
                .setIncludePad(false)
                .build();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (fill == null) {
            return;
        }
        if (style.background != 0) {
            platePaint.setColor(style.plateArgb());
            plate.set(0, 0, getWidth(), getHeight());
            canvas.drawRoundRect(plate, 28, 28, platePaint);
        }
        canvas.save();
        canvas.translate((getWidth() - fill.getWidth()) / 2f, PAD_TOP);
        if (outline != null) {
            outline.draw(canvas);
        }
        fill.draw(canvas);
        canvas.restore();
    }
}
