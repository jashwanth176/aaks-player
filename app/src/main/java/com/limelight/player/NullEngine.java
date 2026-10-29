package com.limelight.player;

import android.view.Surface;

import java.util.ArrayList;
import java.util.List;

/** Empty engine for when the app is launched without a file and only shows the library. */
final class NullEngine implements PlaybackEngine {

    @Override
    public void start(Surface surface, long startMs, boolean play) {
    }

    @Override
    public long positionMs() {
        return 0;
    }

    @Override
    public long durationMs() {
        return 0;
    }

    @Override
    public boolean isPlaying() {
        return false;
    }

    @Override
    public void setPlaying(boolean playing) {
    }

    @Override
    public void seekToMs(long ms) {
    }

    @Override
    public boolean isReady() {
        return false;
    }

    @Override
    public List<PlayerUi.TrackChoice> audioTracks() {
        return new ArrayList<>();
    }

    @Override
    public List<PlayerUi.TrackChoice> subtitleTracks() {
        return new ArrayList<>();
    }

    @Override
    public void chooseAudio(PlayerUi.TrackChoice choice) {
    }

    @Override
    public void chooseSubtitle(PlayerUi.TrackChoice choice) {
    }

    @Override
    public void release() {
    }
}
