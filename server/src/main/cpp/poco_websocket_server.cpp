#include "poco_websocket_server.h"

#include <android/log.h>

#include <Poco/Buffer.h>
#include <Poco/String.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/WebSocket.h>

#include <algorithm>
#include <cstddef>
#include <string>

#define TAG "scrcpy-poco-ws"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace scrcpy {

namespace {

// Qt / browser clients must use: ws://host:port<kWebSocketPath>
// e.g. ws://127.0.0.1:29747/mirror  (query string allowed)
static constexpr char kWebSocketPath[] = "/mirror";

static std::string UriPath(const std::string &uri) {
    const auto q = uri.find('?');
    if (q == std::string::npos) {
        return uri;
    }
    return uri.substr(0, q);
}

static bool IsAllowedWebSocketPath(const std::string &uri) {
    std::string p = UriPath(uri);
    while (p.size() > 1 && p.back() == '/') {
        p.pop_back();
    }
    return Poco::icompare(p, std::string(kWebSocketPath)) == 0;
}

class NotFoundHandler : public Poco::Net::HTTPRequestHandler {
public:
    void handleRequest(Poco::Net::HTTPServerRequest & /*request*/,
                       Poco::Net::HTTPServerResponse &response) override {
        response.setStatus(Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
        response.setContentType("text/plain");
        response.setContentLength(0);
        response.send();
    }
};

// Maximum reassembled message size (64 MB).
constexpr std::size_t kMaxReassembledMessage = 64 * 1024 * 1024;

// Handle binary/text *messages* (possibly fragmented); PING->PONG; CLOSE ends.
// ALL sendFrame calls go through the server's send queue to ensure
// receiveFrame and sendFrame never run concurrently on the same socket.
// Text messages are ignored (no echo).
class WebSocketRequestHandler : public Poco::Net::HTTPRequestHandler {
public:
    explicit WebSocketRequestHandler(PocoWebsocketServer* server) : m_server(server) {}

