package com.mivox.mirror.agent;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.text.TextUtils;

/**
 * Receives the session token delivered by the PC via
 * `adb shell am broadcast -n com.mivox.mirror.agent/.TokenReceiver
 *     -a com.mivox.mirror.agent.action.SET_TOKEN --es token <TOKEN>`.
 *
 * This broadcast is only the fast path: it applies instantly while the
 * process is alive, but it may never execute when the ROM has frozen the
 * process (observed on ColorOS: "Broadcast completed" yet onReceive never
 * runs). The reliable channel is the world-readable token file
 * /data/local/tmp/mivox_agent_token (0644), which NotificationBridgeService
 * reads directly before every IPC connect attempt.
 *
 * The token is cached in SharedPreferences (fallback); when the listener
 * service is already running it is handed over directly (forcing an IPC
 * reconnect), otherwise the service picks it up from the cache on the next
 * listener bind.
 */
public class TokenReceiver extends BroadcastReceiver {

    static final String ACTION_SET_TOKEN = "com.mivox.mirror.agent.action.SET_TOKEN";

    @Override
    public void onReceive(Context context, Intent intent) {
        AgentLog.init(context);
        if (!ACTION_SET_TOKEN.equals(intent.getAction())) {
            return;
        }
        String token = intent.getStringExtra(NotificationBridgeService.EXTRA_TOKEN);
        if (TextUtils.isEmpty(token)) {
            AgentLog.w("token broadcast arrived with empty token");
            return;
        }
        AgentLog.i("token broadcast arrived (token " + AgentLog.tokenTag(token) + ")");
        NotificationBridgeService.storeToken(context, token);
        NotificationBridgeService service = NotificationBridgeService.peekInstance();
        if (service != null) {
            service.onTokenReceived(token);
            return;
        }
        try {
            context.startService(new Intent(context, NotificationBridgeService.class)
                    .putExtra(NotificationBridgeService.EXTRA_TOKEN, token));
        } catch (IllegalStateException e) {
            // Background start restricted: the cached token is picked up when
            // the system binds the listener next time.
            AgentLog.w("service start deferred: " + e.getMessage());
        }
    }
}
