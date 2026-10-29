package com.limelight.player;

import java.util.Locale;

/**
 * How a video packs its two eyes, and the size each eye is shown at.
 *
 * Full SBS (two whole frames, e.g. 3840x1080) keeps each half at its own size; half SBS
 * (two squeezed halves in one 16:9 frame) stretches each half to full width. OU likewise, vertically.
 */
public enum StereoLayout {
    MONO, SBS, OU;

    /**
     * The requested layout, or for "auto" and unknown values the one the file name implies,
     * then full SBS if the frame is at least 3:1.
     */
    public static StereoLayout resolve(String requested, String fileName, int width, int height) {
        if (requested != null) {
            switch (requested.toLowerCase(Locale.ROOT)) {
                case "mono":
                    return MONO;
                case "sbs":
                    return SBS;
                case "ou":
                case "tab":
                    return OU;
                default:
                    break;
            }
        }
        String name = fileName == null ? "" : fileName.toLowerCase(Locale.ROOT);
        // Release-name tokens: Movie.3D.HSBS.1080p, movie_fsbs, movie-ou, movie.tab, H-SBS
        String spaced = " " + name.replaceAll("[^a-z0-9]+", " ") + " ";
        if (spaced.matches(".*\\s(h?f?sbs|h sbs|f sbs|half sbs|full sbs|sidebyside|side by side|lr)\\s.*")) {
            return SBS;
        }
        if (spaced.matches(".*\\s(h?f?ou|h ou|f ou|tab|htab|ftab|half ou|full ou|overunder|over under|topbottom|top bottom|tb)\\s.*")) {
            return OU;
        }
        if (width > 0 && height > 0 && width >= 3 * height) {
            return SBS;
        }
        return MONO;
    }

    /** The width each eye is drawn at. */
    public int eyeWidth(int width, int height) {
        if (this == SBS && width >= 3 * height) {
            return width / 2;
        }
        return width;
    }

    /** The height each eye is drawn at. */
    public int eyeHeight(int width, int height) {
        if (this == OU && height >= width) {
            return height / 2;
        }
        return height;
    }
}
