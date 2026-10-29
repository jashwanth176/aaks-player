package com.limelight.binding.video;

/**
 * State of each depth staging pair: captured by the frame loop, read back by
 * the stage thread, run by the depth thread, then uploaded or dropped and freed.
 * Only one capture waits for the model at a time, and pairs are uploaded or
 * dropped in capture order. Not thread safe: callers hold the depth lock.
 */
final class DepthPairs {
    static final int FREE = 0;
    static final int CAPTURED = 1;
    static final int FINISHING = 2;
    static final int STAGED = 3;
    static final int RUNNING = 4;
    static final int RAN = 5;
    static final int UPLOADING = 6;

    private final int[] state;
    // Capture order of each pair, and whether the model made a map from it
    private final long[] order;
    private final boolean[] made;
    private long captures;

    DepthPairs(int pairs) {
        state = new int[pairs];
        order = new long[pairs];
        made = new boolean[pairs];
    }

    int state(int pair) {
        return state[pair];
    }

    /**
     * The pair the next capture goes into, or -1 when a capture is already waiting
     * for the model or no pair is free. The pair stays free until captured(); only
     * the frame loop hands out free pairs.
     */
    int forCapture() {
        int free = -1;
        for (int i = 0; i < state.length; i++) {
            int s = state[i];
            if (s == CAPTURED || s == FINISHING || s == STAGED) {
                return -1;
            }
            if (s == FREE && free < 0) {
                free = i;
            }
        }
        return free;
    }

    /** A readback was queued into the pair forCapture() gave out. */
    void captured(int pair) {
        order[pair] = ++captures;
        made[pair] = false;
        state[pair] = CAPTURED;
    }

    /** The next capture to read back, now FINISHING, or -1. */
    int takeToFinish() {
        int pair = first(CAPTURED);
        if (pair >= 0) {
            state[pair] = FINISHING;
        }
        return pair;
    }

    /** Readback done: STAGED for the model, or RAN when there was nothing to run on. */
    void finished(int pair, boolean staged) {
        state[pair] = staged ? STAGED : RAN;
    }

    /** The next pair for the model, now RUNNING, or -1 while another runs. */
    int takeToRun() {
        if (first(RUNNING) >= 0) {
            return -1;
        }
        int pair = first(STAGED);
        if (pair >= 0) {
            state[pair] = RUNNING;
        }
        return pair;
    }

    /** The model is done with the pair, and says whether it made a map. */
    void ran(int pair, boolean madeMap) {
        made[pair] = madeMap;
        state[pair] = RAN;
    }

    /** The oldest pair in flight once the model is done with it, now UPLOADING, or -1. */
    int takeToUpload() {
        int oldest = -1;
        for (int i = 0; i < state.length; i++) {
            if (state[i] != FREE && (oldest < 0 || order[i] < order[oldest])) {
                oldest = i;
            }
        }
        if (oldest < 0 || state[oldest] != RAN) {
            return -1;
        }
        state[oldest] = UPLOADING;
        return oldest;
    }

    /** Whether the model made a map, so the pair is uploaded rather than dropped. */
    boolean made(int pair) {
        return made[pair];
    }

    /** The map is up or dropped; the pair is free again. */
    void freed(int pair) {
        state[pair] = FREE;
    }

    private int first(int wanted) {
        for (int i = 0; i < state.length; i++) {
            if (state[i] == wanted) {
                return i;
            }
        }
        return -1;
    }
}
