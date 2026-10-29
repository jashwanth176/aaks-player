package com.limelight.binding.input;

import android.content.Context;

import com.limelight.FileLog;

import java.util.ArrayList;
import java.util.List;

/**
 * The Android XR hand tracking permission, needed for the hand joints (the deliberate pinch).
 * Meta's is granted at install and Pico has none. Requested with the eye tracking permission.
 * The joints are looked up at session start, so a later grant counts from the next session.
 */
final class HandTrackingPermission {

    static final String ANDROID_XR = "android.permission.HAND_TRACKING";
    static final String[] NAMES = { ANDROID_XR };

    // Once per run, like the eyes
    private static boolean askedThisRun;

    private HandTrackingPermission() {
    }

    /** What to add to this session's request, logging why not where it is not asked. */
    static List<String> toAsk(Context context, boolean handsOn) {
        List<String> out = new ArrayList<>();
        if (!handsOn) {
            return out;
        }
        List<String> names = EyeTrackingPermission.runtimeNames(context, NAMES);
        boolean granted = EyeTrackingPermission.anyGranted(context, names);
        if (EyeTrackingPermission.shouldAsk(true, true, !names.isEmpty(), granted, askedThisRun)) {
            askedThisRun = true;
            out.addAll(names);
        }
        else if (granted) {
            FileLog.event("hand tracking permission already granted");
        }
        // Not a runtime permission anywhere but Android XR, so not worth a log line
        return out;
    }

    /** Logs the hands' half of an answer, where the request carried them. */
    static void onResult(Context context, String[] permissions, int[] results) {
        List<String> names = EyeTrackingPermission.runtimeNames(context, NAMES);
        if (!EyeTrackingPermission.asked(permissions, names)) {
            return;
        }
        int answer = EyeTrackingPermission.answer(permissions, results, names);
        if (answer == EyeTrackingPermission.ANSWER_GRANTED) {
            FileLog.event("hand tracking permission granted");
        }
        else if (answer == EyeTrackingPermission.ANSWER_DENIED) {
            FileLog.event("hand tracking permission denied, the runtime's pinch only");
        }
    }
}
