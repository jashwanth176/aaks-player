package com.limelight.binding.video;

import org.junit.Test;

import static com.limelight.binding.video.XrShared.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/** The Picture tab's readouts, tick positions, setting ids and layout. */
public class PictureTabTest {

    @Test
    public void theReadoutsSayWholeUnitsWithTheirUnit() {
        assertEquals("+12", XrPanels.pictureReadout(PICTURE_BRIGHTNESS, 12));
        assertEquals("0", XrPanels.pictureReadout(PICTURE_BRIGHTNESS, 0));
        assertEquals("-7", XrPanels.pictureReadout(PICTURE_BRIGHTNESS, -7));
        assertEquals("+50", XrPanels.pictureReadout(PICTURE_BRIGHTNESS, 50));
        assertEquals("-50", XrPanels.pictureReadout(PICTURE_BRIGHTNESS, -50));
        assertEquals("120%", XrPanels.pictureReadout(PICTURE_CONTRAST, 120));
        assertEquals("50%", XrPanels.pictureReadout(PICTURE_CONTRAST, 50));
        assertEquals("1.20", XrPanels.pictureReadout(PICTURE_GAMMA, 120));
        assertEquals("1.00", XrPanels.pictureReadout(PICTURE_GAMMA, 100));
        assertEquals("0.50", XrPanels.pictureReadout(PICTURE_GAMMA, 50));
        assertEquals("1.05", XrPanels.pictureReadout(PICTURE_GAMMA, 105));
        assertEquals("2.00", XrPanels.pictureReadout(PICTURE_GAMMA, 200));
        assertEquals("80%", XrPanels.pictureReadout(PICTURE_SATURATION, 80));
        assertEquals("0%", XrPanels.pictureReadout(PICTURE_SATURATION, 0));
        assertEquals("200%", XrPanels.pictureReadout(PICTURE_SATURATION, 200));
    }

    @Test
    public void theTicksAreThePictureAsStreamed() {
        assertEquals(0.5f, XrPanels.pictureTickT(PICTURE_BRIGHTNESS), 1e-6f);
        assertEquals(0.5f, XrPanels.pictureTickT(PICTURE_CONTRAST), 1e-6f);
        assertEquals(1.0f / 3.0f, XrPanels.pictureTickT(PICTURE_GAMMA), 1e-6f);
        assertEquals(0.5f, XrPanels.pictureTickT(PICTURE_SATURATION), 1e-6f);
    }

    @Test
    public void eachRowHasASettingOfItsOwn() {
        int[] settings = { SETTING_PICTURE_BRIGHTNESS, SETTING_PICTURE_CONTRAST,
                SETTING_PICTURE_GAMMA, SETTING_PICTURE_SATURATION, SETTING_RESET_PICTURE };
        for (int i = 0; i < settings.length; i++) {
            // Above all earlier ids, and distinct.
            assertTrue(settings[i] > SETTING_CLICK_SOUND);
            for (int j = 0; j < i; j++) {
                assertTrue(settings[i] != settings[j]);
            }
        }
    }

    @Test
    public void theStripCarriesWhichTabAndEveryRow() {
        // One slot for the tab, then one per row of the larger tab.
        assertTrue(READOUT_VALUES >= 1 + PICTURE_VALUES);
        assertTrue(READOUT_VALUES >= 1 + 3);
        assertEquals(IN_READOUT + READOUT_VALUES, IN_STEREO);
        assertTrue(READOUT_ROOM != READOUT_PICTURE);
        assertTrue(READOUT_ROOM >= 0 && READOUT_PICTURE >= 0);
    }

    @Test
    public void thePictureTabIsTheFourth() {
        assertEquals(3, COG_TAB_PICTURE);
        // About follows it.
        assertEquals(5, COG_TAB_COUNT);
        assertEquals(COG_TAB_COUNT, COG_ART_ROOM);
        // One sheet per tab, the fixed Room tab, then the screen and display
        // tabs for a screen not locked to the head.
        assertEquals(COG_ART_ROOM + COG_TAB_COUNT + 1 + 2, COG_ART_COUNT);
        // Same row positions as the other tabs, clear of the reset button.
        for (int row = 0; row < PICTURE_VALUES; row++) {
            assertEquals(COG_ROW_V0 + row * COG_ROW_STEP, XrPanels.cogRowV(COG_TAB_PICTURE, row),
                    1e-6f);
        }
        assertTrue(XrPanels.cogRowV(COG_TAB_PICTURE, PICTURE_VALUES - 1) + COG_ROW_HALF
                < COG_RESET_T);
    }
}
