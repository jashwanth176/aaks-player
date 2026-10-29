package com.limelight.preferences;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.preference.PreferenceManager;

import com.limelight.FileLog;
import com.limelight.binding.video.DepthPresets;
import com.limelight.binding.video.MidasDepthSource;
import com.limelight.binding.video.XrShared;

/** Reads the stored settings, plus the key names, defaults and migrations behind them. */
public class PreferenceConfiguration {
    private static final String DEADZONE_PREF_STRING = "seekbar_deadzone";
    public static final String ENABLE_PERF_OVERLAY_STRING = "checkbox_enable_perf_overlay";
    public static final String VR_HEAD_LOCKED_PREF_STRING = "checkbox_vr_head_locked";
    private static final String VR_DISTANCE_PREF_STRING = "seekbar_vr_distance";
    private static final String VR_SCREEN_SIZE_PREF_STRING = "seekbar_vr_screen_size";
    private static final String VR_CURVATURE_PREF_STRING = "seekbar_vr_curvature";
    public static final String VR_DEPTH_SOURCE_PREF_STRING = "list_vr_depth_source";
    // The two depth source values that name a model rather than a test pattern.
    // MiDaS is stored as "model" for compatibility.
    public static final String VR_DEPTH_SOURCE_ZIPDEPTH = "zipdepth";
    public static final String VR_DEPTH_SOURCE_MIDAS = "model";
    public static final String VR_ENV_RES_PREF_STRING = "list_vr_env_res";
    public static final String VR_SHARPENING_PREF_STRING = "list_vr_sharpening";
    public static final String VR_SUPERSAMPLING_PREF_STRING = "list_vr_supersampling";
    private static final String VR_EYE_SWAP_PREF_STRING = "checkbox_vr_eye_swap";
    public static final String VR_PASSTHROUGH_PREF_STRING = "checkbox_vr_passthrough";
    private static final String VR_POINTER_PREF_STRING = "checkbox_vr_pointer";
    private static final String VR_GAZE_PREF_STRING = "checkbox_vr_gaze";
    private static final String VR_HAND_TRACKING_PREF_STRING = "checkbox_vr_hand_tracking";
    // Written only by the hand lock hint's "Don't show this again".
    static final String VR_HAND_LOCK_HINT_SEEN_PREF_STRING = "checkbox_vr_hand_lock_hint_seen";
    public static final String VR_POINTER_SLEEP_PREF_STRING = "checkbox_vr_pointer_sleep";
    public static final String VR_CLICK_SOUND_PREF_STRING = "checkbox_vr_click_sound";
    public static final String VR_DOFF_GRACE_PREF_STRING = "checkbox_vr_doff_grace";
    public static final String VR_SHOW_RAY_PREF_STRING = "checkbox_vr_show_ray";
    public static final String VR_CONTROLLER_MODEL_PREF_STRING = "checkbox_vr_controller_model";
    // Sensitivity in pixels per degree, dead zone in degrees per second (XrShared HEAD_AIM_ units).
    public static final String VR_HEAD_AIM_PREF_STRING = "checkbox_vr_head_aim";
    public static final String VR_HEAD_AIM_SENSITIVITY_PREF_STRING = "seekbar_vr_head_aim_sensitivity";
    public static final String VR_HEAD_AIM_DEADZONE_PREF_STRING = "seekbar_vr_head_aim_deadzone";
    // Controller shortcut that toggles gamepad mode, and its values in PAD_SHORTCUT_ order.
    // The mode itself is not stored: every session starts in pointer mode.
    public static final String VR_GAMEPAD_TOGGLE_PREF_STRING = "list_vr_gamepad_toggle";
    public static final String VR_GAMEPAD_TOGGLE_MENU_GRIP = "menu_grip";
    public static final String VR_GAMEPAD_TOGGLE_STICKS = "sticks";
    public static final String VR_GAMEPAD_TOGGLE_TRIGGERS_GRIPS = "triggers_grips";
    // Saved pose of a screen moved with the controllers; not a setting.
    public static final String VR_SCREEN_POSE_PREF_STRING = "vr_screen_pose";
    // Legacy grid cell of the last environment, read only to migrate to the id below.
    public static final String VR_ENVIRONMENT_PREF_STRING = "vr_environment";
    // Stable environment id, independent of grid position.
    public static final String VR_ENVIRONMENT_ID_PREF_STRING = "vr_environment_id";
    // List preference that reads and writes VR_ENVIRONMENT_ID_PREF_STRING; no key of its own.
    public static final String VR_ENVIRONMENT_LIST_PREF_STRING = "list_vr_environment";
    // Ids 2-6 and 100+ are retired (see EnvironmentIds). 4-6 were the 3D rooms and
    // stay named so an old saved value is recognised.
    public static final int VR_ENV_PASSTHROUGH = 0;
    public static final int VR_ENV_VOID = 1;
    public static final int VR_ENV_HOME_THEATER = 4;
    public static final int VR_ENV_GRAND_CINEMA = 5;
    public static final int VR_ENV_SYNTHWAVE = 6;
    // Migration markers, not settings.
    public static final String GEN1_PROFILE_PREF_STRING = "perf_profile_gen1";
    public static final String ZIPDEPTH_MOVE_PREF_STRING = "depth_source_zipdepth";
    public static final String DEPTH_RATE_MOVE_PREF_STRING = "depth_rate_per_second";
    public static final String VR_SEPARATION_PREF_STRING = "seekbar_vr_separation";
    private static final String VR_DEPTH_DEBUG_PREF_STRING = "checkbox_vr_depth_debug";
    // Depth maps per second. The key name is from the old every-Nth-frame cadence,
    // migrated once by moveCadenceToDepthRate.
    static final String VR_DEPTH_RATE_PREF_STRING = "seekbar_vr_inference_cadence";
    public static final String VR_CONVERGENCE_PREF_STRING = "seekbar_vr_convergence";
    private static final String VR_DEPTH_SCALE_PREF_STRING = "seekbar_vr_depth_scale";
    public static final String VR_AMBILIGHT_PREF_STRING = "checkbox_vr_ambilight";
    public static final String VR_AMBILIGHT_LEVEL_PREF_STRING = "seekbar_vr_ambilight_level";
    public static final String VR_ROOM_LIGHT_PREF_STRING = "checkbox_vr_room_light";
    // Per-room Room tab values, keyed by environment id (room_brightness_4 is the Home
    // Theater). Brightness and light in hundredths, screen size in percent.
    public static final String ROOM_BRIGHTNESS_PREFIX = "room_brightness_";
    public static final String ROOM_GLOW_PREFIX = "room_glow_";
    public static final String ROOM_LIGHT_PREFIX = "room_light_";
    public static final String ROOM_SCREEN_PREFIX = "room_screen_";
    // Picture grade, global, in XrShared PICTURE_ units.
    public static final String VR_PICTURE_BRIGHTNESS_PREF_STRING = "seekbar_vr_picture_brightness";
    public static final String VR_PICTURE_CONTRAST_PREF_STRING = "seekbar_vr_picture_contrast";
    public static final String VR_PICTURE_GAMMA_PREF_STRING = "seekbar_vr_picture_gamma";
    public static final String VR_PICTURE_SATURATION_PREF_STRING = "seekbar_vr_picture_saturation";
    public static final String FILE_LOG_PREF_STRING = "list_vr_file_log";
    static final String VR_VIRTUAL_SURROUND_PREF_STRING = "checkbox_vr_virtual_surround";

