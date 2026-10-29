package com.limelight.utils;

import android.content.Context;
import android.os.Build;
import android.preference.PreferenceManager;

import com.limelight.BuildConfig;
import com.limelight.FileLog;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.RandomAccessFile;
import java.io.Writer;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Bug reports: the user's note, app and device details, key settings and both
 * log files. Posted as JSON to the build's collector; if that fails or none is
 * configured, saved as a text file beside the log (newest few kept).
 */
public final class BugReport {

    /** How a report went. */
    public enum Result {
        /** Accepted by the collector; nothing saved locally */
        SENT,
        /** Collector unreachable or rejected it; saved */
        NOT_SENT,
        /** Collector rate or size limit hit; saved */
        BUSY,
        /** Build has no collector; saved */
        NO_COLLECTOR,
        /** Neither sent nor saved */
        NOT_WRITTEN
    }

    /** Result, saved path and failure detail. */
    public static final class Outcome {
        public final Result result;
        /** Saved path, or null */
        public final String path;
        /** Failure detail, or null */
        public final String detail;

        Outcome(Result result, String path, String detail) {
            this.result = result;
            this.path = path;
            this.detail = detail;
        }
    }

    /** App, device and settings details for a report. */
    public static final class Details {
        public String version = "";
        public String packageName = "";
        /** Build commit, or empty */
        public String commit = "";
        public boolean root;
        public boolean debug;
        public String manufacturer = "";
        public String brand = "";
        public String model = "";
        public String device = "";
        public String androidRelease = "";
        public int sdk;
        public String buildDisplay = "";
        public boolean headset;
        /** OpenXR runtime of the last session, or null */
        public String runtime;
        /** Failure block from the last failed VR start, or null */
        public String startFailure;
        /** Settings lines, each newline-terminated */
        public String settings = "";
        /** Current session state, or null outside a session */
        public String session;
    }

    /** Posts a report to the collector; replaceable in tests. */
    public interface Transport {
        /** Returns the HTTP status code. */
        int post(String url, Map<String, String> headers, byte[] body) throws IOException;
    }

    /** Reply address, remembered between reports. */
    public static final String EMAIL_PREF = "bug_report_email";
    /** Runtime of the last session, as logged. */
    public static final String RUNTIME_PREF = "xr_last_runtime";
    /** Block logged by a failed VR start; cleared on success. */
    public static final String START_FAILURE_PREF = "xr_start_failure";

    /** First line of every report; the collector rejects summaries without it. */
    static final String HEADER = "AaksVR bug report\n";

    // Collector limits: note and summary in chars, logs and body in UTF-8 bytes.
    static final int NOTE_MAX = 4000;
    static final int SUMMARY_MAX = 8000;
    static final int LOG_MAX_BYTES = 6 * 1024 * 1024;
    static final int BODY_MAX_BYTES = 8 * 1024 * 1024;

    private static final String NAME_PREFIX = "aaks-report-";
    private static final String NAME_SUFFIX = ".txt";
    /** Number of newest reports kept on the device. */
    static final int KEEP_REPORTS = 5;
    private static final int CONNECT_TIMEOUT_MS = 15000;
    private static final int READ_TIMEOUT_MS = 30000;
    // Shared storage mount point, hidden by the headset's file manager.
    private static final String STORAGE_ROOT = "/storage/emulated/0/";

    private BugReport() {
    }

    /** Whether this build has a collector. */
    public static boolean collectorConfigured() {
        return !collectorUrl().isEmpty();
    }

    /** Collector URL, or empty if none. */
    public static String collectorUrl() {
        return usableUrl(BuildConfig.REPORT_URL);
    }

    /** The URL if it is https, else empty: reports carry logs, so never send in clear. */
    static String usableUrl(String url) {
        String trimmed = url == null ? "" : url.trim();
        return trimmed.toLowerCase(Locale.ROOT).startsWith("https://") ? trimmed : "";
    }

    public static boolean hasNote(String note) {
        return note != null && !note.trim().isEmpty();
    }