    void handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response) override {

        using Poco::Net::WebSocket;
        using Poco::Net::WebSocketException;

        LOGI("WebSocketRequestHandler: handleRequest entered from %s, URI=%s, method=%s, Upgrade=%s",
             request.clientAddress().toString().c_str(),
             request.getURI().c_str(),
             request.getMethod().c_str(),
             request.find("Upgrade") != request.end() ? request["Upgrade"].c_str() : "<missing>");

        try {
            LOGI("WebSocketRequestHandler: constructing WebSocket (performing handshake)");
            WebSocket ws(request, response);
            constexpr int kMaxPayload = 64 * 1024 * 1024;
            ws.setMaxPayloadSize(kMaxPayload);
            // Detect silent client disconnects: if no frame arrives within 60s,
            // receiveFrame throws TimeoutException and the handler exits cleanly.
            ws.setReceiveTimeout(Poco::Timespan(60, 0));
            LOGI("WebSocket client connected: %s, URI: %s, method: %s",
                 request.clientAddress().toString().c_str(),
                 request.getURI().c_str(),
                 request.getMethod().c_str());

            // RAII guard: AddConnection on construct, RemoveConnection on destroy.
            class ConnectionGuard {
            public:
                ConnectionGuard(PocoWebsocketServer* s, const WebSocket& w) : m_server(s), m_ws(w) {
                    if (m_server) {
                        m_server->AddConnection(m_ws);
                    }
                }
                ~ConnectionGuard() {
                    if (m_server) {
                        m_server->RemoveConnection(m_ws);
                    }
                }
                ConnectionGuard(const ConnectionGuard&) = delete;
                ConnectionGuard& operator=(const ConnectionGuard&) = delete;
            private:
                PocoWebsocketServer* m_server;
                const WebSocket& m_ws;
            };
            ConnectionGuard guard(m_server, ws);

            Poco::Buffer<char> frameBuf(0);
            Poco::Buffer<char> messageAccum(0);

            // -1 = 没有在组装消息；0x1/0x2 = 正在组装 Text/Binary
            // -1 = not assembling; else FRAME_OP_TEXT or FRAME_OP_BINARY
            int messageOpcode = -1;

            int flags = 0;

            for (;;) {
                frameBuf.resize(0);
                int n = ws.receiveFrame(frameBuf, flags);
                if (n < 0) {
                    continue;
                }
                if (n == 0 && flags == 0) {
                    // 注意: 这段代码并不是绝对严谨
                    // Defensive: Poco may return (0,0) on EOF or closed socket.
                    // Note: This could theoretically match an empty continuation frame,
                    // but such frames are practically never sent.
                    break;
                }

                // flags 是一个整数，包含了 WebSocket frame 的多个标志位
                // FRAME_OP_BITMASK 的值应该是 0x0F（15），用于屏蔽掉 flags 中除 opcode 以外的位。
                const int op = flags & WebSocket::FRAME_OP_BITMASK;
                const bool fin =
                    (flags & WebSocket::FRAME_FLAG_FIN) != 0;
                LOGD("Received frame: op=0x%x, fin=%d, payload=%d bytes", op, fin, n);

                if (op == WebSocket::FRAME_OP_CLOSE) {
                    LOGI("Received CLOSE frame from client");
                    break;
                }
                if (op == WebSocket::FRAME_OP_PING) {
                    LOGD("Received PING frame, payload=%d bytes", n);
                    // Route PONG response through the send queue to avoid
                    // concurrent sendFrame/receiveFrame on the same socket.
                    if (m_server) {
                        std::vector<uint8_t> pong;
                        if (n > 0) {
                            pong.assign(
                                reinterpret_cast<const uint8_t*>(frameBuf.begin()),
                                reinterpret_cast<const uint8_t*>(frameBuf.begin()) + n);
                        }
                        // PONG via send queue; negligible latency is acceptable.
                        m_server->SendPong(std::move(pong));
                    }
                    continue;
                }
                if (op == WebSocket::FRAME_OP_PONG) {
                    continue;
                }
                
                // 消息分片重组（Message Reassembly）
                // Data message reassembly (TEXT / BINARY / CONT)
                if (op == WebSocket::FRAME_OP_CONT) {

                    // 这个检查是在防止**"没有开头的后续分片"**
                    if (messageOpcode < 0) {
                        LOGE("CONT frame without leading TEXT/BINARY");
                        break;
                    }

                     // 防溢出：累积大小不能超过上限
                    if (static_cast<std::size_t>(n) >
                        kMaxReassembledMessage - messageAccum.size()) {
                        LOGE("reassembled message exceeds cap");
                        break;
                    }

                    // 把当前分片的 payload 追加到缓冲区
                    if (n > 0) {
                        messageAccum.append(frameBuf.begin(),
                                            static_cast<std::size_t>(n));
                    }

                    // 如果是最后一个分片（FIN=1），消息完整了
                    if (fin) {
                        if (messageOpcode == WebSocket::FRAME_OP_BINARY && m_server) {
                            LOGD("Complete BINARY message assembled: %zu bytes, enqueue to WorkLoop",
                                 messageAccum.size());
                            // 把重组后的完整二进制消息交给 WorkLoop 去解析 protobuf
                            std::vector<uint8_t> data(
                                reinterpret_cast<const uint8_t*>(messageAccum.begin()),
                                reinterpret_cast<const uint8_t*>(messageAccum.begin()) + messageAccum.size());
                            m_server->EnqueueWork(std::move(data));
                        }
                        // Text 消息直接忽略（本项目只用 Binary 传 protobuf）
                        // Text messages are ignored (no echo)
                        messageOpcode = -1;  // 重置：不在组装状态
                        messageAccum.resize(0, false);  // 清空缓冲区
                    }
                    continue;
                }

                if (op == WebSocket::FRAME_OP_TEXT || op == WebSocket::FRAME_OP_BINARY) {
                    if (messageOpcode >= 0) {
                        LOGE("new TEXT/BINARY while fragmented message unfinished");
                        break;
                    }
                    messageOpcode = op;
                    messageAccum.resize(0, false);
                    if (static_cast<std::size_t>(n) > kMaxReassembledMessage) {
                        LOGE("first fragment exceeds cap");
                        break;
                    }
                    if (n > 0) {
                        messageAccum.append(frameBuf.begin(),
                                            static_cast<std::size_t>(n));
                    }
                    if (fin) {
                        if (messageOpcode == WebSocket::FRAME_OP_BINARY && m_server) {
                            LOGD("Complete BINARY message assembled: %zu bytes, enqueue to WorkLoop",
                                 messageAccum.size());
                            std::vector<uint8_t> data(
                                reinterpret_cast<const uint8_t*>(messageAccum.begin()),
                                reinterpret_cast<const uint8_t*>(messageAccum.begin()) + messageAccum.size());
                            m_server->EnqueueWork(std::move(data));
                        }
                        // Text messages are ignored (no echo)
                        messageOpcode = -1;
                        messageAccum.resize(0, false);
                    }
                    continue;
                }

                LOGE("unexpected WebSocket opcode 0x%x", op);
                break;
            }
        } catch (const WebSocketException &exc) {
            LOGE("WebSocketException: %s", exc.displayText().c_str());
            switch (exc.code()) {
            case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
                response.set("Sec-WebSocket-Version",
                             WebSocket::WEBSOCKET_VERSION);
                // fallthrough
            case WebSocket::WS_ERR_NO_HANDSHAKE:
            case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
            case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
                response.setStatusAndReason(
                    Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                response.setContentLength(0);
                response.send();
                break;
            default:
                break;
            }
        } catch (const std::exception &e) {
            LOGE("WebSocket handler error: %s", e.what());
        }
    }

private:
    PocoWebsocketServer* m_server;
};

class WsHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory {
public:
    explicit WsHandlerFactory(PocoWebsocketServer* server) : m_server(server) {}

