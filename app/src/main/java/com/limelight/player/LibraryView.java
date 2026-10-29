package com.limelight.player;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.text.TextPaint;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import static com.limelight.player.UiKit.BUTTON;
import static com.limelight.player.UiKit.BUTTON_HOVER;
import static com.limelight.player.UiKit.TEXT;
import static com.limelight.player.UiKit.TEXT_SOFT;

/**
 * Library panel with three tabs: Continue (in progress), Films (all found, newest first) and
 * Folders (file browser). Paged rather than scrolled, since the panel is rendered to a texture.
 */
final class LibraryView {

    interface Host {
        void onPlay(File file);
        void onClose();
        /** The panel needs redrawing. */
        void onChanged();
    }

    static final int WIDTH = 1200, HEIGHT = 900;

    private static final int COLUMNS = 3, GRID_ROWS = 2, PER_PAGE = COLUMNS * GRID_ROWS;
    private static final int LIST_ROWS = 6;
    private static final int CARD_HEIGHT = 276;
    private static final int ROW_HEIGHT = 72;
    private static final int BAR_HEIGHT = 84;

    private enum Tab { CONTINUE, FILMS, FOLDERS }

    private final Context context;
    private final PlaybackStore store;
    private final Host host;
    private final ThumbnailLoader thumbs;
    private final Handler main = new Handler(Looper.getMainLooper());

    private final ViewGroup root;
    private final TextView note;
    private TextView doneButton;
    private final TextView empty;
    private final LinearLayout gridBox;
    private final LinearLayout listBox;
    private final LinearLayout crumbBar;
    private File crumbsFor;
    private final ImageView upButton;
    private final TextView pageText;
    private final ImageView prevButton, nextButton;
    private final TextView[] tabButtons = new TextView[Tab.values().length];
    private final List<Card> cards = new ArrayList<>();
    private final List<Row> rows = new ArrayList<>();

    private Tab tab = Tab.FILMS;
    private int page;
    private List<VideoLibrary.Film> films;
    private boolean scanning;
    private File dir = VideoLibrary.storageRoot();
    private List<File> items = new ArrayList<>();

    LibraryView(Context context, PlaybackStore store, Host host) {
        this.context = context;
        this.store = store;
        this.host = host;
        thumbs = new ThumbnailLoader(context, store, () -> {
            refresh();
            host.onChanged();
        });

        LinearLayout sheet = new LinearLayout(context);
        sheet.setOrientation(LinearLayout.VERTICAL);
        sheet.setBackground(UiKit.frost(56));
        sheet.setPadding(56, 36, 56, 32);

        // Header: title, tabs, Done
        LinearLayout head = new LinearLayout(context);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.addView(UiKit.text(context, "Library", 42, true, TEXT),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        String[] names = { "Continue", "Films", "Folders" };
        for (Tab t : Tab.values()) {
            TextView b = UiKit.segment(context, names[t.ordinal()], v -> selectTab(t));
            tabButtons[t.ordinal()] = b;
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(184, 76);
            lp.rightMargin = 10;
            head.addView(b, lp);
        }
        LinearLayout.LayoutParams doneLp = new LinearLayout.LayoutParams(150, 76);
        doneLp.leftMargin = 20;
        doneButton = UiKit.segment(context, "Done", v -> host.onClose());
        head.addView(doneButton, doneLp);
        sheet.addView(head, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 96));

        note = UiKit.text(context, "", 28, false, TEXT_SOFT);
        note.setGravity(Gravity.CENTER_VERTICAL);
        note.setMaxLines(1);
        note.setEllipsize(TextUtils.TruncateAt.END);
        note.setPadding(6, 0, 0, 0);
        sheet.addView(note, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 48));

        FrameLayout body = new FrameLayout(context);

