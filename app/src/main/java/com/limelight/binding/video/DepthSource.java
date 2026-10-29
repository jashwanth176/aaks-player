package com.limelight.binding.video;

import android.content.Context;

import java.nio.ByteBuffer;

/**
 * Produces a depth map from the current video frame. The direct buffers given
 * to initialize() are the staging for the native input and output textures,
 * one of each per staging pair.
 */
public interface DepthSource {
    /**
     * Buffers are at the session's depth size, the model's input size.
     *
     * @param inputs  RGB, width by height, float in 0..1, row 0 at the top
     * @param outputs single channel depth, width by height, float, larger is
     *                nearer, arbitrary scale
     */
    boolean initialize(Context context, ByteBuffer[] inputs, ByteBuffer[] outputs);

    /** Runs one inference from that pair's input buffer into its output. */
    boolean estimate(int pair);

    /** Wall time of the last estimate() call. */
    float getLastInferenceMs();

    boolean isGpuAccelerated();

    void release();
}
