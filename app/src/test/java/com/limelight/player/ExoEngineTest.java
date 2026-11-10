package com.limelight.player;

import static org.junit.Assert.assertEquals;

import androidx.media3.common.PlaybackException;

import org.junit.Test;

public class ExoEngineTest {

    @Test
    public void reasonsFollowTheErrorBlocks() {
        assertEquals("The file is no longer there.",
                ExoEngine.reasonFor(PlaybackException.ERROR_CODE_IO_FILE_NOT_FOUND));
        assertEquals("The file could not be read.",
                ExoEngine.reasonFor(PlaybackException.ERROR_CODE_IO_UNSPECIFIED));
        // What a film copied only part way gives
        assertEquals("The file is damaged or incomplete.",
                ExoEngine.reasonFor(PlaybackException.ERROR_CODE_PARSING_CONTAINER_UNSUPPORTED));
        assertEquals("This headset can't decode its format.",
                ExoEngine.reasonFor(PlaybackException.ERROR_CODE_DECODING_FORMAT_UNSUPPORTED));
        assertEquals("The sound could not start.",
                ExoEngine.reasonFor(PlaybackException.ERROR_CODE_AUDIO_TRACK_INIT_FAILED));
        assertEquals("Playback stopped unexpectedly.",
                ExoEngine.reasonFor(PlaybackException.ERROR_CODE_UNSPECIFIED));
    }
}
