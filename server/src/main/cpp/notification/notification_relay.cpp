#include "notification_relay.h"

#include <android/log.h>

#include <Poco/Net/NetException.h>
#include <Poco/Net/SocketAddress.h>

#include <fstream>
#include <sstream>

#include "mirror_message.pb.h"

#define TAG "scrcpy-notif-relay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace scrcpy {

NotificationRelay::NotificationRelay() {}

NotificationRelay::~NotificationRelay() { Stop(); }

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

bool NotificationRelay::Start(int port) {
    if (m_running) {
        return true;
    }

    try {
        // Bind to loopback only: the companion APK lives on the same device;
        // listening on 0.0.0.0 would expose the port to the LAN.
        const Poco::Net::SocketAddress bindAddr(
            "127.0.0.1", static_cast<Poco::UInt16>(port));
        m_listenSocket = std::make_unique<Poco::Net::ServerSocket>(bindAddr);

        m_running = true;
        m_acceptThread = std::thread(&NotificationRelay::AcceptLoop, this);

        // Token file is re-read per handshake, but warn early if it is already
        // known to be missing: every connection will be rejected until PC
        // writes it (see doc/notification-sync-design.md §2.3).
        if (ReadTokenFile().empty()) {
            LOGW("Token file %s missing/empty at start: all agent connections will be rejected",
                 kTokenFilePath);
        }

        LOGI("Notification relay listening on 127.0.0.1:%d", port);
        return true;
    } catch (const std::exception& e) {
        LOGE("Notification relay start failed: %s", e.what());
        m_running = false;
        m_listenSocket.reset();
        return false;
    }
}

void NotificationRelay::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    // Closing the listen socket unblocks accept() in the accept thread; the
    // accept thread's exit path then closes the client socket (unblocking the
    // client thread's receiveBytes) and joins the client thread.
    if (m_listenSocket) {
        try {
            m_listenSocket->close();
        } catch (...) {}
    }
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
    m_listenSocket.reset();

    LOGI("Notification relay stopped");
}

bool NotificationRelay::IsRunning() const { return m_running; }

bool NotificationRelay::HasAgent() const { return m_clientAuthed; }

void NotificationRelay::SetSendToPcHandler(SendToPcHandler handler) {
    m_sendToPc = std::move(handler);
}

// ---------------------------------------------------------------------------
// Accept thread (relay-lifetime)
// ---------------------------------------------------------------------------

void NotificationRelay::AcceptLoop() {
    LOGI("Relay accept thread started");
    while (m_running) {
        Poco::Net::StreamSocket socket;
        try {
            socket = m_listenSocket->acceptConnection();
        } catch (const std::exception& e) {
            if (m_running) {
                LOGE("Relay accept failed: %s", e.what());
            }
            break;  // listen socket closed (Stop) or unrecoverable
        }

        if (!m_running) {
            try { socket.close(); } catch (...) {}
            break;
        }

        LOGI("Agent connected from %s", socket.peerAddress().toString().c_str());
        // Bound the PC->agent control write so a stalled APK cannot block the
        // WebSocket work thread (which also serves file transfer) forever.
        socket.setSendTimeout(Poco::Timespan(5, 0));

        // Single client: a new connection replaces the previous one (same
        // policy as the WebSocket server). Closing the old socket makes its
        // client thread's receiveBytes fail and exit, then we join it.
        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            if (m_clientSocket) {
                LOGI("New agent connection replaces the previous one");
                CloseClientLocked();
            }
        }
        if (m_clientThread.joinable()) {
            m_clientThread.join();
        }

        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            m_clientSocket = socket;  // shares the ref-counted SocketImpl
            m_clientAuthed = false;
        }
        m_clientThread = std::thread(&NotificationRelay::ClientLoop, this, socket);
    }

    // Exit path (normal Stop or accept failure): tear down the client side.
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        if (m_clientSocket) {
            CloseClientLocked();
        }
    }
    if (m_clientThread.joinable()) {
        m_clientThread.join();
    }
    LOGI("Relay accept thread stopped");
}

// ---------------------------------------------------------------------------
// Client thread (one per agent connection)
// ---------------------------------------------------------------------------