    // XR2 Gen 1 starting values: the depth model dominates frame cost, so its rate is lowered.
    static final int GEN1_DEPTH_RATE = XrShared.DEPTH_RATE_GEN1;
    // Gen 1 lacks the memory and fill rate for a full-size room.
    private static final String GEN1_ENV_RES = "low";

    private static final int DEFAULT_DEADZONE = 7;

    private static final boolean DEFAULT_ENABLE_PERF_OVERLAY = false;
    private static final boolean DEFAULT_VR_HEAD_LOCKED = false;
    // Tenths of a metre.
    private static final int DEFAULT_VR_DISTANCE = 30;
    private static final int DEFAULT_VR_SCREEN_SIZE = 30;
    private static final int DEFAULT_VR_CURVATURE = 0;
    // ZipDepth: the only model a Gen 1 headset can run.
    static final String DEFAULT_VR_DEPTH_SOURCE = VR_DEPTH_SOURCE_ZIPDEPTH;
    // Gen 1 headsets are seeded "low" instead, see seedGen1PerfProfile.
    private static final String DEFAULT_VR_ENV_RES = "standard";
    private static final String DEFAULT_VR_SHARPENING = "quality";
    // Off: costs compositor GPU time.
    static final String DEFAULT_VR_SUPERSAMPLING = "off";
    private static final boolean DEFAULT_VR_EYE_SWAP = false;
    public static final boolean DEFAULT_VR_PASSTHROUGH = false;
    private static final boolean DEFAULT_VR_GAZE = true;
    private static final boolean DEFAULT_VR_HAND_TRACKING = true;
    // Pointer hides after a few still seconds and wakes on movement.
    static final boolean DEFAULT_VR_POINTER_SLEEP = true;
    // Audible feedback for pinch and gaze presses, which have no physical click.
    static final boolean DEFAULT_VR_CLICK_SOUND = true;
    // Keep the session, muted, for a minute after the headset is taken off.
    static final boolean DEFAULT_VR_DOFF_GRACE = true;
    static final boolean DEFAULT_VR_SHOW_RAY = true;
    static final boolean DEFAULT_VR_CONTROLLER_MODEL = false;
    static final boolean DEFAULT_VR_HEAD_AIM = false;
    static final String DEFAULT_VR_GAMEPAD_TOGGLE = VR_GAMEPAD_TOGGLE_MENU_GRIP;
    private static final boolean DEFAULT_VR_POINTER = true;
    // MiDaS separation, tenths of a percent of frame width. Each model has its own
    // default pair (MidasDepthSource.Spec); with nothing stored the running model's applies.
    public static final int DEFAULT_VR_SEPARATION = 5;
    private static final boolean DEFAULT_VR_DEPTH_DEBUG = false;
    // Maps per second. Gen 1 headsets are seeded GEN1_DEPTH_RATE.
    static final int DEFAULT_VR_DEPTH_RATE = XrShared.DEPTH_RATE_DEFAULT;
    public static final int DEFAULT_VR_CONVERGENCE = 50;
    private static final int DEFAULT_VR_DEPTH_SCALE = 100;
    public static final boolean DEFAULT_VR_AMBILIGHT = true;
    public static final int DEFAULT_VR_AMBILIGHT_LEVEL = 50;
    public static final boolean DEFAULT_VR_ROOM_LIGHT = true;
    // Warnings and errors.
    public static final String DEFAULT_FILE_LOG = "basic";
    static final boolean DEFAULT_VR_VIRTUAL_SURROUND = false;

