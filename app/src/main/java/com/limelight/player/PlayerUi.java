package com.limelight.player;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.limelight.binding.video.XrRenderer;
import com.limelight.binding.video.XrShared;
import com.limelight.player.UiKit.SeekLine;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ConcurrentLinkedQueue;

import static com.limelight.binding.video.XrShared.UI_ANCHOR_ABOVE;
import static com.limelight.binding.video.XrShared.UI_ANCHOR_BELOW;
import static com.limelight.binding.video.XrShared.UI_ANCHOR_BOTTOM;
import static com.limelight.binding.video.XrShared.UI_ANCHOR_FRONT;
import static com.limelight.binding.video.XrShared.UI_ANCHOR_LEFT;
import static com.limelight.binding.video.XrShared.UI_PANEL_CONTROLS;
import static com.limelight.binding.video.XrShared.UI_PANEL_COUNT;
import static com.limelight.binding.video.XrShared.UI_PANEL_BACK;
import static com.limelight.binding.video.XrShared.UI_PANEL_DIALOG;
import static com.limelight.binding.video.XrShared.UI_PANEL_LOADING;
import static com.limelight.binding.video.XrShared.UI_PANEL_RAIL;
import static com.limelight.binding.video.XrShared.UI_PANEL_SHEET;
import static com.limelight.binding.video.XrShared.UI_PANEL_SUBS;
import static com.limelight.binding.video.XrShared.UI_PANEL_TOP;
import static com.limelight.player.UiKit.TEXT;
import static com.limelight.player.UiKit.TEXT_SOFT;

/**
 * The player's floating panels: a glass top bar above the screen (back, title, more), a side
 * rail (library, passthrough, tracks, settings), playback controls inside the bottom of the
 * picture, subtitles, and one sheet at a time in front (light, settings, tracks, subtitle style,
 * speed, library). Each panel is a view tree laid out at a fixed pixel size, drawn to a bitmap
 * when dirty and handed to the renderer, which anchors it to the screen.
 *
 * The views are never attached to a window, so input is dispatched by hand: the pointer picks
 * the clickable view under it (hover, click on release) and a SeekLine under it is dragged
 * while pressed. Top bar, rail and controls auto-hide CHROME_HOLD_MS after the last activity
 * unless paused or a sheet is open; a click on the picture toggles them.
 */
final class PlayerUi implements XrRenderer.UiListener {

    /** An audio or subtitle track as listed in the tracks sheet. */
    static final class TrackChoice {
        final String label;
        final String detail;
        final boolean selected;
        final boolean enabled;
        final Object token;

        TrackChoice(String label, String detail, boolean selected, boolean enabled, Object token) {
            this.label = label;
            this.detail = detail;
            this.selected = selected;
            this.enabled = enabled;
            this.token = token;
        }
    }

    private interface ChooseTrack {
        void pick(TrackChoice choice);
    }

    /** Callbacks from the panels. Main thread. */
    interface Actions {
        /** Path of the film open in the player, or null in the library. */
        String filmPath();
        /** Back from a film: to the library. The UI handles sheets and the library itself. */
        void onBack();
        /** Out of the app, confirmed. */
        void onLeave();
        void onTogglePlay();
        void onSeek(float fraction);
        void onTogglePassthrough();
        void onStereoMode(String mode);
        /** done is false while a slider is still held. */
        void onLighting(float ambient, float brightness, boolean done);
        /** Screen curvature, 0 flat to 1 fully wrapped; done is false while a slider is held. */
        void onCurve(float curve, boolean done);
        /** Playback rate, 1 = normal, pitch preserved. */
        void onSpeed(float speed);
        float speed();
        /** A COLOUR_MODE_ value. */
        void onColourMode(int mode);
        int colourMode();
        /** Hide an idle hand's pointer until a palm is shown. */
        void onHandAutoHide(boolean on);
        boolean handAutoHide();
        void onHouseLights(boolean on);
        boolean houseLights();
        /** Per-file subtitle delay, positive = later. */
        void onSubtitleDelay(long ms);
        long subtitleDelayMs();
        void onOpen(File file);
        /** Stereo depth preset name. */
        void onDepth(String name);
        /** 2D-to-3D conversion. */
        void onConvert2d(boolean on);
        void onChooseAudio(TrackChoice choice);
        void onChooseSubtitle(TrackChoice choice);
        /** Position and duration in ms; duration 0 if unknown. */
        long positionMs();
        long durationMs();
        boolean isPlaying();
        boolean isPassthroughOn();
        String stereoMode();
        String depth();
        boolean convert2d();
        float ambient();
        float brightness();
        float curve();
        List<TrackChoice> audioTracks();
        /** Off first, then embedded and sidecar tracks. */
        List<TrackChoice> subtitleTracks();
    }

    // Controls hide this long after the last pointer activity
    private static final long CHROME_HOLD_MS = 3000;
    private static final long TICK_MS = 250;

    // Panel layout sizes in pixels
    private static final int TOP_W = 1240, TOP_H = 144;
    private static final int RAIL_W = 144, RAIL_H = 600;
    // Padding plus one slot per icon
    private static int railHeight(int icons) {
        return 48 + icons * 128;
    }
    private static final int CTRL_W = 1400, CTRL_H = 144;
    private static final int SUBS_W = 1800, SUBS_H = 300;
    private static final int ICON = 112;

    // Sheets, shown one at a time in the sheet panel
    private static final int SHEET_NONE = 0, SHEET_LIGHT = 1, SHEET_SETTINGS = 2, SHEET_TRACKS = 3,
            SHEET_LIBRARY = 4, SHEET_SUBTITLES = 5, SHEET_SPEED = 6;
    private static final float[] SPEEDS = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
    // How long the skip target stays shown after the last skip
    private static final long AIM_SHOW_MS = 1500;
    // Sheets are as wide as the library page; the library and tracks sheets have its height,
    // the rest the height of their content
    private static final int SHEET_W = LibraryView.WIDTH, SHEET_H = LibraryView.HEIGHT;
    private static final boolean[] SHEET_FULL = { false, false, false, true, true, false, false };

    // Panels other than subtitles keep one size whatever the screen's; the renderer sets the
    // scale. Gaps in panel pixels.
    private static final int TOP_GAP = 48, RAIL_GAP = 60;
    // Negative gap: controls sit inside the bottom of the picture, with a margin
    private static final int CTRL_GAP = -(CTRL_H + 72);
    // Sheets float toward the viewer, except in browse mode where they take the library's place
    private static final int SHEET_GAP = 190;
    private static final int LOADING_SIZE = 160;
    private static final int DIALOG_W = 640, DIALOG_H = 600, DIALOG_GAP = 60;
    // Back button to the right of the move bar, scaling with the screen like the bar does:
    // its size, drop below the bottom edge and offset, as shares of the screen width. The bar
    // is 0.14 wide with a hover zone twice that, centred 0.0416 below the edge (xr_layout.h).
    // The circle is drawn smaller than the panel, which is the hit area.
    private static final int BACK_PX = 112, BACK_CIRCLE_PX = 76;
    private static final float BACK_FRAC = 0.05f;
    private static final float BACK_GAP = 0.0416f - BACK_FRAC / 2.0f;
    private static final float BACK_X = -(0.07f + 0.012f + BACK_FRAC / 2.0f);
    // How far panels float toward the viewer, as a share of the distance to the screen
    private static final float CHROME_DEPTH = 0.06f, SHEET_DEPTH = 0.1f, DIALOG_DEPTH = 0.16f;
    // Subtitles keep one angular size whatever the screen's, like the menus, so Small is
    // small on a big screen too. They sit above the picture's bottom edge, lifted as a share
    // of the screen width, further while the controls show.
    private static final float SUBS_LIFT = 0.025f;
    private static final float SUBS_LIFT_RAISED = SUBS_LIFT + 0.095f;
    // A disc's bitmap subtitles keep their authored share of a nominal 2250 px wide picture
    private static final float SUBS_NOMINAL_W = 2250.0f;

    private final Context context;
    private final XrRenderer renderer;
    private final Actions actions;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Panel[] panels = new Panel[UI_PANEL_COUNT];
    private final ViewGroup[] sheetRoots = new ViewGroup[SHEET_FULL.length];

    private String title = "";
    private int sheet = SHEET_NONE;
    private long lastActivityMs;
    private boolean chromeShown;
    // Toggled by clicking the picture; when false the chrome stays hidden
    private boolean chromeWanted = true;
    // No file: library in place of the screen, with only the rail beside it
    private boolean browse;

    // Pointer events queue from the frame thread and are all applied on the main thread
    // before the next render, so drags don't lag
    private static final class PointerEvent {
        final int panel;
        final float u, v;
        final boolean pressed;

        PointerEvent(int panel, float u, float v, boolean pressed) {
            this.panel = panel;
            this.u = u;
            this.v = v;
            this.pressed = pressed;
        }
    }
    private final ConcurrentLinkedQueue<PointerEvent> pointerQueue = new ConcurrentLinkedQueue<>();
    private volatile boolean pointerBusy;
    private volatile int hoverPanel = -1;
    private volatile boolean wasPressed;
    private View hoverView;
    private View pressView;
    private SeekLine dragLine;
    private int dragPanel = -1;
    // Vertical drag on the library scrolls it, once it has moved DRAG_START_PX. A release
    // within CLICK_SLOP_PX of the press clicks even if the pinch drifted off the view.
    private float pressV = -1;
    private float pressX, pressY;
    private float lastV;
    private long lastMoveMs;
    private float dragVelocity;
    private boolean dragScrolled;
    private long lastScrollMs;
    private static final float DRAG_START_PX = 40.0f;
    private static final float CLICK_SLOP_PX = 48.0f;
    private static final long SCROLL_GAP_MS = 300;

