package com.limelight.player;

import java.util.ArrayList;
import java.util.List;

/**
 * Splits a disc PES audio payload into frames with their start times, since hardware decoders
 * expect one frame per buffer. Covers the formats the device decodes itself: AC-3 and E-AC-3.
 */
final class AudioFrames {

    static final class Frame {
        final int offset;
        final int length;
        final long pts90;

        Frame(int offset, int length, long pts90) {
            this.offset = offset;
            this.length = length;
            this.pts90 = pts90;
        }
    }

    private AudioFrames() {
    }

    // Dolby Digital frame sizes in 16 bit words at 48 kHz, by frame size code / 2
    private static final int[] AC3_WORDS_48K = {
        64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024, 1152, 1280
    };
    private static final int AC3_SAMPLES = 1536;
    private static final int[] EAC3_BLOCKS = { 1, 2, 3, 6 };

    /** AC-3: 1536 samples per frame. */
    static List<Frame> ac3(byte[] d, long pts90) {
        List<Frame> out = new ArrayList<>();
        int pos = 0;
        long at = pts90;
        while (pos + 6 <= d.length) {
            if ((d[pos] & 0xFF) != 0x0B || (d[pos + 1] & 0xFF) != 0x77) {
                pos++;
                continue;
            }
            int fscod = (d[pos + 4] >> 6) & 3;
            int frmsizecod = d[pos + 4] & 0x3F;
            int words = fscod == 0 && (frmsizecod >> 1) < AC3_WORDS_48K.length ? AC3_WORDS_48K[frmsizecod >> 1] : 0;
            if (words == 0) {
                // Not 48 kHz (never on disc): pass the packet whole
                out.add(new Frame(0, d.length, pts90));
                return out;
            }
            int len = words * 2;
            if (pos + len > d.length) {
                break;
            }
            out.add(new Frame(pos, len, at));
            at += AC3_SAMPLES * 90000L / 48000L;
            pos += len;
        }
        return out;
    }

    /** E-AC-3: an independent frame plus its following dependent frames form one unit. */
    static List<Frame> eac3(byte[] d, long pts90) {
        List<Frame> out = new ArrayList<>();
        int pos = 0;
        long at = pts90;
        int unitStart = -1;
        int unitLen = 0;
        long unitPts = pts90;
        while (pos + 6 <= d.length) {
            if ((d[pos] & 0xFF) != 0x0B || (d[pos + 1] & 0xFF) != 0x77) {
                pos++;
                continue;
            }
            int strmtyp = (d[pos + 2] >> 6) & 3;
            int len = ((((d[pos + 2] & 7) << 8) | (d[pos + 3] & 0xFF)) + 1) * 2;
            if (len < 8 || pos + len > d.length) {
                break;
            }
            if (strmtyp != 1) {
                // New independent frame closes the previous unit
                if (unitStart >= 0) {
                    out.add(new Frame(unitStart, unitLen, unitPts));
                }
                int fscod = (d[pos + 4] >> 6) & 3;
                int blocks = fscod == 3 ? 6 : EAC3_BLOCKS[(d[pos + 4] >> 4) & 3];
                unitStart = pos;
                unitLen = len;
                unitPts = at;
                at += blocks * 256L * 90000L / 48000L;
            }
            else if (unitStart >= 0) {
                unitLen += len;
            }
            pos += len;
        }
        if (unitStart >= 0) {
            out.add(new Frame(unitStart, unitLen, unitPts));
        }
        return out;
    }
}