    // EnvResTier, as the renderer numbers them
    public static final int VR_ENV_RES_LOW = XrShared.ENV_RES_LOW;
    public static final int VR_ENV_RES_STANDARD = XrShared.ENV_RES_STANDARD;
    public static final int VR_ENV_RES_HIGH = XrShared.ENV_RES_HIGH;
    public static final int VR_ENV_RES_ULTRA = XrShared.ENV_RES_ULTRA;

    // Not stored: set by the player from the probed file.
    public int fps;
    public int deadzonePercentage;
    public boolean enablePerfOverlay;
    public boolean vrHeadLocked;
    // Tenths of a metre
    public int vrDistance;
    public int vrScreenSize;
    // 0 to 100
    public int vrCurvature;
    // 0 off, 1 flat, 2 ramp, 3 blob, 4 eye test, 5 shift test, 6 depth model
    public int vrDepthMode;
    // zipdepth or model (MiDaS). Set to the default in other modes, since test
    // patterns use its map size.
    public String vrDepthModel;
    // One of the VR_ENV_RES_ tiers.
    public int vrEnvResTier;
    // 0 off, 1 normal, 2 quality
    public int vrSharpening;
    // Same scale, for compositor supersampling.
    public int vrSupersampling;
    public boolean vrEyeSwap;
    // Tenths of a percent of frame width
    public int vrStereoSeparation;
    public boolean vrDepthDebug;
    public boolean vrPassthrough;
    public boolean vrPointer;
    public boolean vrGaze;
    public boolean vrHandTracking;
    public boolean vrHandLockHintSeen;
    public boolean vrPointerSleep;
    public boolean vrClickSound;
    public boolean vrDoffGrace;
    // Whether a session starts with the controller ray drawn
    public boolean vrShowRay;
    public boolean vrControllerModel;
    // Pixels per degree; dead zone in degrees per second.
    public boolean vrHeadAim;
    public int vrHeadAimSensitivity;
    public int vrHeadAimDeadZone;
    // PAD_SHORTCUT_ value.
    public int vrGamepadToggle;
    // Maximum depth maps per second; the renderer lowers it temporarily under load.
    public int vrDepthRate;
    public int vrConvergence;
    public int vrDepthScale;
    // Frame colours around the screen; level in percent.
    public boolean vrAmbilight;
    public int vrAmbilightLevel;
    // Frame colours on the walls of a 3D environment.
    public boolean vrRoomLight;
    // Brightness, contrast, gamma, saturation in XrShared PICTURE_ order and units.
    public int[] vrPicture;
    // A VR_ENV_ id, or -1 if none picked. Logging only; the renderer reads the pref itself.
    public int vrEnvironmentId;
    // That environment's Room tab values, or null if it is not a room. Logging only.
    public RoomLevels vrRoomLevels;
    // off, basic or verbose
    public String fileLogLevel;
    // Render 5.1/7.1 audio to binaural stereo.
    public boolean vrVirtualSurround;

    // Meta and Pico declare the head tracking feature, Android XR the OpenXR one;
    // the manufacturer check covers firmware that omits both.
    public static boolean isHeadset(Context context) {
        PackageManager pm = context.getPackageManager();
        if (pm.hasSystemFeature("android.hardware.vr.headtracking")
                || pm.hasSystemFeature("android.software.xr.api.openxr")) {
            return true;
        }
        String maker = Build.MANUFACTURER != null ? Build.MANUFACTURER : "";
        return maker.equalsIgnoreCase("pico") || maker.equalsIgnoreCase("oculus")
                || maker.equalsIgnoreCase("meta");
    }

    // Quest 2, Quest Pro and Pico 4 (XR2 Gen 1): much less GPU headroom than Gen 2.
    public static boolean isXr2Gen1Headset() {
        String model = Build.MODEL != null ? Build.MODEL : "";

        // Board names are more stable than marketing names, so check both.
        if (model.equalsIgnoreCase("Quest 2") || model.equalsIgnoreCase("Quest Pro") ||
                "hollywood".equalsIgnoreCase(Build.DEVICE) || "seacliff".equalsIgnoreCase(Build.DEVICE)) {
            return true;
        }

        // Pico 4 and 4 Enterprise report model numbers starting A81.
        boolean isPico = "pico".equalsIgnoreCase(Build.MANUFACTURER) || "pico".equalsIgnoreCase(Build.BRAND);
        return isPico && model.regionMatches(true, 0, "A81", 0, 3);
    }

    // 0 off, 1 normal, 2 quality; unknown values are off.
    static int supersamplingMode(String value) {
        if ("normal".equals(value)) {
            return 1;
        }
        if ("quality".equals(value)) {
            return 2;
        }
        return 0;
    }

