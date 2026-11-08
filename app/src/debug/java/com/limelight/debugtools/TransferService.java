package com.limelight.debugtools;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Environment;
import android.os.IBinder;

import com.limelight.LimeLog;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URLDecoder;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * Debug builds only: receives films from the PC over the local network straight into Movies,
 * several times faster than adb push, which waits on every packet. Start it with
 *
 *     adb shell am start-foreground-service -n com.aaks.player.debug/com.limelight.debugtools.TransferService
 *
 * and send with tools/questsend.py. Each connection sends one line, then for a file its bytes:
 * "STAT name" replies the file's current length; "SEND name size offset" is followed by the
 * bytes from offset to size and replies "OK received md5" (md5 of the bytes received). Names
 * are URL-encoded and must be plain file names.
 */
public class TransferService extends Service {

    static final int PORT = 8765;
    private static final String CHANNEL = "transfer";
    private static final int BUFFER = 4 * 1024 * 1024;

    private ServerSocket server;

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Film transfer",
                NotificationManager.IMPORTANCE_LOW));
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("AaksVR is receiving films")
                .setContentText("Port " + PORT)
                .build();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        }
        else {
            startForeground(1, n);
        }
    }

    @Override
    public synchronized int onStartCommand(Intent intent, int flags, int startId) {
        if (server == null) {
            try {
                server = new ServerSocket(PORT);
                server.setReceiveBufferSize(BUFFER);
                new Thread(this::acceptLoop, "Transfer accept").start();
                LimeLog.info("Transfer: listening on port " + PORT);
            }
            catch (IOException e) {
                LimeLog.severe("Transfer: cannot listen on port " + PORT + ": " + e);
                stopSelf();
            }
        }
        return START_STICKY;
    }

    private void acceptLoop() {
        ServerSocket s = server;
        while (s != null && !s.isClosed()) {
            try {
                Socket client = s.accept();
                new Thread(() -> serve(client), "Transfer client").start();
            }
            catch (IOException e) {
                return;
            }
        }
    }

    private static String readLine(InputStream in) throws IOException {
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        int c;
        while ((c = in.read()) != -1 && c != '\n') {
            if (line.size() > 4096) {
                throw new IOException("header too long");
            }
            line.write(c);
        }
        return line.toString("UTF-8").trim();
    }

    // A plain file name in Movies, or null
    private static File target(String encoded) throws IOException {
        String name = URLDecoder.decode(encoded, "UTF-8");
        if (name.isEmpty() || name.contains("/") || name.contains("\\") || name.startsWith(".")) {
            return null;
        }
        return new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES), name);
    }

    private static void reply(OutputStream out, String line) throws IOException {
        out.write((line + "\n").getBytes(StandardCharsets.UTF_8));
    }

    private void serve(Socket client) {
        try (Socket c = client) {
            c.setReceiveBufferSize(BUFFER);
            InputStream in = new BufferedInputStream(c.getInputStream(), BUFFER);
            OutputStream out = c.getOutputStream();
            String[] head = readLine(in).split(" ");
            File file = head.length >= 2 ? target(head[1]) : null;
            if (file == null) {
                reply(out, "ERR bad request");
            }
            else if (head[0].equals("STAT")) {
                reply(out, "LEN " + (file.exists() ? file.length() : 0));
            }
            else if (head[0].equals("SEND") && head.length >= 4) {
                reply(out, receive(in, file, Long.parseLong(head[2]), Long.parseLong(head[3])));
            }
            else {
                reply(out, "ERR bad request");
            }
        }
        catch (IOException | RuntimeException e) {
            LimeLog.warning("Transfer: connection failed: " + e);
        }
    }

    private String receive(InputStream in, File file, long size, long offset) throws IOException {
        MessageDigest md5;
        try {
            md5 = MessageDigest.getInstance("MD5");
        }
        catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        LimeLog.info("Transfer: " + file.getName() + " from " + offset + " of " + size);
        long started = System.nanoTime();
        long received = 0;
        try (RandomAccessFile raf = new RandomAccessFile(file, "rw");
             FileChannel channel = raf.getChannel()) {
            if (offset == 0) {
                raf.setLength(0);
            }
            channel.position(offset);
            byte[] buf = new byte[BUFFER];
            long left = size - offset;
            while (left > 0) {
                int n = in.read(buf, 0, (int) Math.min(buf.length, left));
                if (n < 0) {
                    break;
                }
                md5.update(buf, 0, n);
                ByteBuffer bb = ByteBuffer.wrap(buf, 0, n);
                while (bb.hasRemaining()) {
                    channel.write(bb);
                }
                received += n;
                left -= n;
            }
        }
        double secs = (System.nanoTime() - started) / 1e9;
        LimeLog.info(String.format(Locale.ROOT, "Transfer: %s got %d bytes in %.0f s, %.1f MB/s",
                file.getName(), received, secs, received / Math.max(secs, 0.001) / 1048576.0));
        StringBuilder hex = new StringBuilder();
        for (byte b : md5.digest()) {
            hex.append(String.format(Locale.ROOT, "%02x", b));
        }
        return "OK " + received + " " + hex;
    }

    @Override
    public synchronized void onDestroy() {
        try {
            if (server != null) {
                server.close();
            }
        }
        catch (IOException e) {
            // Closing anyway
        }
        server = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
