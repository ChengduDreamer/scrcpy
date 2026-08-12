package com.mivox.mirror.agent;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;
import android.text.TextUtils;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

import tc.MirrorMessage.Message;
import tc.MirrorMessage.MessageType;
import tc.MirrorMessage.NotificationAction;
import tc.MirrorMessage.NotificationEvent;

/**
 * Collects system notifications and reports them to the shell-server
 * notification relay via IpcClient (display-only in M1).
 *
 * The system binds this service once the listener is authorized; the session
 * token is delivered out-of-band — primarily read by the agent directly from
 * the world-readable file /data/local/tmp/mivox_agent_token (written 0644 by
 * the PC) before every IPC connect (the reliable channel), with the
 * SET_TOKEN broadcast (TokenReceiver) as a best-effort fast path — and
 * cached in SharedPreferences as fallback, then used for the IPC handshake
 * whenever the listener connects.
 *
 * Key events are mirrored to files/agent.log via AgentLog because some ROMs
 * (observed on ColorOS) hide the app's logcat output entirely. Notification
 * content and raw tokens are never logged.
 */
public class NotificationBridgeService extends NotificationListenerService {

    private static final String PREFS_NAME = "mivox_agent";
    private static final String PREF_TOKEN = "token";
    private static final String TOKEN_FILE_PATH = "/data/local/tmp/mivox_agent_token";
    /** Read cap; the token itself is 32 hex chars. */
    private static final int TOKEN_FILE_MAX_BYTES = 128;
    static final String EXTRA_TOKEN = "token";

    private static final String FGS_CHANNEL_ID = "keep_alive";
    private static final int FGS_NOTIFICATION_ID = 1;

    private static final long COALESCE_WINDOW_MS = 200;
    private static final int TITLE_MAX_CHARS = 256;
    private static final int TEXT_MAX_CHARS = 2000;
    private static final int PACKAGE_RATE_LIMIT_PER_SEC = 10;

    private static volatile NotificationBridgeService instance;

    private final Map<String, PendingEvent> pendingEvents = new HashMap<>();
    private final Map<String, String> appLabelCache = new HashMap<>();
    private final Map<String, PackageRateLimiter> rateLimiters = new HashMap<>();

    private Handler handler;
    private IpcClient ipcClient;
    private boolean reportingEnabled = true;
    private volatile boolean listenerBound;

    static NotificationBridgeService peekInstance() {
        return instance;
    }

    static void storeToken(Context context, String token) {
        context.getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .edit()
                .putString(PREF_TOKEN, token)
                .apply();
    }

