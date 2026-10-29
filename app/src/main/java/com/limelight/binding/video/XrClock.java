package com.limelight.binding.video;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.text.format.DateFormat;

import com.limelight.R;

import java.text.SimpleDateFormat;
import java.util.Calendar;
import java.util.Locale;

/**
 * The time and battery line for the settings panel. Battery and the 12/24 hour
 * setting are system calls, so they are re-read at most once a minute.
 */
final class XrClock {
    private static final long READ_EVERY_MS = 60000;

    private final Context context;
    private final String batteryFormat;
    private int batteryPercent = -1;
    private boolean twentyFour = true;
    private long readMs;
    private boolean reading;

    XrClock(Context context) {
        this.context = context.getApplicationContext();
        this.batteryFormat = context.getString(R.string.vr_battery);
        // Read once here, off the frame loop, so the first line is complete
        read(System.currentTimeMillis());
    }

    /**
     * The time, then the battery once read. Cheap on any thread; when the last read
     * is a minute old it starts another on its own thread.
     */
    synchronized String line(long nowMillis) {
        if (nowMillis - readMs >= READ_EVERY_MS && !reading) {
            reading = true;
            Thread reader = new Thread() {
                @Override
                public void run() {
                    read(System.currentTimeMillis());
                }
            };
            reader.setName("Video - XR Clock");
            reader.start();
        }
        Calendar now = Calendar.getInstance();
        now.setTimeInMillis(nowMillis);
        return line(timeText(now, twentyFour, Locale.getDefault()), batteryPercent, batteryFormat);
    }

    private void read(long nowMillis) {
        int percent = -1;
        try {
            Intent battery = context.registerReceiver(null,
                    new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (battery != null) {
                percent = batteryPercent(battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1),
                        battery.getIntExtra(BatteryManager.EXTRA_SCALE, -1));
            }
        } catch (RuntimeException ignored) {
            // No battery: the line just shows the time
        }
        boolean hours24 = DateFormat.is24HourFormat(context);
        synchronized (this) {
            batteryPercent = percent;
            twentyFour = hours24;
            readMs = nowMillis;
            reading = false;
        }
    }

    /** Hours and minutes, in the headset's 12 or 24 hour format. */
    static String timeText(Calendar now, boolean twentyFour, Locale locale) {
        SimpleDateFormat format = new SimpleDateFormat(twentyFour ? "H:mm" : "h:mm a", locale);
        format.setTimeZone(now.getTimeZone());
        return format.format(now.getTime());
    }

    /** The time, then the battery if known, two spaces apart. */
    static String line(String time, int batteryPercent, String batteryFormat) {
        if (batteryPercent < 0) {
            return time;
        }
        return time + "  " + String.format(Locale.getDefault(), batteryFormat, batteryPercent);
    }

    /** Battery level as a whole percent, or -1 if unknown. */
    static int batteryPercent(int level, int scale) {
        if (level < 0 || scale <= 0) {
            return -1;
        }
        return Math.min(100, Math.round(level * 100.0f / scale));
    }
}