    /** Whether a list_vr_depth_source value names a model rather than a pattern. */
    public static boolean isDepthModel(String depthSource) {
        return VR_DEPTH_SOURCE_ZIPDEPTH.equals(depthSource)
                || VR_DEPTH_SOURCE_MIDAS.equals(depthSource);
    }

    // MiDaS is too slow on XR2 Gen 1, so it is not offered there.
    public static boolean isDepthSourceOffered(String depthSource, boolean gen1) {
        return !gen1 || !VR_DEPTH_SOURCE_MIDAS.equals(depthSource);
    }

    /** The value a headset runs for a stored one: ZipDepth for a MiDaS it does not offer. */
    public static String depthSourceForHeadset(String depthSource, boolean gen1) {
        return isDepthSourceOffered(depthSource, gen1) ? depthSource : VR_DEPTH_SOURCE_ZIPDEPTH;
    }

    /** A model's default separation, in tenths of a percent of frame width. */
    public static int defaultSeparation(String depthModel) {
        return MidasDepthSource.specFor(depthModel).defaultSeparation;
    }

    /** A model's default convergence, in percent. */
    public static int defaultConvergence(String depthModel) {
        return MidasDepthSource.specFor(depthModel).defaultConvergence;
    }

    // Stored value, or the running model's default. Only user changes are stored, so
    // switching model moves an untouched pair but never a chosen one.
    static int storedSeparation(SharedPreferences prefs, String depthModel) {
        return prefs.getInt(VR_SEPARATION_PREF_STRING, defaultSeparation(depthModel));
    }

    static int storedConvergence(SharedPreferences prefs, String depthModel) {
        return prefs.getInt(VR_CONVERGENCE_PREF_STRING, defaultConvergence(depthModel));
    }

    /** A model's default pair for the log, as separation/convergence. */
    public static String defaultPairLabel(String depthModel) {
        return defaultSeparation(depthModel) + "/" + defaultConvergence(depthModel);
    }

    /** Whether a session with this depth mode starts in 3D, for the log. */
    public static String stereoAtStartLabel(int depthMode) {
        return depthMode != XrShared.DEPTH_MODE_OFF ? "on" : "off";
    }

    static boolean handLockHintSeen(SharedPreferences prefs) {
        return prefs.getBoolean(VR_HAND_LOCK_HINT_SEEN_PREF_STRING, false);
    }

    /** The hand lock hint was put away for good: later sessions skip it. */
    public static void markHandLockHintSeen(SharedPreferences prefs) {
        prefs.edit().putBoolean(VR_HAND_LOCK_HINT_SEEN_PREF_STRING, true).apply();
    }

    static boolean pointerSleepOn(SharedPreferences prefs) {
        return prefs.getBoolean(VR_POINTER_SLEEP_PREF_STRING, DEFAULT_VR_POINTER_SLEEP);
    }

    static boolean clickSoundOn(SharedPreferences prefs) {
        return prefs.getBoolean(VR_CLICK_SOUND_PREF_STRING, DEFAULT_VR_CLICK_SOUND);
    }

    static boolean doffGraceOn(SharedPreferences prefs) {
        return prefs.getBoolean(VR_DOFF_GRACE_PREF_STRING, DEFAULT_VR_DOFF_GRACE);
    }

    static boolean rayShown(SharedPreferences prefs) {
        return prefs.getBoolean(VR_SHOW_RAY_PREF_STRING, DEFAULT_VR_SHOW_RAY);
    }

    static boolean controllerModelOn(SharedPreferences prefs) {
        return prefs.getBoolean(VR_CONTROLLER_MODEL_PREF_STRING, DEFAULT_VR_CONTROLLER_MODEL);
    }

    static boolean headAimOn(SharedPreferences prefs) {
        return prefs.getBoolean(VR_HEAD_AIM_PREF_STRING, DEFAULT_VR_HEAD_AIM);
    }

    /** Head aim pixels per degree, clamped to its range. */
    public static int headAimSensitivity(SharedPreferences prefs) {
        int units = prefs.getInt(VR_HEAD_AIM_SENSITIVITY_PREF_STRING,
                XrShared.HEAD_AIM_SENSITIVITY_DEFAULT);
        return Math.max(XrShared.HEAD_AIM_SENSITIVITY_MIN,
                Math.min(XrShared.HEAD_AIM_SENSITIVITY_MAX, units));
    }

    /** Head aim dead zone in degrees per second, clamped to its range. */
    public static int headAimDeadZone(SharedPreferences prefs) {
        int units = prefs.getInt(VR_HEAD_AIM_DEADZONE_PREF_STRING,
                XrShared.HEAD_AIM_DEADZONE_DEFAULT);
        return Math.max(XrShared.HEAD_AIM_DEADZONE_MIN,
                Math.min(XrShared.HEAD_AIM_DEADZONE_MAX, units));
    }