    Poco::Net::HTTPRequestHandler *createRequestHandler(
        const Poco::Net::HTTPServerRequest &request) override {
        LOGI("WsHandlerFactory: incoming request from %s, method=%s, URI=%s, Upgrade=%s, Connection=%s",
             request.clientAddress().toString().c_str(),
             request.getMethod().c_str(),
             request.getURI().c_str(),
             request.find("Upgrade") != request.end() ? request["Upgrade"].c_str() : "<missing>",
             request.find("Connection") != request.end() ? request["Connection"].c_str() : "<missing>");
        const bool wsUpgrade =
            request.find("Upgrade") != request.end() &&
            Poco::icompare(request["Upgrade"], "websocket") == 0;
        if (!wsUpgrade) {
            LOGI("WsHandlerFactory: rejecting non-WebSocket request (Upgrade missing or not 'websocket')");
            return new NotFoundHandler();
        }
        if (!IsAllowedWebSocketPath(request.getURI())) {
            LOGI("WebSocket upgrade ignored (path mismatch): %s (need %s)",
                 request.getURI().c_str(), kWebSocketPath);
            return new NotFoundHandler();
        }
        LOGI("WsHandlerFactory: accepting WebSocket upgrade for URI=%s", request.getURI().c_str());
        return new WebSocketRequestHandler(m_server);
    }

private:
    PocoWebsocketServer* m_server;
};

}  // namespace

// ===========================================================================
// PocoWebsocketServer implementation
// ===========================================================================

PocoWebsocketServer::PocoWebsocketServer() {}

PocoWebsocketServer::~PocoWebsocketServer() { Stop(); }

// ---------------------------------------------------------------------------
// Send thread (server-lifetime): exclusively calls sendFrame on the active
// WebSocket. This prevents concurrent read/write on the same socket object.
// Handles both BINARY data and PONG control frames.
// ---------------------------------------------------------------------------
void PocoWebsocketServer::SendLoop() {
    LOGI("Send thread started");
    int consecutiveErrors = 0;
    while (true) {
        SendItem item;
        {
            std::unique_lock<std::mutex> lock(m_sendMutex);
            m_sendCv.wait(lock, [this] {
                return !m_sendQueue.empty() || !m_running;
            });
            if (!m_running && m_sendQueue.empty()) {
                break;
            }
            item = std::move(m_sendQueue.front());
            m_sendQueue.pop();
        }

        // sendFrame is called ONLY from this thread, never from the handler thread.
        std::lock_guard<std::mutex> connLock(m_connectionMutex);
        if (!m_connection) {
            continue;  // No active connection, discard
        }
        try {
            if (item.isClose) {
                LOGI("SendLoop sending CLOSE frame");
                m_connection->sendFrame(
                    "", 0,
                    static_cast<int>(Poco::Net::WebSocket::FRAME_FLAG_FIN) |
                    static_cast<int>(Poco::Net::WebSocket::FRAME_OP_CLOSE));
                break;  // graceful exit after CLOSE
            }
            if (item.isPong) {
                LOGD("SendLoop sending PONG, payload=%zu bytes", item.data.size());
                m_connection->sendFrame(
                    item.data.data(), static_cast<int>(item.data.size()),
                    static_cast<int>(Poco::Net::WebSocket::FRAME_FLAG_FIN) |
                    static_cast<int>(Poco::Net::WebSocket::FRAME_OP_PONG));
            } else {
                LOGD("SendLoop sending BINARY, payload=%zu bytes", item.data.size());
                m_connection->sendFrame(
                    item.data.data(), static_cast<int>(item.data.size()),
                    Poco::Net::WebSocket::FRAME_BINARY);
            }
            consecutiveErrors = 0;
        } catch (const std::exception& e) {
            LOGE("SendLoop sendFrame failed: %s", e.what());
            if (++consecutiveErrors >= 3) {
                LOGE("SendLoop consecutive errors, resetting connection");
                {
                    std::lock_guard<std::mutex> connLock(m_connectionMutex);
                    m_connection.reset();
                }
                // Notify outside the lock to avoid re-entrant deadlock if the
                // callback calls back into this class (e.g. SendBinary).
                NotifyConnectionState(false);
                consecutiveErrors = 0;
            }
        }
    }
    LOGI("Send thread stopped");
}

// ---------------------------------------------------------------------------
// Work thread (server-lifetime): processes incoming binary messages from the
// work queue. Decouples the Poco handler thread from business logic
// (protobuf parsing, file I/O, etc.) so receiveFrame is never blocked.
// ---------------------------------------------------------------------------
void PocoWebsocketServer::WorkLoop() {
    LOGI("Work thread started");
    while (true) {
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(m_workMutex);
            m_workCv.wait(lock, [this] {
                return !m_workQueue.empty() || !m_running;
            });
            if (!m_running && m_workQueue.empty()) {
                break;  // 没活干且服务器停了，退出
            }
            data = std::move(m_workQueue.front());
            m_workQueue.pop();
        }

        // 调用回调：解析 protobuf，分发到文件传输模块
        if (m_onBinaryMessage) {
            LOGD("WorkLoop processing binary message: %zu bytes", data.size());
            try {
                m_onBinaryMessage(data.data(), data.size());
            } catch (const std::exception& e) {
                LOGE("WorkLoop handler exception: %s", e.what());
            }
        }
    }
    LOGI("Work thread stopped");
}

