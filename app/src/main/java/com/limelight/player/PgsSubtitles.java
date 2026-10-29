package com.limelight.player;

import androidx.media3.common.text.Cue;
import androidx.media3.extractor.text.SubtitleParser;
import androidx.media3.extractor.text.pgs.PgsParser;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Blu-ray PGS subtitles: assembles one stream's PES packets into display sets and parses them
 * with Media3's PgsParser. An empty cue list clears. Not thread safe.
 */
final class PgsSubtitles {

    /** A parsed display set and its presentation time. */
    static final class Event {
        final long pts90;
        final List<Cue> cues;

        Event(long pts90, List<Cue> cues) {
            this.pts90 = pts90;
            this.cues = cues;
        }
    }

    interface Output {
        void onEvent(Event event);
    }

    private static final int SEGMENT_END = 0x80;

    private final Output output;
    private final PgsParser parser = new PgsParser();
    private final ByteArrayOutputStream set = new ByteArrayOutputStream();
    private long setPts90 = -1;

    PgsSubtitles(Output output) {
        this.output = output;
    }

    /** Appends one PES payload: segments of type, 16-bit length, data. */
    void feed(long pts90, byte[] payload) {
        if (set.size() == 0) {
            setPts90 = pts90;
        }
        set.write(payload, 0, payload.length);
        if (endsSet(payload)) {
            byte[] data = set.toByteArray();
            set.reset();
            final long at = setPts90;
            parser.parse(data, 0, data.length, SubtitleParser.OutputOptions.allCues(),
                    timing -> output.onEvent(new Event(at, new ArrayList<>(timing.cues))));
        }
    }

    /** Drops any partial display set, for seeks. */
    void reset() {
        set.reset();
        setPts90 = -1;
    }

    private static boolean endsSet(byte[] d) {
        int pos = 0;
        boolean end = false;
        while (pos + 3 <= d.length) {
            int type = d[pos] & 0xFF;
            int len = ((d[pos + 1] & 0xFF) << 8) | (d[pos + 2] & 0xFF);
            if (type == SEGMENT_END) {
                end = true;
            }
            pos += 3 + len;
        }
        return end;
    }
}
