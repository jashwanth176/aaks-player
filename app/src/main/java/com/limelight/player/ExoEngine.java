package com.limelight.player;

import android.content.Context;
import android.graphics.Bitmap;
import android.media.MediaFormat;
import android.net.Uri;
import android.text.SpannableStringBuilder;
import android.view.Surface;

import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MediaItem;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.common.TrackSelectionOverride;
import androidx.media3.common.TrackSelectionParameters;
import androidx.media3.common.Tracks;
import androidx.media3.common.VideoSize;
import androidx.media3.common.text.Cue;
import androidx.media3.common.text.CueGroup;
import androidx.media3.exoplayer.ExoPlayer;

import com.limelight.LimeLog;
import com.limelight.binding.video.XrShared;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Plays a video file with ExoPlayer. */
final class ExoEngine implements PlaybackEngine {

    private final Context context;
    private final Uri uri;
    private final Listener listener;
    private ExoPlayer player;
    // Positive = subtitles later
    private volatile long subtitleDelayUs;

    ExoEngine(Context context, Uri uri, Listener listener) {
        this.context = context;
        this.uri = uri;
        this.listener = listener;
    }

    @Override
    public void start(Surface surface, long startMs, boolean play) {
        player = new ExoPlayer.Builder(context, new FfAudioRenderer.Factory(context, () -> subtitleDelayUs)).build();
        player.setVideoSurface(surface);
        // Handle audio focus: pause on loss, resume on regain
        player.setAudioAttributes(new androidx.media3.common.AudioAttributes.Builder()
                .setUsage(C.USAGE_MEDIA).setContentType(C.AUDIO_CONTENT_TYPE_MOVIE).build(), true);
        player.setTrackSelectionParameters(player.getTrackSelectionParameters().buildUpon()
                .setPreferredAudioLanguages("eng", "en").build());
        player.addListener(new Player.Listener() {
            @Override
            public void onVideoSizeChanged(VideoSize size) {
                LimeLog.info("PlayerXR: decoding " + size.width + "x" + size.height);
                listener.onVideoSize(size.width, size.height);
            }

            @Override
            public void onPlayerError(PlaybackException error) {
                LimeLog.severe("PlayerXR: " + error.getErrorCodeName() + " " + error.getMessage());
                listener.onError("Can't play this file", error.getErrorCodeName());
            }

            @Override
            public void onCues(CueGroup cues) {
                showCues(cues);
            }
        });
        player.setVideoFrameMetadataListener(
                (presentationTimeUs, releaseTimeNs, format, mediaFormat) -> noteColour(format, mediaFormat));
        MediaItem.Builder item = new MediaItem.Builder().setUri(uri);
        item.setSubtitleConfigurations(subtitlesBeside(uri));
        player.setMediaItem(item.build(), startMs);
        if (speed != 1.0f) {
            player.setPlaybackSpeed(speed);
        }
        player.prepare();
        player.setPlayWhenReady(play);
    }

    // ---- colour -------------------------------------------------------------------------

    // Last reported transfer, -1 before the first frame. Playback thread only.
    private int toldTransfer = -1;

    // Called per frame. Prefers the decoder output format over the track format; reports
    // only on change.
    private void noteColour(Format format, MediaFormat out) {
        int coded = Format.NO_VALUE;
        if (out != null && out.containsKey(MediaFormat.KEY_COLOR_TRANSFER)) {
            coded = out.getInteger(MediaFormat.KEY_COLOR_TRANSFER);
        }
        else if (format != null && format.colorInfo != null) {
            coded = format.colorInfo.colorTransfer;
        }
        int transfer = coded == C.COLOR_TRANSFER_ST2084 ? XrShared.VIDEO_TRANSFER_CODE_PQ
                : coded == C.COLOR_TRANSFER_HLG ? XrShared.VIDEO_TRANSFER_CODE_HLG
                : XrShared.VIDEO_TRANSFER_CODE_SDR;
        if (transfer == toldTransfer) {
            return;
        }
        toldTransfer = transfer;

        // CTA-861.3 static metadata, little endian: mastering peak at 17, MaxCLL at 21, in nits
        byte[] info = null;
        if (out != null && out.containsKey(MediaFormat.KEY_HDR_STATIC_INFO)) {
            ByteBuffer b = out.getByteBuffer(MediaFormat.KEY_HDR_STATIC_INFO);
            if (b != null) {
                info = new byte[b.remaining()];
                b.duplicate().get(info);
            }
        }
        if (info == null && format != null && format.colorInfo != null) {
            info = format.colorInfo.hdrStaticInfo;
        }
        float mastering = 0, maxCll = 0;
        if (info != null && info.length >= 25) {
            ByteBuffer b = ByteBuffer.wrap(info).order(ByteOrder.LITTLE_ENDIAN);
            mastering = b.getShort(17) & 0xffff;
            maxCll = b.getShort(21) & 0xffff;
        }
        LimeLog.info("PlayerXR: picture is " + (transfer == XrShared.VIDEO_TRANSFER_CODE_PQ ? "HDR10 (PQ)"
                : transfer == XrShared.VIDEO_TRANSFER_CODE_HLG ? "HLG" : "SDR")
                + (transfer != XrShared.VIDEO_TRANSFER_CODE_SDR
                        ? ", MaxCLL " + (int) maxCll + ", mastered to " + (int) mastering + " nits" : "")
                + (out != null && out.containsKey(MediaFormat.KEY_COLOR_TRANSFER) ? " (decoder)" : " (track)"));
        listener.onVideoColour(transfer, maxCll, mastering);
    }