// ---------------------------------------------------------------------------
// Public API: EnqueueWork (called by handler thread)
// ---------------------------------------------------------------------------
void PocoWebsocketServer::EnqueueWork(std::vector<uint8_t> data) {
    {
        std::unique_lock<std::mutex> lock(m_workMutex);
        // 如果队列满了，阻塞等待（背压）
        m_workCv.wait(lock, [this] {
            return m_workQueue.size() < kMaxWorkQueueSize || !m_running;
        });
        if (!m_running) {
            return; // 服务器已停止，不收了
        }
        // 把数据放入工作队列
        m_workQueue.push(std::move(data));
        LOGD("EnqueueWork: %zu bytes, queue depth %zu/%zu",
             data.size(), m_workQueue.size(), kMaxWorkQueueSize);
    }
    // 唤醒 Work 线程："有新活了！"
    m_workCv.notify_one();
}

// ---------------------------------------------------------------------------
// Public API: SendPong (called by handler thread for PING responses)
// Routes PONG through the send queue so handler never calls sendFrame.
// ---------------------------------------------------------------------------
void PocoWebsocketServer::SendPong(std::vector<uint8_t> payload) {
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (!m_running) {
            return;
        }
        if (m_sendQueue.size() >= kMaxSendQueueSize) {
            LOGW("Send queue full, dropping PONG");
            return;
        }
        SendItem item;
        item.isPong = true;
        item.data = std::move(payload);
        m_sendQueue.push(std::move(item));
    }
    m_sendCv.notify_one();
}

