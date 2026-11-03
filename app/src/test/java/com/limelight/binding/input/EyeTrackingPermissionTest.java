package com.limelight.binding.input;

import org.junit.Test;

import java.util.Arrays;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** When the eyes may point, and which manifest names count as eye tracking. */
public class EyeTrackingPermissionTest {

    @Test
    public void theEyesPointUnlessARuntimePermissionIsRefused() {
        assertTrue(EyeTrackingPermission.gazeAllowed(false, false));
        assertTrue(EyeTrackingPermission.gazeAllowed(true, true));
        assertFalse(EyeTrackingPermission.gazeAllowed(true, false));
    }

    @Test
    public void picksTheEyeTrackingNamesOutOfTheManifest() {
        String[] requested = {
                "android.permission.INTERNET", EyeTrackingPermission.PICO,
                "com.oculus.permission.HAND_TRACKING", EyeTrackingPermission.META,
                "android.permission.HAND_TRACKING"
        };
        assertEquals(Arrays.asList(EyeTrackingPermission.META, EyeTrackingPermission.PICO),
                EyeTrackingPermission.declared(requested, EyeTrackingPermission.NAMES));
        assertTrue(EyeTrackingPermission.declared(null, EyeTrackingPermission.NAMES).isEmpty());
        assertTrue(EyeTrackingPermission.declared(new String[] { "x" },
                EyeTrackingPermission.NAMES).isEmpty());
    }
}
