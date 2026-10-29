package com.limelight;

import android.content.Context;
import android.os.Build;
import android.os.Environment;
import android.util.Log;

import com.limelight.binding.video.XrShared;
import com.limelight.utils.HeadsetFeatures;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

/**
 * Plain text copy of the log in Download/Aaks, where a headset's file manager can reach it.
 * Logcat output is unchanged. Two files at most: when the log fills it replaces the previous
 * log and a fresh one starts with the device header.
 */
public class FileLog {
    public static final int OFF = XrShared.FILE_LOG_OFF;
    public static final int BASIC = XrShared.FILE_LOG_BASIC;
    public static final int VERBOSE = XrShared.FILE_LOG_VERBOSE;

    private static final String TAG = "aaks";
    private static final String PUBLIC_DIR = "Aaks";
    private static final String LOG_DIR = "logs";
    private static final String LOG_NAME = "aaks.log";
    private static final String PREVIOUS_LOG_NAME = "aaks.previous.log";

    // Rotated once at this size
    private static final long MAX_BYTES = 5 * 1024 * 1024;
    // Holds a burst of stats lines without letting a stuck writer eat the heap
    private static final int QUEUE_CAPACITY = 512;

    private static final SimpleDateFormat TIMESTAMP =
            new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US);

    // Guards the file, so a crashing thread can write its stack without racing the writer thread
    private static final Object fileLock = new Object();
    private static final AtomicInteger dropped = new AtomicInteger();

    private static volatile int level = OFF;
    private static volatile String logPath;
    private static boolean initialized;

    private static File logFile;
    private static FileOutputStream stream;
    private static BufferedWriter writer;
    private static long fileBytes;

    private static ArrayBlockingQueue<Entry> queue;
    // Strong reference, so the log manager cannot collect the logger the handler is on
    private static Logger hookedLogger;
    // Written again at the top of each new file
    private static String[] headerLines;

    private static class Entry {
        final long millis;
        final char level;
        final String message;

        Entry(long millis, char level, String message) {
            this.millis = millis;
            this.level = level;
            this.message = message;
        }
    }

    /** Opens the log and attaches the handlers. Does nothing when wanted is OFF. */
    public static synchronized void init(Context context, int wanted) {
        if (initialized || wanted == OFF) {
            return;
        }
        initialized = true;

        if (!openFirstWritableDir(context)) {
            return;
        }

        level = wanted;
        logPath = logFile.getAbsolutePath();
        // Before the writer thread starts, which reads them on a wipe
        headerLines = buildHeader(context);
        queue = new ArrayBlockingQueue<>(QUEUE_CAPACITY);

        Thread writerThread = new Thread() {
            @Override
            public void run() {
                drainQueue();
            }
        };
        writerThread.setName("File log");
        writerThread.setDaemon(true);
        writerThread.start();

        hookLimeLog();
        hookCrashes();
        for (String line : headerLines) {
            event(line);
        }
    }

    /**
     * Opens the log in Download first, else the app's own folder: no storage permission is
     * asked for, and after a reinstall the old files there belong to the previous install.
     */
    private static boolean openFirstWritableDir(Context context) {
        File download = new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOWNLOADS), PUBLIC_DIR);
        File own = context.getExternalFilesDir(null);
        File[] candidates = own != null
                ? new File[] { download, new File(own, LOG_DIR) }
                : new File[] { download };

        for (File dir : candidates) {
            if (!dir.isDirectory() && !dir.mkdirs()) {
                continue;
            }
            logFile = new File(dir, LOG_NAME);
            synchronized (fileLock) {
                if (openFile()) {
                    return true;
                }
            }
            logFile = null;
        }
        return false;
    }

    /** A line kept even at the basic level. Also goes to logcat. */
    public static void event(String message) {
        Log.i(TAG, message);
        if (level != OFF) {
            enqueue('I', message);
        }
    }

    /** The file to point a bug reporter at, or null when logging is off. */
    public static String getLogPath() {
        return logPath;
    }

    /** Writes everything queued so far to disk, for a report about to read the file. */
    public static void flush() {
        if (queue == null) {
            return;
        }
        // Give the writer thread a moment to drain the queue, then flush the stream
        long waitUntil = System.currentTimeMillis() + 200;
        while (!queue.isEmpty() && System.currentTimeMillis() < waitUntil) {
            try {
                Thread.sleep(10);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        flushFile();
    }

    /** The log before this one, which may not exist, or null when logging is off. */
    public static File getPreviousLogFile() {
        if (logPath == null) {
            return null;
        }
        return new File(new File(logPath).getParentFile(), PREVIOUS_LOG_NAME);
    }

    public static int getLevel() {
        return level;
    }

    /** Maps the setting value onto one of the level constants. */
    public static int levelFromName(String name) {
        if ("verbose".equals(name)) {
            return VERBOSE;
        }
        if ("basic".equals(name)) {
            return BASIC;
        }
        return OFF;
    }

    // Device lines at the top of each log file
    private static String[] buildHeader(Context context) {
        return new String[] {
                "aaks " + BuildConfig.VERSION_NAME + " " + context.getPackageName()
                        + (BuildConfig.ROOT_BUILD ? " root" : ""),
                "device " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")",
                "android " + Build.VERSION.RELEASE + " sdk " + Build.VERSION.SDK_INT,
                HeadsetFeatures.line(context),
                "log " + logPath
        };
    }

    /**
     * Copies LimeLog records to the file: warnings and errors always, info only at verbose.
     * Parent handlers stay on, so logcat is unchanged.
     */
    private static void hookLimeLog() {
        hookedLogger = Logger.getLogger(LimeLog.class.getName());
        hookedLogger.addHandler(new Handler() {
            @Override
            public void publish(LogRecord record) {
                if (record == null || record.getMessage() == null) {
                    return;
                }
                int value = record.getLevel().intValue();
                if (value >= Level.SEVERE.intValue()) {
                    enqueue('E', record.getMessage());
                }
                else if (value >= Level.WARNING.intValue()) {
                    enqueue('W', record.getMessage());
                }
                else if (level == VERBOSE) {
                    enqueue('V', record.getMessage());
                }
            }

            @Override
            public void flush() {
            }

            @Override
            public void close() {
            }
        });
    }

    /** Crashes go straight to disk, since the process is gone before the writer thread runs. */
    private static void hookCrashes() {
        final Thread.UncaughtExceptionHandler previous =
                Thread.getDefaultUncaughtExceptionHandler();

        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread thread, Throwable error) {
                try {
                    StringWriter trace = new StringWriter();
                    error.printStackTrace(new PrintWriter(trace));
                    writeSync("FATAL EXCEPTION in " + thread.getName() + "\n" + trace);
                } catch (Throwable ignored) {
                    // Never get in the way of the real handler
                }
                if (previous != null) {
                    previous.uncaughtException(thread, error);
                }
            }
        });
    }

    private static void enqueue(char entryLevel, String message) {
        ArrayBlockingQueue<Entry> q = queue;
        if (q == null) {
            return;
        }
        if (!q.offer(new Entry(System.currentTimeMillis(), entryLevel, message))) {
            dropped.incrementAndGet();
        }
    }

    private static void drainQueue() {
        while (true) {
            Entry entry = queue.poll();
            if (entry == null) {
                flushFile();
                try {
                    entry = queue.take();
                } catch (InterruptedException e) {
                    return;
                }
            }

            // Taking one entry made room, so the count of dropped lines goes in ahead of it
            int lost = dropped.getAndSet(0);
            if (lost > 0) {
                writeLine(entry.millis, 'W', lost + " log lines dropped");
            }
            writeLine(entry.millis, entry.level, entry.message);
        }
    }

    private static void writeLine(long millis, char entryLevel, String message) {
        synchronized (fileLock) {
            if (writer == null) {
                return;
            }
            try {
                String line = TIMESTAMP.format(new Date(millis)) + " " + entryLevel + " "
                        + message + "\n";
                writer.write(line);
                fileBytes += line.length();
                if (fileBytes > MAX_BYTES) {
                    wipe();
                }
            } catch (IOException e) {
                closeFile();
            }
        }
    }

    private static void writeSync(String message) {
        synchronized (fileLock) {
            if (writer == null) {
                return;
            }
            try {
                writer.write(TIMESTAMP.format(new Date()) + " F " + message + "\n");
                writer.flush();
                stream.getFD().sync();
            } catch (IOException e) {
                closeFile();
            }
        }
    }

    private static void flushFile() {
        synchronized (fileLock) {
            if (writer == null) {
                return;
            }
            try {
                writer.flush();
                // Also picks up what the native renderer appended to the same file
                fileBytes = logFile.length();
                if (fileBytes > MAX_BYTES) {
                    wipe();
                }
            } catch (IOException e) {
                closeFile();
            }
        }
    }

    /** Moves the full log to the previous log and starts a fresh one. Caller holds fileLock. */
    private static void wipe() {
        closeFile();

        // If the rename fails, keep writing to the oversize log; the next write tries again
        File previous = new File(logFile.getParentFile(), PREVIOUS_LOG_NAME);
        previous.delete();
        if (!logFile.renameTo(previous)) {
            openFile();
            return;
        }
        if (!openFile()) {
            return;
        }

        // The header lines cannot reach the cap, so this cannot roll over again
        long now = System.currentTimeMillis();
        writeLine(now, 'I', "log reached " + (MAX_BYTES / (1024 * 1024))
                + " MB, the earlier part is in " + PREVIOUS_LOG_NAME);
        for (String line : headerLines) {
            writeLine(now, 'I', line);
        }
    }

    // Caller holds fileLock
    private static boolean openFile() {
        try {
            stream = new FileOutputStream(logFile, true);
            writer = new BufferedWriter(new OutputStreamWriter(stream));
            fileBytes = logFile.length();
            return true;
        } catch (IOException e) {
            stream = null;
            writer = null;
            return false;
        }
    }

    // Caller holds fileLock
    private static void closeFile() {
        try {
            if (writer != null) {
                writer.close();
            }
        } catch (IOException ignored) {
        }
        writer = null;
        stream = null;
    }
}
