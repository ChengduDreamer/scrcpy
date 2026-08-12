package com.mivox.mirror.agent;

import android.os.SystemClock;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;

import tc.MirrorMessage.Message;
import tc.MirrorMessage.MessageType;
import tc.MirrorMessage.NotificationHandshake;

/**
 * IPC client towards the shell-server notification relay (127.0.0.1:29748).
 *
 * Frame format: 4-byte big-endian length + serialized tc::Message, 64KB cap.
 * The first frame after every (re)connect must be a kNotificationHandshake
 * carrying the session token; the relay drops the connection otherwise.
 * Reconnects use exponential backoff (1s -> 30s cap) and NEVER give up while
 * running: the relay serves a single client, so a new connection replaces and
 * closes the previous one, and the agent must keep retrying until it wins the
 * slot back. The backoff is reset only after a connection stayed up for more
 * than STABLE_CONNECTION_MS — short-lived connections (e.g. the relay drops
 * us right after a rejected handshake) count as failures, so a token mismatch
 * cannot cause a reconnect storm. Before every connect attempt (retries
 * included) the token is refreshed via TokenRefresher: the SET_TOKEN
 * broadcast may never reach a process frozen by the ROM, so the PC also
 * publishes the token in the world-readable file
 * /data/local/tmp/mivox_agent_token (0644) for the agent to read directly.
 * All socket writes run on a single-thread executor so frames never
 * interleave.
 *
 * Robustness notes:
 * - The IO loop catches RuntimeException/Error in addition to IOException.
 *   An uncaught throw used to kill the IO thread while leaving running=true,
 *   which turned the client into a permanent zombie (start() early-returned)
 *   that never reconnected again — invisible on ROMs that hide logcat.
 * - A generation counter retires stale IO threads after stop()/start() races,
 *   so two loops can never fight over the relay's single connection slot.
 * - kick() cuts the current backoff sleep short so a notification that
 *   arrives while disconnected triggers an immediate reconnect attempt.
 */
final class IpcClient {

    interface ControlListener {
        void onNotificationControl(boolean enable);
    }

    /** Supplies a possibly-newer session token before every connect attempt. */
    interface TokenRefresher {
        /** Called on the IO thread; null/empty keeps the current token. */
        String refreshToken();
    }

    private static final String HOST = "127.0.0.1";
    private static final int PORT = 29748;
    private static final int MAX_FRAME_BYTES = 64 * 1024;
    private static final int CONNECT_TIMEOUT_MS = 3000;
    private static final long BACKOFF_INITIAL_MS = 1000;
    private static final long BACKOFF_MAX_MS = 30000;
    /** Connections shorter than this count as failures for backoff purposes. */
    private static final long STABLE_CONNECTION_MS = 5000;

    private final ControlListener controlListener;
    private final ExecutorService writeExecutor = Executors.newSingleThreadExecutor();

    private volatile boolean running;
    private volatile String token;
    private volatile TokenRefresher tokenRefresher;

    private Thread ioThread;
    /** Guarded by this; bumped on start/stop so a stale IO thread exits. */
    private int generation;
    /** Guarded by this; throttles "frame dropped, not connected" logs. */
    private int droppedFrames;
    private Socket socket;
    private DataOutputStream output;

    IpcClient(ControlListener controlListener) {
        this.controlListener = controlListener;
    }

    void setTokenRefresher(TokenRefresher refresher) {
        this.tokenRefresher = refresher;
    }

    synchronized void start(String token) {
        this.token = token;
        if (running) {
            Thread thread = ioThread;
            if (thread != null && thread.isAlive()) {
                return;
            }
            // The IO thread died unexpectedly while running stayed true;
            // revive it instead of remaining a zombie that never reconnects.
            AgentLog.w("ipc: IO thread dead while running, reviving");
        }
        running = true;
        generation++;
        ioThread = new Thread(this::runIoLoop, "MivoxAgent-Ipc");
        ioThread.start();
    }

    /** Force an immediate reconnect so the next handshake carries the new token. */
    synchronized void updateToken(String token) {
        this.token = token;
        closeSocketLocked();
        if (ioThread != null) {
            ioThread.interrupt();
        }
    }

    synchronized void stop() {
        running = false;
        generation++;
        closeSocketLocked();
        if (ioThread != null) {
            ioThread.interrupt();
            ioThread = null;
        }
    }

    /** Terminal shutdown (service destroyed); the client cannot be restarted afterwards. */
    synchronized void release() {
        stop();
        writeExecutor.shutdownNow();
    }

    boolean isRunning() {
        return running;
    }

    /** True while a live IO thread is driving the reconnect loop. */
    synchronized boolean isAlive() {
        return running && ioThread != null && ioThread.isAlive();
    }

    /** True while a connection is established (handshake sent). */
    synchronized boolean isConnected() {
        return output != null;
    }

    /** Cut the current backoff sleep short so the next connect attempt happens now. */
    void kick() {
        Thread thread;
        synchronized (this) {
            thread = ioThread;
        }
        if (thread != null) {
            thread.interrupt();
        }
    }

