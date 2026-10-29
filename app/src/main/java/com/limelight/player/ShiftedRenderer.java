package com.limelight.player;

import androidx.media3.common.Format;
import androidx.media3.common.Timeline;
import androidx.media3.common.util.Clock;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.ExoPlaybackException;
import androidx.media3.exoplayer.MediaClock;
import androidx.media3.exoplayer.Renderer;
import androidx.media3.exoplayer.RendererCapabilities;
import androidx.media3.exoplayer.RendererConfiguration;
import androidx.media3.exoplayer.analytics.PlayerId;
import androidx.media3.exoplayer.source.MediaSource;
import androidx.media3.exoplayer.source.SampleStream;

import java.io.IOException;
import java.util.function.LongSupplier;

/**
 * Delegating renderer that adds a time shift to the stream offset, used for subtitle delay
 * since TextRenderer is final. The shift is read only in enable/replaceStream, so a new value
 * takes effect when the track is re-enabled.
 */
@UnstableApi
final class ShiftedRenderer implements Renderer {

    private final Renderer inner;
    private final LongSupplier shiftUs;

    ShiftedRenderer(Renderer inner, LongSupplier shiftUs) {
        this.inner = inner;
        this.shiftUs = shiftUs;
    }

    @Override
    public String getName() {
        return inner.getName();
    }

    @Override
    public int getTrackType() {
        return inner.getTrackType();
    }

    @Override
    public RendererCapabilities getCapabilities() {
        return inner.getCapabilities();
    }

    @Override
    public void init(int index, PlayerId playerId, Clock clock) {
        inner.init(index, playerId, clock);
    }

    @Override
    public MediaClock getMediaClock() {
        return inner.getMediaClock();
    }

    @Override
    public int getState() {
        return inner.getState();
    }

    @Override
    public void enable(RendererConfiguration configuration, Format[] formats, SampleStream stream,
                       long positionUs, boolean joining, boolean mayRenderStartOfStream,
                       long startPositionUs, long offsetUs, MediaSource.MediaPeriodId mediaPeriodId)
            throws ExoPlaybackException {
        inner.enable(configuration, formats, stream, positionUs, joining, mayRenderStartOfStream,
                startPositionUs, offsetUs + shiftUs.getAsLong(), mediaPeriodId);
    }

    @Override
    public void start() throws ExoPlaybackException {
        inner.start();
    }

    @Override
    public void replaceStream(Format[] formats, SampleStream stream, long startPositionUs, long offsetUs,
                              MediaSource.MediaPeriodId mediaPeriodId) throws ExoPlaybackException {
        inner.replaceStream(formats, stream, startPositionUs, offsetUs + shiftUs.getAsLong(), mediaPeriodId);
    }

    @Override
    public SampleStream getStream() {
        return inner.getStream();
    }

    @Override
    public boolean hasReadStreamToEnd() {
        return inner.hasReadStreamToEnd();
    }

    @Override
    public long getReadingPositionUs() {
        return inner.getReadingPositionUs();
    }

    @Override
    public long getDurationToProgressUs(long positionUs, long elapsedRealtimeUs) {
        return inner.getDurationToProgressUs(positionUs, elapsedRealtimeUs);
    }

    @Override
    public void setCurrentStreamFinal() {
        inner.setCurrentStreamFinal();
    }

    @Override
    public boolean isCurrentStreamFinal() {
        return inner.isCurrentStreamFinal();
    }

    @Override
    public void maybeThrowStreamError() throws IOException {
        inner.maybeThrowStreamError();
    }

    @Override
    public void resetPosition(long positionUs, boolean sampleStreamIsResetToKeyFrame)
            throws ExoPlaybackException {
        inner.resetPosition(positionUs, sampleStreamIsResetToKeyFrame);
    }

    @Override
    public boolean supportsResetPositionWithoutKeyFrameReset(long positionUs) {
        return inner.supportsResetPositionWithoutKeyFrameReset(positionUs);
    }

    @Override
    public void setPlaybackSpeed(float currentPlaybackSpeed, float targetPlaybackSpeed)
            throws ExoPlaybackException {
        inner.setPlaybackSpeed(currentPlaybackSpeed, targetPlaybackSpeed);
    }

    @Override
    public void enableMayRenderStartOfStream() {
        inner.enableMayRenderStartOfStream();
    }

    @Override
    public void setTimeline(Timeline timeline) {
        inner.setTimeline(timeline);
    }

    @Override
    public void render(long positionUs, long elapsedRealtimeUs) throws ExoPlaybackException {
        inner.render(positionUs, elapsedRealtimeUs);
    }

    @Override
    public boolean isReady() {
        return inner.isReady();
    }

    @Override
    public boolean isEnded() {
        return inner.isEnded();
    }

    @Override
    public void stop() {
        inner.stop();
    }

    @Override
    public void disable() {
        inner.disable();
    }

    @Override
    public void reset() {
        inner.reset();
    }

    @Override
    public void release() {
        inner.release();
    }

    @Override
    public void handleMessage(int messageType, Object message) throws ExoPlaybackException {
        inner.handleMessage(messageType, message);
    }
}