// ===========================================================================
// Start / Stop
// ===========================================================================

bool PocoWebsocketServer::Start(int port) {
    if (m_running) {
        return true;
    }

    try {
        m_socket = std::make_unique<Poco::Net::ServerSocket>(
            static_cast<Poco::UInt16>(port));
        auto *params = new Poco::Net::HTTPServerParams();
        params->setMaxQueued(64);
        params->setMaxThreads(8);
        // NOTE: Do NOT set timeout to 0. Poco's HTTPServerSession::hasMoreRequests()
        // uses this timeout in socket().poll(timeout, SELECT_READ). A 0 timeout
        // causes poll() to return immediately, often before the client request
        // arrives, making the server close every connection with an empty reply.
        // Default is 60s, which is fine for the HTTP upgrade phase.
        // params->setTimeout(Poco::Timespan(0, 0));

        // Start work & send threads BEFORE the HTTP server begins accepting
        m_running = true;
        m_sendThread = std::thread(&PocoWebsocketServer::SendLoop, this);
        m_workThread = std::thread(&PocoWebsocketServer::WorkLoop, this);

        m_server = std::make_unique<Poco::Net::HTTPServer>(
            new WsHandlerFactory(this), *m_socket, params);
        m_server->start();

        LOGI("Poco WebSocket server listening on port %d, path %s", port,
             kWebSocketPath);
        return true;
    } catch (const std::exception &e) {
        LOGE("Poco WebSocket server start failed: %s", e.what());
        m_running = false;
        // Stop threads if they were started
        m_sendCv.notify_one();
        m_workCv.notify_one();
        if (m_sendThread.joinable()) m_sendThread.join();
        if (m_workThread.joinable()) m_workThread.join();
        m_server.reset();
        m_socket.reset();
        return false;
    }
}

void PocoWebsocketServer::Stop() {
    if (!m_running) {
        return;
    }
    m_running = false;

    // 1. Signal SendLoop to send CLOSE frame and exit.
    //    CLOSE item bypasses the bound check because m_running is already false.
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        SendItem item;
        item.isClose = true;
        m_sendQueue.push(std::move(item));
    }
    m_sendCv.notify_one();

    // 2. Close the socket BEFORE joining SendLoop. If SendLoop is blocked in
    //    sendFrame() on a dead/stalled peer, closing the socket unblocks it and
    //    lets it exit. Closing before the join prevents Stop() from hanging.
    {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        if (m_connection) {
            try {
                m_connection->close();
            } catch (...) {}
        }
    }

    // 3. Join SendLoop — after this, NO thread calls sendFrame.
    if (m_sendThread.joinable()) m_sendThread.join();

    // 4. Stop Poco HTTP server (waits for handler threads to finish)
    try {
        if (m_server) {
            m_server->stopAll(true);
        }
    } catch (...) {}
    m_server.reset();
    m_socket.reset();

    // 5. Wake up and join work thread
    m_workCv.notify_one();
    if (m_workThread.joinable()) m_workThread.join();

    // 6. Clear residual queues
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        std::queue<SendItem> empty;
        m_sendQueue.swap(empty);
    }
    {
        std::lock_guard<std::mutex> lock(m_workMutex);
        std::queue<std::vector<uint8_t>> empty;
        m_workQueue.swap(empty);
    }

    // 7. Clear connection and notify disconnected (outside lock to avoid deadlock)
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        if (m_connection) {
            m_connection.reset();
            shouldNotify = true;
        }
    }
    if (shouldNotify) {
        NotifyConnectionState(false);
    }

    LOGI("Poco WebSocket server stopped");
}

