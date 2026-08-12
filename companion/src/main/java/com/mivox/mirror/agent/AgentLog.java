package com.mivox.mirror.agent;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Minimal file logger for on-device debugging. Some ROMs hide the app's
 * logcat output entirely (observed on ColorOS: the MivoxAgent tag never
 * reaches any buffer), so key agent events are also appended to
 * files/agent.log — read it via
 * `adb shell run-as com.mivox.mirror.agent cat files/agent.log` (debug build).
 *
 * Lines carry a wall-clock timestamp and the caller's thread name. All file
 * writes run on a single daemon thread and every failure is swallowed, so
 * logging can never break the agent. Once the file exceeds 256KB it is
 * truncated and rewritten from the head.
 *
 * Notification content and raw session tokens must never be written here;
 * use tokenTag() for token correlation.
 */
final class AgentLog {

    private static final String TAG = "MivoxAgent";
    private static final String FILE_NAME = "agent.log";
    private static final long MAX_BYTES = 256L * 1024L;

    private static File file;
    private static ExecutorService writer;
    /** Writer-thread confined; created lazily on first use. */
    private static SimpleDateFormat tsFormat;

    private AgentLog() {
    }

    /** Idempotent; call once from any entry point that has a context. */
    static synchronized void init(Context context) {
        if (writer != null) {
            return;
        }
        try {
            file = new File(context.getApplicationContext().getFilesDir(), FILE_NAME);
            writer = Executors.newSingleThreadExecutor(r -> {
                Thread thread = new Thread(r, "MivoxAgent-Log");
                thread.setDaemon(true);
                return thread;
            });
        } catch (RuntimeException e) {
            file = null;
            writer = null;
        }
    }

    static void i(String msg) {
        log(Log.INFO, 'I', msg, null);
    }

    static void w(String msg) {
        log(Log.WARN, 'W', msg, null);
    }

    static void e(String msg, Throwable tr) {
        log(Log.ERROR, 'E', msg, tr);
    }

    /**
     * Non-reversible token fingerprint for log correlation (first/last 4
     * chars plus length); the raw token is never logged.
     */
    static String tokenTag(String token) {
        if (token == null || token.isEmpty()) {
            return "none";
        }
        if (token.length() < 8) {
            return "***len" + token.length();
        }
        return token.substring(0, 4) + "~" + token.substring(token.length() - 4)
                + "/len" + token.length();
    }

    private static void log(int priority, char level, String msg, Throwable tr) {
        String full = tr == null ? msg
                : msg + " (" + tr.getClass().getSimpleName() + ": " + tr.getMessage() + ")";
        // Keep the logcat path too, for ROMs where it works.
        Log.println(priority, TAG, full);
        ExecutorService w;
        File f;
        synchronized (AgentLog.class) {
            w = writer;
            f = file;
        }
        if (w == null || f == null) {
            return;
        }
        long nowMs = System.currentTimeMillis();
        String threadName = Thread.currentThread().getName();
        try {
            w.execute(() -> writeLine(f, nowMs, level, threadName, full));
        } catch (RuntimeException ignored) {
            // Logging must never throw into the caller.
        }
    }

    /** Runs on the writer thread only; a fresh stream per line keeps it crash-safe. */
    private static void writeLine(File f, long nowMs, char level, String threadName, String msg) {
        if (tsFormat == null) {
            tsFormat = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US);
        }
        String line = tsFormat.format(new Date(nowMs)) + ' ' + level
                + " [" + threadName + "] " + msg + '\n';
        FileOutputStream out = null;
        try {
            boolean truncate = f.isFile() && f.length() > MAX_BYTES;
            out = new FileOutputStream(f, !truncate);
            if (truncate) {
                String marker = tsFormat.format(new Date(nowMs))
                        + " W [MivoxAgent-Log] --- log truncated, restarting from head ---\n";
                out.write(marker.getBytes(StandardCharsets.UTF_8));
            }
            out.write(line.getBytes(StandardCharsets.UTF_8));
        } catch (IOException | RuntimeException ignored) {
            // Logging must never affect the agent.
        } finally {
            if (out != null) {
                try {
                    out.close();
                } catch (IOException ignored) {
                    // nothing to do
                }
            }
        }
    }
}