    /** The gamepad mode shortcut as a PAD_SHORTCUT_ value; unknown values are menu + grip. */
    static int gamepadToggle(SharedPreferences prefs) {
        String value = prefs.getString(VR_GAMEPAD_TOGGLE_PREF_STRING, DEFAULT_VR_GAMEPAD_TOGGLE);
        if (VR_GAMEPAD_TOGGLE_STICKS.equals(value)) {
            return XrShared.PAD_SHORTCUT_STICKS;
        }
        if (VR_GAMEPAD_TOGGLE_TRIGGERS_GRIPS.equals(value)) {
            return XrShared.PAD_SHORTCUT_TRIGGERS_GRIPS;
        }
        return XrShared.PAD_SHORTCUT_MENU_GRIP;
    }

    /** The gamepad shortcut for a log line; join separates key and value. */
    public static String gamepadToggleLabel(int shortcut, String join) {
        String value = shortcut == XrShared.PAD_SHORTCUT_STICKS ? VR_GAMEPAD_TOGGLE_STICKS
                : shortcut == XrShared.PAD_SHORTCUT_TRIGGERS_GRIPS
                        ? VR_GAMEPAD_TOGGLE_TRIGGERS_GRIPS : VR_GAMEPAD_TOGGLE_MENU_GRIP;
        return "gamepadShortcut" + join + value;
    }

    /** Pointer and controller drawing switches for a log line; join separates key and value. */
    public static String inputLabel(boolean pointerSleep, boolean showRay, boolean controllerModel,
                                    String join) {
        return "pointerSleep" + join + pointerSleep + " showRay" + join + showRay
                + " controllerModel" + join + controllerModel;
    }

    /** Head aim settings for a log line; join separates key and value. */
    public static String headAimLabel(boolean on, int sensitivity, int deadZone, String join) {
        return "headAim" + join + on + " headAimSensitivity" + join + sensitivity
                + " headAimDeadZone" + join + deadZone;
    }

    /** The 3D tab preset a separation matches under that model, or none, for the log. */
    public static String presetLabel(int separation, String depthModel) {
        return DepthPresets.name(DepthPresets.presetFor(separation, defaultSeparation(depthModel)));
    }

    /** One room's values for the headset panel's Room tab. */
    public static final class RoomLevels {
        // Hundredths of the baked brightness
        public final int brightness;
        public final boolean glow;
        // Hundredths
        public final int light;
        // Percent of the room's screen
        public final int screen;

        public RoomLevels(int brightness, boolean glow, int light, int screen) {
            this.brightness = brightness;
            this.glow = glow;
            this.light = light;
            this.screen = screen;
        }

        // For a log line; join separates key and value.
        public String describe(String join) {
            return "roomBrightness" + join + brightness + " roomGlow" + join + glow
                    + " roomLight" + join + light + " roomScreen" + join + screen;
        }
    }

    /** Whether an environment id is a baked room. No rooms ship, so always false. */
    public static boolean isRoomEnvironment(int envId) {
        return false;
    }

    public static String roomBrightnessKey(int envId) {
        return ROOM_BRIGHTNESS_PREFIX + envId;
    }

    public static String roomGlowKey(int envId) {
        return ROOM_GLOW_PREFIX + envId;
    }

    public static String roomLightKey(int envId) {
        return ROOM_LIGHT_PREFIX + envId;
    }

    public static String roomScreenKey(int envId) {
        return ROOM_SCREEN_PREFIX + envId;
    }

    // Defaults match the renderer's room table via the shared XrShared constants.
    public static int defaultRoomBrightness(int envId) {
        switch (envId) {
            case VR_ENV_HOME_THEATER: return XrShared.ROOM_THEATER_BRIGHTNESS;
            case VR_ENV_GRAND_CINEMA: return XrShared.ROOM_GRAND_CINEMA_BRIGHTNESS;
            case VR_ENV_SYNTHWAVE: return XrShared.ROOM_SYNTHWAVE_BRIGHTNESS;
            default: return 100;
        }
    }

    public static boolean defaultRoomGlow(int envId) {
        switch (envId) {
            case VR_ENV_HOME_THEATER: return XrShared.ROOM_THEATER_GLOW != 0;
            case VR_ENV_GRAND_CINEMA: return XrShared.ROOM_GRAND_CINEMA_GLOW != 0;
            case VR_ENV_SYNTHWAVE: return XrShared.ROOM_SYNTHWAVE_GLOW != 0;
            default: return DEFAULT_VR_AMBILIGHT;
        }
    }

    // Same in every room
    public static int defaultRoomLight() {
        return XrShared.ROOM_LIGHT_DEFAULT;
    }

    public static int defaultRoomScreen(int envId) {
        switch (envId) {
            case VR_ENV_HOME_THEATER: return XrShared.ROOM_THEATER_SCREEN;
            case VR_ENV_GRAND_CINEMA: return XrShared.ROOM_GRAND_CINEMA_SCREEN;
            case VR_ENV_SYNTHWAVE: return XrShared.ROOM_SYNTHWAVE_SCREEN;
            default: return XrShared.ROOM_SCREEN_MAX;
        }
    }

    public static boolean roomResizable(int envId) {
        switch (envId) {
            case VR_ENV_HOME_THEATER: return XrShared.ROOM_THEATER_RESIZABLE != 0;
            case VR_ENV_GRAND_CINEMA: return XrShared.ROOM_GRAND_CINEMA_RESIZABLE != 0;
            case VR_ENV_SYNTHWAVE: return XrShared.ROOM_SYNTHWAVE_RESIZABLE != 0;
            default: return false;
        }
    }

