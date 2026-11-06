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
import android.os.SystemClock;
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
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static com.limelight.player.UiKit.BUTTON;
import static com.limelight.player.UiKit.BUTTON_HOVER;
import static com.limelight.player.UiKit.TEXT;
import static com.limelight.player.UiKit.TEXT_SOFT;

/**
 * Library panel with three tabs: Continue (in progress), Films (all found, newest first) and
 * Folders (file browser). The grid and the file list scroll under the pointer and glide a
 * little after a release; each tab and folder keeps its place.
 */
final class LibraryView {

    interface Host {
        void onPlay(File file);
        void onClose();
        /** The panel needs redrawing. */
        void onChanged();
        /** Whether this file is the film open in the player now. */
        boolean isPlaying(File file);
    }

    static final int WIDTH = 1200, HEIGHT = 900;

    private static final int COLUMNS = 3;
    private static final int CARD_HEIGHT = 276, CARD_GAP = 10;
    private static final int ROW_HEIGHT = 72, ROW_GAP = 8;
    private static final int BAR_HEIGHT = 84;
    // Glide after a release: the velocity's time constant, the speed it stops at, and the
    // speed a thumbstick click gives
    private static final float GLIDE_TAU_S = 0.3f, GLIDE_STOP = 30.0f, NUDGE_SPEED = 2400.0f;
    private static final long GLIDE_TICK_MS = 16;

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
    private final Pane gridPane, listPane;
    private final LinearLayout gridBox;
    private final LinearLayout listBox;
    private final LinearLayout crumbBar;
    private File crumbsFor;
    private final ImageView upButton;
    private final TextView[] tabButtons = new TextView[Tab.values().length];
    // Grid rows, cells, cards and trash buttons, made as the list grows
    private final List<LinearLayout> lines = new ArrayList<>();
    private final List<FrameLayout> cells = new ArrayList<>();
    private final List<Card> cards = new ArrayList<>();
    private final List<ImageView> trashButtons = new ArrayList<>();
    private final List<Row> rows = new ArrayList<>();
    // Scroll position per tab and folder
    private final Map<String, Integer> places = new HashMap<>();
    private int dragFrom;
    private float glideY, glideVelocity;
    private long glideLastMs;
    // Delete confirmation: a dim over the whole panel, and the dialog in its own panel
    private final View confirm;
    private final ViewGroup dialog;
    private final TextView confirmName, confirmInfo;
    private File pendingDelete;

    private Tab tab = Tab.FILMS;
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
        head.addView(UiKit.text(context, "Library", UiKit.TYPE_TITLE, true, TEXT),
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

        note = UiKit.text(context, "", UiKit.TYPE_SMALL, false, TEXT_SOFT);
        note.setGravity(Gravity.CENTER_VERTICAL);
        note.setMaxLines(1);
        note.setEllipsize(TextUtils.TruncateAt.END);
        note.setPadding(6, 0, 0, 0);
        sheet.addView(note, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 48));

        FrameLayout body = new FrameLayout(context);