    private ImageView backButton;
    private ImageView tracksButton;
    private ImageView playButton;
    private TextView elapsedText;
    private TextView remainingText;
    private SeekLine seekLine;
    private TextView titleText;
    private ImageView passthroughButton;
    private final Map<String, TextView> stereoOptions = new LinkedHashMap<>();
    private final Map<String, TextView> depthOptions = new LinkedHashMap<>();
    private TextView passthroughOn, passthroughOff;
    private TextView convertOn, convertOff;
    // Settings that only apply to a playing film, hidden in the library
    private View[] filmRows;
    // "Leave?" dialog, in the dialog panel in place of the library's
    private ViewGroup exitDialog;
    private boolean exitAsking;
    private TextView colourNatural, colourVivid;
    private TextView handHide, handAlways;
    private TextView houseOff, houseOn;
    private TextView jumpText;
    private TextView speedButton;
    private final java.util.Map<Float, TextView> speedOptions = new java.util.HashMap<>();
    // Skip target, total offset of the current skip run, and display deadline; aimMs -1 if none
    private long aimMs = -1, aimJumpMs, aimUntil;
    private SeekLine ambientLine, brightnessLine, curveLine;
    private TextView ambientValue, brightnessValue, curveValue;
    private OptionList audioList, subtitleList;
    private String audioSignature = "", subtitleSignature = "";
    private LibraryView library;
    private SubtitleView subsText;
    private ImageView subsImage;
    private final SubtitleStyle subStyle;
    // Current subtitle, kept for redrawing on style change
    private CharSequence lastSubText;
    private Bitmap lastSubImage;
    private float lastSubShare;
    private SubtitleView subPreview;
    private TextView syncLabel;
    private final List<TextView[]> subStyleRows = new ArrayList<>();
    private boolean subsVisible;

    private static final class Panel {
        final int id;
        ViewGroup root;
        int width, height;
        final int anchor;
        // Fixed: one size whatever the screen's, gap in pixels. Otherwise width and gap are
        // fractions of the screen width.
        boolean fixed = true;
        float widthFrac;
        float gap;
        float xOff;
        // Height from the content, up to `height`; the height last drawn
        boolean wrap;
        int drawn;
        boolean dirty = true;
        // Debug builds save the next render as PNG
        boolean dumpNext = true;
        Bitmap bitmap;

        Panel(int id, ViewGroup root, int width, int height, int anchor, float gap) {
            this.id = id;
            this.root = root;
            this.width = width;
            this.height = height;
            this.anchor = anchor;
            this.gap = gap;
            drawn = height;
        }
    }

    PlayerUi(Context context, XrRenderer renderer, Actions actions) {
        this.context = context;
        this.renderer = renderer;
        this.actions = actions;
        this.subStyle = SubtitleStyle.load(context);
    }

    /** Builds the panels and starts the update tick. Main thread. */
    void start(String title) {
        this.title = title;
        panels[UI_PANEL_TOP] = new Panel(UI_PANEL_TOP, buildTop(), TOP_W, TOP_H,
                UI_ANCHOR_ABOVE, TOP_GAP);
        panels[UI_PANEL_RAIL] = new Panel(UI_PANEL_RAIL, buildRail(), RAIL_W, RAIL_H,
                UI_ANCHOR_LEFT, RAIL_GAP);
        panels[UI_PANEL_CONTROLS] = new Panel(UI_PANEL_CONTROLS, buildControls(), CTRL_W, CTRL_H,
                UI_ANCHOR_BELOW, CTRL_GAP);
        panels[UI_PANEL_SUBS] = new Panel(UI_PANEL_SUBS, buildSubtitles(), SUBS_W, SUBS_H,
                UI_ANCHOR_BOTTOM, SUBS_LIFT);
        panels[UI_PANEL_LOADING] = new Panel(UI_PANEL_LOADING, buildLoading(), LOADING_SIZE,
                LOADING_SIZE, UI_ANCHOR_FRONT, 4);
        panels[UI_PANEL_BACK] = new Panel(UI_PANEL_BACK, buildBack(), BACK_PX, BACK_PX,
                UI_ANCHOR_BELOW, BACK_GAP);
        panels[UI_PANEL_BACK].fixed = false;
        panels[UI_PANEL_BACK].widthFrac = BACK_FRAC;
        panels[UI_PANEL_BACK].xOff = BACK_X;
        sheetRoots[SHEET_LIGHT] = buildLight();
        sheetRoots[SHEET_SETTINGS] = buildSettings();
        sheetRoots[SHEET_TRACKS] = buildTracks();
        sheetRoots[SHEET_SUBTITLES] = buildSubtitleStyle();
        sheetRoots[SHEET_SPEED] = buildSpeed();
        renderer.setUiPanelDepth(UI_PANEL_SUBS, subStyle.depthK());
        // Floated in front of the picture, so a 3D film's pop-out never comes through them
        renderer.setUiPanelDepth(UI_PANEL_TOP, CHROME_DEPTH);
        renderer.setUiPanelDepth(UI_PANEL_RAIL, CHROME_DEPTH);
        renderer.setUiPanelDepth(UI_PANEL_CONTROLS, CHROME_DEPTH);
        renderer.setUiPanelDepth(UI_PANEL_SHEET, SHEET_DEPTH);
        library = new LibraryView(context, new PlaybackStore(context), new LibraryView.Host() {
            @Override
            public void onPlay(File file) {
                actions.onOpen(file);
            }

            @Override
            public void onClose() {
                openSheet(SHEET_NONE);
            }

            @Override
            public boolean isPlaying(File file) {
                String playing = actions.filmPath();
                return playing != null
                        && PlaybackStore.norm(playing).equals(PlaybackStore.norm(file.getAbsolutePath()));
            }

            @Override
            public void onChanged() {
                if (sheet == SHEET_LIBRARY) {
                    markDirty(UI_PANEL_SHEET);
                    if (!exitAsking) {
                        panels[UI_PANEL_DIALOG].root = library.dialog();
                        markDirty(UI_PANEL_DIALOG);
                    }
                    renderDirty();
                    applyVisibility(true);
                }
            }
        });
        exitDialog = buildExitDialog();
        panels[UI_PANEL_DIALOG] = new Panel(UI_PANEL_DIALOG, library.dialog(), DIALOG_W, DIALOG_H,
                UI_ANCHOR_FRONT, DIALOG_GAP);
        panels[UI_PANEL_DIALOG].wrap = true;
        panels[UI_PANEL_DIALOG].dirty = false;
        renderer.setUiPanelDepth(UI_PANEL_DIALOG, DIALOG_DEPTH);
        sheetRoots[SHEET_LIBRARY] = library.root();
        panels[UI_PANEL_SHEET] = new Panel(UI_PANEL_SHEET, sheetRoots[SHEET_SETTINGS],
                SHEET_W, SHEET_H, UI_ANCHOR_FRONT, SHEET_GAP);
        // Empty until a sheet opens or a subtitle arrives
        panels[UI_PANEL_SHEET].dirty = false;
        panels[UI_PANEL_SUBS].dirty = false;
        lastActivityMs = SystemClock.uptimeMillis();
        refreshState();
        renderDirty();
        applyVisibility(true);
        main.postDelayed(tick, TICK_MS);
        if (com.limelight.BuildConfig.DEBUG) {
            previewSheets();
        }
    }

    String libraryPlace() {
        return library.place();
    }

    void restoreLibrary(String place) {
        library.restore(place);
    }

    /** Browse mode (no file): shows the library in place of the screen. Main thread. */
    void startBrowsing() {
        browse = true;
        title = "Library";
        library.setCloseable(false);
        tracksButton.setVisibility(View.GONE);
        for (View row : filmRows) {
            row.setVisibility(View.GONE);
        }
        for (Pages pages : pageSets) {
            pages.showAll(true);
        }
        // The library window is what the handles frame, so it and the rail beside it stay on
        // the screen plane; only the dialog floats
        renderer.setUiPanelDepth(UI_PANEL_SHEET, 0.0f);
        renderer.setUiPanelDepth(UI_PANEL_RAIL, 0.0f);
        // Shrink the rail to the remaining icons
        panels[UI_PANEL_RAIL].height = railHeight(3);
        markDirty(UI_PANEL_RAIL);
        openSheet(SHEET_LIBRARY);
    }

    /** A film opened in the running session: back from browse mode to the film's panels. Main thread. */
    void startFilm(String title) {
        this.title = title;
        if (browse) {
            browse = false;
            library.setCloseable(true);
            tracksButton.setVisibility(View.VISIBLE);
            for (View row : filmRows) {
                row.setVisibility(View.VISIBLE);
            }
            for (Pages pages : pageSets) {
                pages.showAll(false);
            }
            renderer.setUiPanelDepth(UI_PANEL_SHEET, SHEET_DEPTH);
            renderer.setUiPanelDepth(UI_PANEL_RAIL, CHROME_DEPTH);
            panels[UI_PANEL_RAIL].height = RAIL_H;
            markDirty(UI_PANEL_RAIL);
        }
        audioSignature = "";
        subtitleSignature = "";
        setSubtitle(null, null, 0.0f);
        aimUntil = 0;
        openSheet(SHEET_NONE);
        nudge();
    }