    private String loadToken() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        return prefs.getString(PREF_TOKEN, null);
    }

    /**
     * Reads the latest token from /data/local/tmp/mivox_agent_token, written
     * by the PC deploy chain via
     * `adb shell "echo -n <token> > <path> && chmod 644 <path>"`.
     * This is the reliable delivery channel: the SET_TOKEN broadcast may
     * never execute while the process is frozen by the ROM (observed on
     * ColorOS: "Broadcast completed" yet onReceive never runs), and
     * publishing the token to global settings was rejected by ColorOS
     * outright (SecurityException: shell lacks WRITE_SECURE_SETTINGS),
     * whereas the 0644 token file is directly readable under the app identity
     * (verified on-device). Called before every IPC connect attempt.
     *
     * Security note: mode 0644 makes the token readable by any app on the
     * device, so injection resistance stays at "honest user" level.
     * Acceptable for the M1 threat model (loopback relay,
     * anti-accidental-spoofing); to be hardened again in the remote
     * milestone.
     *
     * Runs on the IPC IO thread as well; null means "keep the current token".
     */
    private String refreshTokenFromFile() {
        byte[] buf = new byte[TOKEN_FILE_MAX_BYTES];
        int total = 0;
        try (FileInputStream in = new FileInputStream(new File(TOKEN_FILE_PATH))) {
            while (total < buf.length) {
                int n = in.read(buf, total, buf.length - total);
                if (n < 0) {
                    break;
                }
                total += n;
            }
        } catch (IOException | RuntimeException e) {
            // Missing file, permission denial (SELinux) etc.: keep the
            // current token and retry on the next connect attempt.
            AgentLog.w("token refresh failed: " + e.getMessage());
            return null;
        }
        if (total == 0) {
            AgentLog.w("token refresh: file empty");
            return null;
        }
        String fresh = new String(buf, 0, total, StandardCharsets.UTF_8).trim();
        if (TextUtils.isEmpty(fresh)) {
            AgentLog.w("token refresh: file blank");
            return null;
        }
        if (!fresh.equals(loadToken())) {
            AgentLog.i("token refresh: updated (" + AgentLog.tokenTag(fresh) + ")");
            storeToken(this, fresh);
        } else {
            AgentLog.i("token refresh: same (" + AgentLog.tokenTag(fresh) + ")");
        }
        return fresh;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        AgentLog.init(this);
        startForegroundKeepAlive();
        handler = new Handler(Looper.getMainLooper());
        ipcClient = new IpcClient(this::onNotificationControl);
        ipcClient.setTokenRefresher(this::refreshTokenFromFile);
        instance = this;
        AgentLog.i("service created, version " + BuildConfig.VERSION_NAME);
    }

    /**
     * 前台服务保活：ColorOS 实证会冻结 BFGS 状态的进程导致系统通知回调丢失。
     * FGS 进程状态高于 cached，不会被缓存冻结器冻结。代价是状态栏一条
     * IMPORTANCE_MIN 常驻通知（无声音无震动）。
     */
    private void startForegroundKeepAlive() {
        try {
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm.getNotificationChannel(FGS_CHANNEL_ID) == null) {
                nm.createNotificationChannel(new NotificationChannel(
                        FGS_CHANNEL_ID, getString(R.string.app_name),
                        NotificationManager.IMPORTANCE_MIN));
            }
            Notification notification = new Notification.Builder(this, FGS_CHANNEL_ID)
                    .setContentTitle(getString(R.string.fgs_title))
                    .setSmallIcon(android.R.drawable.stat_notify_sync)
                    .setOngoing(true)
                    .build();
            startForeground(FGS_NOTIFICATION_ID, notification);
            AgentLog.i("foreground keep-alive started");
        } catch (Throwable t) {
            // 保活失败不阻断主流程（个别 ROM 对 specialUse 有限制时只记日志）
            AgentLog.w("startForeground failed: " + t);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null) {
            String token = intent.getStringExtra(EXTRA_TOKEN);
            if (!TextUtils.isEmpty(token)) {
                onTokenReceived(token);
            }
        }
        return START_STICKY;
    }

    @Override
    public void onListenerConnected() {
        listenerBound = true;
        AgentLog.i("listener connected");
        String token = refreshTokenFromFile();
        if (TextUtils.isEmpty(token)) {
            token = loadToken();
        }
        if (TextUtils.isEmpty(token)) {
            AgentLog.w("listener connected but no token yet, IPC start deferred");
            return;
        }
        AgentLog.i("starting IPC client (token " + AgentLog.tokenTag(token) + ")");
        ipcClient.start(token);
    }

    @Override
    public void onListenerDisconnected() {
        listenerBound = false;
        AgentLog.w("listener disconnected, IPC stopped");
        cancelPendingEvents();
        ipcClient.stop();
        // Some ROMs leave the listener unbound for a long time after a
        // disconnect; nudge the system to rebind (no-op if unauthorized).
        requestRebind(new ComponentName(this, NotificationBridgeService.class));
        AgentLog.i("listener rebind requested");
    }

    @Override
    public void onDestroy() {
        AgentLog.i("service destroyed");
        instance = null;
        cancelPendingEvents();
        ipcClient.stop();
        ipcClient.release();
        super.onDestroy();
    }

    @Override
    public void onNotificationPosted(StatusBarNotification sbn) {
        forwardNotification(sbn, NotificationAction.kNotifPosted);
    }

    @Override
    public void onNotificationRemoved(StatusBarNotification sbn) {
        forwardNotification(sbn, NotificationAction.kNotifRemoved);
    }

    void onTokenReceived(String token) {
        AgentLog.i("token received (" + AgentLog.tokenTag(token) + ")");
        storeToken(this, token);
        if (ipcClient.isRunning()) {
            AgentLog.i("forcing IPC reconnect with new token");
            ipcClient.updateToken(token);
        } else {
            ipcClient.start(token);
        }
    }

    private void onNotificationControl(boolean enable) {
        // Called on the IPC reader thread; hop to the main thread.
        handler.post(() -> {
            if (reportingEnabled == enable) {
                return;
            }
            reportingEnabled = enable;
            if (!enable) {
                cancelPendingEvents();
            }
            AgentLog.i("notification reporting " + (enable ? "resumed" : "paused"));
        });
    }

    private void forwardNotification(StatusBarNotification sbn, NotificationAction action) {
        String packageName = sbn.getPackageName();
        AgentLog.i("notif " + action.name() + " pkg=" + packageName + " key=" + sbn.getKey());
        // A notification arriving while the IPC link is down triggers an
        // immediate reconnect attempt instead of waiting out the backoff.
        kickIpcIfDown();
        if (!reportingEnabled) {
            AgentLog.i("notif dropped: reporting disabled pkg=" + packageName);
            return;
        }
        if (sbn.isOngoing()) {
            AgentLog.i("notif dropped: ongoing pkg=" + packageName);
            return;
        }
        Notification notification = sbn.getNotification();
        if ((notification.flags & Notification.FLAG_GROUP_SUMMARY) != 0) {
            AgentLog.i("notif dropped: group summary pkg=" + packageName);
            return;
        }
        if (getPackageName().equals(packageName)) {
            AgentLog.i("notif dropped: own package");
            return;
        }
        PackageRateLimiter limiter = rateLimiters.get(packageName);
        if (limiter == null) {
            limiter = new PackageRateLimiter(PACKAGE_RATE_LIMIT_PER_SEC);
            rateLimiters.put(packageName, limiter);
        }
        if (!limiter.tryAcquire()) {
            long dropped = limiter.droppedCount();
            if (dropped == 1 || dropped % 50 == 0) {
                AgentLog.w("rate limit: dropped " + dropped + " events from " + packageName);
            }
            return;
        }
        NotificationEvent event = buildEvent(sbn, notification, action);
        coalesceAndSend(sbn.getKey(), event);
    }

    /** Wake or restart the IPC client when a notification finds it down. */
    private void kickIpcIfDown() {
        if (!listenerBound) {
            AgentLog.w("notif arrived while listener unbound, leaving IPC stopped");
            return;
        }
        if (ipcClient.isConnected()) {
            return;
        }
        if (ipcClient.isAlive()) {
            AgentLog.w("IPC down when notif arrived, cutting backoff short");
            ipcClient.kick();
            return;
        }
        String token = refreshTokenFromFile();
        if (TextUtils.isEmpty(token)) {
            token = loadToken();
        }
        if (TextUtils.isEmpty(token)) {
            AgentLog.w("IPC down when notif arrived, but no token to connect with");
            return;
        }
        AgentLog.w("IPC client not running when notif arrived, restarting");
        ipcClient.start(token);
    }

    private NotificationEvent buildEvent(StatusBarNotification sbn, Notification notification,
            NotificationAction action) {
        Bundle extras = notification.extras;
        CharSequence title = extras == null ? null : extras.getCharSequence(Notification.EXTRA_TITLE);
        CharSequence text = extras == null ? null : extras.getCharSequence(Notification.EXTRA_TEXT);
        return NotificationEvent.newBuilder()
                .setKey(sbn.getKey())
                .setPackageName(sbn.getPackageName())
                .setAppName(appLabelFor(sbn.getPackageName()))
                .setTitle(truncate(title, TITLE_MAX_CHARS))
                .setText(truncate(text, TEXT_MAX_CHARS))
                .setPostTime(sbn.getPostTime())
                .setAction(action)
                .build();
    }

    private String appLabelFor(String packageName) {
        String cached = appLabelCache.get(packageName);
        if (cached != null) {
            return cached;
        }
        String label;
        try {
            PackageManager pm = getPackageManager();
            label = pm.getApplicationLabel(pm.getApplicationInfo(packageName, 0)).toString();
        } catch (PackageManager.NameNotFoundException e) {
            label = packageName;
        }
        appLabelCache.put(packageName, label);
        return label;
    }

    private static String truncate(CharSequence value, int maxChars) {
        if (value == null) {
            return "";
        }
        String str = value.toString();
        return str.length() <= maxChars ? str : str.substring(0, maxChars);
    }

    /** Same-key updates within COALESCE_WINDOW_MS collapse into the latest one. */
    private void coalesceAndSend(String key, NotificationEvent event) {
        PendingEvent previous = pendingEvents.remove(key);
        if (previous != null) {
            handler.removeCallbacks(previous.sendTask);
        }
        PendingEvent pending = new PendingEvent();
        pending.event = event;
        pending.sendTask = () -> {
            pendingEvents.remove(key);
            sendEvent(pending.event);
        };
        pendingEvents.put(key, pending);
        handler.postDelayed(pending.sendTask, COALESCE_WINDOW_MS);
    }

    private void sendEvent(NotificationEvent event) {
        Message message = Message.newBuilder()
                .setType(MessageType.kNotificationEvent)
                .setSendTime(System.currentTimeMillis())
                .setNotificationEvent(event)
                .build();
        AgentLog.i("event queued: " + event.getAction().name() + " pkg=" + event.getPackageName());
        ipcClient.send(message);
    }

    private void cancelPendingEvents() {
        for (PendingEvent pending : pendingEvents.values()) {
            handler.removeCallbacks(pending.sendTask);
        }
        pendingEvents.clear();
    }

    /** Trailing-edge coalescing state for one notification key. */
    private static final class PendingEvent {
        NotificationEvent event;
        Runnable sendTask;
    }

    /** Token bucket: capacity tokens refilled per second; drops are counted. */
    private static final class PackageRateLimiter {
        private final int capacity;
        private double tokens;
        private long lastRefillMs;
        private long dropped;

        PackageRateLimiter(int capacityPerSec) {
            capacity = capacityPerSec;
            tokens = capacityPerSec;
            lastRefillMs = SystemClock.elapsedRealtime();
        }

        boolean tryAcquire() {
            long now = SystemClock.elapsedRealtime();
            long elapsedMs = now - lastRefillMs;
            if (elapsedMs > 0) {
                tokens = Math.min(capacity, tokens + elapsedMs * capacity / 1000.0);
                lastRefillMs = now;
            }
            if (tokens >= 1.0) {
                tokens -= 1.0;
                return true;
            }
            dropped++;
            return false;
        }

        long droppedCount() {
            return dropped;
        }
    }
}