    @Override
    public void setSubtitleDelayMs(long ms) {
        long us = ms * 1000L;
        if (us == subtitleDelayUs) {
            return;
        }
        subtitleDelayUs = us;
        // ShiftedRenderer reads the shift on enable, so toggle the text track to apply it
        ExoPlayer p = player;
        if (p == null) {
            return;
        }
        TrackSelectionParameters now = p.getTrackSelectionParameters();
        if (now.disabledTrackTypes.contains(C.TRACK_TYPE_TEXT)) {
            return;
        }
        p.setTrackSelectionParameters(now.buildUpon().setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true).build());
        p.setTrackSelectionParameters(now);
        listener.onSubtitle(null, null, 0.0f);
    }

    // Applied in start() if set before the player exists
    private float speed = 1.0f;

    @Override
    public void setSpeed(float value) {
        speed = value;
        if (player != null) {
            player.setPlaybackSpeed(value);
        }
    }

    @Override
    public long positionMs() {
        return player != null ? player.getCurrentPosition() : 0;
    }

    @Override
    public long durationMs() {
        long d = player != null ? player.getDuration() : 0;
        return d > 0 ? d : 0;
    }

    @Override
    public boolean isPlaying() {
        return player != null && player.getPlayWhenReady();
    }

    @Override
    public void setPlaying(boolean playing) {
        if (player != null) {
            player.setPlayWhenReady(playing);
        }
    }

    @Override
    public void seekToMs(long ms) {
        if (player != null) {
            player.seekTo(ms);
        }
    }

    @Override
    public boolean isReady() {
        return player != null && player.getDuration() > 0 && player.getPlaybackState() != Player.STATE_IDLE;
    }

    @Override
    public void release() {
        if (player != null) {
            player.release();
            player = null;
        }
    }

    // ---- tracks and subtitles ------------------------------------------------------------

    // Sidecar subtitle files: same base name, optionally followed by a language tag
    private static List<MediaItem.SubtitleConfiguration> subtitlesBeside(Uri uri) {
        List<MediaItem.SubtitleConfiguration> out = new ArrayList<>();
        if (!"file".equals(uri.getScheme()) || uri.getPath() == null) {
            return out;
        }
        File film = new File(uri.getPath());
        File dir = film.getParentFile();
        String name = film.getName();
        int dot = name.lastIndexOf('.');
        String base = (dot > 0 ? name.substring(0, dot) : name).toLowerCase(Locale.ROOT);
        File[] files = dir == null ? null : dir.listFiles();
        if (files == null) {
            return out;
        }
        for (File f : files) {
            String n = f.getName().toLowerCase(Locale.ROOT);
            String mime = n.endsWith(".srt") ? MimeTypes.APPLICATION_SUBRIP
                    : (n.endsWith(".ass") || n.endsWith(".ssa")) ? MimeTypes.TEXT_SSA
                    : n.endsWith(".vtt") ? MimeTypes.TEXT_VTT : null;
            if (mime == null || !n.startsWith(base)) {
                continue;
            }
            // "Film.en.srt" -> "en"
            String middle = n.substring(base.length(), n.lastIndexOf('.')).replaceAll("^[._\\- ]+", "");
            MediaItem.SubtitleConfiguration.Builder sub = new MediaItem.SubtitleConfiguration.Builder(Uri.fromFile(f))
                    .setMimeType(mime)
                    .setLabel(f.getName());
            if (middle.matches("[a-z]{2,3}")) {
                sub.setLanguage(middle);
            }
            out.add(sub.build());
        }
        return out;
    }

    private void showCues(CueGroup group) {
        SpannableStringBuilder text = new SpannableStringBuilder();
        Bitmap image = null;
        float share = 0.0f;
        for (Cue cue : group.cues) {
            if (cue.text != null && cue.text.length() > 0) {
                if (text.length() > 0) {
                    text.append('\n');
                }
                text.append(cue.text);
            }
            else if (cue.bitmap != null && image == null) {
                image = cue.bitmap;
                share = cue.size != Cue.DIMEN_UNSET ? cue.size : 0.0f;
            }
        }
        listener.onSubtitle(text, image, share);
    }

    @Override
    public List<PlayerUi.TrackChoice> audioTracks() {
        return trackChoices(C.TRACK_TYPE_AUDIO);
    }

    @Override
    public List<PlayerUi.TrackChoice> subtitleTracks() {
        return trackChoices(C.TRACK_TYPE_TEXT);
    }

    private List<PlayerUi.TrackChoice> trackChoices(int type) {
        List<PlayerUi.TrackChoice> out = new ArrayList<>();
        ExoPlayer p = player;
        if (p == null) {
            return out;
        }
        List<PlayerUi.TrackChoice> found = new ArrayList<>();
        boolean anySelected = false;
        for (Tracks.Group group : p.getCurrentTracks().getGroups()) {
            if (group.getType() != type) {
                continue;
            }
            for (int i = 0; i < group.length; i++) {
                Format f = group.getTrackFormat(i);
                boolean selected = group.isTrackSelected(i);
                anySelected |= selected;
                int n = found.size() + 1;
                found.add(new PlayerUi.TrackChoice(trackName(f, n, type), trackDetail(f, type), selected,
                        group.isTrackSupported(i), new Object[] { group, i }));
            }
        }
        if (type == C.TRACK_TYPE_TEXT) {
            out.add(new PlayerUi.TrackChoice("Off", "", !anySelected, true, null));
        }
        out.addAll(found);
        return out;
    }

    @Override
    public void chooseAudio(PlayerUi.TrackChoice choice) {
        chooseTrack(C.TRACK_TYPE_AUDIO, choice);
    }

    @Override
    public void chooseSubtitle(PlayerUi.TrackChoice choice) {
        chooseTrack(C.TRACK_TYPE_TEXT, choice);
        if (choice.token == null) {
            listener.onSubtitle(null, null, 0.0f);
        }
    }

    private void chooseTrack(int type, PlayerUi.TrackChoice choice) {
        ExoPlayer p = player;
        if (p == null) {
            return;
        }
        TrackSelectionParameters.Builder b = p.getTrackSelectionParameters().buildUpon();
        if (choice.token == null) {
            b.setTrackTypeDisabled(type, true);
        }
        else {
            Object[] t = (Object[]) choice.token;
            Tracks.Group group = (Tracks.Group) t[0];
            b.setTrackTypeDisabled(type, false);
            b.setOverrideForType(new TrackSelectionOverride(group.getMediaTrackGroup(), (Integer) t[1]));
        }
        p.setTrackSelectionParameters(b.build());
    }

    static String trackName(Format f, int n, int type) {
        if (f.label != null && !f.label.isEmpty()) {
            return f.label;
        }
        String lang = f.language;
        if (lang != null && !lang.equals("und") && !lang.isEmpty()) {
            return languageName(lang);
        }
        return (type == C.TRACK_TYPE_AUDIO ? "Audio " : "Subtitles ") + n;
    }

    static String languageName(String lang) {
        String name = new Locale(lang).getDisplayLanguage(Locale.ENGLISH);
        if (name != null && !name.isEmpty() && !name.equalsIgnoreCase(lang)) {
            return Character.toUpperCase(name.charAt(0)) + name.substring(1);
        }
        return lang.toUpperCase(Locale.ROOT);
    }

    private static String trackDetail(Format f, int type) {
        String mime = f.sampleMimeType == null ? "" : f.sampleMimeType;
        List<String> parts = new ArrayList<>();
        if (type == C.TRACK_TYPE_AUDIO) {
            switch (mime) {
                case MimeTypes.AUDIO_E_AC3_JOC: parts.add("Dolby Atmos"); break;
                case MimeTypes.AUDIO_E_AC3: parts.add("Dolby Digital Plus"); break;
                case MimeTypes.AUDIO_AC3: parts.add("Dolby Digital"); break;
                case MimeTypes.AUDIO_TRUEHD: parts.add("Dolby TrueHD"); break;
                case MimeTypes.AUDIO_DTS: parts.add("DTS"); break;
                case MimeTypes.AUDIO_DTS_HD: parts.add("DTS-HD"); break;
                case MimeTypes.AUDIO_DTS_EXPRESS: parts.add("DTS Express"); break;
                case MimeTypes.AUDIO_AAC: parts.add("AAC"); break;
                case MimeTypes.AUDIO_FLAC: parts.add("FLAC"); break;
                case MimeTypes.AUDIO_OPUS: parts.add("Opus"); break;
                default: break;
            }
            if (f.channelCount > 0) {
                parts.add(f.channelCount == 2 ? "Stereo" : f.channelCount == 1 ? "Mono"
                        : f.channelCount == 6 ? "5.1" : f.channelCount == 8 ? "7.1" : f.channelCount + " ch");
            }
        }
        else {
            // Media3 parses subtitles during extraction; the original MIME type is in codecs
            if (MimeTypes.APPLICATION_MEDIA3_CUES.equals(mime) && f.codecs != null) {
                mime = f.codecs;
            }
            switch (mime) {
                case MimeTypes.APPLICATION_SUBRIP: parts.add("SRT"); break;
                case MimeTypes.TEXT_SSA: parts.add("ASS"); break;
                case MimeTypes.TEXT_VTT: parts.add("WebVTT"); break;
                case MimeTypes.APPLICATION_PGS: parts.add("PGS"); break;
                case MimeTypes.APPLICATION_VOBSUB: parts.add("VobSub"); break;
                default: break;
            }
            if ((f.selectionFlags & C.SELECTION_FLAG_FORCED) != 0) {
                parts.add("Forced");
            }
        }
        return android.text.TextUtils.join(" · ", parts);
    }
}
