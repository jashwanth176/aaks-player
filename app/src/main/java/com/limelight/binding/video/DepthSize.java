package com.limelight.binding.video;

/**
 * Depth map size, which is also the model input size: 16:9 for ZipDepth on
 * XR2 Gen 2, square for MiDaS and for ZipDepth on Gen 1. Prints as WxH.
 */
public final class DepthSize {
    public final int width;
    public final int height;

    public DepthSize(int width, int height) {
        this.width = width;
        this.height = height;
    }

    public static DepthSize square(int edge) {
        return new DepthSize(edge, edge);
    }

    public int pixels() {
        return width * height;
    }

    public boolean isSquare() {
        return width == height;
    }

    @Override
    public boolean equals(Object other) {
        if (!(other instanceof DepthSize)) {
            return false;
        }
        DepthSize that = (DepthSize)other;
        return width == that.width && height == that.height;
    }

    @Override
    public int hashCode() {
        return 31 * width + height;
    }

    @Override
    public String toString() {
        return width+"x"+height;
    }
}