    /** Screen size percent, clamped to its range; always the maximum in a non-resizable room. */
    public static int clampRoomScreen(int envId, int percent) {
        if (!roomResizable(envId)) {
            return XrShared.ROOM_SCREEN_MAX;
        }
        return Math.max(XrShared.ROOM_SCREEN_MIN, Math.min(XrShared.ROOM_SCREEN_MAX, percent));
    }

    /** One room's stored values, clamped, with the room's defaults where unset. */
    public static RoomLevels readRoomLevels(SharedPreferences prefs, int envId) {
        int brightness = prefs.getInt(roomBrightnessKey(envId), defaultRoomBrightness(envId));
        int light = prefs.getInt(roomLightKey(envId), defaultRoomLight());
        return new RoomLevels(
                Math.max(XrShared.ROOM_BRIGHTNESS_MIN,
                        Math.min(XrShared.ROOM_BRIGHTNESS_MAX, brightness)),
                prefs.getBoolean(roomGlowKey(envId), defaultRoomGlow(envId)),
                Math.max(XrShared.ROOM_LIGHT_MIN, Math.min(XrShared.ROOM_LIGHT_MAX, light)),
                clampRoomScreen(envId, prefs.getInt(roomScreenKey(envId),
                        defaultRoomScreen(envId))));
    }

    /** The key a picture value is kept under, by its PICTURE_ row. */
    public static String pictureKey(int row) {
        switch (row) {
            case XrShared.PICTURE_CONTRAST: return VR_PICTURE_CONTRAST_PREF_STRING;
            case XrShared.PICTURE_GAMMA: return VR_PICTURE_GAMMA_PREF_STRING;
            case XrShared.PICTURE_SATURATION: return VR_PICTURE_SATURATION_PREF_STRING;
            default: return VR_PICTURE_BRIGHTNESS_PREF_STRING;
        }
    }

    /** A picture row's neutral default. */
    public static int pictureDefault(int row) {
        switch (row) {
            case XrShared.PICTURE_CONTRAST: return XrShared.PICTURE_CONTRAST_DEFAULT;
            case XrShared.PICTURE_GAMMA: return XrShared.PICTURE_GAMMA_DEFAULT;
            case XrShared.PICTURE_SATURATION: return XrShared.PICTURE_SATURATION_DEFAULT;
            default: return XrShared.PICTURE_BRIGHTNESS_DEFAULT;
        }
    }

    /** A picture value clamped to its row's range. */
    public static int clampPicture(int row, int units) {
        int min, max;
        switch (row) {
            case XrShared.PICTURE_CONTRAST:
                min = XrShared.PICTURE_CONTRAST_MIN;
                max = XrShared.PICTURE_CONTRAST_MAX;
                break;
            case XrShared.PICTURE_GAMMA:
                min = XrShared.PICTURE_GAMMA_MIN;
                max = XrShared.PICTURE_GAMMA_MAX;
                break;
            case XrShared.PICTURE_SATURATION:
                min = XrShared.PICTURE_SATURATION_MIN;
                max = XrShared.PICTURE_SATURATION_MAX;
                break;
            default:
                min = XrShared.PICTURE_BRIGHTNESS_MIN;
                max = XrShared.PICTURE_BRIGHTNESS_MAX;
                break;
        }
        return Math.max(min, Math.min(max, units));
    }

    /** All four stored picture values, clamped, with defaults where unset. */
    public static int[] readPicture(SharedPreferences prefs) {
        int[] picture = new int[XrShared.PICTURE_VALUES];
        for (int row = 0; row < picture.length; row++) {
            picture[row] = clampPicture(row, prefs.getInt(pictureKey(row), pictureDefault(row)));
        }
        return picture;
    }

    /** The four picture values for a log line; join separates key and value. */
    public static String pictureLabel(int[] picture, String join) {
        return "pictureBrightness" + join + picture[XrShared.PICTURE_BRIGHTNESS]
                + " pictureContrast" + join + picture[XrShared.PICTURE_CONTRAST]
                + " pictureGamma" + join + picture[XrShared.PICTURE_GAMMA]
                + " pictureSaturation" + join + picture[XrShared.PICTURE_SATURATION];
    }

    // One-time move of a stored MiDaS to ZipDepth. A chosen "model" cannot be told from
    // one written by setDefaultValues, so all are moved once. Returns whether it moved.
    static boolean moveDepthSourceToZipDepth(SharedPreferences prefs) {
        if (prefs.contains(ZIPDEPTH_MOVE_PREF_STRING)) {
            return false;
        }
        boolean move = VR_DEPTH_SOURCE_MIDAS.equals(
                prefs.getString(VR_DEPTH_SOURCE_PREF_STRING, null));
        SharedPreferences.Editor editor = prefs.edit();
        if (move) {
            editor.putString(VR_DEPTH_SOURCE_PREF_STRING, VR_DEPTH_SOURCE_ZIPDEPTH);
        }
        editor.putBoolean(ZIPDEPTH_MOVE_PREF_STRING, move);
        editor.apply();
        return move;
    }