bool PocoWebsocketServer::IsRunning() const { return m_running; }

// ===========================================================================
// SendBinary: non-blocking enqueue for the send thread
// ===========================================================================
bool PocoWebsocketServer::SendBinary(const void* data, size_t len) {
    {
        std::unique_lock<std::mutex> lock(m_sendMutex);
        m_sendCv.wait(lock, [this] {
            return m_sendQueue.size() < kMaxSendQueueSize || !m_running;
        });
        if (!m_running) {
            return false;
        }
        // Fast-fail if no active connection to avoid queuing data that would be discarded.
        {
            std::lock_guard<std::mutex> connLock(m_connectionMutex);
            if (!m_connection) {
                return false;
            }
        }
        SendItem item;
        item.isPong = false;
        item.isClose = false;
        LOGD("SendBinary: %zu bytes requested", len);
        item.data.assign(
            static_cast<const uint8_t*>(data),
            static_cast<const uint8_t*>(data) + len);
        m_sendQueue.push(std::move(item));
    }
    m_sendCv.notify_one();
    return true;
}

bool PocoWebsocketServer::HasConnection() const {
    std::lock_guard<std::mutex> lock(m_connectionMutex);
    return m_connection.has_value();
}

void PocoWebsocketServer::SetOnBinaryMessage(BinaryMessageHandler handler) {
    m_onBinaryMessage = std::move(handler);
}

void PocoWebsocketServer::SetOnConnectionStateChanged(ConnectionStateHandler handler) {
    m_onConnectionStateChanged = std::move(handler);
}

// ===========================================================================
// Connection lifecycle (no thread management — threads are server-lifetime)
// ===========================================================================

void PocoWebsocketServer::AddConnection(const Poco::Net::WebSocket& ws) {
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        if (m_connection) {
            LOGI("New WebSocket connection overrides previous one, closing old socket");
            try {
                m_connection->close();
            } catch (const std::exception& e) {
                LOGE("Failed to close old WebSocket: %s", e.what());
            }
        }
        m_connection = ws;
        shouldNotify = true;
        LOGI("AddConnection: WebSocket connection active");
    }
    if (shouldNotify) {
        NotifyConnectionState(true);
    }
}

void PocoWebsocketServer::RemoveConnection(const Poco::Net::WebSocket& ws) {
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        if (m_connection && *m_connection == ws) {
            LOGI("RemoveConnection: WebSocket connection closed");
            m_connection.reset();
            shouldNotify = true;
        } else if (m_connection) {
            LOGD("RemoveConnection called by stale handler (already replaced)");
        }
    }
    if (shouldNotify) {
        NotifyConnectionState(false);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void PocoWebsocketServer::NotifyConnectionState(bool connected) {
    if (m_onConnectionStateChanged) {
        try {
            m_onConnectionStateChanged(connected);
        } catch (const std::exception& e) {
            LOGE("ConnectionStateHandler exception: %s", e.what());
        }
    }
}

size_t PocoWebsocketServer::GetSendQueueDepth() const {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    return m_sendQueue.size();
}

size_t PocoWebsocketServer::GetWorkQueueDepth() const {
    std::lock_guard<std::mutex> lock(m_workMutex);
    return m_workQueue.size();
}

}  // namespace scrcpy