        // Card grid, rows added as needed
        gridBox = new LinearLayout(context);
        gridBox.setOrientation(LinearLayout.VERTICAL);
        gridPane = new Pane(context);
        gridPane.addView(gridBox, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        body.addView(gridPane, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        // File browser: up button and breadcrumbs over the scrolling rows
        LinearLayout browser = new LinearLayout(context);
        browser.setOrientation(LinearLayout.VERTICAL);
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
        browser.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, BAR_HEIGHT));
        listPane = new Pane(context);
        listPane.addView(listBox, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        browser.addView(listPane, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        body.addView(browser, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        empty = UiKit.text(context, "", UiKit.TYPE_BODY, false, TEXT_SOFT);
        empty.setGravity(Gravity.CENTER);
        body.addView(empty, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        LinearLayout.LayoutParams bodyLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1);
        bodyLp.bottomMargin = 8;
        sheet.addView(body, bodyLp);

        root = UiKit.wrap(context, sheet);

        // Delete confirmation: a dimmed panel with a small dialog. A click on the dim cancels.
        FrameLayout scrim = new FrameLayout(context);
        GradientDrawable dim = new GradientDrawable();
        dim.setCornerRadius(56);
        dim.setColor(0xB30C0C10);
        scrim.setBackground(dim);
        scrim.setClickable(true);
        scrim.setOnClickListener(v -> cancelDelete());
        LinearLayout dialog = new LinearLayout(context);
        dialog.setOrientation(LinearLayout.VERTICAL);
        dialog.setBackground(UiKit.frost(40, UiKit.DIALOG_BASE));
        dialog.setPadding(48, 40, 48, 40);
        dialog.setClickable(true);
        dialog.addView(UiKit.text(context, "Delete this film?", UiKit.TYPE_HEADING, true, TEXT));
        confirmName = UiKit.text(context, "", UiKit.TYPE_BODY, false, TEXT);
        confirmName.setMaxLines(2);
        confirmName.setEllipsize(TextUtils.TruncateAt.END);
        confirmName.setPadding(0, 18, 0, 6);
        dialog.addView(confirmName);
        confirmInfo = UiKit.text(context, "", UiKit.TYPE_SMALL, false, TEXT_SOFT);
        dialog.addView(confirmInfo);
        LinearLayout buttons = new LinearLayout(context);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        TextView cancel = UiKit.segment(context, "Cancel", v -> cancelDelete());
        TextView delete = UiKit.segment(context, "Delete", v -> confirmDelete());
        GradientDrawable red = new GradientDrawable();
        delete.setBackground(UiKit.states(red, 44, DANGER, DANGER_HOVER, DANGER_HOVER));
        delete.setTextColor(TEXT);
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(0, 84, 1);
        bl.rightMargin = 16;
        buttons.addView(cancel, bl);
        buttons.addView(delete, new LinearLayout.LayoutParams(0, 84, 1));
        LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 84);
        rowLp.topMargin = 36;
        dialog.addView(buttons, rowLp);
        this.dialog = UiKit.wrap(context, dialog);
        scrim.setVisibility(View.GONE);
        root.addView(scrim, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        confirm = scrim;

        // The list from the last scan shows at once; a fresh scan replaces it when done
        films = VideoLibrary.cached(context);
        refresh();
    }

    private static final int DANGER = 0xFFD84B4B, DANGER_HOVER = 0xFFEA6262;

    private void askDelete(int index) {
        if (index >= items.size()) {
            return;
        }
        File f = items.get(index);
        if (host.isPlaying(f)) {
            note.setText("Close the film before deleting it.");
            host.onChanged();
            return;
        }
        pendingDelete = f;
        confirmName.setText(PlayerXR.titleFor(Uri.fromFile(f)));
        confirmInfo.setText(VideoLibrary.sizeText(f.length()) + "  ·  removed from the headset for good");
        confirm.setVisibility(View.VISIBLE);
        host.onChanged();
    }

    private void cancelDelete() {
        pendingDelete = null;
        confirm.setVisibility(View.GONE);
        host.onChanged();
    }

    private void confirmDelete() {
        File f = pendingDelete;
        cancelDelete();
        if (f == null) {
            return;
        }
        String title = PlayerXR.titleFor(Uri.fromFile(f));
        thumbs.forget(f);
        new Thread(() -> {
            boolean ok = VideoLibrary.delete(context, f);
            main.post(() -> {
                if (ok) {
                    store.forget(f.getAbsolutePath());
                    if (films != null) {
                        List<VideoLibrary.Film> left = new ArrayList<>(films);
                        left.removeIf(film -> film.file.equals(f));
                        films = left;
                        new Thread(() -> VideoLibrary.saveCache(context, left), "LibraryView cache").start();
                    }
                }
                note.setText(ok ? "Deleted “" + title + "”." : "Couldn't delete “" + title + "”.");
                refresh();
                host.onChanged();
            });
        }, "LibraryView delete").start();
    }

    /** The delete dialog, drawn in its own panel while confirming(). */
    ViewGroup dialog() {
        return dialog;
    }

    boolean confirming() {
        return pendingDelete != null;
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
        pendingDelete = null;
        confirm.setVisibility(View.GONE);
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

    /** Current tab, scroll position and folder, for restoring after playback. */
    String place() {
        return tab.name() + "|" + pane().offset + "|" + dir.getAbsolutePath();
    }

    /** Restores a place() string. Call before onOpened. */
    void restore(String place) {
        String[] parts = place.split("\\|", 3);
        if (parts.length != 3) {
            return;
        }
        int offset;
        try {
            tab = Tab.valueOf(parts[0]);
            offset = Integer.parseInt(parts[1]);
        }
        catch (IllegalArgumentException e) {
            return;
        }
        File d = new File(parts[2]);
        if (d.isDirectory()) {
            dir = d;
        }
        places.put(placeKey(), offset);
        // Keep the restored tab rather than switching to Continue
        openedBefore = true;
    }

    // ---- scrolling --------------------------------------------------------------------

    private Pane pane() {
        return tab == Tab.FOLDERS ? listPane : gridPane;
    }

    private String placeKey() {
        return tab == Tab.FOLDERS ? "F:" + dir.getAbsolutePath() : tab.name();
    }

    /** The pointer went down: a drag from here scrolls. Main thread. */
    void dragBegin() {
        stopGlide();
        dragFrom = pane().offset;
    }

    /** The pointer moved `dy` panel pixels up since it went down. Main thread. */
    void dragTo(float dy) {
        scrollTo(Math.round(dragFrom + dy));
    }

    /** The pointer let go while moving `velocity` panel pixels per second up. Main thread. */
    void release(float velocity) {
        if (Math.abs(velocity) > GLIDE_STOP) {
            glide(velocity);
        }
    }

    /** Thumbstick: a screenful down (1) or up (-1), gliding. Main thread. */
    void nudge(int direction) {
        glide(direction * NUDGE_SPEED);
    }

    private void glide(float velocity) {
        glideVelocity = velocity;
        glideY = pane().offset;
        glideLastMs = SystemClock.uptimeMillis();
        main.removeCallbacks(glideTick);
        main.postDelayed(glideTick, GLIDE_TICK_MS);
    }

    private void stopGlide() {
        main.removeCallbacks(glideTick);
    }

    private final Runnable glideTick = new Runnable() {
        @Override
        public void run() {
            long now = SystemClock.uptimeMillis();
            float dt = Math.min(0.1f, (now - glideLastMs) / 1000.0f);
            glideLastMs = now;
            glideY += glideVelocity * dt;
            glideVelocity *= (float) Math.exp(-dt / GLIDE_TAU_S);
            int wanted = Math.round(glideY);
            scrollTo(wanted);
            if (Math.abs(glideVelocity) > GLIDE_STOP && pane().offset == wanted) {
                main.postDelayed(this, GLIDE_TICK_MS);
            }
        }
    };

    private void scrollTo(int y) {
        Pane pane = pane();
        if (pane.set(y)) {
            places.put(placeKey(), pane.offset);
            wantThumbs();
            host.onChanged();
        }
    }

    // Thumbnails for the cards near the viewport only, so a long list does not queue them all
    private void wantThumbs() {
        if (tab == Tab.FOLDERS) {
            return;
        }
        int top = gridPane.offset;
        int viewport = gridPane.getHeight() > 0 ? gridPane.getHeight() : HEIGHT;
        for (int i = 0; i < items.size() && i < cards.size(); i++) {
            int rowTop = (i / COLUMNS) * (CARD_HEIGHT + CARD_GAP);
            if (rowTop < top + viewport + CARD_HEIGHT && rowTop + CARD_HEIGHT > top - CARD_HEIGHT) {
                Card card = cards.get(i);
                if (card.thumb == null) {
                    card.thumb = thumbs.get(items.get(i));
                }
            }
        }
    }

    /** A clipped viewport whose content scrolls, with a thin bar in a gutter at its right. */
    private static final class Pane extends FrameLayout {
        private static final int GUTTER = 24;
        // Wanted scroll, clamped to the content once it is laid out
        int offset;
        private final Paint bar = new Paint(Paint.ANTI_ALIAS_FLAG);

        Pane(Context context) {
            super(context);
            setWillNotDraw(false);
            bar.setColor(0x55FFFFFF);
        }

        private int range() {
            View child = getChildAt(0);
            return child == null ? 0 : Math.max(0, child.getHeight() - getHeight());
        }

        /** Scrolls to y (clamped when laid out); whether the view moved. */
        boolean set(int y) {
            int to = getHeight() > 0 ? Math.max(0, Math.min(range(), y)) : Math.max(0, y);
            boolean moved = to != offset;
            offset = to;
            scrollTo(0, to);
            return moved;
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            super.onMeasure(widthSpec, heightSpec);
            View child = getChildAt(0);
            if (child != null) {
                // The content keeps its full height, whatever the viewport's
                child.measure(MeasureSpec.makeMeasureSpec(getMeasuredWidth() - GUTTER, MeasureSpec.EXACTLY),
                        MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED));
            }
        }

        @Override
        protected void onLayout(boolean changed, int l, int t, int r, int b) {
            View child = getChildAt(0);
            if (child != null) {
                child.layout(0, 0, r - l - GUTTER, child.getMeasuredHeight());
            }
            set(offset);
        }

        @Override
        protected void dispatchDraw(Canvas canvas) {
            super.dispatchDraw(canvas);
            View child = getChildAt(0);
            int h = getHeight();
            if (child == null || child.getHeight() <= h || h <= 0) {
                return;
            }
            float thumb = Math.max(40.0f, h * (float) h / child.getHeight());
            float top = (h - thumb) * (float) offset / range();
            float x = getWidth() - 10;
            // In scrolled coordinates, so the bar stays put
            canvas.drawRoundRect(x, getScrollY() + top, x + 6, getScrollY() + top + thumb, 3, 3, bar);
        }
    }

    private void scan() {
        if (scanning) {
            return;
        }
        scanning = true;
        new Thread(() -> {
            List<VideoLibrary.Film> found = VideoLibrary.scan(context);
            VideoLibrary.saveCache(context, found);
            main.post(() -> {
                scanning = false;
                films = found;
                refresh();
                host.onChanged();
            });
        }, "LibraryView scan").start();
    }

    /** On the Films or Continue page, where back leaves the app. */
    boolean atHome() {
        return tab != Tab.FOLDERS;
    }

    /** Back from the folders: to the Films page. */
    void goHome() {
        selectTab(Tab.FILMS);
    }

    private void selectTab(Tab t) {
        stopGlide();
        tab = t;
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
        stopGlide();
        dir = folder;
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
        paint.setTextSize(UiKit.TYPE_BODY);
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
        TextView crumb = UiKit.text(context, "", UiKit.TYPE_BODY, true, current ? TEXT : TEXT_SOFT);
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
        TextView sep = UiKit.text(context, "›", UiKit.TYPE_BODY, false, TEXT_SOFT);
        sep.setGravity(Gravity.CENTER);
        crumbBar.addView(sep, new LinearLayout.LayoutParams(CRUMB_GAP, 60));
    }

    private void open(int index) {
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
        for (Tab t : Tab.values()) {
            tabButtons[t.ordinal()].setSelected(t == tab);
        }
        boolean folders = tab == Tab.FOLDERS;
        gridPane.setVisibility(folders ? View.INVISIBLE : View.VISIBLE);
        ((View) listPane.getParent()).setVisibility(folders ? View.VISIBLE : View.INVISIBLE);

        if (!folders) {
            growGrid(items.size());
            for (int i = 0; i < cards.size(); i++) {
                boolean shown = i < items.size();
                if (shown) {
                    fill(cards.get(i), items.get(i));
                }
                cells.get(i).setVisibility(shown ? View.VISIBLE : View.INVISIBLE);
                trashButtons.get(i).setVisibility(shown ? View.VISIBLE : View.INVISIBLE);
            }
            int linesShown = (items.size() + COLUMNS - 1) / COLUMNS;
            for (int i = 0; i < lines.size(); i++) {
                lines.get(i).setVisibility(i < linesShown ? View.VISIBLE : View.GONE);
            }
        }
        else {
            growList(items.size());
            for (int i = 0; i < rows.size(); i++) {
                if (i < items.size()) {
                    fill(rows.get(i), items.get(i));
                    rows.get(i).setVisibility(View.VISIBLE);
                }
                else {
                    rows.get(i).setVisibility(View.GONE);
                }
            }
        }
        Integer kept = places.get(placeKey());
        pane().set(kept != null ? kept : 0);
        wantThumbs();

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
    }

    // Enough grid rows and cells for `count` films
    private void growGrid(int count) {
        while (cards.size() < count) {
            int index = cards.size();
            if (index % COLUMNS == 0) {
                LinearLayout line = new LinearLayout(context);
                line.setOrientation(LinearLayout.HORIZONTAL);
                LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, CARD_HEIGHT);
                rowLp.bottomMargin = CARD_GAP;
                gridBox.addView(line, rowLp);
                lines.add(line);
            }
            FrameLayout cell = new FrameLayout(context);
            Card card = new Card(context);
            card.setOnClickListener(v -> open(index));
            cards.add(card);
            cell.addView(card, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
            // Delete, in the thumbnail's top right corner
            ImageView trash = new ImageView(context);
            trash.setImageResource(com.limelight.R.drawable.player_ic_delete);
            trash.setScaleType(ImageView.ScaleType.FIT_CENTER);
            trash.setPadding(15, 15, 15, 15);
            GradientDrawable dot = new GradientDrawable();
            dot.setShape(GradientDrawable.OVAL);
            trash.setBackground(UiKit.states(dot, 0, 0x8C000000, DANGER, DANGER));
            trash.setClickable(true);
            trash.setOnClickListener(v -> askDelete(index));
            trashButtons.add(trash);
            FrameLayout.LayoutParams tp = new FrameLayout.LayoutParams(64, 64, Gravity.TOP | Gravity.END);
            tp.topMargin = 20;
            tp.rightMargin = 20;
            cell.addView(trash, tp);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, CARD_HEIGHT, 1);
            lp.leftMargin = 8;
            lp.rightMargin = 8;
            lines.get(lines.size() - 1).addView(cell, lp);
            cells.add(cell);
        }
        // The last row keeps three columns so its cards are the same width
        int short_ = cards.size() % COLUMNS;
        if (short_ != 0) {
            growGrid(cards.size() + COLUMNS - short_);
        }
    }

    private void growList(int count) {
        while (rows.size() < count) {
            int index = rows.size();
            Row row = new Row(context);
            row.setOnClickListener(v -> open(index));
            rows.add(row);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ROW_HEIGHT);
            lp.topMargin = ROW_GAP;
            listBox.addView(row, lp);
        }
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
        card.thumb = thumbs.peek(f);
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
    /** A film in the list, which a touch may be scrolling rather than opening. */
    static boolean inList(View view) {
        return view instanceof Card || view instanceof Row;
    }

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
            titlePaint.setTextSize(UiKit.TYPE_SMALL);
            titlePaint.setColor(TEXT);
            titlePaint.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            subPaint.setTextSize(UiKit.TYPE_TINY);
            subPaint.setColor(TEXT_SOFT);
            badgePaint.setTextSize(UiKit.TYPE_TINY);
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
            titlePaint.setTextSize(UiKit.TYPE_BODY);
            titlePaint.setColor(TEXT);
            titlePaint.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            infoPaint.setTextSize(UiKit.TYPE_SMALL);
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
