package com.limelight.binding.video;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** Geometry of the splash's film-reel logo. */
public class SplashLogoTest {

    @Test
    public void thereIsAHoleForEveryTick() {
        assertEquals(XrShared.SPLASH_WEDGES, XrSplash.Logo.HOLES);
        assertEquals(XrShared.SPLASH_ROWS, XrSplash.Logo.HOLES + 1);
    }

    @Test
    public void theHolesSitInsideTheDiscWithARim() {
        for (int k = 0; k < XrSplash.Logo.HOLES; k++) {
            float[] c = XrSplash.Logo.hole(k);
            double reach = Math.hypot(c[0], c[1]) + XrSplash.Logo.HOLE_R;
            assertTrue("hole " + k + " reaches " + reach, reach <= 0.85);
        }
    }

    @Test
    public void noHoleTouchesAnotherOrThePlayTriangle() {
        for (int k = 0; k < XrSplash.Logo.HOLES; k++) {
            float[] a = XrSplash.Logo.hole(k);
            float[] b = XrSplash.Logo.hole((k + 1) % XrSplash.Logo.HOLES);
            double between = Math.hypot(a[0] - b[0], a[1] - b[1]) - 2 * XrSplash.Logo.HOLE_R;
            assertTrue("holes " + k + " and the next are " + between + " apart", between > 0.1);
            // 0.05: a spoke's width.
            float[] p = XrSplash.Logo.play();
            for (int i = 0; i < 3; i++) {
                double d = Math.hypot(p[2 * i] - a[0], p[2 * i + 1] - a[1]) - XrSplash.Logo.HOLE_R;
                assertTrue("corner " + i + " is " + d + " from hole " + k, d > 0.05);
            }
            assertFalse(XrSplash.Logo.inPlay(a[0], a[1]));
        }
    }

    @Test
    public void thePlayTrianglePointsRightFromTheHub() {
        float[] p = XrSplash.Logo.play();
        // Tip is the rightmost corner, level with the centre.
        assertTrue(p[0] > p[2] && p[0] > p[4]);
        assertEquals(0.0, p[1], 1e-6);
        assertTrue(XrSplash.Logo.inPlay(0.0, 0.0));
        assertTrue(XrSplash.Logo.inPlay(XrSplash.Logo.PLAY_SHIFT, 0.0));
        assertFalse(XrSplash.Logo.inPlay(0.5, 0.0));
        assertFalse(XrSplash.Logo.inPlay(-0.3, 0.0));
    }

    @Test
    public void theHolesOpenClockwiseFromTwelve() {
        float[] first = XrSplash.Logo.hole(0);
        assertEquals(0.0, first[0], 1e-6);
        assertTrue(first[1] < 0.0f);
        float[] second = XrSplash.Logo.hole(1);
        // y points down, so clockwise from twelve moves right.
        assertTrue(second[0] > 0.0f);
        for (int k = 0; k < XrSplash.Logo.HOLES; k++) {
            float[] c = XrSplash.Logo.hole(k);
            assertTrue(XrSplash.Logo.inHole(k, c[0], c[1]));
            assertFalse(XrSplash.Logo.inHole(k, 0.0, 0.0));
        }
    }
}
