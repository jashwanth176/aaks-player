package com.limelight.binding.video;

import com.limelight.R;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Why a VR session did not start, as the native start reported it: the step,
 * the failing call and its result, the runtime if an instance was made, and the
 * extensions offered and requested.
 */
public final class XrStartFailure {

    // Field order of the nativeTakeStartFailure array
    static final int FIELD_STEP = 0;
    static final int FIELD_CALL = 1;
    static final int FIELD_RESULT = 2;
    static final int FIELD_RESULT_NAME = 3;
    static final int FIELD_DETAIL = 4;
    static final int FIELD_RUNTIME = 5;
    static final int FIELD_OFFERED = 6;
    static final int FIELD_REQUESTED = 7;
    static final int FIELDS = 8;

    // Extensions the instance requires
    static final String[] REQUIRED = { "XR_KHR_opengl_es_enable", "XR_KHR_android_create_instance" };

    /**
     * Step it stopped at: loader, runtime, extensions, instance, system, graphics, egl, session,
     * space, swapchain, gl, timeout or unknown.
     */
    public final String step;
    /** The failing call, empty when not a single call. */
    public final String call;
    /** The XrResult, 0 if none. */
    public final int result;
    /** The XrResult's name, empty if none. */
    public final String resultName;
    public final String detail;
    /** Runtime name and version, or null when no instance was made. */
    public final String runtime;
    /** Extensions offered, empty for none, null if unknown. */
    public final String[] offered;
    /** Extensions requested, empty if it never got that far, null if unknown. */
    public final String[] requested;
    // Seconds waited for a start that never answered
    private final int waitedSeconds;

    XrStartFailure(String step, String call, int result, String resultName, String detail,
                   String runtime, String[] offered, String[] requested) {
        this(step, call, result, resultName, detail, runtime, offered, requested, 0);
    }

    private XrStartFailure(String step, String call, int result, String resultName,
                           String detail, String runtime, String[] offered, String[] requested,
                           int waitedSeconds) {
        this.step = step == null || step.isEmpty() ? "unknown" : step;
        this.call = call == null ? "" : call;
        this.result = result;
        this.resultName = resultName == null ? "" : resultName;
        this.detail = detail == null ? "" : detail;
        this.runtime = runtime == null || runtime.isEmpty() ? null : runtime;
        this.offered = offered;
        this.requested = requested;
        this.waitedSeconds = waitedSeconds;
    }

    /** From the native start's fields, or unknown() if they are unusable. */
    static XrStartFailure fromNative(String[] fields) {
        if (fields == null || fields.length < FIELDS) {
            return unknown();
        }
        int result = 0;
        try {
            result = Integer.parseInt(fields[FIELD_RESULT]);
        }
        catch (NumberFormatException e) {
            // Left at 0
        }
        return new XrStartFailure(fields[FIELD_STEP], fields[FIELD_CALL], result,
                fields[FIELD_RESULT_NAME], fields[FIELD_DETAIL], fields[FIELD_RUNTIME],
                names(fields[FIELD_OFFERED]), names(fields[FIELD_REQUESTED]));
    }

    /** A start that had not answered when the wait for it ran out. */
    static XrStartFailure timedOut(int seconds) {
        return new XrStartFailure("timeout", "", 0, "", "no answer within " + seconds + " s",
                null, null, null, seconds);
    }

    /** A start that failed without saying where. */
    static XrStartFailure unknown() {
        return new XrStartFailure("unknown", "", 0, "", "", null, null, null);
    }

    private static String[] names(String list) {
        if (list == null || list.trim().isEmpty()) {
            return new String[0];
        }
        return list.trim().split("\\s+");
    }

    /** Required extensions the runtime did not offer; empty if the offer is unknown. */
    List<String> missing() {
        List<String> out = new ArrayList<>();
        if (offered == null) {
            return out;
        }
        List<String> have = Arrays.asList(offered);
        for (String name : REQUIRED) {
            if (!have.contains(name)) {
                out.add(name);
            }
        }
        return out;
    }

    // The result name or code, else the detail, else the call
    private String answer() {
        if (!resultName.isEmpty()) {
            return resultName;
        }
        if (result != 0) {
            return Integer.toString(result);
        }
        if (!detail.isEmpty()) {
            return detail;
        }
        return call;
    }

    /** String resource for the one-line reason. */
    public int reasonRes() {
        switch (step) {
            case "loader":
            case "runtime":
                return R.string.vr_fail_no_runtime;
            case "extensions":
                return R.string.vr_fail_extensions;
            case "instance":
                return R.string.vr_fail_instance;
            case "system":
                return R.string.vr_fail_system;
            case "graphics":
                return R.string.vr_fail_graphics;
            case "egl":
                return R.string.vr_fail_egl;
            case "session":
                return R.string.vr_fail_session;
            case "space":
                return R.string.vr_fail_space;
            case "swapchain":
                return R.string.vr_fail_swapchain;
            case "gl":
                return R.string.vr_fail_gl;
            case "timeout":
                return R.string.vr_fail_timeout;
            default:
                return R.string.vr_fail_unknown;
        }
    }

    /** The argument for that string, or null if it takes none. */
    public String reasonArg() {
        switch (step) {
            case "extensions": {
                List<String> missing = missing();
                return missing.isEmpty() ? detail : join(", ", missing);
            }
            case "instance":
            case "system":
            case "graphics":
            case "session":
            case "space":
            case "swapchain":
                return answer();
            case "egl":
                return call.isEmpty() ? detail : call + ", " + detail;
            case "timeout":
                return Integer.toString(waitedSeconds);
            default:
                return null;
        }
    }

    /**
     * Everything known, as a heading and indented lines, for the log and reports.
     * when is a timestamp for a copy kept outside the log, or null.
     */
    public String block(String when) {
        StringBuilder b = new StringBuilder("VR session did not start");
        if (when != null) {
            b.append(", ").append(when);
        }
        b.append('\n');
        b.append("  stopped at: ").append(step);
        if (!call.isEmpty()) {
            b.append(" (").append(call).append(')');
        }
        b.append('\n');
        if (result != 0 || !resultName.isEmpty()) {
            b.append("  result: ").append(resultName.isEmpty() ? "?" : resultName)
                    .append(" (").append(result).append(")\n");
        }
        if (!detail.isEmpty()) {
            b.append("  detail: ").append(detail).append('\n');
        }
        b.append("  runtime: ").append(runtime != null ? runtime : "none, no instance was made")
                .append('\n');
        if (requested != null && requested.length > 0) {
            b.append("  asked for ").append(requested.length).append(": ")
                    .append(join(" ", Arrays.asList(requested))).append('\n');
        }
        if (offered == null) {
            b.append("  offered: not known\n");
        }
        else if (offered.length == 0) {
            b.append("  offered: nothing\n");
        }
        else {
            b.append("  offered ").append(offered.length).append(": ")
                    .append(join(" ", Arrays.asList(offered))).append('\n');
        }
        return b.toString();
    }

    private static String join(String with, List<String> parts) {
        StringBuilder b = new StringBuilder();
        for (String part : parts) {
            if (b.length() > 0) {
                b.append(with);
            }
            b.append(part);
        }
        return b.toString();
    }
}
