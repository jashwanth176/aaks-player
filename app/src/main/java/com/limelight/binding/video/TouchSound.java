package com.limelight.binding.video;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;

import com.limelight.LimeLog;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * A fingertip has no haptics, so a touch is heard instead: a soft click on contact and a
 * lighter tick on release, made here rather than shipped as sound files.
 */
final class TouchSound {

    private static final int RATE = 48000;

    private final ExecutorService player = Executors.newSingleThreadExecutor(
            r -> new Thread(r, "Video - XR Touch sound"));
    private final AudioTrack press;
    private final AudioTrack release;
    private boolean played;

    TouchSound() {
        press = track(click(0.04f, new float[] { 1700.0f, 3100.0f }, 0.006f, 0.45f));
        release = track(click(0.025f, new float[] { 2600.0f }, 0.0035f, 0.22f));
    }

    // Decaying tones with a half millisecond rise, so the start does not pop
    static short[] click(float seconds, float[] tones, float decay, float gain) {
        short[] pcm = new short[Math.round(seconds * RATE)];
        for (int i = 0; i < pcm.length; i++) {
            float t = i / (float) RATE;
            float s = 0.0f;
            for (float tone : tones) {
                s += (float) Math.sin(2.0 * Math.PI * tone * t);
            }
            float envelope = Math.min(1.0f, t / 0.0005f) * (float) Math.exp(-t / decay);
            pcm[i] = (short) Math.round(s / tones.length * envelope * gain * Short.MAX_VALUE);
        }
        return pcm;
    }

    private static AudioTrack track(short[] pcm) {
        try {
            AudioTrack track = new AudioTrack.Builder()
                    // Game, not UI sounds: it then follows the media volume, which the
                    // headset's system sound volume may have turned down
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(RATE)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                            .build())
                    .setBufferSizeInBytes(pcm.length * 2)
                    .setTransferMode(AudioTrack.MODE_STATIC)
                    .build();
            track.write(pcm, 0, pcm.length);
            return track;
        }
        catch (RuntimeException e) {
            LimeLog.warning("Touch sound unavailable: " + e);
            return null;
        }
    }

    void press() {
        play(press);
    }

    void release() {
        play(release);
    }

    private void play(AudioTrack track) {
        if (track == null || player.isShutdown()) {
            return;
        }
        player.execute(() -> {
            try {
                track.stop();
                track.reloadStaticData();
                track.play();
                if (!played) {
                    played = true;
                    LimeLog.info("Touch sound playing, state " + track.getPlayState());
                }
            }
            catch (IllegalStateException e) {
                // Released meanwhile
            }
        });
    }

    // A failed start cleans up twice
    synchronized void close() {
        if (player.isShutdown()) {
            return;
        }
        player.execute(() -> {
            if (press != null) {
                press.release();
            }
            if (release != null) {
                release.release();
            }
        });
        player.shutdown();
    }
}
