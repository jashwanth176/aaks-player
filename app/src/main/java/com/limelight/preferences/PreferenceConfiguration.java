package com.limelight.preferences;

import android.os.Build;

import com.limelight.binding.video.MidasDepthSource;
import com.limelight.binding.video.XrShared;

/**
 * What a VR session starts with. None of it is a stored setting: the player sets the screen
 * and the depth mode per film, and the rest are fixed.
 */
public class PreferenceConfiguration {
    // Passthrough or the dark void around the screen, saved by the renderer. Other saved ids
    // were 3D rooms and read as the void.
    public static final String VR_ENVIRONMENT_ID_PREF_STRING = "vr_environment_id";
    public static final int VR_ENV_PASSTHROUGH = 0;
    public static final int VR_ENV_VOID = 1;

    public static final String VR_DEPTH_SOURCE_ZIPDEPTH = "zipdepth";
    public static final int DEFAULT_VR_CONVERGENCE = 50;

    // The video's frame rate; the display rate is matched to it
    public int fps;
    public boolean vrHeadLocked;
    // Tenths of a metre
    public int vrDistance = 25;
    public int vrScreenSize = 24;
    // 0 to 100
    public int vrCurvature;
    // A DEPTH_MODE_ value
    public int vrDepthMode;
    public String vrDepthModel = VR_DEPTH_SOURCE_ZIPDEPTH;
    // Compositor sharpening and supersampling: 0 off, 1 normal, 2 quality
    public int vrSharpening = 2;
    public int vrSupersampling;
    public boolean vrEyeSwap;
    // Tenths of a percent of frame width
    public int vrStereoSeparation = MidasDepthSource.ZIPDEPTH.defaultSeparation;
    public boolean vrDepthDebug;
    public boolean vrPointer = true;
    public boolean vrGaze = true;
    public boolean vrHandTracking = true;
    public boolean vrPointerSleep = true;
    // Whether a session starts with the controller ray drawn
    public boolean vrShowRay = true;
    public boolean vrControllerModel;
    // Maximum depth maps per second; the renderer lowers it under load
    public int vrDepthRate = isXr2Gen1Headset() ? XrShared.DEPTH_RATE_GEN1
                                                : XrShared.DEPTH_RATE_DEFAULT;
    public int vrConvergence = MidasDepthSource.ZIPDEPTH.defaultConvergence;
    public int vrDepthScale = 100;
    // Brightness, contrast, gamma, saturation in XrShared PICTURE_ order and units
    public int[] vrPicture = {
            XrShared.PICTURE_BRIGHTNESS_DEFAULT, XrShared.PICTURE_CONTRAST_DEFAULT,
            XrShared.PICTURE_GAMMA_DEFAULT, XrShared.PICTURE_SATURATION_DEFAULT,
    };

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
}