void NotificationRelay::ClientLoop(Poco::Net::StreamSocket socket) {
    LOGI("Relay client thread started");
    std::vector<uint8_t> frame;
    bool authed = false;

    while (m_running) {
        frame.clear();
        if (!ReadFrame(socket, frame)) {
            break;  // EOF, error, or socket closed by Stop/replacement
        }

        if (!authed) {
            std::string agentVersion;
            if (!VerifyHandshake(frame.data(), frame.size(), agentVersion)) {
                LOGE("Agent handshake rejected, closing connection");
                break;
            }
            authed = true;
            {
                std::lock_guard<std::mutex> lock(m_clientMutex);
                // Only mark authed if this connection is still the current one.
                if (m_clientSocket && *m_clientSocket == socket) {
                    m_clientAuthed = true;
                }
            }
            LOGI("Agent handshake OK (version=%s), entering relay mode",
                 agentVersion.c_str());
            // Forward the authenticated handshake to the PC: the panel's
            // "agent connected" status is driven by kNotificationHandshake.
            if (m_sendToPc) {
                m_sendToPc(frame.data(), frame.size());
            }
            continue;
        }

        // Whitelist: only kNotificationEvent is forwarded to the PC. The
        // envelope bytes are forwarded unchanged (already serialized by APK).
        tc::Message msg;
        if (!msg.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
            LOGE("Agent frame is not a valid tc::Message (%zu bytes), closing", frame.size());
            break;
        }
        if (msg.type() != tc::kNotificationEvent) {
            const uint64_t n = ++m_dropBadTypeCount;
            if (n == 1 || n % 60 == 0) {
                LOGW("Dropped non-event agent frame type=%d (total=%llu)",
                     static_cast<int>(msg.type()), static_cast<unsigned long long>(n));
            }
            continue;
        }
        if (!m_sendToPc || !m_sendToPc(frame.data(), frame.size())) {
            const uint64_t n = ++m_dropNoPcCount;
            if (n == 1 || n % 60 == 0) {
                LOGW("Dropped notification event: no PC channel (total=%llu)",
                     static_cast<unsigned long long>(n));
            }
        }
    }

    // Clear the shared client slot only if it still refers to this connection
    // (a replacement connection may already have taken it over).
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        if (m_clientSocket && *m_clientSocket == socket) {
            m_clientSocket.reset();
            m_clientAuthed = false;
        }
    }
    LOGI("Relay client thread stopped (authed=%d)", authed ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Frame I/O: 4-byte big-endian length + serialized tc::Message
// ---------------------------------------------------------------------------

bool NotificationRelay::ReadExact(Poco::Net::StreamSocket& socket, uint8_t* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        int n = 0;
        try {
            n = socket.receiveBytes(buf + received, static_cast<int>(len - received));
        } catch (const std::exception& e) {
            if (m_running) {
                LOGD("receiveBytes failed: %s", e.what());
            }
            return false;
        }
        if (n <= 0) {
            return false;  // orderly shutdown by peer
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

bool NotificationRelay::ReadFrame(Poco::Net::StreamSocket& socket, std::vector<uint8_t>& out) {
    uint8_t header[4];
    if (!ReadExact(socket, header, sizeof(header))) {
        return false;
    }
    const uint32_t len = (static_cast<uint32_t>(header[0]) << 24) |
                         (static_cast<uint32_t>(header[1]) << 16) |
                         (static_cast<uint32_t>(header[2]) << 8) |
                         (static_cast<uint32_t>(header[3]));
    if (len == 0 || len > kMaxFrameSize) {
        LOGE("Invalid frame length %u (max %zu), closing connection", len, kMaxFrameSize);
        return false;
    }
    out.resize(len);
    return ReadExact(socket, out.data(), len);
}

// ---------------------------------------------------------------------------
// Handshake / token
// ---------------------------------------------------------------------------

std::string NotificationRelay::ReadTokenFile() {
    std::ifstream in(kTokenFilePath, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string token = ss.str();
    // Tolerate a trailing newline if the token was written with `echo` instead
    // of `echo -n`.
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
        token.pop_back();
    }
    return token;
}

bool NotificationRelay::VerifyHandshake(const uint8_t* data, size_t len,
                                        std::string& agentVersionOut) {
    tc::Message msg;
    if (!msg.ParseFromArray(data, static_cast<int>(len))) {
        LOGE("First frame is not a valid tc::Message");
        return false;
    }
    if (msg.type() != tc::kNotificationHandshake || !msg.has_notification_handshake()) {
        LOGE("First frame must be kNotificationHandshake, got type=%d",
             static_cast<int>(msg.type()));
        return false;
    }

    const std::string fileToken = ReadTokenFile();
    if (fileToken.empty()) {
        // No token provisioned: reject everything, but never crash.
        LOGE("Token file %s missing/empty, rejecting agent", kTokenFilePath);
        return false;
    }
    if (msg.notification_handshake().token() != fileToken) {
        LOGE("Agent token mismatch, rejecting connection");
        return false;
    }
    agentVersionOut = msg.notification_handshake().agent_version();
    return true;
}

// ---------------------------------------------------------------------------
// PC -> agent control path
// ---------------------------------------------------------------------------

bool NotificationRelay::SendControl(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0 || len > kMaxFrameSize) {
        LOGE("SendControl: invalid length %zu", len);
        return false;
    }

    // Copy the socket under the lock, send outside it (same pattern as
    // PocoWebsocketServer::SendLoop): a blocked sendBytes must not hold
    // m_clientMutex, otherwise Stop()/connection-replacement would wait on it.
    // If the socket is closed concurrently, sendBytes throws and we drop.
    Poco::Net::StreamSocket socket;
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        if (!m_clientSocket || !m_clientAuthed) {
            LOGW("SendControl dropped: no verified agent connected (%zu bytes)", len);
            return false;
        }
        socket = *m_clientSocket;
    }

    uint8_t header[4];
    header[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
    header[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
    header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>(len & 0xFF);

    // Raw TCP socket: concurrent sendBytes (here) / receiveBytes (client
    // thread) on the same full-duplex socket is safe.
    try {
        size_t sent = 0;
        while (sent < sizeof(header)) {
            int n = socket.sendBytes(header + sent,
                                     static_cast<int>(sizeof(header) - sent));
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        sent = 0;
        while (sent < len) {
            int n = socket.sendBytes(data + sent, static_cast<int>(len - sent));
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        LOGI("SendControl: %zu bytes written to agent", len);
        return true;
    } catch (const std::exception& e) {
        LOGE("SendControl sendBytes failed: %s", e.what());
        return false;
    }
}

void NotificationRelay::CloseClientLocked() {
    m_clientAuthed = false;
    try {
        m_clientSocket->shutdown();
    } catch (...) {}
    try {
        m_clientSocket->close();
    } catch (...) {}
    m_clientSocket.reset();
}

}  // namespace scrcpy
