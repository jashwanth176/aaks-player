package com.limelight.binding.video;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/** The touch clicks: short, starting from silence, never louder than asked. */
public class TouchSoundTest {

    @Test
    public void aClickStartsSilentPeaksWithinItsGainAndDiesAway() {
        short[] pcm = TouchSound.click(0.03f, new float[] { 1700.0f, 3100.0f }, 0.0045f, 0.22f);
        assertEquals(1440, pcm.length);
        assertEquals(0, pcm[0]);
        int peak = 0;
        for (short s : pcm) {
            peak = Math.max(peak, Math.abs(s));
        }
        assertTrue(peak > 0.1f * Short.MAX_VALUE);
        assertTrue(peak <= 0.22f * Short.MAX_VALUE + 1);
        int tail = 0;
        for (int i = pcm.length - 48; i < pcm.length; i++) {
            tail = Math.max(tail, Math.abs(pcm[i]));
        }
        assertTrue(tail < 0.01f * Short.MAX_VALUE);
    }
}
