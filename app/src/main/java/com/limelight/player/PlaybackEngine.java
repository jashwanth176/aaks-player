package com.limelight.player;

import android.graphics.Bitmap;
import android.view.Surface;

import java.util.List;

/**
 * Playback backend: ExoPlayer for video files, libdisc for Blu-ray 3D disc images.
 * The UI and the resume store use only this interface.
 */
interface PlaybackEngine {

    /** Engine callbacks. Any thread. */
    interface Listener {
        /** Current subtitle: text, or a bitmap and its width as a fraction of the video width; nulls clear. */
        void onSubtitle(CharSequence text, Bitmap image, float widthShare);

        void onError(String title, String detail);

        void onVideoSize(int width, int height);

        /**
         * Transfer function (a VIDEO_TRANSFER_CODE_ value), MaxCLL and mastering display peak
         * in nits; 0 when the file does not say. Playback thread.
         */
        default void onVideoColour(int transfer, float maxCll, float masteringNits) {
        }
    }

    /** Playback rate, 1 = normal; audio is time-stretched with pitch kept. Main thread. */
    default void setSpeed(float speed) {
    }

    /** Starts playback into the surface at startMs. Main thread. */
    void start(Surface surface, long startMs, boolean play);

    long positionMs();

    /** 0 while unknown. */
    long durationMs();

    /** True while playing or about to play; false when paused. */
    boolean isPlaying();

    void setPlaying(boolean playing);

    /** Subtitle offset, positive = later. May be called before start and during playback. */
    default void setSubtitleDelayMs(long ms) {
    }

    void seekToMs(long ms);

    /** Whether the position is valid enough to save. */
    boolean isReady();

    List<PlayerUi.TrackChoice> audioTracks();

    /** First entry is Off. */
    List<PlayerUi.TrackChoice> subtitleTracks();

    void chooseAudio(PlayerUi.TrackChoice choice);

    void chooseSubtitle(PlayerUi.TrackChoice choice);

    void release();
}