    /** Queue one frame; dropped while disconnected, oversized or released. */
    void send(Message message) {
        byte[] payload = message.toByteArray();
        if (payload.length > MAX_FRAME_BYTES) {
            AgentLog.w("ipc: drop oversized frame: " + payload.length + " bytes");
            return;
        }
        try {
            writeExecutor.execute(() -> {
                DataOutputStream out;
                synchronized (IpcClient.this) {
                    out = output;
                }
                if (out == null) {
                    int dropped;
                    synchronized (IpcClient.this) {
                        dropped = ++droppedFrames;
                    }
                    if (dropped == 1 || dropped % 20 == 0) {
                        AgentLog.w("ipc: frame dropped, not connected (total " + dropped + ")");
                    }
                    return;
                }
                try {
                    out.writeInt(payload.length);
                    out.write(payload);
                    out.flush();
                    AgentLog.i("ipc: frame written, " + payload.length + " bytes");
                } catch (IOException e) {
                    AgentLog.w("ipc: frame write failed, closing connection: " + e.getMessage());
                    synchronized (IpcClient.this) {
                        closeSocketLocked();
                    }
                }
            });
        } catch (RejectedExecutionException e) {
            // released concurrently; drop the frame
        }
    }

    private void runIoLoop() {
        final int gen;
        synchronized (this) {
            gen = generation;
        }
        long backoffMs = BACKOFF_INITIAL_MS;
        while (isActive(gen)) {
            long connectAtMs = SystemClock.elapsedRealtime();
            try {
                // Pull the latest token before every connect attempt (retries
                // included): the SET_TOKEN broadcast may never execute while
                // the process is frozen by the ROM, so reading the token file
                // is the only reliable delivery path.
                refreshToken();
                Socket newSocket = new Socket();
                synchronized (this) {
                    socket = newSocket;
                }
                AgentLog.i("ipc: connecting to " + HOST + ":" + PORT);
                newSocket.connect(new InetSocketAddress(HOST, PORT), CONNECT_TIMEOUT_MS);
                newSocket.setTcpNoDelay(true);
                DataOutputStream out =
                        new DataOutputStream(new BufferedOutputStream(newSocket.getOutputStream()));
                // The handshake must be the first frame: publish the output stream only
                // after it has been written, so queued events cannot overtake it.
                writeHandshake(out, token);
                synchronized (this) {
                    output = out;
                    droppedFrames = 0;
                }
                AgentLog.i("ipc: connected, handshake sent (token " + AgentLog.tokenTag(token) + ")");
                readLoop(newSocket, gen);
            } catch (IOException e) {
                if (isActive(gen)) {
                    AgentLog.w("ipc: connection lost: " + e.getMessage());
                }
            } catch (RuntimeException | Error e) {
                // Never let the reconnect loop die: with the thread gone and
                // running left true, start() used to early-return forever and
                // the agent never reconnected again (observed on-device).
                AgentLog.e("ipc: unexpected error, reconnect loop continues", e);
            } finally {
                synchronized (this) {
                    closeSocketLocked();
                }
            }
            if (!isActive(gen)) {
                break;
            }
            // Only a stable connection resets the backoff. A connection that
            // dies within STABLE_CONNECTION_MS (e.g. the relay drops us right
            // after a rejected handshake) counts as a failure and keeps the
            // backoff growing — otherwise a token mismatch retries every ~1s
            // forever and floods the relay with TIME_WAIT sockets.
            if (SystemClock.elapsedRealtime() - connectAtMs >= STABLE_CONNECTION_MS) {
                backoffMs = BACKOFF_INITIAL_MS;
            }
            AgentLog.i("ipc: reconnecting in " + backoffMs + " ms");
            try {
                Thread.sleep(backoffMs);
            } catch (InterruptedException e) {
                if (!isActive(gen)) {
                    break;
                }
                // updateToken()/kick() cut the backoff short to reconnect immediately
            }
            backoffMs = Math.min(backoffMs * 2, BACKOFF_MAX_MS);
        }
        AgentLog.i("ipc: client stopped");
    }

    /** True while this IO thread generation is still the active one. */
    private boolean isActive(int gen) {
        synchronized (this) {
            return running && generation == gen;
        }
    }

    private void refreshToken() {
        TokenRefresher refresher = tokenRefresher;
        if (refresher == null) {
            return;
        }
        String fresh = refresher.refreshToken();
        if (fresh != null && !fresh.isEmpty()) {
            token = fresh;
        }
    }

    private void writeHandshake(DataOutputStream out, String token) throws IOException {
        Message handshake = Message.newBuilder()
                .setType(MessageType.kNotificationHandshake)
                .setSendTime(System.currentTimeMillis())
                .setNotificationHandshake(NotificationHandshake.newBuilder()
                        .setToken(token)
                        .setAgentVersion(BuildConfig.VERSION_NAME)
                        .setCapabilities(0))
                .build();
        byte[] payload = handshake.toByteArray();
        out.writeInt(payload.length);
        out.write(payload);
        out.flush();
    }

    private void readLoop(Socket socket, int gen) throws IOException {
        DataInputStream in = new DataInputStream(new BufferedInputStream(socket.getInputStream()));
        while (isActive(gen)) {
            int length = in.readInt();
            if (length <= 0 || length > MAX_FRAME_BYTES) {
                throw new IOException("invalid frame length: " + length);
            }
            byte[] payload = new byte[length];
            in.readFully(payload);
            Message message = Message.parseFrom(payload);
            if (message.getType() == MessageType.kNotificationControl) {
                boolean enable = message.getNotificationControl().getEnable();
                AgentLog.i("ipc: control received, enable=" + enable);
                controlListener.onNotificationControl(enable);
            }
        }
    }

    private void closeSocketLocked() {
        output = null;
        if (socket != null) {
            try {
                socket.close();
            } catch (IOException ignored) {
                // nothing to do
            }
            socket = null;
        }
    }
}