    /** Debug: opens a sheet and dumps it a few times as its content loads. */
    void debugOpen(int which) {
        openSheet(which);
        for (int i = 1; i <= 3; i++) {
            main.postDelayed(() -> {
                panels[UI_PANEL_SHEET].dumpNext = true;
                markDirty(UI_PANEL_SHEET);
                renderDirty();
            }, 5000L * i);
        }
    }

    void stop() {
        main.removeCallbacksAndMessages(null);
        if (library != null) {
            library.stop();
        }
    }

    /** Thumbstick scroll: a screenful per SCROLL_GAP_MS regardless of magnitude. Main thread. */
    void onScroll(int clicks) {
        long now = SystemClock.uptimeMillis();
        if (sheet != SHEET_LIBRARY || clicks == 0 || now - lastScrollMs < SCROLL_GAP_MS) {
            return;
        }
        lastScrollMs = now;
        // Stick forward = up
        library.nudge(clicks > 0 ? -1 : 1);
    }

    /**
     * Back: closes an open sheet, returns the library to its Films page, or asks before leaving.
     * False when the caller should leave the film for the library. Main thread.
     */
    boolean onBackPressed() {
        if (exitAsking) {
            return true;
        }
        if (sheet != SHEET_NONE && !(browse && sheet == SHEET_LIBRARY)) {
            openSheet(SHEET_NONE);
            return true;
        }
        if (!browse) {
            return false;
        }
        if (!library.atHome()) {
            library.goHome();
            return true;
        }
        askExit();
        return true;
    }

    private void askExit() {
        exitAsking = true;
        panels[UI_PANEL_DIALOG].root = exitDialog;
        markDirty(UI_PANEL_DIALOG);
        renderDirty();
        applyVisibility(true);
    }

    private void cancelExit() {
        exitAsking = false;
        panels[UI_PANEL_DIALOG].root = library.dialog();
        markDirty(UI_PANEL_DIALOG);
        applyVisibility(true);
    }

