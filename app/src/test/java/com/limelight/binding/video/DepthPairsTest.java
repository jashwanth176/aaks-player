package com.limelight.binding.video;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** Depth staging pairs moving through capture, readback, model and upload. */
public class DepthPairsTest {

    /** Captures and reads back a pair, and leaves the model running it. */
    private static int toModel(DepthPairs pairs) {
        int pair = pairs.forCapture();
        pairs.captured(pair);
        assertEquals(pair, pairs.takeToFinish());
        pairs.finished(pair, true);
        assertEquals(pair, pairs.takeToRun());
        return pair;
    }

    /** Captures and reads back a pair, leaving it staged. */
    private static int staged(DepthPairs pairs) {
        int pair = pairs.forCapture();
        pairs.captured(pair);
        assertEquals(pair, pairs.takeToFinish());
        pairs.finished(pair, true);
        return pair;
    }

    @Test
    public void anIdlePipelineOnlyTakesACapture() {
        DepthPairs pairs = new DepthPairs(2);
        assertEquals(0, pairs.forCapture());
        assertEquals(-1, pairs.takeToFinish());
        assertEquals(-1, pairs.takeToRun());
        assertEquals(-1, pairs.takeToUpload());
    }

    @Test
    public void theNextCaptureIsReadBackWhileTheModelRuns() {
        DepthPairs pairs = new DepthPairs(2);
        int running = toModel(pairs);
        int next = pairs.forCapture();
        assertTrue(next >= 0 && next != running);
        pairs.captured(next);
        assertEquals(next, pairs.takeToFinish());
        pairs.finished(next, true);
        // Staged; the model is still busy with the first.
        assertEquals(-1, pairs.takeToRun());
        assertEquals(DepthPairs.STAGED, pairs.state(next));
    }

    @Test
    public void onlyOneCaptureWaitsForTheModel() {
        DepthPairs pairs = new DepthPairs(3);
        toModel(pairs);
        int waiting = pairs.forCapture();
        pairs.captured(waiting);
        assertEquals(-1, pairs.forCapture());
        pairs.takeToFinish();
        assertEquals(-1, pairs.forCapture());
        pairs.finished(waiting, true);
        assertEquals(-1, pairs.forCapture());
    }

    @Test
    public void theModelTakesTheWaitingPairStraightAfter() {
        DepthPairs pairs = new DepthPairs(2);
        int first = toModel(pairs);
        int second = staged(pairs);

        pairs.ran(first, true);
        assertEquals(second, pairs.takeToRun());
        // The first map uploads while the second runs.
        assertEquals(first, pairs.takeToUpload());
        assertTrue(pairs.made(first));
        pairs.freed(first);
        assertEquals(first, pairs.forCapture());
    }

    @Test
    public void noCaptureWithBothPairsBusy() {
        DepthPairs pairs = new DepthPairs(2);
        int first = toModel(pairs);
        staged(pairs);
        pairs.ran(first, true);
        pairs.takeToRun();
        assertEquals(-1, pairs.forCapture());
    }

    /** A failed readback is dropped only after the older map uploads, so its cut check lands in order. */
    @Test
    public void aDropWaitsBehindAnOlderMap() {
        DepthPairs pairs = new DepthPairs(2);
        int older = toModel(pairs);
        int newer = pairs.forCapture();
        pairs.captured(newer);
        pairs.takeToFinish();
        pairs.finished(newer, false);
        assertEquals(DepthPairs.RAN, pairs.state(newer));
        assertEquals(-1, pairs.takeToUpload());

        pairs.ran(older, true);
        assertEquals(older, pairs.takeToUpload());
        assertEquals(-1, pairs.takeToUpload());
        pairs.freed(older);
        assertEquals(newer, pairs.takeToUpload());
        assertFalse(pairs.made(newer));
    }

    @Test
    public void aNewerMapWaitsForTheOlderOne() {
        DepthPairs pairs = new DepthPairs(2);
        int older = toModel(pairs);
        int newer = staged(pairs);
        // The single model thread cannot finish out of order, but upload order
        // must not depend on that.
        pairs.ran(newer, true);
        assertEquals(-1, pairs.takeToUpload());
        pairs.ran(older, true);
        assertEquals(older, pairs.takeToUpload());
        pairs.freed(older);
        assertEquals(newer, pairs.takeToUpload());
    }

    @Test
    public void mapsGoUpInCaptureOrderAsPairsComeRound() {
        DepthPairs pairs = new DepthPairs(2);
        int a = toModel(pairs);
        int b = staged(pairs);
        pairs.ran(a, true);
        pairs.takeToRun();
        pairs.takeToUpload();
        pairs.freed(a);

        // a again, captured after b
        assertEquals(a, pairs.forCapture());
        pairs.captured(a);
        pairs.takeToFinish();
        pairs.finished(a, true);
        pairs.ran(b, true);
        assertEquals(a, pairs.takeToRun());
        pairs.ran(a, true);
        assertEquals(b, pairs.takeToUpload());
        pairs.freed(b);
        assertEquals(a, pairs.takeToUpload());
    }

    @Test
    public void aFailedRunIsDropped() {
        DepthPairs pairs = new DepthPairs(2);
        int pair = toModel(pairs);
        pairs.ran(pair, false);
        assertEquals(pair, pairs.takeToUpload());
        assertFalse(pairs.made(pair));
        pairs.freed(pair);
        assertEquals(DepthPairs.FREE, pairs.state(pair));
    }

    @Test
    public void aPairUsedAgainStartsClean() {
        DepthPairs pairs = new DepthPairs(2);
        int pair = toModel(pairs);
        pairs.ran(pair, true);
        pairs.takeToUpload();
        pairs.freed(pair);
        assertEquals(pair, pairs.forCapture());
        pairs.captured(pair);
        assertFalse(pairs.made(pair));
        assertEquals(DepthPairs.CAPTURED, pairs.state(pair));
    }
}