    // Must run before any activity applies the XML defaults.
    public static void migrateDepthSource(Context context) {
        if (moveDepthSourceToZipDepth(PreferenceManager.getDefaultSharedPreferences(context))) {
            FileLog.event("depth model: stored MiDaS moved to ZipDepth, the new default");
        }
    }

    // Maps per second for an old cadence, assuming 60 fps: 1 -> max (45), 2 -> 30,
    // 3 -> 20, 4 -> 15, 5 -> 12, 6 -> 10.
    static int depthRateForCadence(int cadence) {
        if (cadence <= 1) {
            return XrShared.DEPTH_RATE_MAX;
        }
        return clampDepthRate(Math.round(60.0f / cadence));
    }

    static int clampDepthRate(int perSecond) {
        return Math.max(XrShared.DEPTH_RATE_MIN, Math.min(XrShared.DEPTH_RATE_MAX, perSecond));
    }

    // Stored rate clamped, or this headset's default.
    static int storedDepthRate(SharedPreferences prefs, boolean gen1) {
        return clampDepthRate(prefs.getInt(VR_DEPTH_RATE_PREF_STRING,
                gen1 ? GEN1_DEPTH_RATE : DEFAULT_VR_DEPTH_RATE));
    }

    // One-time conversion of a stored cadence to a rate under the same key. Must run
    // before the Gen 1 seed and the XML defaults. Returns the cadence moved, or 0.
    static int moveCadenceToDepthRate(SharedPreferences prefs) {
        if (prefs.contains(DEPTH_RATE_MOVE_PREF_STRING)) {
            return 0;
        }
        int cadence = prefs.contains(VR_DEPTH_RATE_PREF_STRING)
                ? prefs.getInt(VR_DEPTH_RATE_PREF_STRING, 0) : 0;
        SharedPreferences.Editor editor = prefs.edit();
        if (cadence > 0) {
            editor.putInt(VR_DEPTH_RATE_PREF_STRING, depthRateForCadence(cadence));
        }
        editor.putBoolean(DEPTH_RATE_MOVE_PREF_STRING, cadence > 0);
        editor.apply();
        return cadence;
    }