    private ViewGroup buildExitDialog() {
        LinearLayout dialog = new LinearLayout(context);
        dialog.setOrientation(LinearLayout.VERTICAL);
        dialog.setBackground(UiKit.frost(40, UiKit.DIALOG_BASE));
        dialog.setPadding(48, 40, 48, 40);
        dialog.setClickable(true);
        dialog.addView(UiKit.text(context, "Leave AaksVR?", 40, true, TEXT));
        TextView note = UiKit.text(context, "Where you stopped is saved.", 28, false, TEXT_SOFT);
        note.setPadding(0, 14, 0, 0);
        dialog.addView(note);
        LinearLayout buttons = new LinearLayout(context);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams left = new LinearLayout.LayoutParams(0, 84, 1);
        left.rightMargin = 16;
        buttons.addView(UiKit.segment(context, "Stay", v -> cancelExit()), left);
        buttons.addView(UiKit.segment(context, "Leave", v -> actions.onLeave()),
                new LinearLayout.LayoutParams(0, 84, 1));
        LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 84);
        rowLp.topMargin = 36;
        dialog.addView(buttons, rowLp);
        return UiKit.wrap(context, dialog);
    }

    /** Whether a sheet is open. Main thread. */
    boolean sheetOpen() {
        return sheet != SHEET_NONE;
    }

    /** Shows the controls briefly, e.g. after a play/pause from the gamepad. Main thread. */
    void nudge() {
        if (browse) {
            return;
        }
        chromeWanted = true;
        lastActivityMs = SystemClock.uptimeMillis();
        refreshState();
        applyVisibility(true);
        renderDirty();
    }

    /** Pointer activity on the picture; postpones auto-hide. Any thread. */
    void onActivity() {
        lastActivityMs = SystemClock.uptimeMillis();
    }

    /** Toggles the controls. Any thread. */
    void onPictureClick() {
        main.post(() -> {
            if (browse) {
                return;
            }
            if (chromeShown) {
                chromeWanted = false;
                if (sheet != SHEET_NONE) {
                    sheet = SHEET_NONE;
                }
            }
            else {
                chromeWanted = true;
                lastActivityMs = SystemClock.uptimeMillis();
            }
            applyVisibility(true);
        });
    }

    /** Current subtitle: text, a bitmap (PGS), or neither. Main thread. */
    void setSubtitle(CharSequence text, Bitmap image, float widthShare) {
        lastSubText = text;
        lastSubImage = image;
        lastSubShare = widthShare;
        showSubtitle();
    }

    private void showSubtitle() {
        if (subsText == null) {
            return;
        }
        CharSequence text = lastSubText;
        Bitmap image = lastSubImage;
        boolean hasText = text != null && text.length() > 0;
        subsText.set(hasText ? text : "", subStyle);
        subsText.setVisibility(hasText ? View.VISIBLE : View.GONE);
        subsImage.setImageBitmap(hasText ? null : image);
        subsImage.setVisibility(!hasText && image != null ? View.VISIBLE : View.GONE);
        if (!hasText && image != null) {
            float share = lastSubShare > 0.0f ? lastSubShare * SUBS_NOMINAL_W : SUBS_W;
            int width = Math.min(SUBS_W, Math.round(share * subStyle.scale()));
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) subsImage.getLayoutParams();
            lp.width = width;
            lp.height = ViewGroup.LayoutParams.WRAP_CONTENT;
            subsImage.setLayoutParams(lp);
        }
        boolean visible = hasText || image != null;
        markDirty(UI_PANEL_SUBS);
        if (visible) {
            renderDirty();
        }
        if (visible != subsVisible) {
            subsVisible = visible;
            applyVisibility(true);
        }
    }

    // Saves the style and redraws the subtitle and preview
    private void styleChanged() {
        subStyle.save(context);
        showSubtitle();
        updateSubPreview();
        refreshSubStyleSelection();
        markDirty(UI_PANEL_SUBS);
        markDirty(UI_PANEL_SHEET);
        renderer.setUiPanelDepth(UI_PANEL_SUBS, subStyle.depthK());
        applyVisibility(true);
        renderDirty();
    }

    // Debug: dumps every sheet to PNG at startup
    private void previewSheets() {
        for (int which = SHEET_LIGHT; which < SHEET_FULL.length; which++) {
            ViewGroup root = sheetRoots[which];
            int w = SHEET_W, h = SHEET_H;
            if (!SHEET_FULL[which]) {
                root.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                             View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
                h = Math.min(h, Math.max(1, root.getMeasuredHeight()));
            }
            root.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                         View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY));
            root.layout(0, 0, w, h);
            Bitmap bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            root.draw(new Canvas(bitmap));
            dumpPanel("sheet_" + which, bitmap);
            bitmap.recycle();
        }
    }

    // ---- pointer --------------------------------------------------------------

    @Override
    public void onUiPointer(int panel, float u, float v, boolean pressed) {
        if (panel >= 0) {
            lastActivityMs = SystemClock.uptimeMillis();
        }
        if (panel < 0 && hoverPanel < 0 && !pressed && !wasPressed && pointerQueue.isEmpty()) {
            return;
        }
        pointerQueue.add(new PointerEvent(panel, u, v, pressed));
        if (!pointerBusy) {
            pointerBusy = true;
            main.post(drainPointer);
        }
    }

    private final Runnable drainPointer = new Runnable() {
        @Override
        public void run() {
            pointerBusy = false;
            PointerEvent e;
            while ((e = pointerQueue.poll()) != null) {
                handlePointer(e.panel, e.u, e.v, e.pressed);
            }
            renderDirty();
        }
    };

    private void handlePointer(int panelId, float u, float v, boolean pressed) {
        Panel panel = panelId >= 0 ? panels[panelId] : null;
        float x = panel != null ? u * panel.width : 0.0f;
        float y = panel != null ? v * panel.drawn : 0.0f;
        View target = panel != null ? clickableAt(panel.root, x, y) : null;

        if (dragLine != null) {
            if (panelId == dragPanel) {
                dragTo(x);
            }
            if (!pressed) {
                finishDrag();
            }
        }
        else if (pressed && !wasPressed) {
            pressView = target;
            pressX = x;
            pressY = y;
            pressV = panelId == UI_PANEL_SHEET && sheet == SHEET_LIBRARY ? v : -1;
            lastV = v;
            lastMoveMs = SystemClock.uptimeMillis();
            dragVelocity = 0.0f;
            dragScrolled = false;
            if (pressV >= 0) {
                library.dragBegin();
            }
            if (target instanceof SeekLine) {
                dragLine = (SeekLine) target;
                dragPanel = panelId;
                dragTo(x);
            }
        }
        else if (pressed && pressV >= 0 && panelId == UI_PANEL_SHEET) {
            // The list follows the pointer; a drag cancels the click
            float moved = (pressV - v) * panel.drawn;
            long now = SystemClock.uptimeMillis();
            if (now > lastMoveMs) {
                float speed = (lastV - v) * panel.drawn * 1000.0f / (now - lastMoveMs);
                dragVelocity = dragVelocity * 0.5f + speed * 0.5f;
            }
            lastV = v;
            lastMoveMs = now;
            if (!dragScrolled && Math.abs(moved) > DRAG_START_PX) {
                dragScrolled = true;
                pressView = null;
            }
            if (dragScrolled) {
                library.dragTo(moved);
            }
        }
        else if (!pressed && wasPressed) {
            boolean near = panel != null && Math.hypot(x - pressX, y - pressY) <= CLICK_SLOP_PX;
            View clicked = pressView != null ? pressView : (near ? target : null);
            if (clicked != null && (clicked == target || near) && !dragScrolled) {
                clicked.performClick();
            }
            if (dragScrolled) {
                library.release(dragVelocity);
            }
            pressView = null;
            pressV = -1;
        }
        wasPressed = pressed;

        if (target != hoverView) {
            if (hoverView != null) {
                hoverView.setHovered(false);
                markDirty(hoverPanel);
            }
            hoverView = target;
            if (target != null) {
                target.setHovered(true);
                markDirty(panelId);
            }
        }
        hoverPanel = panelId;
    }

    private void dragTo(float x) {
        Panel panel = panels[dragPanel];
        dragLine.setFraction(dragLine.fractionAt(x - offsetX(dragLine, panel.root)));
        markDirty(dragPanel);
        if (dragLine.listener != null) {
            dragLine.listener.onMoved(dragLine.fraction, false);
        }
    }

    private void finishDrag() {
        if (dragLine.listener != null) {
            dragLine.listener.onMoved(dragLine.fraction, true);
        }
        dragLine = null;
        dragPanel = -1;
    }

    // Deepest clickable view at a point in panel pixels
    private static View clickableAt(View view, float x, float y) {
        if (view.getVisibility() != View.VISIBLE || x < view.getLeft() || x >= view.getRight()
                || y < view.getTop() || y >= view.getBottom()) {
            return null;
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            float cx = x - view.getLeft() + group.getScrollX();
            float cy = y - view.getTop() + group.getScrollY();
            for (int i = group.getChildCount() - 1; i >= 0; i--) {
                View hit = clickableAt(group.getChildAt(i), cx, cy);
                if (hit != null) {
                    return hit;
                }
            }
        }
        return view.isClickable() && view.isEnabled() ? view : null;
    }

    // View's left edge in panel pixels
    private static float offsetX(View view, View root) {
        float x = 0.0f;
        for (View v = view; v != null && v != root; v = (View) v.getParent()) {
            x += v.getLeft();
        }
        return x;
    }

    // ---- state and visibility -------------------------------------------------------

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            refreshState();
            applyVisibility(false);
            renderDirty();
            main.postDelayed(this, TICK_MS);
        }
    };

    private void refreshState() {
        boolean playing = actions.isPlaying();
        setIcon(playButton, playing ? com.limelight.R.drawable.player_ic_pause
                                    : com.limelight.R.drawable.player_ic_play, UI_PANEL_CONTROLS);
        long pos = actions.positionMs();
        long dur = actions.durationMs();
        // While dragging or skipping, show the target time
        boolean dragging = dragLine == seekLine && dur > 0;
        boolean skipping = !dragging && aimMs >= 0 && SystemClock.uptimeMillis() < aimUntil;
        if (!dragging && !skipping) {
            aimMs = -1;
        }
        long shown = dragging ? (long) (seekLine.fraction * dur) : skipping ? aimMs : pos;
        setText(elapsedText, UiKit.formatTime(shown), UI_PANEL_CONTROLS);
        setColour(elapsedText, dragging || skipping ? UiKit.AIM : TEXT, UI_PANEL_CONTROLS);
        setText(remainingText, dur > 0 ? "-" + UiKit.formatTime(Math.max(0, dur - shown)) : "", UI_PANEL_CONTROLS);
        setText(jumpText, skipping ? UiKit.formatJump(aimJumpMs) : "", UI_PANEL_CONTROLS);
        setVisible(jumpText, skipping, UI_PANEL_CONTROLS);
        if (!dragging && dur > 0) {
            float f = (float) shown / (float) dur;
            if (Math.abs(f - seekLine.fraction) > 0.0005f) {
                seekLine.setFraction(f);
                markDirty(UI_PANEL_CONTROLS);
            }
        }
        float speed = actions.speed();
        setText(speedButton, UiKit.formatSpeed(speed), UI_PANEL_CONTROLS);
        // Highlighted when not at 1x
        setSelected(speedButton, Math.abs(speed - 1.0f) > 0.001f, UI_PANEL_CONTROLS);
        for (java.util.Map.Entry<Float, TextView> option : speedOptions.entrySet()) {
            setSelected(option.getValue(), Math.abs(option.getKey() - speed) < 0.001f, UI_PANEL_SHEET);
        }
        setText(titleText, title, UI_PANEL_TOP);

        boolean pass = actions.isPassthroughOn();
        setSelected(passthroughOn, pass, UI_PANEL_SHEET);
        setSelected(passthroughOff, !pass, UI_PANEL_SHEET);
        setSelected(passthroughButton, pass, UI_PANEL_RAIL);
        String mode = actions.stereoMode();
        for (Map.Entry<String, TextView> e : stereoOptions.entrySet()) {
            setSelected(e.getValue(), e.getKey().equals(mode), UI_PANEL_SHEET);
        }
        String depth = actions.depth();
        for (Map.Entry<String, TextView> e : depthOptions.entrySet()) {
            setSelected(e.getValue(), e.getKey().equals(depth), UI_PANEL_SHEET);
        }
        setSelected(convertOn, actions.convert2d(), UI_PANEL_SHEET);
        setSelected(convertOff, !actions.convert2d(), UI_PANEL_SHEET);
        boolean vivid = actions.colourMode() == XrShared.COLOUR_MODE_VIVID;
        setSelected(colourVivid, vivid, UI_PANEL_SHEET);
        setSelected(colourNatural, !vivid, UI_PANEL_SHEET);
        boolean hide = actions.handAutoHide();
        setSelected(handHide, hide, UI_PANEL_SHEET);
        setSelected(handAlways, !hide, UI_PANEL_SHEET);
        boolean house = actions.houseLights();
        setSelected(houseOn, house, UI_PANEL_SHEET);
        setSelected(houseOff, !house, UI_PANEL_SHEET);

        if (dragLine != ambientLine && Math.abs(ambientLine.fraction - actions.ambient()) > 0.001f) {
            ambientLine.setFraction(actions.ambient());
            markDirty(UI_PANEL_SHEET);
        }
        if (dragLine != brightnessLine && Math.abs(brightnessLine.fraction - actions.brightness()) > 0.001f) {
            brightnessLine.setFraction(actions.brightness());
            markDirty(UI_PANEL_SHEET);
        }
        if (dragLine != curveLine && Math.abs(curveLine.fraction - actions.curve()) > 0.001f) {
            curveLine.setFraction(actions.curve());
            markDirty(UI_PANEL_SHEET);
        }
        setText(curveValue, UiKit.percent(curveLine.fraction), UI_PANEL_SHEET);
        setText(ambientValue, UiKit.percent(ambientLine.fraction), UI_PANEL_SHEET);
        setText(brightnessValue, UiKit.percent(brightnessLine.fraction), UI_PANEL_SHEET);

        if (sheet == SHEET_TRACKS) {
            audioSignature = audioList.set(actions.audioTracks(), audioSignature, this);
            subtitleSignature = subtitleList.set(actions.subtitleTracks(), subtitleSignature, this);
        }
    }

    private void applyVisibility(boolean force) {
        boolean show = chromeWanted && (sheet != SHEET_NONE || !actions.isPlaying()
                || SystemClock.uptimeMillis() - lastActivityMs < CHROME_HOLD_MS);
        // Once auto-hidden, stay hidden until the next click
        if (!show) {
            chromeWanted = false;
        }
        if (show == chromeShown && !force) {
            return;
        }
        chromeShown = show;
        for (Panel p : panels) {
            boolean visible;
            switch (p.id) {
                case UI_PANEL_SHEET:
                    visible = sheet != SHEET_NONE;
                    break;
                case UI_PANEL_TOP:
                case UI_PANEL_CONTROLS:
                    visible = show && !browse;
                    break;
                case UI_PANEL_RAIL:
                    // In browse mode the rail shows only beside the library
                    visible = show && (!browse || sheet == SHEET_LIBRARY);
                    break;
                case UI_PANEL_SUBS:
                    visible = subsVisible;
                    p.gap = (show ? SUBS_LIFT_RAISED : SUBS_LIFT) + subStyle.lift();
                    break;
                case UI_PANEL_DIALOG:
                    visible = exitAsking || (sheet == SHEET_LIBRARY && library.confirming());
                    p.gap = browse ? DIALOG_GAP : SHEET_GAP + DIALOG_GAP;
                    break;
                case UI_PANEL_BACK:
                    // Whenever the bar shows, which the renderer decides
                    visible = true;
                    break;
                default:
                    visible = show;
                    break;
            }
            renderer.setUiPanel(p.id, visible, p.anchor, p.fixed, p.widthFrac, p.gap, p.xOff);
        }
    }

    private void toggleSheet(int which) {
        openSheet(sheet == which ? SHEET_NONE : which);
    }

    private void openSheet(int which) {
        renderer.beginUiChange();
        try {
            openSheetNow(which);
        }
        finally {
            renderer.endUiChange();
        }
    }

    private void openSheetNow(int which) {
        // In browse mode, closing a sheet returns to the library
        if (browse && which == SHEET_NONE) {
            which = SHEET_LIBRARY;
        }
        if (hoverView != null && hoverPanel == UI_PANEL_SHEET) {
            hoverView.setHovered(false);
            hoverView = null;
        }
        sheet = which;
        lastActivityMs = SystemClock.uptimeMillis();
        if (which != SHEET_NONE) {
            Panel p = panels[UI_PANEL_SHEET];
            p.root = sheetRoots[which];
            p.width = SHEET_W;
            p.height = SHEET_H;
            p.wrap = !SHEET_FULL[which];
            // In browse mode the library is the window, which the viewer sizes; other sheets
            // take its place at the menus' own size
            p.fixed = !(browse && which == SHEET_LIBRARY);
            p.widthFrac = 1.0f;
            p.gap = browse ? 0.0f : SHEET_GAP;
            p.dirty = true;
            p.dumpNext = true;
            if (which == SHEET_LIBRARY) {
                library.onOpened();
            }
            for (Pages pages : pageSets) {
                pages.reset();
            }
            if (which == SHEET_SUBTITLES) {
                updateSyncLabel();
                updateSubPreview();
                refreshSubStyleSelection();
            }
            if (which == SHEET_TRACKS) {
                // Force a rebuild of the track lists
                audioSignature = "";
                subtitleSignature = "";
            }
        }
        refreshState();
        renderDirty();
        applyVisibility(true);
    }

    // ---- drawing ----------------------------------------------------------------------

    private void markDirty(int panel) {
        if (panel >= 0 && panel < UI_PANEL_COUNT && panels[panel] != null) {
            panels[panel].dirty = true;
        }
    }

    void markSheetDirty() {
        markDirty(UI_PANEL_SHEET);
    }

    private void renderDirty() {
        for (Panel p : panels) {
            if (p != null && p.dirty) {
                p.dirty = false;
                // A closed sheet or dialog is redrawn when it next opens
                if (p.id == UI_PANEL_SHEET && sheet == SHEET_NONE) {
                    continue;
                }
                if (p.id == UI_PANEL_DIALOG && !library.confirming() && !exitAsking) {
                    continue;
                }
                render(p);
            }
        }
    }

    private void render(Panel p) {
        int width = View.MeasureSpec.makeMeasureSpec(p.width, View.MeasureSpec.EXACTLY);
        int height = p.height;
        if (p.wrap) {
            p.root.measure(width, View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            height = Math.min(p.height, Math.max(1, p.root.getMeasuredHeight()));
        }
        p.root.measure(width, View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
        p.root.layout(0, 0, p.width, height);
        p.drawn = height;
        if (p.bitmap == null || p.bitmap.getWidth() != p.width || p.bitmap.getHeight() != height) {
            p.bitmap = Bitmap.createBitmap(p.width, height, Bitmap.Config.ARGB_8888);
        }
        else {
            p.bitmap.eraseColor(Color.TRANSPARENT);
        }
        p.root.draw(new Canvas(p.bitmap));
        renderer.uploadUiPanel(p.id, p.bitmap);
        if (com.limelight.BuildConfig.DEBUG && p.dumpNext) {
            p.dumpNext = false;
            dumpPanel(p.id == UI_PANEL_SHEET ? "open_" + sheet : "panel_" + p.id, p.bitmap);
        }
    }

    // Debug: saves a panel as PNG under files/ui. Done once per open, not per render, since
    // PNG compression is slow enough to make sliders lag.
    private void dumpPanel(String name, Bitmap bitmap) {
        File dir = context.getExternalFilesDir("ui");
        if (dir == null) {
            return;
        }
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(new File(dir, name + ".png"))) {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
        }
        catch (java.io.IOException ignored) {
        }
    }

    private void setText(TextView view, String text, int panel) {
        if (view != null && !TextUtils.equals(view.getText(), text)) {
            view.setText(text);
            markDirty(panel);
        }
    }

    private void setColour(TextView view, int colour, int panel) {
        if (view != null && view.getCurrentTextColor() != colour) {
            view.setTextColor(colour);
            markDirty(panel);
        }
    }

    private void setVisible(View view, boolean visible, int panel) {
        int want = visible ? View.VISIBLE : View.GONE;
        if (view != null && view.getVisibility() != want) {
            view.setVisibility(want);
            markDirty(panel);
        }
    }

    private void setIcon(ImageView view, int res, int panel) {
        if (view != null && !Integer.valueOf(res).equals(view.getTag())) {
            view.setImageResource(res);
            view.setTag(res);
            markDirty(panel);
        }
    }

    private void setSelected(View view, boolean selected, int panel) {
        if (view != null && view.isSelected() != selected) {
            view.setSelected(selected);
            markDirty(panel);
        }
    }

    // ---- building: bars -----------------------------------------------------------------

    private LinearLayout pill(int orientation, int thickness) {
        LinearLayout pill = new LinearLayout(context);
        pill.setOrientation(orientation);
        pill.setGravity(Gravity.CENTER);
        pill.setBackground(UiKit.glass(thickness / 2f));
        return pill;
    }

    private ViewGroup buildTop() {
        LinearLayout pill = pill(LinearLayout.HORIZONTAL, TOP_H);
        pill.setPadding(16, 0, 16, 0);
        backButton = UiKit.iconButton(context, com.limelight.R.drawable.player_ic_back, ICON, v -> actions.onBack());
        pill.addView(backButton);
        titleText = UiKit.text(context, "", 40, true, TEXT);
        titleText.setGravity(Gravity.CENTER);
        titleText.setMaxLines(1);
        titleText.setEllipsize(TextUtils.TruncateAt.END);
        titleText.setPadding(24, 0, 24, 0);
        pill.addView(titleText, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));
        pill.addView(UiKit.iconButton(context, com.limelight.R.drawable.player_ic_more, ICON,
                v -> toggleSheet(SHEET_LIGHT)));
        return UiKit.wrap(context, pill);
    }

    private ViewGroup buildRail() {
        LinearLayout pill = pill(LinearLayout.VERTICAL, RAIL_W);
        pill.setPadding(0, 24, 0, 24);
        pill.addView(UiKit.iconButton(context, com.limelight.R.drawable.player_ic_library, ICON,
                v -> toggleSheet(SHEET_LIBRARY)), railSlot());
        passthroughButton = UiKit.iconButton(context, com.limelight.R.drawable.player_ic_passthrough, ICON,
                v -> actions.onTogglePassthrough());
        pill.addView(passthroughButton, railSlot());
        tracksButton = UiKit.iconButton(context, com.limelight.R.drawable.player_ic_captions, ICON,
                v -> toggleSheet(SHEET_TRACKS));
        pill.addView(tracksButton, railSlot());
        pill.addView(UiKit.iconButton(context, com.limelight.R.drawable.player_ic_settings, ICON,
                v -> toggleSheet(SHEET_SETTINGS)), railSlot());
        return UiKit.wrap(context, pill);
    }

    private LinearLayout.LayoutParams railSlot() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ICON, 0, 1);
        lp.gravity = Gravity.CENTER;
        lp.topMargin = 8;
        lp.bottomMargin = 8;
        return lp;
    }

    private ViewGroup buildControls() {
        LinearLayout pill = pill(LinearLayout.HORIZONTAL, CTRL_H);
        pill.setPadding(16, 0, 40, 0);
        playButton = UiKit.iconButton(context, com.limelight.R.drawable.player_ic_pause, ICON,
                v -> actions.onTogglePlay());
        pill.addView(playButton);
        elapsedText = UiKit.text(context, "0:00", 34, false, TEXT);
        elapsedText.setPadding(20, 0, 24, 0);
        elapsedText.setGravity(Gravity.CENTER_VERTICAL);
        pill.addView(elapsedText, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        jumpText = UiKit.text(context, "", 30, true, UiKit.AIM);
        jumpText.setPadding(0, 0, 24, 0);
        jumpText.setGravity(Gravity.CENTER_VERTICAL);
        jumpText.setVisibility(View.GONE);
        pill.addView(jumpText, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        seekLine = new SeekLine(context);
        seekLine.listener = (f, done) -> {
            if (done) {
                actions.onSeek(f);
            }
            refreshState();
        };
        pill.addView(seekLine, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));
        remainingText = UiKit.text(context, "", 34, false, TEXT_SOFT);
        remainingText.setPadding(24, 0, 0, 0);
        remainingText.setGravity(Gravity.CENTER_VERTICAL);
        pill.addView(remainingText, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        speedButton = UiKit.segment(context, UiKit.formatSpeed(1.0f), v -> toggleSheet(SHEET_SPEED));
        LinearLayout.LayoutParams speedLp = new LinearLayout.LayoutParams(140, 84);
        speedLp.gravity = Gravity.CENTER_VERTICAL;
        speedLp.leftMargin = 24;
        pill.addView(speedButton, speedLp);
        return UiKit.wrap(context, pill);
    }

    /** Shows a skip's target time and total offset. Main thread. */
    void showSkip(long toMs, long jumpMs) {
        aimMs = toMs;
        aimJumpMs = jumpMs;
        aimUntil = SystemClock.uptimeMillis() + AIM_SHOW_MS;
        nudge();
    }

    // Speed sheet: Slower and Faster rows of three, 1x labelled Normal
    private ViewGroup buildSpeed() {
        LinearLayout box = sheetFrame("Playback speed");
        LinearLayout slower = segmentRow();
        LinearLayout faster = segmentRow();
        for (float speed : SPEEDS) {
            String name = speed == 1.0f ? "Normal" : UiKit.formatSpeed(speed);
            TextView option = UiKit.segment(context, name, v -> {
                actions.onSpeed(speed);
                refreshState();
                renderDirty();
            });
            (speed <= 1.0f ? slower : faster).addView(option, segmentParams());
            speedOptions.put(speed, option);
        }
        box.addView(label("Slower"));
        box.addView(slower);
        box.addView(label("Faster"));
        box.addView(faster);
        TextView note = UiKit.text(context, "The sound keeps its pitch. Each film starts at Normal.",
                28, false, TEXT_SOFT);
        note.setPadding(4, 30, 0, 0);
        box.addView(note);
        return UiKit.wrap(context, box);
    }

    // Subtitle strip: styled text or a bitmap
    // Round back button in the bar's white: to the library from a film, out of the app from
    // the library. The renderer shows it while the bar is hovered.
    private ViewGroup buildBack() {
        FrameLayout root = new FrameLayout(context);
        View button = new View(context) {
            private final android.graphics.Paint paint = new android.graphics.Paint(
                    android.graphics.Paint.ANTI_ALIAS_FLAG);
            private final android.graphics.drawable.Drawable arrow =
                    context.getDrawable(com.limelight.R.drawable.player_ic_back).mutate();

            @Override
            protected void onDraw(Canvas canvas) {
                float cx = getWidth() / 2.0f, cy = getHeight() / 2.0f;
                paint.setColor(isHovered() ? 0xFFFFFFFF : 0xEBFFFFFF);
                canvas.drawCircle(cx, cy, BACK_CIRCLE_PX / 2.0f, paint);
                int r = BACK_CIRCLE_PX * 5 / 16;
                arrow.setTint(0xFF14141A);
                arrow.setBounds((int) cx - r, (int) cy - r, (int) cx + r, (int) cy + r);
                arrow.draw(canvas);
            }
        };
        button.setClickable(true);
        button.setOnClickListener(v -> actions.onBack());
        root.addView(button, new FrameLayout.LayoutParams(BACK_PX, BACK_PX));
        return root;
    }

    // A faint ring with a bright arc; the renderer turns it
    private ViewGroup buildLoading() {
        FrameLayout root = new FrameLayout(context);
        root.addView(new View(context) {
            private final android.graphics.Paint paint = new android.graphics.Paint(
                    android.graphics.Paint.ANTI_ALIAS_FLAG);

            @Override
            protected void onDraw(Canvas canvas) {
                float stroke = LOADING_SIZE * 0.09f;
                float inset = stroke / 2.0f + 6.0f;
                android.graphics.RectF ring = new android.graphics.RectF(inset, inset,
                        getWidth() - inset, getHeight() - inset);
                paint.setStyle(android.graphics.Paint.Style.STROKE);
                paint.setStrokeWidth(stroke);
                paint.setStrokeCap(android.graphics.Paint.Cap.ROUND);
                paint.setColor(0x40FFFFFF);
                canvas.drawOval(ring, paint);
                paint.setColor(0xE6FFFFFF);
                canvas.drawArc(ring, -90.0f, 100.0f, false, paint);
            }
        }, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        return root;
    }

    private ViewGroup buildSubtitles() {
        FrameLayout strip = new FrameLayout(context);
        subsText = new SubtitleView(context);
        subsText.setVisibility(View.GONE);
        strip.addView(subsText, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
        subsImage = new ImageView(context);
        subsImage.setScaleType(ImageView.ScaleType.FIT_CENTER);
        subsImage.setAdjustViewBounds(true);
        subsImage.setVisibility(View.GONE);
        strip.addView(subsImage, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
        return strip;
    }

    // ---- building: sheets ---------------------------------------------------------------

    // Sheet frame: glass background, heading with a Done button, body below
    private LinearLayout sheetFrame(String heading) {
        LinearLayout box = new LinearLayout(context);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(UiKit.frost(60));
        box.setPadding(52, 36, 52, 40);

        LinearLayout head = new LinearLayout(context);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.addView(UiKit.text(context, heading, 46, true, TEXT),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        head.addView(UiKit.segment(context, "Done", v -> openSheet(SHEET_NONE)),
                new LinearLayout.LayoutParams(200, 88));
        box.addView(head);
        return box;
    }

    // Two-page sheet body: common options first, the rest behind "More options"
    private final List<Pages> pageSets = new ArrayList<>();

    private final class Pages {
        final LinearLayout first = column();
        final LinearLayout second = column();
        private TextView more, back;
        // Both pages at once, with no page buttons (the library has the room)
        private boolean all;

        Pages(LinearLayout box) {
            LinearLayout holder = column();
            holder.addView(first, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));
            holder.addView(second, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));
            second.setVisibility(View.GONE);
            box.addView(holder, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));
            pageSets.add(this);
        }

        /** Adds the page navigation buttons; call after adding the rows. */
        void finish() {
            more = nav("More options  \u203A", true);
            back = nav("\u2039  Back", false);
            first.addView(more);
            second.addView(back);
        }

        void showAll(boolean on) {
            all = on;
            if (more != null) {
                more.setVisibility(on ? View.GONE : View.VISIBLE);
                back.setVisibility(on ? View.GONE : View.VISIBLE);
            }
            reset();
        }

        private TextView nav(String text, boolean toSecond) {
            TextView b = UiKit.segment(context, text, v -> show(toSecond));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 84);
            lp.topMargin = 22;
            b.setLayoutParams(lp);
            return b;
        }

        void show(boolean secondPage) {
            if (all) {
                return;
            }
            first.setVisibility(secondPage ? View.GONE : View.VISIBLE);
            second.setVisibility(secondPage ? View.VISIBLE : View.GONE);
            markDirty(UI_PANEL_SHEET);
            renderDirty();
        }

        void reset() {
            first.setVisibility(View.VISIBLE);
            second.setVisibility(all ? View.VISIBLE : View.GONE);
        }
    }

    private LinearLayout column() {
        LinearLayout c = new LinearLayout(context);
        c.setOrientation(LinearLayout.VERTICAL);
        return c;
    }

    // Light sheet (from "more"): immersion (how far screen light spills into the room),
    // room brightness, and screen curve
    private ViewGroup buildLight() {
        LinearLayout box = sheetFrame("Light");

        LinearLayout ambientRow = row(com.limelight.R.drawable.player_ic_ambient, "Immersion",
                "How far the screen's light fills the room");
        ambientValue = UiKit.text(context, "", 34, false, TEXT_SOFT);
        ambientValue.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        ambientRow.addView(ambientValue, new LinearLayout.LayoutParams(140, ViewGroup.LayoutParams.MATCH_PARENT));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 112);
        lp.topMargin = 20;
        box.addView(ambientRow, lp);
        ambientLine = new SeekLine(context);
        ambientLine.listener = (f, done) -> {
            setText(ambientValue, UiKit.percent(f), UI_PANEL_SHEET);
            actions.onLighting(f, brightnessLine.fraction, done);
        };
        box.addView(ambientLine, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 72));

        LinearLayout brightRow = row(com.limelight.R.drawable.player_ic_brightness, "Brightness",
                "Lower is a darker room");
        brightnessValue = UiKit.text(context, "", 34, false, TEXT_SOFT);
        brightnessValue.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        brightRow.addView(brightnessValue, new LinearLayout.LayoutParams(140, ViewGroup.LayoutParams.MATCH_PARENT));
        lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 112);
        lp.topMargin = 10;
        box.addView(brightRow, lp);
        brightnessLine = new SeekLine(context);
        brightnessLine.listener = (f, done) -> {
            setText(brightnessValue, UiKit.percent(f), UI_PANEL_SHEET);
            actions.onLighting(ambientLine.fraction, f, done);
        };
        box.addView(brightnessLine, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 72));

        LinearLayout curveRow = row(com.limelight.R.drawable.player_ic_curve, "Curve",
                "Flat to wrapped round you");
        curveValue = UiKit.text(context, "", 34, false, TEXT_SOFT);
        curveValue.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        curveRow.addView(curveValue, new LinearLayout.LayoutParams(140, ViewGroup.LayoutParams.MATCH_PARENT));
        lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 112);
        lp.topMargin = 10;
        box.addView(curveRow, lp);
        curveLine = new SeekLine(context);
        curveLine.listener = (f, done) -> {
            setText(curveValue, UiKit.percent(f), UI_PANEL_SHEET);
            actions.onCurve(f, done);
        };
        box.addView(curveLine, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 72));

        LinearLayout houseRow = row(com.limelight.R.drawable.player_ic_lamp, "House lights",
                "Up when you pause, down as the film plays");
        houseOff = UiKit.segment(context, "Off", v -> {
            actions.onHouseLights(false);
            refreshState();
            renderDirty();
        });
        houseOn = UiKit.segment(context, "On", v -> {
            actions.onHouseLights(true);
            refreshState();
            renderDirty();
        });
        for (TextView option : new TextView[] { houseOff, houseOn }) {
            LinearLayout.LayoutParams olp = new LinearLayout.LayoutParams(130, 76);
            olp.leftMargin = 12;
            houseRow.addView(option, olp);
        }
        lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 112);
        lp.topMargin = 10;
        box.addView(houseRow, lp);
        return UiKit.wrap(context, box);
    }

    // Icon, title and hint, with space on the right for a value
    private LinearLayout row(int icon, String name, String hint) {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        ImageView image = new ImageView(context);
        image.setImageResource(icon);
        image.setScaleType(ImageView.ScaleType.FIT_CENTER);
        image.setPadding(14, 14, 14, 14);
        image.setBackground(UiKit.circle(UiKit.BUTTON));
        row.addView(image, new LinearLayout.LayoutParams(88, 88));
        LinearLayout words = new LinearLayout(context);
        words.setOrientation(LinearLayout.VERTICAL);
        words.setPadding(28, 0, 0, 0);
        words.addView(UiKit.text(context, name, 36, true, TEXT));
        TextView sub = UiKit.text(context, hint, 28, false, TEXT_SOFT);
        sub.setPadding(0, 6, 0, 0);
        words.addView(sub);
        row.addView(words, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return row;
    }

    private ViewGroup buildSettings() {
        LinearLayout box = sheetFrame("Settings");
        Pages pages = new Pages(box);

        View stereoLabel = label(com.limelight.R.drawable.player_ic_stereo, "3D");
        pages.first.addView(stereoLabel);
        LinearLayout stereo = segmentRow();
        addOption(stereo, "auto", "Auto");
        addOption(stereo, "mono", "2D");
        addOption(stereo, "sbs", "Side by side");
        addOption(stereo, "ou", "Over under");
        pages.first.addView(stereo);

        pages.first.addView(label(com.limelight.R.drawable.player_ic_convert, "2D films in 3D"));
        LinearLayout convert = segmentRow();
        convertOff = UiKit.segment(context, "Off", v -> actions.onConvert2d(false));
        convertOn = UiKit.segment(context, "On", v -> actions.onConvert2d(true));
        convert.addView(convertOff, segmentParams());
        convert.addView(convertOn, segmentParams());
        pages.first.addView(convert);

        View depthLabel = label(com.limelight.R.drawable.player_ic_layers, "Depth");
        pages.first.addView(depthLabel);
        LinearLayout depthRow = segmentRow();
        addDepth(depthRow, PlayerXR.DEPTH_POP, "Pop out");
        addDepth(depthRow, PlayerXR.DEPTH_BALANCED, "Balanced");
        addDepth(depthRow, PlayerXR.DEPTH_FULL, "Full");
        pages.first.addView(depthRow);
        filmRows = new View[] { stereoLabel, stereo, depthLabel, depthRow };

        // Natural: as graded. Vivid: expanded toward the display's full gamut.
        pages.second.addView(label(com.limelight.R.drawable.player_ic_palette, "Colours"));
        LinearLayout colours = segmentRow();
        colourNatural = UiKit.segment(context, "Natural", v -> {
            actions.onColourMode(XrShared.COLOUR_MODE_NATURAL);
            refreshState();
            renderDirty();
        });
        colourVivid = UiKit.segment(context, "Vivid", v -> {
            actions.onColourMode(XrShared.COLOUR_MODE_VIVID);
            refreshState();
            renderDirty();
        });
        colours.addView(colourNatural, segmentParams());
        colours.addView(colourVivid, segmentParams());
        pages.second.addView(colours);

        pages.second.addView(label(com.limelight.R.drawable.player_ic_passthrough, "Environment"));
        LinearLayout env = segmentRow();
        passthroughOn = UiKit.segment(context, "Passthrough", v -> {
            if (!actions.isPassthroughOn()) {
                actions.onTogglePassthrough();
            }
        });
        passthroughOff = UiKit.segment(context, "Dark", v -> {
            if (actions.isPassthroughOn()) {
                actions.onTogglePassthrough();
            }
        });
        env.addView(passthroughOn, segmentParams());
        env.addView(passthroughOff, segmentParams());
        pages.second.addView(env);

        // Hand tracking: hide the pointer when the hand is idle, show it again on an open palm
        pages.second.addView(label(com.limelight.R.drawable.player_ic_hand, "Hand pointer"));
        LinearLayout hand = segmentRow();
        handHide = UiKit.segment(context, "Hide when idle", v -> {
            actions.onHandAutoHide(true);
            refreshState();
            renderDirty();
        });
        handAlways = UiKit.segment(context, "Always on", v -> {
            actions.onHandAutoHide(false);
            refreshState();
            renderDirty();
        });
        hand.addView(handHide, segmentParams());
        hand.addView(handAlways, segmentParams());
        pages.second.addView(hand);
        pages.finish();
        return UiKit.wrap(context, box);
    }

    private void addDepth(LinearLayout row, String name, String text) {
        TextView option = UiKit.segment(context, text, v -> {
            actions.onDepth(name);
            refreshState();
            renderDirty();
        });
        depthOptions.put(name, option);
        row.addView(option, segmentParams());
    }

    private void addOption(LinearLayout row, String mode, String name) {
        TextView option = UiKit.segment(context, name, v -> actions.onStereoMode(mode));
        stereoOptions.put(mode, option);
        row.addView(option, segmentParams());
    }

    // Group heading: a small icon and the name
    private View label(int icon, String name) {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, 22, 0, 12);
        row.addView(icon(icon), new LinearLayout.LayoutParams(ICON_SMALL, ICON_SMALL));
        TextView label = UiKit.text(context, name, 32, false, TEXT_SOFT);
        label.setPadding(16, 0, 0, 0);
        row.addView(label);
        return row;
    }

    // Plain sub-heading
    private TextView label(String name) {
        TextView label = UiKit.text(context, name, 32, false, TEXT_SOFT);
        label.setPadding(4, 26, 0, 12);
        return label;
    }

    private static final int ICON_SMALL = 56;

    private ImageView icon(int res) {
        ImageView image = new ImageView(context);
        image.setImageResource(res);
        image.setScaleType(ImageView.ScaleType.FIT_CENTER);
        image.setPadding(10, 10, 10, 10);
        image.setBackground(UiKit.circle(UiKit.BUTTON));
        return image;
    }

    private LinearLayout segmentRow() {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        return row;
    }

    private LinearLayout.LayoutParams segmentParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, 92, 1);
        lp.rightMargin = 14;
        return lp;
    }

    // Tracks sheet: audio and subtitle lists side by side
    private ViewGroup buildTracks() {
        LinearLayout box = sheetFrame("Audio and subtitles");
        TextView style = UiKit.segment(context, "Style", v -> openSheet(SHEET_SUBTITLES));
        LinearLayout.LayoutParams styleLp = new LinearLayout.LayoutParams(200, 88);
        styleLp.rightMargin = 16;
        ((LinearLayout) box.getChildAt(0)).addView(style, 1, styleLp);
        LinearLayout columns = new LinearLayout(context);
        columns.setOrientation(LinearLayout.HORIZONTAL);
        audioList = new OptionList("Audio", c -> actions.onChooseAudio(c));
        subtitleList = new OptionList("Subtitles", c -> actions.onChooseSubtitle(c));
        LinearLayout.LayoutParams left = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1);
        left.rightMargin = 24;
        LinearLayout.LayoutParams right = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1);
        right.leftMargin = 24;
        columns.addView(audioList.view, left);
        columns.addView(subtitleList.view, right);
        box.addView(columns, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        return UiKit.wrap(context, box);
    }

    // Subtitle style sheet: live preview, then one row of options per property
    private ViewGroup buildSubtitleStyle() {
        LinearLayout box = sheetFrame("Subtitle style");

        FrameLayout preview = new FrameLayout(context);
        android.graphics.drawable.GradientDrawable backdrop = new android.graphics.drawable.GradientDrawable(
                android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM,
                new int[] { 0xFF5B6B7B, 0xFF2B323A });
        backdrop.setCornerRadius(32);
        preview.setBackground(backdrop);
        subPreview = new SubtitleView(context);
        preview.addView(subPreview, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 156);
        plp.topMargin = 22;
        plp.bottomMargin = 6;
        box.addView(preview, plp);

        subStyleRows.clear();
        Pages pages = new Pages(box);
        pages.first.addView(styleRow(com.limelight.R.drawable.player_ic_text_size, "Size", SubtitleStyle.SIZE_NAMES, i -> subStyle.size = i));
        pages.first.addView(styleRow(com.limelight.R.drawable.player_ic_text_colour, "Colour", SubtitleStyle.COLOUR_NAMES, i -> subStyle.colour = i));
        pages.first.addView(styleRow(com.limelight.R.drawable.player_ic_fill, "Background", SubtitleStyle.BACKGROUND_NAMES, i -> subStyle.background = i));
        pages.first.addView(styleRow(com.limelight.R.drawable.player_ic_edge, "Edge", SubtitleStyle.EDGE_NAMES, i -> subStyle.edge = i));
        // Row order must match refreshSubStyleSelection
        pages.second.addView(styleRow(com.limelight.R.drawable.player_ic_height, "Height", SubtitleStyle.HEIGHT_NAMES, i -> subStyle.height = i));
        pages.second.addView(styleRow(com.limelight.R.drawable.player_ic_layers, "Depth", SubtitleStyle.DEPTH_NAMES, i -> subStyle.depth = i));
        pages.second.addView(styleRow(com.limelight.R.drawable.player_ic_file, "File styling", new String[] { "On", "Off" }, i -> subStyle.fileStyling = i == 0));
        pages.second.addView(syncRow());
        pages.finish();
        return UiKit.wrap(context, box);
    }

    private interface Pick {
        void set(int index);
    }

    // Icon and label plus a row of mutually exclusive options
    private LinearLayout styleRow(int icon, String name, String[] options, Pick pick) {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(icon(icon), new LinearLayout.LayoutParams(ICON_SMALL, ICON_SMALL));
        TextView label = UiKit.text(context, name, 30, false, TEXT_SOFT);
        label.setPadding(16, 0, 0, 0);
        row.addView(label, new LinearLayout.LayoutParams(STYLE_LABEL_W - ICON_SMALL, ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView[] views = new TextView[options.length];
        for (int i = 0; i < options.length; i++) {
            final int index = i;
            views[i] = UiKit.segment(context, options[i], v -> {
                pick.set(index);
                styleChanged();
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, 84, 1);
            lp.rightMargin = 10;
            row.addView(views[i], lp);
        }
        subStyleRows.add(views);
        LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 92);
        rowLp.topMargin = 10;
        row.setLayoutParams(rowLp);
        return row;
    }

    private static final int STYLE_LABEL_W = 230;

    // Per-file subtitle delay: step buttons and reset, clamped to +/-60 s
    private LinearLayout syncRow() {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(icon(com.limelight.R.drawable.player_ic_clock), new LinearLayout.LayoutParams(ICON_SMALL, ICON_SMALL));
        syncLabel = UiKit.text(context, "", 30, false, TEXT_SOFT);
        syncLabel.setPadding(16, 0, 0, 0);
        row.addView(syncLabel, new LinearLayout.LayoutParams(STYLE_LABEL_W - ICON_SMALL, ViewGroup.LayoutParams.WRAP_CONTENT));
        String[] names = { "\u2212 1 s", "\u2212 0.1 s", "Reset", "+ 0.1 s", "+ 1 s" };
        long[] steps = { -1000, -100, 0, 100, 1000 };
        for (int i = 0; i < names.length; i++) {
            final long step = steps[i];
            TextView b = UiKit.segment(context, names[i], v -> {
                long now = step == 0 ? 0 : actions.subtitleDelayMs() + step;
                actions.onSubtitleDelay(Math.max(-60000, Math.min(60000, now)));
                updateSyncLabel();
                markDirty(UI_PANEL_SHEET);
                renderDirty();
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, 84, 1);
            lp.rightMargin = 10;
            row.addView(b, lp);
        }
        LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 92);
        rowLp.topMargin = 10;
        row.setLayoutParams(rowLp);
        updateSyncLabel();
        return row;
    }

    private void updateSyncLabel() {
        if (syncLabel == null) {
            return;
        }
        long ms = actions.subtitleDelayMs();
        syncLabel.setText(ms == 0 ? "Sync" : String.format(Locale.ROOT, "Sync  %+.1f s", ms / 1000.0));
    }

    private void refreshSubStyleSelection() {
        int[] chosen = { subStyle.size, subStyle.colour, subStyle.background, subStyle.edge, subStyle.height,
                subStyle.depth, subStyle.fileStyling ? 0 : 1 };
        for (int r = 0; r < subStyleRows.size() && r < chosen.length; r++) {
            TextView[] views = subStyleRows.get(r);
            for (int i = 0; i < views.length; i++) {
                setSelected(views[i], i == chosen[r], UI_PANEL_SHEET);
            }
        }
    }

    // Sample text with italic and coloured spans, to show the effect of File styling
    private void updateSubPreview() {
        if (subPreview == null) {
            return;
        }
        android.text.SpannableStringBuilder text = new android.text.SpannableStringBuilder(
                "Subtitles look like this, in italics and colour");
        int italic = text.toString().indexOf("italics");
        text.setSpan(new android.text.style.StyleSpan(android.graphics.Typeface.ITALIC), italic, italic + 7,
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        int colour = text.toString().indexOf("colour");
        text.setSpan(new android.text.style.ForegroundColorSpan(0xFFFF8A65), colour, colour + 6,
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        subPreview.set(text, subStyle);
    }

    /** Paged track list column with a heading. */
    private final class OptionList {
        static final int ROWS = 4;
        final LinearLayout view;
        private final TextView[] rows = new TextView[ROWS];
        private final TextView pageText;
        private final ImageView prev, next;
        private final ChooseTrack choose;
        private List<TrackChoice> items = new ArrayList<>();
        private int page;

        OptionList(String heading, ChooseTrack choose) {
            this.choose = choose;
            view = new LinearLayout(context);
            view.setOrientation(LinearLayout.VERTICAL);
            TextView head = UiKit.text(context, heading, 32, false, TEXT_SOFT);
            head.setPadding(4, 22, 0, 12);
            view.addView(head);
            for (int i = 0; i < ROWS; i++) {
                final int slot = i;
                TextView row = UiKit.segment(context, "", v -> {
                    int index = page * ROWS + slot;
                    if (index < items.size() && items.get(index).enabled) {
                        OptionList.this.choose.pick(items.get(index));
                    }
                });
                row.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
                row.setPadding(32, 0, 32, 0);
                row.setMaxLines(2);
                row.setEllipsize(TextUtils.TruncateAt.END);
                rows[i] = row;
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 100);
                lp.bottomMargin = 10;
                view.addView(row, lp);
            }
            LinearLayout pager = new LinearLayout(context);
            pager.setOrientation(LinearLayout.HORIZONTAL);
            pager.setGravity(Gravity.CENTER);
            prev = UiKit.iconButton(context, com.limelight.R.drawable.player_ic_prev, 72, v -> turn(-1));
            next = UiKit.iconButton(context, com.limelight.R.drawable.player_ic_next, 72, v -> turn(1));
            pageText = UiKit.text(context, "", 28, false, TEXT_SOFT);
            pageText.setGravity(Gravity.CENTER);
            pager.addView(prev);
            pager.addView(pageText, new LinearLayout.LayoutParams(150, ViewGroup.LayoutParams.WRAP_CONTENT));
            pager.addView(next);
            view.addView(pager, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 80));
        }

        private void turn(int step) {
            int p = page + step;
            if (p >= 0 && p * ROWS < Math.max(1, items.size())) {
                page = p;
                show();
                markSheetDirty();
                renderDirty();
            }
        }

        /** Updates the list if its signature changed; returns the new signature. */
        String set(List<TrackChoice> tracks, String previous, PlayerUi ui) {
            StringBuilder sig = new StringBuilder();
            for (TrackChoice t : tracks) {
                sig.append(t.label).append('|').append(t.detail).append('|').append(t.selected)
                        .append(t.enabled).append(';');
            }
            String now = sig.toString();
            if (now.equals(previous)) {
                return previous;
            }
            boolean first = previous.isEmpty();
            items = tracks;
            if (first) {
                page = 0;
                for (int i = 0; i < tracks.size(); i++) {
                    if (tracks.get(i).selected) {
                        page = i / ROWS;
                        break;
                    }
                }
            }
            page = Math.min(page, Math.max(0, (items.size() - 1) / ROWS));
            show();
            ui.markSheetDirty();
            return now;
        }

        // Ellipsized name, with the detail line in smaller grey text below
        private CharSequence rowText(TextView row, TrackChoice t) {
            CharSequence name = TextUtils.ellipsize(t.label, row.getPaint(), 430, TextUtils.TruncateAt.END);
            if (t.detail.isEmpty()) {
                return name;
            }
            android.text.SpannableStringBuilder b = new android.text.SpannableStringBuilder(name);
            b.append('\n');
            int from = b.length();
            b.append(t.detail);
            b.setSpan(new android.text.style.AbsoluteSizeSpan(26), from, b.length(),
                    android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            b.setSpan(new android.text.style.ForegroundColorSpan(0xFF9A9AA0), from, b.length(),
                    android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            return b;
        }

        private void show() {
            for (int i = 0; i < ROWS; i++) {
                int index = page * ROWS + i;
                TextView row = rows[i];
                if (index < items.size()) {
                    TrackChoice t = items.get(index);
                    row.setText(rowText(row, t));
                    row.setSelected(t.selected);
                    row.setEnabled(t.enabled);
                    row.setAlpha(t.enabled ? 1.0f : 0.4f);
                    row.setVisibility(View.VISIBLE);
                }
                else {
                    row.setVisibility(View.INVISIBLE);
                }
            }
            int pages = Math.max(1, (items.size() + ROWS - 1) / ROWS);
            pageText.setText(items.size() <= ROWS ? "" : String.format(Locale.ROOT, "%d / %d", page + 1, pages));
            boolean multi = items.size() > ROWS;
            prev.setVisibility(multi ? View.VISIBLE : View.INVISIBLE);
            next.setVisibility(multi ? View.VISIBLE : View.INVISIBLE);
            prev.setEnabled(page > 0);
            next.setEnabled(page < pages - 1);
            prev.setAlpha(prev.isEnabled() ? 1.0f : 0.3f);
            next.setAlpha(next.isEnabled() ? 1.0f : 0.3f);
        }
    }
}
