package com.limelight.binding.video;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/** The tick the panels make, and how much silence the frame loop keeps queued ahead of it. */
public class XrClickSoundTest {

    @Test
    public void theTickIsShortQuietAndEndsOnSilence() {
        short[] tick = XrClickSound.tickSamples(48000);
        // 18 ms at 48 kHz
        assertEquals(864, tick.length);
        assertEquals(0, tick[0]);
        int peak = 0;
        for (short s : tick) {
            peak = Math.max(peak, Math.abs(s));
        }
        assertTrue(peak < Short.MAX_VALUE * 0.3);
        assertTrue(peak > Short.MAX_VALUE * 0.1);
        // Tail ends near zero so the cut-off does not click.
        for (int i = tick.length - 8; i < tick.length; i++) {
            assertTrue(Math.abs(tick[i]) < 50);
        }
        // 18 ms at 44.1 kHz
        assertEquals(793, XrClickSound.tickSamples(44100).length);
    }

    @Test
    public void theQueueIsToppedUpToItsTarget() {
        assertEquals(1440, XrClickSound.framesToQueue(0, 0, 1440));
        assertEquals(0, XrClickSound.framesToQueue(1440, 0, 1440));
        // 940 frames still queued.
        assertEquals(500, XrClickSound.framesToQueue(10000, 9060, 1440));
        // Already over the target after a tick.
        assertEquals(0, XrClickSound.framesToQueue(12000, 9000, 1440));
    }

    @Test
    public void aTickAlwaysFitsBehindTheSilence() {
        // 1440 = 30 ms at 48 kHz.
        assertEquals(1440, XrClickSound.queueFor(4608, 864));
        // Tight buffer: leave room for the tick plus a quarter of it (216).
        assertEquals(2016 - 864 - 216, XrClickSound.queueFor(2016, 864));
        assertTrue(XrClickSound.queueFor(2016, 864) + 864 <= 2016);
        // Floor of 144 frames (3 ms).
        assertEquals(144, XrClickSound.queueFor(900, 864));
    }

    @Test
    public void aSlowFrameLoopQueuesMore() {
        // 90 Hz frame (533) plus a mixer cycle (960) just exceeds 1440; faster loops keep 1440.
        assertEquals(1493, XrClickSound.queueTarget(1440, 533 + 960, 4608, 864));
        assertEquals(1440, XrClickSound.queueTarget(1440, 400 + 960, 4608, 864));
        // 40 ms frame (1920), e.g. while the session loads.
        assertEquals(1920 + 960, XrClickSound.queueTarget(1440, 1920 + 960, 4608, 864));
        // Capped so a tick still fits.
        assertEquals(4608 - 864 - 216, XrClickSound.queueTarget(1440, 9000, 4608, 864));
    }

    @Test
    public void theTrackHeadWrapsWithoutHarm() {
        // The playback head is an int: negative past 2^31, wraps at 2^32.
        long written = (1L << 32) + 1000;
        int head = 0;
        assertEquals(440, XrClickSound.framesToQueue(written, head, 1440));
        long nearTop = (1L << 31) + 2000;
        int headPast = (int)((1L << 31) + 1000);
        assertTrue(headPast < 0);
        assertEquals(440, XrClickSound.framesToQueue(nearTop, headPast, 1440));
    }
}