    /** Loose sanity check (one @, a dot in the domain, no spaces); not full validation. */
    public static boolean looksLikeEmail(String address) {
        String text = address == null ? "" : address.trim();
        int at = text.indexOf('@');
        if (at <= 0 || at != text.lastIndexOf('@') || text.matches(".*\\s.*")) {
            return false;
        }
        String host = text.substring(at + 1);
        return host.indexOf('.') > 0 && host.lastIndexOf('.') < host.length() - 1;
    }

    /** Blank, or looks like an email address. */
    public static boolean addressOk(String address) {
        return address == null || address.trim().isEmpty() || looksLikeEmail(address);
    }

    /** A non-blank note and an acceptable address. */
    public static boolean canSend(String note, String address) {
        return hasNote(note) && addressOk(address);
    }

    /** The report text before the logs: the collector's summary and the top of a saved report. */
    public static String compose(String message, String email, Details d) {
        String note = message == null ? "" : message.trim();
        StringBuilder text = new StringBuilder();
        text.append(HEADER);
        text.append("Version: ").append(d.version).append('\n');
        text.append("From: ").append(email == null ? "" : email.trim()).append("\n\n");
        text.append(note.isEmpty() ? "(no message)" : note).append('\n');

        // First: when VR never starts this is the key information.
        if (d.startFailure != null && !d.startFailure.trim().isEmpty()) {
            text.append("\n----- last VR start -----\n");
            text.append(d.startFailure.trim()).append('\n');
        }

        text.append("\n----- app and device -----\n");
        text.append("aaks ").append(d.version).append(' ').append(d.packageName)
                .append(d.root ? " root" : "").append(d.debug ? " debug" : "");
        if (d.commit != null && !d.commit.isEmpty()) {
            text.append(" commit ").append(d.commit);
        }
        text.append('\n');
        text.append("device ").append(d.manufacturer).append(' ').append(d.brand).append(' ')
                .append(d.model).append(" (").append(d.device).append(")\n");
        text.append("android ").append(d.androidRelease).append(" sdk ").append(d.sdk)
                .append(" build ").append(d.buildDisplay).append('\n');
        text.append("headset ").append(d.headset).append('\n');
        if (d.runtime != null && !d.runtime.isEmpty()) {
            text.append("runtime ").append(d.runtime).append('\n');
        }

        text.append("\n----- settings -----\n");
        text.append(d.settings);
        if (d.session != null && !d.session.isEmpty()) {
            text.append("session ").append(d.session).append('\n');
        }
        return text.toString();
    }

    /** JSON content type and the bearer token; the token never goes in the body. */
    public static Map<String, String> headers(String token) {
        Map<String, String> headers = new LinkedHashMap<>();
        headers.put("Content-Type", "application/json");
        if (token != null && !token.isEmpty()) {
            headers.put("Authorization", "Bearer " + token);
        }
        return headers;
    }

    /** File name for a report made at that time. */
    static String reportName(Date when) {
        return NAME_PREFIX + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(when)
                + NAME_SUFFIX;
    }

    /** Writes the header then each existing log, prunes to KEEP_REPORTS, and returns the file. */
    public static File write(File dir, String name, String header, File... logs)
            throws IOException {
        if (dir == null || (!dir.isDirectory() && !dir.mkdirs())) {
            throw new IOException("no writable storage for the report");
        }
        File report = new File(dir, name);
        Writer out = new OutputStreamWriter(new FileOutputStream(report), StandardCharsets.UTF_8);
        try {
            out.write(header);
            for (File log : logs) {
                appendLog(out, log);
            }
        } finally {
            out.close();
        }
        prune(dir, KEEP_REPORTS);
        return report;
    }

    /** Whether a file name is a report rather than a log in the same folder. */
    public static boolean isReportName(String name) {
        return name != null && name.startsWith(NAME_PREFIX) && name.endsWith(NAME_SUFFIX);
    }

    /** Deletes all but the newest keep reports (names sort by time) and returns the count. */
    static int prune(File dir, int keep) {
        File[] files = dir != null ? dir.listFiles() : null;
        if (files == null) {
            return 0;
        }
        List<String> names = new ArrayList<>();
        for (File file : files) {
            if (file.isFile() && isReportName(file.getName())) {
                names.add(file.getName());
            }
        }
        Collections.sort(names);
        int deleted = 0;
        for (int i = 0; i < names.size() - keep; i++) {
            if (new File(dir, names.get(i)).delete()) {
                deleted++;
            }
        }
        return deleted;
    }