        // Card grid
        gridBox = new LinearLayout(context);
        gridBox.setOrientation(LinearLayout.VERTICAL);
        for (int r = 0; r < GRID_ROWS; r++) {
            LinearLayout line = new LinearLayout(context);
            line.setOrientation(LinearLayout.HORIZONTAL);
            for (int c = 0; c < COLUMNS; c++) {
                Card card = new Card(context);
                final int slot = r * COLUMNS + c;
                card.setOnClickListener(v -> open(slot));
                cards.add(card);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, CARD_HEIGHT, 1);
                lp.leftMargin = 8;
                lp.rightMargin = 8;
                line.addView(card, lp);
            }
            LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, CARD_HEIGHT);
            rowLp.bottomMargin = 10;
            gridBox.addView(line, rowLp);
        }
        body.addView(gridBox, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        // File browser: up button, breadcrumbs, rows
        listBox = new LinearLayout(context);
        listBox.setOrientation(LinearLayout.VERTICAL);
        LinearLayout bar = new LinearLayout(context);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        upButton = UiKit.iconButton(context, com.limelight.R.drawable.player_ic_up, 84, v -> goUp());
        bar.addView(upButton);
        crumbBar = new LinearLayout(context);
        crumbBar.setOrientation(LinearLayout.HORIZONTAL);
        crumbBar.setGravity(Gravity.CENTER_VERTICAL);
        crumbBar.setPadding(16, 0, 0, 0);
        bar.addView(crumbBar, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));
        listBox.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, BAR_HEIGHT));
        for (int i = 0; i < LIST_ROWS; i++) {
            Row row = new Row(context);
            final int slot = i;
            row.setOnClickListener(v -> open(slot));
            rows.add(row);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ROW_HEIGHT);
            lp.topMargin = 8;
            listBox.addView(row, lp);
        }
        body.addView(listBox, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        empty = UiKit.text(context, "", 34, false, TEXT_SOFT);
        empty.setGravity(Gravity.CENTER);
        body.addView(empty, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        sheet.addView(body, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        // Pager
        LinearLayout pager = new LinearLayout(context);
        pager.setOrientation(LinearLayout.HORIZONTAL);
        pager.setGravity(Gravity.CENTER);
        prevButton = UiKit.iconButton(context, com.limelight.R.drawable.player_ic_prev, 80, v -> turn(-1));
        nextButton = UiKit.iconButton(context, com.limelight.R.drawable.player_ic_next, 80, v -> turn(1));
        pageText = UiKit.text(context, "", 30, false, TEXT_SOFT);
        pageText.setGravity(Gravity.CENTER);
        pager.addView(prevButton);
        pager.addView(pageText, new LinearLayout.LayoutParams(180, ViewGroup.LayoutParams.WRAP_CONTENT));
        pager.addView(nextButton);
        sheet.addView(pager, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 88));

        root = UiKit.wrap(context, sheet);
        refresh();
    }

    /** Hides Done when there is nothing to return to. */
    void setCloseable(boolean closeable) {
        doneButton.setVisibility(closeable ? View.VISIBLE : View.INVISIBLE);
    }

    ViewGroup root() {
        return root;
    }

    void stop() {
        thumbs.stop();
    }

    private boolean openedBefore;

    /** Rescans; on first open, starts on Continue if anything is in progress. */
    void onOpened() {
        note.setText("");
        boolean anyInProgress = !store.inProgress().isEmpty();
        if (!openedBefore && anyInProgress) {
            tab = Tab.CONTINUE;
        }
        openedBefore = true;
        if (!anyInProgress && tab == Tab.CONTINUE) {
            tab = Tab.FILMS;
        }
        scan();
        refresh();
    }

    /** Current tab, page and folder, for restoring after playback. */
    String place() {
        return tab.name() + "|" + page + "|" + dir.getAbsolutePath();
    }

    /** Restores a place() string. Call before onOpened. */
    void restore(String place) {
        String[] parts = place.split("\\|", 3);
        if (parts.length != 3) {
            return;
        }
        try {
            tab = Tab.valueOf(parts[0]);
            page = Integer.parseInt(parts[1]);
        }
        catch (IllegalArgumentException e) {
            return;
        }
        File d = new File(parts[2]);
        if (d.isDirectory()) {
            dir = d;
        }
        // Keep the restored tab rather than switching to Continue
        openedBefore = true;
    }

    private void scan() {
        if (scanning) {
            return;
        }
        scanning = true;
        new Thread(() -> {
            List<VideoLibrary.Film> found = VideoLibrary.scan(context);
            main.post(() -> {
                scanning = false;
                films = found;
                refresh();
                host.onChanged();
            });
        }, "LibraryView scan").start();
    }

    private void selectTab(Tab t) {
        tab = t;
        page = 0;
        note.setText("");
        refresh();
        host.onChanged();
    }

    private void goUp() {
        File parent = dir.getParentFile();
        if (!dir.equals(VideoLibrary.storageRoot()) && parent != null) {
            goTo(parent);
        }
    }

    private void goTo(File folder) {
        dir = folder;
        page = 0;
        refresh();
        host.onChanged();
    }

    // Clickable breadcrumbs; the current folder is plain. Leading folders that don't fit
    // collapse into "…".
    private void buildCrumbs() {
        if (dir.equals(crumbsFor)) {
            return;
        }
        crumbsFor = dir;
        crumbBar.removeAllViews();
        File root = VideoLibrary.storageRoot();
        List<File> chain = new ArrayList<>();
        for (File f = dir; f != null; f = f.getParentFile()) {
            chain.add(0, f);
            if (f.equals(root)) {
                break;
            }
        }
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setTextSize(32);
        paint.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        int budget = WIDTH - 112 - 84 - 40;
        List<CharSequence> names = new ArrayList<>();
        int[] widths = new int[chain.size()];
        for (int i = 0; i < chain.size(); i++) {
            File f = chain.get(i);
            String name = f.equals(root) ? "Internal storage" : f.getName().isEmpty() ? "/" : f.getName();
            CharSequence shown = TextUtils.ellipsize(name, new TextPaint(paint), 420, TextUtils.TruncateAt.END);
            names.add(shown);
            widths[i] = (int) paint.measureText(shown.toString()) + CRUMB_PAD * 2 + CRUMB_GAP;
        }
        int start = chain.size() - 1;
        int used = widths[start];
        while (start > 0 && used + widths[start - 1] <= budget - (start - 1 > 0 ? 80 : 0)) {
            start--;
            used += widths[start];
        }
        if (start > 0) {
            addCrumb("…", chain.get(start - 1), false);
        }
        for (int i = start; i < chain.size(); i++) {
            if (i > start || start > 0) {
                addCrumbSeparator();
            }
            addCrumb(names.get(i), chain.get(i), i == chain.size() - 1);
        }
    }

    private static final int CRUMB_PAD = 16, CRUMB_GAP = 36;

    private void addCrumb(CharSequence name, File target, boolean current) {
        TextView crumb = UiKit.text(context, "", 32, true, current ? TEXT : TEXT_SOFT);
        crumb.setText(name);
        crumb.setMaxLines(1);
        crumb.setGravity(Gravity.CENTER);
        crumb.setPadding(CRUMB_PAD, 0, CRUMB_PAD, 0);
        if (!current) {
            crumb.setBackground(UiKit.states(new GradientDrawable(), 28, 0x00FFFFFF, BUTTON_HOVER, BUTTON_HOVER));
            crumb.setClickable(true);
            crumb.setOnClickListener(v -> goTo(target));
        }
        crumbBar.addView(crumb, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, 60));
    }

    private void addCrumbSeparator() {
        TextView sep = UiKit.text(context, "›", 32, false, TEXT_SOFT);
        sep.setGravity(Gravity.CENTER);
        crumbBar.addView(sep, new LinearLayout.LayoutParams(CRUMB_GAP, 60));
    }

    /** Turns pages from a scroll or drag. Main thread. */
    void scroll(int pages) {
        turn(pages);
    }

    private void turn(int step) {
        int pages = pageCount();
        int p = page + step;
        if (p >= 0 && p < pages) {
            page = p;
            refresh();
            host.onChanged();
        }
    }

    private int perPage() {
        return tab == Tab.FOLDERS ? LIST_ROWS : PER_PAGE;
    }

    private int pageCount() {
        return Math.max(1, (items.size() + perPage() - 1) / perPage());
    }

    private void open(int slot) {
        int index = page * perPage() + slot;
        if (index >= items.size()) {
            return;
        }
        File f = items.get(index);
        if (f.isDirectory()) {
            goTo(f);
        }
        else {
            host.onPlay(f);
        }
    }

    private void refresh() {
        items = new ArrayList<>();
        switch (tab) {
            case CONTINUE:
                for (String key : store.inProgress()) {
                    File f = new File(key);
                    if (f.isFile()) {
                        items.add(f);
                    }
                }
                break;
            case FILMS:
                if (films != null) {
                    for (VideoLibrary.Film film : films) {
                        items.add(film.file);
                    }
                }
                break;
            default:
                items = VideoLibrary.list(dir);
                break;
        }
        page = Math.min(page, pageCount() - 1);

        for (Tab t : Tab.values()) {
            tabButtons[t.ordinal()].setSelected(t == tab);
        }
        boolean folders = tab == Tab.FOLDERS;
        gridBox.setVisibility(folders ? View.INVISIBLE : View.VISIBLE);
        listBox.setVisibility(folders ? View.VISIBLE : View.INVISIBLE);

        int start = page * perPage();
        for (int i = 0; i < cards.size(); i++) {
            Card c = cards.get(i);
            int index = start + i;
            if (!folders && index < items.size()) {
                fill(c, items.get(index));
                c.setVisibility(View.VISIBLE);
            }
            else {
                c.setVisibility(View.INVISIBLE);
            }
        }
        for (int i = 0; i < rows.size(); i++) {
            Row r = rows.get(i);
            int index = start + i;
            if (folders && index < items.size()) {
                fill(r, items.get(index));
                r.setVisibility(View.VISIBLE);
            }
            else {
                r.setVisibility(View.INVISIBLE);
            }
        }

        buildCrumbs();
        upButton.setEnabled(!dir.equals(VideoLibrary.storageRoot()));
        upButton.setAlpha(upButton.isEnabled() ? 1.0f : 0.3f);

        String message = "";
        if (items.isEmpty()) {
            if (tab == Tab.CONTINUE) {
                message = "Nothing half watched.\nA film you stop part way waits here.";
            }
            else if (tab == Tab.FILMS) {
                message = films == null ? "Looking for films…"
                        : "No films found.\nCopy them to Movies or Download on the headset.";
            }
            else {
                message = "Nothing here.";
            }
        }
        empty.setText(message);
        pageText.setText(items.isEmpty() ? "" : String.format(Locale.ROOT, "%d / %d", page + 1, pageCount()));
        prevButton.setEnabled(page > 0);
        nextButton.setEnabled(page < pageCount() - 1);
        prevButton.setAlpha(prevButton.isEnabled() ? 1.0f : 0.3f);
        nextButton.setAlpha(nextButton.isEnabled() ? 1.0f : 0.3f);
    }

    private void fill(Card card, File f) {
        PlaybackStore.Entry e = store.get(f.getAbsolutePath());
        card.title = PlayerXR.titleFor(Uri.fromFile(f));
        StringBuilder sub = new StringBuilder();
        if (e.durationMs > 0) {
            sub.append(VideoLibrary.durationText(e.durationMs)).append("  ·  ");
        }
        sub.append(VideoLibrary.sizeText(f.length()));
        card.sub = sub.toString();
        card.disc = VideoLibrary.isDisc(f);
        card.thumb = card.disc ? null : thumbs.get(f);
        card.progress = e.inProgress() ? e.progress() : 0.0f;
        card.badge = card.disc ? "3D DISC" : (StereoLayout.resolve(null, f.getName(), 1, 1) != StereoLayout.MONO ? "3D" : "");
    }

    private void fill(Row row, File f) {
        row.title = f.getName();
        row.folder = f.isDirectory();
        row.disc = !row.folder && VideoLibrary.isDisc(f);
        if (row.folder) {
            int n = VideoLibrary.filmCount(f);
            row.info = n == 0 ? "" : n + (n == 1 ? " film" : " films");
        }
        else {
            row.info = VideoLibrary.sizeText(f.length());
        }
    }

    // ---- drawn views ------------------------------------------------------------------

    private static Drawable icon(Context context, int res) {
        Drawable d = context.getDrawable(res);
        return d == null ? null : d.mutate();
    }

    /** Grid card: thumbnail with progress bar and badge, title and details below. */
    private static final class Card extends View {
        // Height reserved below the thumbnail for two text lines
        private static final int TEXT_AREA = 78;
        String title = "", sub = "", badge = "";
        Bitmap thumb;
        float progress;
        boolean disc;

        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint bitmapPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final TextPaint titlePaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        private final TextPaint subPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        private final TextPaint badgePaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        private final Rect source = new Rect();
        private final Path clip = new Path();
        private final Drawable film, discIcon;

        Card(Context context) {
            super(context);
            setClickable(true);
            titlePaint.setTextSize(28);
            titlePaint.setColor(TEXT);
            titlePaint.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            subPaint.setTextSize(23);
            subPaint.setColor(TEXT_SOFT);
            badgePaint.setTextSize(24);
            badgePaint.setColor(UiKit.TEXT_ON_LIGHT);
            badgePaint.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            film = icon(context, com.limelight.R.drawable.player_ic_movie);
            discIcon = icon(context, com.limelight.R.drawable.player_ic_disc);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            paint.setColor(isHovered() ? BUTTON_HOVER : BUTTON);
            c.drawRoundRect(0, 0, w, h, 26, 26, paint);

            float pad = 10;
            float tw = w - 2 * pad;
            // Up to 16:9, limited by the text area; the thumbnail is center-cropped to fit
            float th = Math.min(tw * 9f / 16f, h - 2 * pad - TEXT_AREA);
            rect.set(pad, pad, pad + tw, pad + th);
            int save = c.save();
            clip.reset();
            clip.addRoundRect(rect, 18, 18, Path.Direction.CW);
            c.clipPath(clip);
            if (thumb != null) {
                float want = tw / th;
                float have = thumb.getWidth() / (float) thumb.getHeight();
                if (have > want) {
                    int cw = Math.round(thumb.getHeight() * want);
                    int x0 = (thumb.getWidth() - cw) / 2;
                    source.set(x0, 0, x0 + cw, thumb.getHeight());
                }
                else {
                    int ch = Math.round(thumb.getWidth() / want);
                    int y0 = (thumb.getHeight() - ch) / 2;
                    source.set(0, y0, thumb.getWidth(), y0 + ch);
                }
                c.drawBitmap(thumb, source, rect, bitmapPaint);
            }
            else {
                paint.setColor(0x33000000);
                c.drawRect(rect, paint);
                Drawable d = disc ? discIcon : film;
                if (d != null) {
                    int cx = (int) rect.centerX(), cy = (int) rect.centerY();
                    d.setBounds(cx - 52, cy - 52, cx + 52, cy + 52);
                    d.setAlpha(110);
                    d.draw(c);
                }
            }
            if (progress > 0.0f) {
                float y = rect.bottom - 16;
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(0x80000000);
                c.drawRect(rect.left, y - 6, rect.right, rect.bottom, paint);
                paint.setColor(TEXT);
                c.drawRect(rect.left, y - 6, rect.left + rect.width() * progress, rect.bottom, paint);
            }
            c.restoreToCount(save);

            if (!badge.isEmpty()) {
                float bw = badgePaint.measureText(badge) + 28;
                rect.set(pad + 14, pad + 14, pad + 14 + bw, pad + 14 + 40);
                paint.setColor(UiKit.CHOSEN);
                c.drawRoundRect(rect, 20, 20, paint);
                c.drawText(badge, rect.left + 14, rect.top + 29, badgePaint);
            }

            float tx = pad + 6;
            float maxW = w - 2 * tx;
            c.drawText(TextUtils.ellipsize(title, titlePaint, maxW, TextUtils.TruncateAt.END).toString(),
                    tx, pad + th + 36, titlePaint);
            c.drawText(TextUtils.ellipsize(sub, subPaint, maxW, TextUtils.TruncateAt.END).toString(),
                    tx, pad + th + 65, subPaint);
        }
    }

    /** File browser row: icon, name, and size or video count. */
    private static final class Row extends View {
        String title = "", info = "";
        boolean folder, disc;

        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final TextPaint titlePaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        private final TextPaint infoPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        private final Drawable folderIcon, film, discIcon;

        Row(Context context) {
            super(context);
            setClickable(true);
            titlePaint.setTextSize(32);
            titlePaint.setColor(TEXT);
            titlePaint.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            infoPaint.setTextSize(28);
            infoPaint.setColor(TEXT_SOFT);
            folderIcon = icon(context, com.limelight.R.drawable.player_ic_folder);
            film = icon(context, com.limelight.R.drawable.player_ic_movie);
            discIcon = icon(context, com.limelight.R.drawable.player_ic_disc);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            paint.setColor(isHovered() ? BUTTON_HOVER : 0x0DFFFFFF);
            c.drawRoundRect(0, 0, w, h, 30, 30, paint);
            Drawable d = folder ? folderIcon : (disc ? discIcon : film);
            if (d != null) {
                d.setBounds(24, (int) (h / 2) - 24, 72, (int) (h / 2) + 24);
                d.setAlpha(folder ? 255 : 200);
                d.draw(c);
            }
            float infoW = infoPaint.measureText(info);
            float x = 100;
            float maxW = w - x - infoW - 60;
            c.drawText(TextUtils.ellipsize(title, titlePaint, maxW, TextUtils.TruncateAt.END).toString(),
                    x, h / 2 + 11, titlePaint);
            c.drawText(info, w - infoW - 30, h / 2 + 10, infoPaint);
        }
    }
}
