package com.limelight.binding.input;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.os.Build;

import java.util.ArrayList;
import java.util.List;

/**
 * The eye tracking permission (Meta, Pico and Android XR names). Where the platform makes it a
 * runtime permission the gaze pose never arrives without the grant, so the eyes only point
 * once it is granted.
 */
public final class EyeTrackingPermission {

    public static final String META = "com.oculus.permission.EYE_TRACKING";
    public static final String PICO = "com.picovr.permission.EYE_TRACKING";
    // Android XR's name for gaze as an input, the one XR_EXT_eye_gaze_interaction wants
    public static final String ANDROID_XR = "android.permission.EYE_TRACKING_FINE";
    static final String[] NAMES = { META, PICO, ANDROID_XR };

    private EyeTrackingPermission() {
    }

    /** Whether the eyes may point: nothing to grant here, or it is granted. */
    static boolean gazeAllowed(boolean runtime, boolean granted) {
        return !runtime || granted;
    }

    /** Those of names the manifest asks for, in the order of names. */
    static List<String> declared(String[] requested, String[] names) {
        List<String> out = new ArrayList<>();
        if (requested == null) {
            return out;
        }
        for (String name : names) {
            for (String r : requested) {
                if (name.equals(r)) {
                    out.add(name);
                    break;
                }
            }
        }
        return out;
    }

    // The declared names this platform defines as runtime permissions
    private static List<String> runtimeNames(Context context) {
        List<String> out = new ArrayList<>();
        // Nothing is a runtime permission before Android 6
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return out;
        }
        PackageManager pm = context.getPackageManager();
        String[] requested;
        try {
            PackageInfo info = pm.getPackageInfo(context.getPackageName(),
                    PackageManager.GET_PERMISSIONS);
            requested = info.requestedPermissions;
        }
        catch (PackageManager.NameNotFoundException e) {
            return out;
        }
        for (String name : declared(requested, NAMES)) {
            try {
                PermissionInfo p = pm.getPermissionInfo(name, 0);
                int base = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P ? p.getProtection()
                        : p.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE;
                if (base == PermissionInfo.PROTECTION_DANGEROUS) {
                    out.add(name);
                }
            }
            catch (PackageManager.NameNotFoundException e) {
                // Not a permission this platform has
            }
        }
        return out;
    }

    private static boolean anyGranted(Context context, List<String> names) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return false;
        }
        for (String name : names) {
            if (context.checkSelfPermission(name) == PackageManager.PERMISSION_GRANTED) {
                return true;
            }
        }
        return false;
    }

    /** Whether the eyes may point on this headset as things stand. */
    public static boolean gazeAllowed(Context context) {
        List<String> names = runtimeNames(context);
        return gazeAllowed(!names.isEmpty(), anyGranted(context, names));
    }
}