    private static String logTitle(File log) {
        return "\n----- " + log.getName() + " -----\n";
    }

    private static void appendLog(Writer out, File log) throws IOException {
        if (log == null || !log.isFile()) {
            return;
        }
        out.write(logTitle(log));
        BufferedReader in = new BufferedReader(new InputStreamReader(
                new FileInputStream(log), StandardCharsets.UTF_8));
        try {
            char[] chunk = new char[16384];
            int read;
            while ((read = in.read(chunk)) > 0) {
                out.write(chunk, 0, read);
            }
        } finally {
            in.close();
        }
    }

    /**
     * The logs, oldest first, each under a title line, in at most max UTF-8 bytes.
     * Over the limit the oldest data is dropped at a line boundary and a leading
     * line says how much. Sizes are taken up front since the current log is still
     * being written.
     */
    static String logText(File[] logs, int max) throws IOException {
        List<File> present = new ArrayList<>();
        List<Long> sizes = new ArrayList<>();
        long total = 0;
        for (File log : logs) {
            if (log != null && log.isFile()) {
                present.add(log);
                sizes.add(log.length());
                total += utf8(logTitle(log)).length + log.length();
            }
        }

        // Room for the "cut" line
        final int cutLineRoom = 160;
        long drop = total <= max ? 0 : total - (max - cutLineRoom);
        long leftOut = 0;
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < present.size(); i++) {
            File log = present.get(i);
            long size = sizes.get(i);
            String title = logTitle(log);
            long titleBytes = utf8(title).length;
            if (drop >= titleBytes + size) {
                drop -= titleBytes + size;
                leftOut += size;
                continue;
            }
            long from = Math.max(0, drop - titleBytes);
            drop = 0;
            byte[] bytes = readRange(log, from, size);
            int start = 0;
            if (from > 0) {
                start = lineStart(bytes);
                leftOut += from + start;
            }
            text.append(title).append(new String(bytes, start, bytes.length - start,
                    StandardCharsets.UTF_8));
        }
        if (leftOut > 0) {
            String limit = max % (1024 * 1024) == 0 ? (max / (1024 * 1024)) + " MB" : max + " bytes";
            text.insert(0, "----- cut to the newest " + limit + " of the logs, the oldest "
                    + leftOut + " bytes left out -----\n");
        }
        return text.toString();
    }

    // Start of the first whole line, or failing that the first UTF-8 lead byte.
    private static int lineStart(byte[] bytes) {
        for (int i = 0; i < bytes.length; i++) {
            if (bytes[i] == '\n') {
                return i + 1;
            }
        }
        int i = 0;
        while (i < bytes.length && (bytes[i] & 0xC0) == 0x80) {
            i++;
        }
        return i;
    }

    private static byte[] readRange(File file, long from, long to) throws IOException {
        RandomAccessFile in = new RandomAccessFile(file, "r");
        try {
            long end = Math.min(to, in.length());
            int length = (int) Math.max(0, end - from);
            byte[] bytes = new byte[length];
            in.seek(from);
            in.readFully(bytes);
            return bytes;
        } finally {
            in.close();
        }
    }

    private static byte[] utf8(String text) {
        return text.getBytes(StandardCharsets.UTF_8);
    }

    /** 429 or 413: a collector limit rather than a fault. */
    static boolean busy(int code) {
        return code == 429 || code == 413;
    }

    /** At most max chars, never splitting a surrogate pair. */
    static String clip(String text, int max) {
        if (text.length() <= max) {
            return text;
        }
        int end = Character.isHighSurrogate(text.charAt(max - 1)) ? max - 1 : max;
        return text.substring(0, end);
    }

    /**
     * The JSON body in UTF-8. If escaping pushes it past BODY_MAX_BYTES, more of the
     * oldest log is dropped until it fits.
     */
    static byte[] body(String note, String replyTo, String summary, String log, String fileName) {
        String reply = replyTo == null || replyTo.trim().isEmpty() ? null : replyTo.trim();
        String kept = log;
        for (;;) {
            StringBuilder json = new StringBuilder(kept.length() + 16384);
            json.append("{\"note\":");
            quote(json, clip(note == null ? "" : note.trim(), NOTE_MAX));
            json.append(",\"replyTo\":");
            if (reply == null) {
                json.append("null");
            }
            else {
                quote(json, reply);
            }
            json.append(",\"summary\":");
            quote(json, clip(summary, SUMMARY_MAX));
            json.append(",\"log\":");
            quote(json, kept);
            json.append(",\"fileName\":");
            quote(json, fileName);
            json.append('}');
            byte[] bytes = utf8(json.toString());
            if (bytes.length <= BODY_MAX_BYTES || kept.isEmpty()) {
                return bytes;
            }
            // Each char dropped saves at least one byte.
            int cut = Math.min(kept.length(), bytes.length - BODY_MAX_BYTES + 65536);
            int next = kept.indexOf('\n', cut);
            kept = "----- cut further to fit the collector -----\n"
                    + (next < 0 ? "" : kept.substring(next + 1));
        }
    }

    // JSON string escaping, plus U+2028/U+2029 which break JavaScript.
    private static void quote(StringBuilder out, String text) {
        out.append('"');
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if (c < 0x20 || c == '\u2028' || c == '\u2029') {
                        out.append(String.format(Locale.ROOT, "\\u%04x", (int) c));
                    }
                    else {
                        out.append(c);
                    }
                    break;
            }
        }
        out.append('"');
    }

    /** Posts to the collector, saving into dir if that fails or there is none. No Android deps. */
    static Outcome sendOrSave(File dir, String name, String summary, String note, String email,
                              File[] logs, String url, String token, Transport transport) {
        String target = usableUrl(url);
        if (target.isEmpty()) {
            return save(Result.NO_COLLECTOR, null, dir, name, summary, logs);
        }
        Result failed;
        String detail;
        try {
            byte[] body = body(note, email, summary, logText(logs, LOG_MAX_BYTES), name);
            int code = transport.post(target, headers(token), body);
            if (code / 100 == 2) {
                return new Outcome(Result.SENT, null, null);
            }
            failed = busy(code) ? Result.BUSY : Result.NOT_SENT;
            detail = "server answered " + code;
        } catch (IOException e) {
            failed = Result.NOT_SENT;
            detail = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
        }
        return save(failed, detail, dir, name, summary, logs);
    }

    // Fallback: the full report with uncut logs.
    private static Outcome save(Result result, String detail, File dir, String name,
                                String summary, File[] logs) {
        try {
            File report = write(dir, name, summary, logs);
            return new Outcome(result, report.getAbsolutePath(), detail);
        } catch (IOException e) {
            String why = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
            return new Outcome(Result.NOT_WRITTEN, null, detail != null ? detail + "; " + why : why);
        }
    }

    /** Beside the log (visible in the file manager), or the app's folder when logging is off. */
    public static File reportDir(Context context) {
        File besideLog = logDir();
        return besideLog != null ? besideLog : context.getExternalFilesDir("reports");
    }

    /** Sends or saves a full report. Blocks on network: not on the main thread or frame loop. */
    public static Outcome send(Context context, String message, String email, String session) {
        // Flush pending log lines so the report includes them.
        FileLog.flush();
        String summary = compose(message, email, gather(context, session));
        return sendOrSave(reportDir(context), reportName(new Date()), summary, message, email,
                logFiles(), collectorUrl(), BuildConfig.REPORT_TOKEN, HTTP);
    }

    /** A path relative to shared storage, as the file manager shows it. */
    public static String shortPath(String path) {
        if (path == null) {
            return null;
        }
        return path.startsWith(STORAGE_ROOT) ? path.substring(STORAGE_ROOT.length()) : path;
    }

    private static File[] logFiles() {
        String current = FileLog.getLogPath();
        return new File[] { FileLog.getPreviousLogFile(),
                current != null ? new File(current) : null };
    }

    private static File logDir() {
        String current = FileLog.getLogPath();
        return current != null ? new File(current).getParentFile() : null;
    }

    /** Collects app, device and settings details. */
    public static Details gather(Context context, String session) {
        Details d = new Details();
        d.version = BuildConfig.VERSION_NAME;
        d.packageName = context.getPackageName();
        d.commit = BuildConfig.GIT_HASH;
        d.root = BuildConfig.ROOT_BUILD;
        d.debug = BuildConfig.DEBUG;
        d.manufacturer = Build.MANUFACTURER;
        d.brand = Build.BRAND;
        d.model = Build.MODEL;
        d.device = Build.DEVICE;
        d.androidRelease = Build.VERSION.RELEASE;
        d.sdk = Build.VERSION.SDK_INT;
        d.buildDisplay = Build.DISPLAY;
        d.headset = PreferenceConfiguration.isHeadset(context);
        d.runtime = PreferenceManager.getDefaultSharedPreferences(context)
                .getString(RUNTIME_PREF, null);
        d.startFailure = PreferenceManager.getDefaultSharedPreferences(context)
                .getString(START_FAILURE_PREF, null);
        d.settings = settingsLines(PreferenceConfiguration.readPreferences(context));
        d.session = session;
        return d;
    }

    private static String settingsLines(PreferenceConfiguration prefs) {
        return "virtualSurround " + prefs.vrVirtualSurround + "\n"
                + "depthMode " + prefs.vrDepthMode
                + " stereo3d " + PreferenceConfiguration.stereoAtStartLabel(prefs.vrDepthMode)
                + " depthModel " + prefs.vrDepthModel
                + " depthRate " + prefs.vrDepthRate + " separation " + prefs.vrStereoSeparation
                + " convergence " + prefs.vrConvergence
                + " defaultPair " + PreferenceConfiguration.defaultPairLabel(prefs.vrDepthModel)
                + " preset " + PreferenceConfiguration.presetLabel(prefs.vrStereoSeparation,
                        prefs.vrDepthModel)
                + " envRes " + prefs.vrEnvResTier
                + " sharpening " + prefs.vrSharpening + " supersampling " + prefs.vrSupersampling
                + " passthrough " + prefs.vrPassthrough
                + " environment " + prefs.vrEnvironmentId
                + " " + (prefs.vrRoomLevels == null ? "room none"
                        : prefs.vrRoomLevels.describe(" "))
                + " hands " + prefs.vrHandTracking + " gaze " + prefs.vrGaze
                + " " + PreferenceConfiguration.inputLabel(prefs.vrPointerSleep,
                        prefs.vrShowRay, prefs.vrControllerModel, " ")
                + " " + PreferenceConfiguration.headAimLabel(prefs.vrHeadAim,
                        prefs.vrHeadAimSensitivity, prefs.vrHeadAimDeadZone, " ")
                + " " + PreferenceConfiguration.gamepadToggleLabel(prefs.vrGamepadToggle, " ")
                + " clickSound " + prefs.vrClickSound + " doffGrace " + prefs.vrDoffGrace
                + " " + PreferenceConfiguration.pictureLabel(prefs.vrPicture, " ")
                + " headLocked " + prefs.vrHeadLocked + " ambilight " + prefs.vrAmbilight + "\n"
                + "fileLog " + prefs.fileLogLevel + "\n";
    }

    /** HTTP transport with timeouts sized for a few MB over headset Wi-Fi. */
    public static final Transport HTTP = new Transport() {
        @Override
        public int post(String url, Map<String, String> headers, byte[] body) throws IOException {
            HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
            try {
                conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
                conn.setReadTimeout(READ_TIMEOUT_MS);
                conn.setRequestMethod("POST");
                conn.setDoOutput(true);
                conn.setFixedLengthStreamingMode(body.length);
                for (Map.Entry<String, String> header : headers.entrySet()) {
                    conn.setRequestProperty(header.getKey(), header.getValue());
                }
                OutputStream out = conn.getOutputStream();
                try {
                    out.write(body);
                } finally {
                    out.close();
                }
                return conn.getResponseCode();
            } finally {
                conn.disconnect();
            }
        }
    };
}