    public static void migrateDepthRate(Context context) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        int cadence = moveCadenceToDepthRate(prefs);
        if (cadence > 0) {
            FileLog.event("depth rate: stored cadence " + cadence + " moved to "
                    + depthRateForCadence(cadence) + " maps a second");
        }
    }

    // Seeds lighter defaults on XR2 Gen 1. Runs before the XML defaults so it only
    // affects unset keys. The marker's presence means the decision was made; its
    // value says whether anything was seeded.
    public static void seedGen1PerfProfile(Context context) {
        if (!isXr2Gen1Headset()) {
            return;
        }

        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);

        // Seeded independently of the marker, which predates this key; an existing
        // choice is left alone.
        if (!prefs.contains(VR_ENV_RES_PREF_STRING)) {
            prefs.edit().putString(VR_ENV_RES_PREF_STRING, GEN1_ENV_RES).apply();
            FileLog.event("perf profile: XR2 Gen 1 headset, environment res " + GEN1_ENV_RES);
        }

        if (prefs.contains(GEN1_PROFILE_PREF_STRING)) {
            return;
        }

        boolean seedDepthRate = !prefs.contains(VR_DEPTH_RATE_PREF_STRING);

        SharedPreferences.Editor editor = prefs.edit();
        if (seedDepthRate) {
            editor.putInt(VR_DEPTH_RATE_PREF_STRING, GEN1_DEPTH_RATE);
        }
        editor.putBoolean(GEN1_PROFILE_PREF_STRING, seedDepthRate);
        editor.apply();

        FileLog.event("perf profile: XR2 Gen 1 headset (" + Build.MANUFACTURER + " " + Build.MODEL
                + "/" + Build.DEVICE + "), "
                + (seedDepthRate ? "applied depth " + GEN1_DEPTH_RATE + " maps/s"
                        : "existing settings left alone"));
    }

    public static PreferenceConfiguration readPreferences(Context context) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        PreferenceConfiguration config = new PreferenceConfiguration();

        config.deadzonePercentage = prefs.getInt(DEADZONE_PREF_STRING, DEFAULT_DEADZONE);

        config.enablePerfOverlay = prefs.getBoolean(ENABLE_PERF_OVERLAY_STRING, DEFAULT_ENABLE_PERF_OVERLAY);
        config.vrHeadLocked = prefs.getBoolean(VR_HEAD_LOCKED_PREF_STRING, DEFAULT_VR_HEAD_LOCKED);
        config.vrDistance = prefs.getInt(VR_DISTANCE_PREF_STRING, DEFAULT_VR_DISTANCE);
        config.vrScreenSize = prefs.getInt(VR_SCREEN_SIZE_PREF_STRING, DEFAULT_VR_SCREEN_SIZE);
        config.vrCurvature = prefs.getInt(VR_CURVATURE_PREF_STRING, DEFAULT_VR_CURVATURE);
        // A Gen 1 headset runs ZipDepth for a stored MiDaS it does not offer
        String depthSource = depthSourceForHeadset(
                prefs.getString(VR_DEPTH_SOURCE_PREF_STRING, DEFAULT_VR_DEPTH_SOURCE),
                isXr2Gen1Headset());
        config.vrDepthModel = DEFAULT_VR_DEPTH_SOURCE;
        if (depthSource.equals("flat")) {
            config.vrDepthMode = XrShared.DEPTH_MODE_FLAT;
        }
        else if (depthSource.equals("ramp")) {
            config.vrDepthMode = XrShared.DEPTH_MODE_RAMP;
        }
        else if (depthSource.equals("blob")) {
            config.vrDepthMode = XrShared.DEPTH_MODE_BLOB;
        }
        else if (depthSource.equals("eyetest")) {
            config.vrDepthMode = XrShared.DEPTH_MODE_EYETEST;
        }
        else if (depthSource.equals("shifttest")) {
            config.vrDepthMode = XrShared.DEPTH_MODE_SHIFTTEST;
        }
        else if (isDepthModel(depthSource)) {
            config.vrDepthMode = XrShared.DEPTH_MODE_MODEL;
            config.vrDepthModel = depthSource;
        }
        else {
            config.vrDepthMode = XrShared.DEPTH_MODE_OFF;
        }
        String envRes = prefs.getString(VR_ENV_RES_PREF_STRING, DEFAULT_VR_ENV_RES);
        if (envRes.equals("low")) {
            config.vrEnvResTier = VR_ENV_RES_LOW;
        }
        else if (envRes.equals("high")) {
            config.vrEnvResTier = VR_ENV_RES_HIGH;
        }
        else if (envRes.equals("ultra")) {
            config.vrEnvResTier = VR_ENV_RES_ULTRA;
        }
        else {
            config.vrEnvResTier = VR_ENV_RES_STANDARD;
        }
        String sharpening = prefs.getString(VR_SHARPENING_PREF_STRING, DEFAULT_VR_SHARPENING);
        if (sharpening.equals("off")) {
            config.vrSharpening = 0;
        }
        else if (sharpening.equals("normal")) {
            config.vrSharpening = 1;
        }
        else {
            config.vrSharpening = 2;
        }
        config.vrSupersampling = supersamplingMode(
                prefs.getString(VR_SUPERSAMPLING_PREF_STRING, DEFAULT_VR_SUPERSAMPLING));
        config.vrEyeSwap = prefs.getBoolean(VR_EYE_SWAP_PREF_STRING, DEFAULT_VR_EYE_SWAP);
        config.vrPassthrough = prefs.getBoolean(VR_PASSTHROUGH_PREF_STRING, DEFAULT_VR_PASSTHROUGH);
        config.vrPointer = prefs.getBoolean(VR_POINTER_PREF_STRING, DEFAULT_VR_POINTER);
        config.vrGaze = prefs.getBoolean(VR_GAZE_PREF_STRING, DEFAULT_VR_GAZE);
        config.vrHandTracking = prefs.getBoolean(VR_HAND_TRACKING_PREF_STRING, DEFAULT_VR_HAND_TRACKING);
        config.vrHandLockHintSeen = handLockHintSeen(prefs);
        config.vrPointerSleep = pointerSleepOn(prefs);
        config.vrClickSound = clickSoundOn(prefs);
        config.vrDoffGrace = doffGraceOn(prefs);
        config.vrShowRay = rayShown(prefs);
        config.vrControllerModel = controllerModelOn(prefs);
        config.vrHeadAim = headAimOn(prefs);
        config.vrHeadAimSensitivity = headAimSensitivity(prefs);
        config.vrHeadAimDeadZone = headAimDeadZone(prefs);
        config.vrGamepadToggle = gamepadToggle(prefs);
        config.vrStereoSeparation = storedSeparation(prefs, config.vrDepthModel);
        config.vrDepthDebug = prefs.getBoolean(VR_DEPTH_DEBUG_PREF_STRING, DEFAULT_VR_DEPTH_DEBUG);
        config.vrDepthRate = storedDepthRate(prefs, isXr2Gen1Headset());
        config.vrConvergence = storedConvergence(prefs, config.vrDepthModel);
        config.vrDepthScale = prefs.getInt(VR_DEPTH_SCALE_PREF_STRING, DEFAULT_VR_DEPTH_SCALE);
        config.vrAmbilight = prefs.getBoolean(VR_AMBILIGHT_PREF_STRING, DEFAULT_VR_AMBILIGHT);
        config.vrAmbilightLevel = prefs.getInt(VR_AMBILIGHT_LEVEL_PREF_STRING,
                DEFAULT_VR_AMBILIGHT_LEVEL);
        config.vrRoomLight = prefs.getBoolean(VR_ROOM_LIGHT_PREF_STRING, DEFAULT_VR_ROOM_LIGHT);
        config.vrPicture = readPicture(prefs);
        config.vrEnvironmentId = prefs.getInt(VR_ENVIRONMENT_ID_PREF_STRING, -1);
        config.vrRoomLevels = isRoomEnvironment(config.vrEnvironmentId)
                ? readRoomLevels(prefs, config.vrEnvironmentId) : null;
        config.fileLogLevel = prefs.getString(FILE_LOG_PREF_STRING, DEFAULT_FILE_LOG);
        config.vrVirtualSurround = prefs.getBoolean(VR_VIRTUAL_SURROUND_PREF_STRING,
                DEFAULT_VR_VIRTUAL_SURROUND);

        return config;
    }
}
